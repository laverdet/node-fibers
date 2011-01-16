#include "coroutine.h"
#include <assert.h>
#define __GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <ucontext.h>

#include <stdexcept>
#include <stack>
#include <vector>

#define STACK_SIZE (1024 * 1024 * 2)

#include <iostream>
using namespace std;

/**
 * These are all the pthread functions we hook. Some are more complicated to hook than others.
 */
typedef void(*pthread_dtor_t)(void*);

static int (*o_pthread_create)(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*);
static int (*o_pthread_key_delete)(pthread_key_t);
static int (*o_pthread_equal)(pthread_t, pthread_t);
static void* (*o_pthread_getspecific)(pthread_key_t);
static int (*o_pthread_join)(pthread_key_t, void**);
static pthread_t (*o_pthread_self)(void);
static int (*o_pthread_setspecific)(pthread_key_t, const void*);

typedef int(pthread_key_create_t)(pthread_key_t*, pthread_dtor_t);
static pthread_key_create_t* o_pthread_key_create = NULL;
static pthread_key_create_t& dyn_pthread_key_create();

/**
 * Very early on when this library is loaded there are callers to pthread_key_create,
 * pthread_getspecific, and pthread_setspecific. We normally could pass these calls through down to
 * the original implementation but that doesn't work because dlsym() tries to get a lock which ends
 * up calling pthread_getspecific and pthread_setspecific. So have to implement our own versions of
 * these functions assuming one thread only and then as soon as we can, put all that saved data into
 * real TLS.
 *
 * This code assumes the underlying pthread library uses increasing TLS keys, while remaining
 * under a constant number of them. These are both not safe assumptions since pthread_t is
 * technically opaque.
 */
static const size_t MAX_EARLY_KEYS = 500;
static const void* pthread_early_vals[500] = { NULL };
static pthread_key_t last_non_fiber_key = NULL;

/**
 * General bookkeeping for this library.
 */
static bool initialized = false;
static bool did_hook_pthreads = false;
static pthread_key_t thread_key;

/**
 * Boing
 */
void thread_trampoline(void** data);

/**
 * Thread is only used internally for this library. It keeps track of all the fibers this thread
 * is currently running, and handles all the fiber-local storage logic. We store a handle to a
 * Thread object in TLS, and then it emulates TLS on top of fibers.
 */
class Thread {
  private:
    size_t fiber_ids;
    stack<size_t> freed_fiber_ids;
    vector<vector<const void*> > fls_data;

    static vector<pthread_dtor_t> dtors;

  public:
    pthread_t handle;
    Coroutine* current_fiber;

    static void free(void* that) {
      delete static_cast<Thread*>(that);
    }

    Thread() : fiber_ids(1), fls_data(1), handle(NULL) {
      current_fiber = new Coroutine(*this, 0);
    }

    ~Thread() {
      assert(freed_fiber_ids.size() == fiber_ids);
    }

    void fiber_did_finish(Coroutine& fiber) {
      // delete ???
    }

    Coroutine& new_fiber(Coroutine::entry_t& entry, void* arg) {
      size_t id;
      if (!freed_fiber_ids.empty()) {
        id = freed_fiber_ids.top();
        freed_fiber_ids.pop();
        // TODO: clear existing TLS
      } else {
        fls_data.resize(fls_data.size() + 1);
        id = fiber_ids++;
      }
      return *new Coroutine(*this, id, *entry, arg);
    }

    void* get_specific(pthread_key_t key) {
      if (fls_data[current_fiber->getid()].size() <= key) {
        return NULL;
      }
      return (void*)fls_data[current_fiber->getid()][key];
    }

    void set_specific(pthread_key_t key, const void* data) {
      if (fls_data[current_fiber->getid()].size() <= key) {
        fls_data[current_fiber->getid()].resize(key + 1);
      }
      fls_data[current_fiber->getid()][key] = data;
    }

    void key_create(pthread_key_t* key, pthread_dtor_t dtor) {
      dtors.push_back(dtor);
      *key = dtors.size() - 1; // TODO: This is NOT thread-safe! =O
    }
};
vector<pthread_dtor_t> Thread::dtors;

/**
 * Just a simple library initialization hook.
 */
class Loader {
  public: Loader();
};

/**
 * Coroutine class definition
 */
void Coroutine::trampoline(Coroutine& that, entry_t& entry, void* arg) {
  entry(arg);
  that.thread.fiber_did_finish(that);
}

Coroutine& Coroutine::current() {
  Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
  return *thread.current_fiber;
}

const bool Coroutine::is_local_storage_enabled() {
  return did_hook_pthreads;
}


Coroutine::Coroutine(Thread& t, size_t id) : thread(t), id(id) {}

Coroutine::Coroutine(Thread& t, size_t id, entry_t& entry, void* arg) :
  thread(t),
  id(id),
  stack(new char[STACK_SIZE]) {
  getcontext(&context);
  context.uc_stack.ss_size = STACK_SIZE;
  context.uc_stack.ss_sp = stack.get();
  makecontext(&context, (void(*)(void))trampoline, 3, this, entry, arg);
}

void Coroutine::run() {
  Coroutine& current = *thread.current_fiber;
  if (current == this) {
    throw runtime_error("fiber is already running");
  }
  context.uc_link = &current.context;
  thread.current_fiber = this;
  swapcontext(&current.context, &context);
  thread.current_fiber = &current;
}

Coroutine& Coroutine::new_fiber(entry_t* entry, void* arg) {
  return thread.new_fiber(*entry, arg);
}

