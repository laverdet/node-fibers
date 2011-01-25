#include "coroutine.h"
#include <assert.h>
#include <node/v8.h>

#include <vector>

#include <iostream>

#define THROW(x, m) return ThrowException(x(String::New(m)))

using namespace std;
using namespace v8;

class Fiber {
#define Unwrap(target, handle) \
  assert(!handle.IsEmpty()); \
  if (!handle->IsObject() || handle->GetHiddenValue(fiber_token).IsEmpty()) { \
    THROW(Exception::TypeError, "Illegal invocation"); \
  } \
  assert(handle->InternalFieldCount() == 1); \
  target = *static_cast<Fiber*>(handle->GetPointerFromInternalField(0));

  private:
    static Locker* locker; // Node does not use locks or threads, so we need a global lock
    static Persistent<FunctionTemplate> tmpl;
    static Fiber* current;
    static vector<Fiber*> orphaned_fibers;
    static Persistent<Value> fatal_stack;
    static Persistent<String> fiber_token;

    Persistent<Object> handle;
    Persistent<Function> cb;
    Persistent<Context> v8_context;
    Persistent<Value> zombie_exception;
    Persistent<Value> yielded;
    bool yielded_exception;
    Coroutine* entry_fiber;
    Coroutine* this_fiber;
    bool started;
    bool yielding;
    bool zombie;
    bool resetting;

    Fiber(Persistent<Object> handle, Persistent<Function> cb, Persistent<Context> v8_context) :
      handle(handle),
      cb(cb),
      v8_context(v8_context),
      started(false),
      yielding(false),
      zombie(false),
      resetting(false) {
      MakeWeak();
      handle->SetPointerInInternalField(0, this);
    }

    virtual ~Fiber() {
      assert(!this->started);
      handle.Dispose();
      cb.Dispose();
      v8_context.Dispose();
    }

    /**
     * Call MakeWeak if it's ok for v8 to garbage collect this Fiber.
     * i.e. After fiber completes, while yielded, or before started
     */
    void MakeWeak() {
      handle.MakeWeak(this, WeakCallback);
    }

    /**
     * And call ClearWeak if it's not ok for v8 to garbage collect this Fiber.
     * i.e. While running.
     */
    void ClearWeak() {
      handle.ClearWeak();
    }

    /**
     * Called when there are no more references to this object in Javascript. If this happens and
     * the fiber is currently suspended we'll unwind the fiber's stack by throwing exceptions in
     * order to clear all references.
     */
    static void WeakCallback(Persistent<Value> value, void *data) {
      Fiber& that = *static_cast<Fiber*>(data);
      assert(that.handle == value);
      assert(value.IsNearDeath());
      assert(current != &that);

      // We'll unwind running fibers later... doing it from the garbage collector is bad news.
      if (that.started) {
        assert(that.yielding);
        orphaned_fibers.push_back(&that);
        that.ClearWeak();
        return;
      }

      delete &that;
    }

    /**
     * When the v8 garbage collector notifies us about dying fibers instead of unwindng their
     * stack as soon as possible we put them aside to unwind later. Unwinding from the garbage
     * collector leads to exponential time garbage collections if there are many orphaned Fibers,
     * there's also the possibility of running out of stack space. It's generally bad news.
     *
     * So instead we have this function to clean up all the fibers after the garbage collection
     * has finished.
     */
    static void DestroyOrphans() {
      if (orphaned_fibers.empty()) {
        return;
      }
      vector<Fiber*> orphans(orphaned_fibers);
      orphaned_fibers.clear();

      for (vector<Fiber*>::iterator ii = orphans.begin(); ii != orphans.end(); ++ii) {
        Fiber& that = **ii;
        that.UnwindStack();

        if (that.yielded_exception) {
          // If you throw an exception from a fiber that's being garbage collected there's no way
          // to bubble that exception up to the application.
          String::Utf8Value stack(fatal_stack);
          cerr <<
            "An exception was thrown from a Fiber which was being garbage collected. This error "
            "can not be gracefully recovered from. The only acceptable behavior is to terminate "
            "this application. The exception appears below:\n\n"
            <<*stack <<"\n";
          exit(1);
        } else {
          fatal_stack.Dispose();
        }

        that.yielded.Dispose();
        that.MakeWeak();
      }
    }

