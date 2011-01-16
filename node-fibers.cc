#include "coroutine.h"
#include <node/node.h>

#include <iostream>

#define THROW(x, m) return ThrowException(x(String::New(m)))

using namespace std;
using namespace v8;
using namespace node;

class Fiber: ObjectWrap {
  private:
    static Locker locker; // Node does not use locks or threads, so we need a global lock
    static Persistent<FunctionTemplate> tmpl;
    static Fiber* current;

    Persistent<Function> cb;
    Persistent<Context> v8_context;
    Persistent<Value> yielded;
    Coroutine* entry_fiber;
    Coroutine* this_fiber;
    bool started;
    bool zombie;

  public:
    Fiber(Persistent<Function> cb, Persistent<Context> v8_context) :
      ObjectWrap(),
      cb(cb),
      v8_context(v8_context),
      started(false),
      zombie(false) {}
    virtual ~Fiber() {
      if (this->started) {
        this->zombie = true;

        // Swap context back to `Fiber::Yield()` to finish execution.
        // TODO: What to do about thrown JS exceptions here?
        this->this_fiber->run();
      }
      cb.Dispose();
      v8_context.Dispose();
      yielded.Dispose();
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
      proto->Set(String::New("run"), FunctionTemplate::New(Run));
      proto->Set(String::New("yield"), FunctionTemplate::New(Yield));
      proto->SetAccessor(String::New("started"), GetStarted);

      Handle<Function> fn = tmpl->GetFunction();
      fn->SetAccessor(String::New("current"), GetCurrent);
      target->Set(String::NewSymbol("Fiber"), fn);
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
      Fiber* that = new Fiber(
        Persistent<Function>::New(fn),
        Persistent<Context>::New(Context::GetCurrent()));
      that->Wrap(args.This());
      return args.This();
    }

    /**
     * Begin or resume the current fiber. If the fiber is not currently running a new context will
     * be created and the callback will start. Otherwise we switch back into the exist context.
     */
    static Handle<Value> Run(const Arguments& args) {
      HandleScope scope;
      Fiber* that = ObjectWrap::Unwrap<Fiber>(args.This());
      that->entry_fiber = &Coroutine::current();

      if (!that->started) {
        // Create a new context with entry point `Fiber::RunFiber()`.
        that->started = true;
        void** data = new void*[2];
        data[0] = (void*)&args;
        data[1] = that;
        that->this_fiber = &that->entry_fiber->new_fiber((void (*)(void*))RunFiber, data);
      } else {
        // If the fiber is currently running put the first parameter to `run()` on `yielded`, then
        // the pending call to `yield()` will return that value. `yielded` in this case is just a
        // misnomer, we're just reusing the same handle.
        if (args.Length()) {
          that->yielded = Persistent<Value>::New(args[0]);
        } else {
          that->yielded = Persistent<Value>::New(Undefined());
        }
      }

      // This will jump into either `RunFiber()` or `Yield()`, depending on if the fiber was
      // already running.
      Fiber* last_fiber = current;
      current = that;
      {
        Unlocker unlocker;
        that->this_fiber->run();
      }
      // At this point the fiber either returned or called `yield()`.
      current = last_fiber;

      // Return the yielded value.
      Handle<Value> val = Local<Value>::New(that->yielded);
      that->yielded.Dispose();
      return val;
    }

    /**
     * This is the entry point for a new fiber, from `run()`.
     */
    static void RunFiber(void** data) {
      const Arguments* args = (const Arguments*)data[0];
      Fiber* that = (Fiber*)data[1];
      delete[] data;

      Locker locker;
      HandleScope scope;

      ResourceConstraints constraints;
      constraints.set_stack_limit((uint32_t*)that->this_fiber->bottom());
      SetResourceConstraints(&constraints);

      that->v8_context->Enter();
      if (args->Length()) {
        Local<Value> argv[1] = { Local<Value>::New((*args)[0]) };
        that->yielded = Persistent<Value>::New(that->cb->Call(args->This(), 1, argv));
      } else {
        that->yielded = Persistent<Value>::New(that->cb->Call(args->This(), 0, NULL));
      }
      that->v8_context->Exit();

      // The function returned (instead of yielding).
      that->started = false;
    }

    /**
     * Yield control back to the function that called `run()`. The first parameter to this function
     * is returned from `run()`. The context is saved, to be later resumed from `run()`.
     */
    static Handle<Value> Yield(const Arguments& args) {
      HandleScope scope;
      Fiber* that = ObjectWrap::Unwrap<Fiber>(args.This());
      if (that->zombie) {
        THROW(Exception::Error, "This Fiber is a zombie");
      }

      if (args.Length()) {
        that->yielded = Persistent<Value>::New(args[0]);
      } else {
        that->yielded = Persistent<Value>::New(Undefined());
      }

      // Return control back to `Fiber::run()`
      {
        Unlocker unlocker;
        that->entry_fiber->run();
      }
      current = that;

      // `yielded` will contain the first parameter to `run()`
      Handle<Value> val = Local<Value>::New(that->yielded);
      that->yielded.Dispose();
      return val;
    }

    /**
     * Getters for `started`, and `current`.
     */
    static Handle<Value> GetStarted(Local<String> property, const AccessorInfo& info) {
      Fiber* that = ObjectWrap::Unwrap<Fiber>(info.This());
      return Boolean::New(that->started);
    }

    static Handle<Value> GetCurrent(Local<String> property, const AccessorInfo& info) {
      if (current) {
        return current->handle_;
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
  if (Coroutine::is_local_storage_enabled()) {
    Fiber::Init(target);
  } else {
    target->Set(String::New("Fiber"), FunctionTemplate::New(FiberNotSupported)->GetFunction());
  }
}
