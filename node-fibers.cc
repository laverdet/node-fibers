#include "coroutine.h"
#include <assert.h>
#include <node/node.h>

#include <iostream>

#define THROW(x, m) return ThrowException(x(String::New(m)))

using namespace std;
using namespace v8;

class Fiber {
  private:
    static Locker locker; // Node does not use locks or threads, so we need a global lock
    static Persistent<FunctionTemplate> tmpl;
    static Fiber* current;

    Persistent<Object> handle;
    Persistent<Function> cb;
    Persistent<Context> v8_context;
    Persistent<Value> yielded;
    bool yielded_exception;
    Coroutine* entry_fiber;
    Coroutine* this_fiber;
    bool started;
    bool zombie;

  public:
    Fiber(Persistent<Object> handle, Persistent<Function> cb, Persistent<Context> v8_context) :
      handle(handle),
      cb(cb),
      v8_context(v8_context),
      started(false),
      zombie(false) {
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
      if (that.started) {
        Locker locker;
        HandleScope scope;
        that.zombie = true;

        // Swap context back to `Fiber::Yield()` which will throw an exception to unwind the stack.
        // Futher calls to yield from this fiber will also throw.
        that.yielded = Persistent<Value>::New(
          Exception::Error(String::New("This Fiber is a zombie")));
        that.yielded_exception = true;
        {
          Unlocker locker;
          that.this_fiber->run();
          assert(!that.started);
        }

        that.yielded.Dispose();
        if (that.yielded_exception) {
          // TODO: Check for Zombie exception?
        }

        // It's possible that someone else grabbed a reference to the currently running fiber while
        // we were unwinding it. In this case they can reuse the fiber, but the stack in progress
        // is already gone.
        if (!value.IsNearDeath()) {
          that.zombie = false;
          that.MakeWeak();
          return;
        }
      }

      delete &that;
    }

    /**
     * Unwrap a Fiber instance from a `this` pointer.
     * TODO: Check that `handle` is actually a Fiber or Bad Things may happen.
     */
    static Fiber& Unwrap(Handle<Object> handle) {
      assert(!handle.IsEmpty());
      assert(handle->InternalFieldCount() == 1);
      return *static_cast<Fiber*>(handle->GetPointerFromInternalField(0));
    }

    /**
     * Initialize the Fiber library.
     */
    static void Init(Handle<Object> target) {
      HandleScope scope;
      tmpl = Persistent<FunctionTemplate>::New(FunctionTemplate::New(New));
      tmpl->InstanceTemplate()->SetInternalFieldCount(1);
      tmpl->SetClassName(String::NewSymbol("Fiber"));

      Handle<ObjectTemplate> proto = tmpl->PrototypeTemplate();
      proto->Set(String::NewSymbol("run"), FunctionTemplate::New(Run));
      proto->SetAccessor(String::NewSymbol("started"), GetStarted);

      Handle<Function> fn = tmpl->GetFunction();
      fn->SetAccessor(String::NewSymbol("current"), GetCurrent);
      target->Set(String::NewSymbol("Fiber"), fn);
      target->Set(String::NewSymbol("yield"), FunctionTemplate::New(Yield)->GetFunction());
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
        Local<Value> argv[1] = { Local<Value>::New(args[0]) };
        return tmpl->GetFunction()->NewInstance(1, argv);
      }