    /**
     * Instantiate a new Fiber object. When a fiber is created it only grabs a handle to the
     * callback; it doesn't create any new contexts until run() is called.
     */
    static Handle<Value> New(const Arguments& args) {

      HandleScope scope;
      if (args.Length() != 1) {
        THROW(Exception::TypeError, "Fiber expects 1 argument");
      } else if (!args[0]->IsFunction()) {
        THROW(Exception::TypeError, "Fiber expects a function");
      } else if (!args.IsConstructCall()) {
        Handle<Value> argv[1] = { args[0] };
        return tmpl->GetFunction()->NewInstance(1, argv);
      }

      Handle<Function> fn = Handle<Function>::Cast(args[0]);
      args.This()->SetHiddenValue(fiber_token, Boolean::New(true));
      new Fiber(
        Persistent<Object>::New(args.This()),
        Persistent<Function>::New(fn),
        Persistent<Context>::New(Context::GetCurrent()));
      return args.This();
    }

    /**
     * Resume the fiber.
     */
    static Handle<Value> Resume(const Arguments& args) {
      HandleScope scope;
      Unwrap(Fiber& that, args.This());

      // There seems to be no better place to put this check..
      DestroyOrphans();

      if (&that == current) {
        THROW(Exception::Error, "This Fiber is already running");
      } else if (args.Length() > 1) {
        THROW(Exception::TypeError, "resume() excepts 1 or no arguments");
      } else if (!that.started) {
        THROW(Exception::Error, "This Fiber is not started");
      }

      assert(that.yielding);
      that.yielded_exception = false;
      if (args.Length()) {
        that.yielded = Persistent<Value>::New(args[0]);
      } else {
        that.yielded = Persistent<Value>::New(Undefined());
      }
      that.SwapContext();
      return that.ReturnYielded();
    }

    /**
     * Begin or resume the fiber. If the fiber is not currently running a new context will
     * be created and the callback will start. Otherwise we switch back into the exist context.
     */
    static Handle<Value> Run(const Arguments& args) {
      HandleScope scope;
      Unwrap(Fiber& that, args.This());

      // There seems to be no better place to put this check..
      DestroyOrphans();

      if (&that == current) {
        THROW(Exception::Error, "This Fiber is already running");
      } else if (args.Length() > 1) {
        THROW(Exception::TypeError, "run() excepts 1 or no arguments");
      }

      if (!that.started) {
        // Create a new context with entry point `Fiber::RunFiber()`.
        that.started = true;
        void** data = new void*[2];
        data[0] = (void*)&args;
        data[1] = &that;
        that.this_fiber = &Coroutine::create_fiber((void (*)(void*))RunFiber, data);
        V8::AdjustAmountOfExternalAllocatedMemory(that.this_fiber->size());
      } else {
        // If the fiber is currently running put the first parameter to `run()` on `yielded`, then
        // the pending call to `yield()` will return that value. `yielded` in this case is just a
        // misnomer, we're just reusing the same handle.
        that.yielded_exception = false;
        if (args.Length()) {
          that.yielded = Persistent<Value>::New(args[0]);
        } else {
          that.yielded = Persistent<Value>::New(Undefined());
        }
      }
      that.SwapContext();
      return that.ReturnYielded();
    }

    /**
     * Throw an exception into a currently yielding fiber.
     */
    static Handle<Value> ThrowInto(const Arguments& args) {
      HandleScope scope;
      Unwrap(Fiber& that, args.This());

      if (!that.yielding) {
        THROW(Exception::Error, "This Fiber is not yielding");
      } else if (args.Length() == 0) {
        that.yielded = Persistent<Value>::New(Undefined());
      } else if (args.Length() == 1) {
        that.yielded = Persistent<Value>::New(args[0]);
      } else {
        THROW(Exception::TypeError, "throwInto() expects 1 or no arguments");
      }
      that.yielded_exception = true;
      that.SwapContext();
      return that.ReturnYielded();
    }