void* Coroutine::bottom() const {
  return stack.get() - STACK_SIZE;
}

bool Coroutine::operator==(const Coroutine& that) const {
  return this == &that;
}

bool Coroutine::operator==(const Coroutine* that) const {
  return this == that;
}

/**
 * TLS hooks
 */

// See comment above MAX_EARLY_KEYS as to why these functions are difficult to hook.
// Note well that in the `!initialized` case there is no heap. Calls to malloc, etc will crash your
// shit.
void* pthread_getspecific(pthread_key_t key) {
  if (initialized) {
    if (last_non_fiber_key >= key) {
      // If this key was reserved before the library loaded then go to the original TLS. This should
      // generally be very low-level stuff.
      return o_pthread_getspecific(key);
    }
    Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
    return thread.get_specific(key - last_non_fiber_key - 1);
  } else {
    // We can't invoke the original function because dlsym tries to call pthread_getspecific
    return const_cast<void*>(pthread_early_vals[key]);
  }
}

int pthread_setspecific(pthread_key_t key, const void* data) {
  if (initialized) {
    if (last_non_fiber_key >= key) {
      return o_pthread_setspecific(key, data);
    }
    Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
    thread.set_specific(key - last_non_fiber_key - 1, data);
    return 0;
  } else {
    pthread_early_vals[key] = data;
    return 0;
  }
}

static pthread_key_create_t& dyn_pthread_key_create() {
  if (o_pthread_key_create == NULL) {
    o_pthread_key_create = (pthread_key_create_t*)dlsym(RTLD_NEXT, "pthread_key_create");
  }
  return *o_pthread_key_create;
}

int pthread_key_create(pthread_key_t* key, pthread_dtor_t dtor) {
  if (initialized) {
    Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
    thread.key_create(key, dtor);
    *key += last_non_fiber_key + 1;
    return 0;
  } else {
    int ret = dyn_pthread_key_create()(key, dtor);
    assert(*key < MAX_EARLY_KEYS);
    if (*key > last_non_fiber_key) {
      last_non_fiber_key = *key;
    }
    return ret;
  }
}

/**
 * Other pthread-related hooks.
 */

// Entry point for pthread_create. We need this to record the Thread in real TLS.
void thread_trampoline(void** args_vector) {
  void* (*entry)(void*) = (void*(*)(void*))args_vector[0];
  void* arg = args_vector[1];
  Thread& thread = *static_cast<Thread*>(args_vector[1]);
  delete[] args_vector;
  thread.handle = o_pthread_self();
  o_pthread_setspecific(thread_key, &thread);
  entry(arg);
}

int pthread_create(pthread_t* handle, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
  assert(initialized);
  void** args_vector = new void*[3];
  args_vector[0] = (void*)entry;
  args_vector[1] = arg;
  Thread* thread = new Thread;
  args_vector[2] = thread;
  *handle = (pthread_t)thread;
  return o_pthread_create(
    &thread->handle, attr, (void* (*)(void*))thread_trampoline, (void*)args_vector);
}

int pthread_key_delete(pthread_key_t key) {
  assert(initialized);
  if (key <= last_non_fiber_key) {
    return o_pthread_key_delete(key);
  } else {
    // TODO: Call all dtors
    return 0;
  }
}

int pthread_equal(pthread_t left, pthread_t right) {
  return left == right;
}

int pthread_join(pthread_t thread, void** retval) {
  assert(initialized);
  // pthread_join should return EDEADLK if you try to join with yourself..
  return pthread_join(reinterpret_cast<Thread&>(thread).handle, retval);
}

pthread_t pthread_self() {
  assert(initialized);
  did_hook_pthreads = true;
  Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
  return (pthread_t)thread.current_fiber;
}

/**
 * Initialization of this library. By the time we make it here the heap should be good to go. Also
 * it's possible the TLS functions have been called, so we need to clean up that mess.
 */
Loader::Loader() {
  // Grab hooks to the real version of all hooked functions.
  o_pthread_create = (int(*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*))dlsym(RTLD_NEXT, "pthread_create");
  o_pthread_key_delete = (int(*)(pthread_key_t))dlsym(RTLD_NEXT, "pthread_key_delete");
  o_pthread_equal = (int(*)(pthread_t, pthread_t))dlsym(RTLD_NEXT, "pthread_equal");
  o_pthread_getspecific = (void*(*)(pthread_key_t))dlsym(RTLD_NEXT, "pthread_getspecific");
  o_pthread_join = (int(*)(pthread_key_t, void**))dlsym(RTLD_NEXT, "pthread_join");
  o_pthread_self = (pthread_t(*)(void))dlsym(RTLD_NEXT, "pthread_self");
  o_pthread_setspecific = (int(*)(pthread_key_t, const void*))dlsym(RTLD_NEXT, "pthread_setspecific");
  dyn_pthread_key_create();

  // Create a real TLS key to store the handle to Thread.
  o_pthread_key_create(&thread_key, Thread::free);
  if (thread_key > last_non_fiber_key) {
    last_non_fiber_key = thread_key;
  }
  Thread* thread = new Thread;
  thread->handle = o_pthread_self();
  o_pthread_setspecific(thread_key, thread);

  // Put all the data from the fake pthread_setspecific into real TLS
  initialized = true;
  for (size_t ii = 0; ii < last_non_fiber_key; ++ii) {
    pthread_setspecific((pthread_key_t)ii, pthread_early_vals[ii]);
  }
};
Loader loader;