      Handle<Function> fn = Local<Function>::Cast(args[0]);
      new Fiber(
        Persistent<Object>::New(args.This()),
        Persistent<Function>::New(fn),
        Persistent<Context>::New(Context::GetCurrent()));
      return args.This();
    }

    /**
     * Begin or resume the current fiber. If the fiber is not currently running a new context will
     * be created and the callback will start. Otherwise we switch back into the exist context.
     */
    static Handle<Value> Run(const Arguments& args) {
      HandleScope scope;
      Fiber& that = Unwrap(args.This());
      that.entry_fiber = &Coroutine::current();

      if (!that.started) {
        // Create a new context with entry point `Fiber::RunFiber()`.
        that.started = true;
        void** data = new void*[2];
        data[0] = (void*)&args;
        data[1] = &that;
        that.this_fiber = &that.entry_fiber->new_fiber((void (*)(void*))RunFiber, data);
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

      // This will jump into either `RunFiber()` or `Yield()`, depending on if the fiber was
      // already running.
      Fiber* last_fiber = current;
      current = &that;
      {
        Unlocker unlocker;
        that.this_fiber->run();
      }
      // At this point the fiber either returned or called `yield()`.
      current = last_fiber;

      // Return the yielded value.
      Handle<Value> val = Local<Value>::New(that.yielded);
      that.yielded.Dispose();
      if (that.yielded_exception) {
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
        Local<Value> argv[1] = { Local<Value>::New((*args)[0]) };
        that.yielded = Persistent<Value>::New(that.cb->Call(that.v8_context->Global(), 1, argv));
      } else {
        that.yielded = Persistent<Value>::New(that.cb->Call(that.v8_context->Global(), 0, NULL));
      }

      if (try_catch.HasCaught()) {
        that.yielded.Dispose();
        that.yielded = Persistent<Value>::New(try_catch.Exception());
        that.yielded_exception = true;
      } else {
        that.yielded_exception = false;
      }

      // Do not invoke the garbage collector if there's no context on the stack. It will seg fault
      // otherwise.
      V8::AdjustAmountOfExternalAllocatedMemory(-that.this_fiber->size());

      // Don't make weak until after notifying the garbage collector. Otherwise it may try and
      // free this very fiber!
      that.MakeWeak();

      // Now safe to leave the context, this stack is done with JS.
      that.v8_context->Exit();

      // The function returned (instead of yielding).
      that.started = false;
    }

    /**
     * Yield control back to the function that called `run()`. The first parameter to this function
     * is returned from `run()`. The context is saved, to be later resumed from `run()`.
     */
    static Handle<Value> Yield(const Arguments& args) {
      HandleScope scope;
      Fiber& that = *current;
      if (that.zombie) {
        THROW(Exception::Error, "This Fiber is a zombie");
      }

      that.yielded_exception = false;
      if (args.Length()) {
        that.yielded = Persistent<Value>::New(args[0]);
      } else {
        that.yielded = Persistent<Value>::New(Undefined());
      }

      // While not running this can be garbage collected if no one has a handle.
      that.MakeWeak();

      // Return control back to `Fiber::run()`. While control is outside this function we mark it as
      // ok to garbage collect. If no one ever has a handle to resume the function it's harmful to
      // keep the handle around.
      {
        Unlocker unlocker;
        that.entry_fiber->run();
      }
      // Now `run()` has been called again.

      // Don't garbage collect anymore!
      that.ClearWeak();

      // `yielded` will contain the first parameter to `run()`
      Handle<Value> val = Local<Value>::New(that.yielded);
      that.yielded.Dispose();
      if (that.yielded_exception) {
        return ThrowException(val);
      } else {
        return val;
      }
    }

    /**
     * Getters for `started`, and `current`.
     */
    static Handle<Value> GetStarted(Local<String> property, const AccessorInfo& info) {
      Fiber& that = Unwrap(info.This());
      return Boolean::New(that.started);
    }

    static Handle<Value> GetCurrent(Local<String> property, const AccessorInfo& info) {
      if (current) {
        return current->handle;
      } else {
        return Undefined();
      }
    }
};

Persistent<FunctionTemplate> Fiber::tmpl;
Locker Fiber::locker;
Fiber* Fiber::current = NULL;

/**
 * If the library wasn't preloaded then we should gracefully fail instead of segfaulting if they
 * attempt to use a Fiber.
 */
Handle<Value> FiberNotSupported(const Arguments&) {
  THROW(Exception::Error,
"Fiber support was not enabled when you ran node. To enable support for fibers, please run \
node with the included `fiber-shim` script. For example, instead of running:\n\
\n\
  node script.js\n\
\n\
You should run:\n\
\n\
  ./fiber-shim node script.js\n\
\n\
You will not be able to use Fiber without this support enabled.");
}

extern "C" void init(Handle<Object> target) {
  HandleScope scope;
  Handle<Object> global = Context::GetCurrent()->Global();
  if (Coroutine::is_local_storage_enabled()) {
    Fiber::Init(global);
  } else {
    global->Set(
      String::NewSymbol("Fiber"), FunctionTemplate::New(FiberNotSupported)->GetFunction());
  }
}