    /**
     * Unwinds a currently running fiber. If the fiber is not running then this function has no
     * effect.
     */
    static Handle<Value> Reset(const Arguments& args) {
      HandleScope scope;
      Unwrap(Fiber& that, args.This());

      if (!that.started) {
        return Undefined();
      } else if (!that.yielding) {
        THROW(Exception::Error, "This Fiber is not yielding");
      } else if (args.Length()) {
        THROW(Exception::TypeError, "reset() expects no arguments");
      }

      that.resetting = true;
      that.UnwindStack();
      that.resetting = false;
      that.MakeWeak();

      Handle<Value> val = that.yielded;
      that.yielded.Dispose();
      if (that.yielded_exception) {
        return ThrowException(val);
      } else {
        return val;
      }
    }

    /**
     * Turns the fiber into a zombie and unwinds its whole stack.
     *
     * After calling this function you must either destroy this fiber or call MakeWeak() or it will
     * be leaked.
     */
    void UnwindStack() {
      assert(!zombie);
      assert(started);
      assert(yielding);
      HandleScope scope;
      zombie = true;

      // Setup an exception which will be thrown and rethrown from Fiber::Yield()
      Local<Value> zombie_exception = Exception::Error(String::New("This Fiber is a zombie"));
      this->zombie_exception = Persistent<Value>::New(zombie_exception);
      yielded = Persistent<Value>::New(zombie_exception);
      yielded_exception = true;

      // Swap context back to Fiber::Yield() which will throw an exception to unwind the stack.
      // Futher calls to yield from this fiber will rethrow the same exception.
      SwapContext();
      assert(!started);
      zombie = false;

      // Make sure this is the exception we threw
      if (yielded_exception && yielded == zombie_exception) {
        yielded_exception = false;
        yielded.Dispose();
        yielded = Persistent<Value>::New(Undefined());
      }
      this->zombie_exception.Dispose();
    }

    /**
     * Common logic between Run(), ThrowInto(), and UnwindStack(). This is essentially just a
     * wrapper around this->fiber->() which also handles all the bookkeeping needed.
     */
    void SwapContext() {

      entry_fiber = &Coroutine::current();
      Fiber* last_fiber = current;
      current = this;

      // This will jump into either `RunFiber()` or `Yield()`, depending on if the fiber was
      // already running.
      {
        Unlocker unlocker;
        this_fiber->run();
      }

      // At this point the fiber either returned or called `yield()`.
      current = last_fiber;
    }

    /**
     * Grabs and resets this fiber's yielded value.
     */
    Handle<Value> ReturnYielded() {
      HandleScope scope;
      Handle<Value> val = yielded;
      yielded.Dispose();
      if (yielded_exception) {
        return ThrowException(val);
      } else {
        return val;
      }
    }

    /**
     * This is the entry point for a new fiber, from `run()`.
     */
    static void RunFiber(void** data) {
      const Arguments* args = (const Arguments*)data[0];
      Fiber& that = *(Fiber*)data[1];
      delete[] data;

      {
        Locker locker;
        HandleScope scope;

        // Set stack guard for this "thread"
        ResourceConstraints constraints;
        constraints.set_stack_limit((uint32_t*)that.this_fiber->bottom());
        SetResourceConstraints(&constraints);

        TryCatch try_catch;
        that.ClearWeak();
        that.v8_context->Enter();

        if (args->Length()) {
          Handle<Value> argv[1] = { (*args)[0] };
          that.yielded = Persistent<Value>::New(that.cb->Call(that.v8_context->Global(), 1, argv));
        } else {
          that.yielded = Persistent<Value>::New(that.cb->Call(that.v8_context->Global(), 0, NULL));
        }

        if (try_catch.HasCaught()) {
          that.yielded.Dispose();
          that.yielded = Persistent<Value>::New(try_catch.Exception());
          that.yielded_exception = true;
          if (that.zombie && !that.resetting && that.yielded != that.zombie_exception) {
            // Throwing an exception from a garbage sweep
            fatal_stack = Persistent<Value>::New(try_catch.StackTrace());
          }
        } else {
          that.yielded_exception = false;
        }

        // Do not invoke the garbage collector if there's no context on the stack. It will seg fault
        // otherwise.
        V8::AdjustAmountOfExternalAllocatedMemory(-that.this_fiber->size());

        // Don't make weak until after notifying the garbage collector. Otherwise it may try and
        // free this very fiber!
        if (!that.zombie) {
          that.MakeWeak();
        }

        // Now safe to leave the context, this stack is done with JS.
        that.v8_context->Exit();
      }

      // The function returned (instead of yielding).
      that.started = false;
      that.this_fiber->finish(*that.entry_fiber);
    }

    /**
     * Yield control back to the function that called `run()`. The first parameter to this function
     * is returned from `run()`. The context is saved, to be later resumed from `run()`.
     */
    static Handle<Value> Yield(const Arguments& args) {
      HandleScope scope;

      if (current == NULL) {
        THROW(Exception::Error, "yield() called with no fiber running");
      }

      Fiber& that = *current;

      if (that.zombie) {
        return ThrowException(that.zombie_exception);
      } else if (args.Length() == 0) {
        that.yielded = Persistent<Value>::New(Undefined());
      } else if (args.Length() == 1) {
        that.yielded = Persistent<Value>::New(args[0]);
      } else {
        THROW(Exception::TypeError, "yield() expects 1 or no arguments");
      }
      that.yielded_exception = false;

      // While not running this can be garbage collected if no one has a handle.
      that.MakeWeak();

      // Return control back to `Fiber::run()`. While control is outside this function we mark it as
      // ok to garbage collect. If no one ever has a handle to resume the function it's harmful to
      // keep the handle around.
      {
        Unlocker unlocker;
        that.yielding = true;
        that.entry_fiber->run();
        that.yielding = false;
      }
      // Now `run()` has been called again.

      // Don't garbage collect anymore!
      that.ClearWeak();

      // Return the yielded value
      return that.ReturnYielded();
    }

    /**
     * Getters for `started`, and `current`.
     */
    static Handle<Value> GetStarted(Local<String> property, const AccessorInfo& info) {
      Unwrap(Fiber& that, info.This());
      return Boolean::New(that.started);
    }

    static Handle<Value> GetCurrent(Local<String> property, const AccessorInfo& info) {
      if (current) {
        return current->handle;
      } else {
        return Undefined();
      }
    }

  public:
    /**
     * Initialize the Fiber library.
     */
    static void Init(Handle<Object> target) {
      // Use a locker which won't get destroyed when this library gets unloaded. This is a hack
      // to prevent v8 from trying to clean up this "thread" while the whole application is
      // shutting down. TODO: There's likely a better way to accomplish this, but since the
      // application is going down lost memory isn't the end of the world. But with a regular lock
      // there's seg faults when node shuts down.
      Fiber::locker = new Locker;
      current = NULL;
      HandleScope scope;
      tmpl = Persistent<FunctionTemplate>::New(FunctionTemplate::New(New));
      tmpl->SetClassName(String::NewSymbol("Fiber"));

      fiber_token = Persistent<String>::New(String::NewSymbol("is_fiber"));
      tmpl->InstanceTemplate()->SetInternalFieldCount(1);

      Handle<ObjectTemplate> proto = tmpl->PrototypeTemplate();
      proto->Set(String::NewSymbol("reset"), FunctionTemplate::New(Reset));
      proto->Set(String::NewSymbol("resume"), FunctionTemplate::New(Resume));
      proto->Set(String::NewSymbol("run"), FunctionTemplate::New(Run));
      proto->Set(String::NewSymbol("throwInto"), FunctionTemplate::New(ThrowInto));
      proto->SetAccessor(String::NewSymbol("started"), GetStarted);

      Handle<Function> fn = tmpl->GetFunction();
      fn->SetAccessor(String::NewSymbol("current"), GetCurrent);
      target->Set(String::NewSymbol("Fiber"), fn);
      target->Set(String::NewSymbol("yield"), FunctionTemplate::New(Yield)->GetFunction());
    }
};

Persistent<FunctionTemplate> Fiber::tmpl;
Locker* Fiber::locker;
Fiber* Fiber::current = NULL;
vector<Fiber*> Fiber::orphaned_fibers;
Persistent<Value> Fiber::fatal_stack;
Persistent<String> Fiber::fiber_token;

extern "C" void init(Handle<Object> target) {
  HandleScope scope;
  Handle<Object> global = Context::GetCurrent()->Global();
  assert(Coroutine::is_local_storage_enabled());
  Fiber::Init(global);
}
