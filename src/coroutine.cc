#include "coroutine.h"
#include <assert.h>
#define __GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <ucontext.h>

#include <stdexcept>
#include <stack>
#include <vector>

#define MAX_POOL_SIZE 120

#include <iostream>
using namespace std;

/**
 * These are all the pthread functions we hook. Some are more complicated to hook than others.
 */
typedef void(*pthread_dtor_t)(void*);

static int (*o_pthread_create)(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*);
static int (*o_pthread_key_create)(pthread_key_t*, pthread_dtor_t);
static int (*o_pthread_key_delete)(pthread_key_t);
static int (*o_pthread_equal)(pthread_t, pthread_t);
static void* (*o_pthread_getspecific)(pthread_key_t);
static int (*o_pthread_join)(pthread_key_t, void**);
static pthread_t (*o_pthread_self)(void);
static int (*o_pthread_setspecific)(pthread_key_t, const void*);

/**
 * Very early on when this library is loaded there are callers to pthread_key_create,
 * pthread_getspecific, and pthread_setspecific. We normally could pass these calls through down to
 * the original implementation but that doesn't work because dlsym() tries to get a lock which ends
 * up calling pthread_getspecific and pthread_setspecific. So have to implement our own versions of
 * these functions assuming one thread only and then as soon as we can, put all that saved data into
 * a better structure.
 *
 * If there are keys reserved that are lower than `thread_key` we always pass those through to the
 * underlying implementation.
 *
 * This code assumes the underlying pthread library uses increasing TLS keys, while remaining
 * under a constant number of them. These are both not safe assumptions since pthread_t is
 * technically opaque.
 */
static const size_t MAX_EARLY_KEYS = 500;
static void* pthread_early_vals[500] = { NULL };
static pthread_dtor_t pthread_early_dtors[500] = { NULL };
static pthread_key_t prev_synthetic_key = 0;
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
		static vector<pthread_dtor_t> dtors;
		vector<Coroutine*> fiber_pool;

	public:
		pthread_t handle;
		Coroutine* current_fiber;
		Coroutine* delete_me;

		static void free(void* that) {
			delete static_cast<Thread*>(that);
		}

		Thread() : handle(0), delete_me(NULL) {
			current_fiber = new Coroutine(*this);
		}

		~Thread() {
			for (size_t ii = 0; ii < fiber_pool.size(); ++ii) {
				delete fiber_pool[ii];
			}
		}

		void coroutine_fls_dtor(Coroutine& fiber) {
			bool did_delete;
			do {
				did_delete = false;
				for (size_t ii = 0; ii < fiber.fls_data.size(); ++ii) {
					if (fiber.fls_data[ii]) {
						if (dtors[ii]) {
							void* tmp = fiber.fls_data[ii];
							fiber.fls_data[ii] = NULL;
							dtors[ii](tmp);
							did_delete = true;
						} else {
							fiber.fls_data[ii] = NULL;
						}
					}
				}
			} while (did_delete);
		}

		void fiber_did_finish(Coroutine& fiber) {
			if (fiber_pool.size() < MAX_POOL_SIZE) {
				fiber_pool.push_back(&fiber);
			} else {
				coroutine_fls_dtor(fiber);
				// Can't delete right now because we're currently on this stack!
				assert(delete_me == NULL);
				delete_me = &fiber;
			}
		}

		Coroutine& create_fiber(Coroutine::entry_t& entry, void* arg) {
			if (!fiber_pool.empty()) {
				Coroutine& fiber = *fiber_pool.back();
				fiber_pool.pop_back();
				fiber.reset(entry, arg);
				return fiber;
			}

			return *new Coroutine(*this, entry, arg);
		}

		static Thread* current() {
			return static_cast<Thread*>(o_pthread_getspecific(thread_key));
		}

		static pthread_key_t key_create(pthread_dtor_t dtor) {
			dtors.push_back(dtor);
			return dtors.size() - 1; // This is NOT thread-safe! =O!
		}

		void key_delete(pthread_key_t key) {
			if (!dtors[key]) {
				return;
			}
			// This doesn't call the dtor on all threads / fibers. Do I really care?
			while (current_fiber->get_specific(key)) {
				dtors[key](current_fiber->get_specific(key));
				current_fiber->set_specific(key, NULL);
			}
		}
};
vector<pthread_dtor_t> Thread::dtors;

/**
 * Coroutine class definition
 */
size_t Coroutine::stack_size = 0;
void Coroutine::trampoline(Coroutine &that) {
	while (true) {
		that.entry(const_cast<void*>(that.arg));
	}
}

Coroutine& Coroutine::current() {
	return *Thread::current()->current_fiber;
}

const bool Coroutine::is_local_storage_enabled() {
	return did_hook_pthreads;
}

void Coroutine::set_stack_size(size_t size) {
	assert(!Coroutine::stack_size);
	Coroutine::stack_size = size;
}

Coroutine::Coroutine(Thread& t) : thread(t) {}

Coroutine::Coroutine(Thread& t, entry_t& entry, void* arg) :
	thread(t),
	stack(stack_size),
	entry(entry),
	arg(arg) {
	getcontext(&context);
	context.uc_stack.ss_size = stack_size;
	context.uc_stack.ss_sp = &stack[0];
	makecontext(&context, (void(*)(void))trampoline, 1, this);
}

Coroutine& Coroutine::create_fiber(entry_t* entry, void* arg) {
	Thread& thread = *static_cast<Thread*>(o_pthread_getspecific(thread_key));
	return thread.create_fiber(*entry, arg);
}

void Coroutine::reset(entry_t* entry, void* arg) {
	this->entry = entry;
	this->arg = arg;
}

void Coroutine::run() {
	// memoize `thread`, as `this` might be deleted before this function returns (perhaps in a
	// different stack.
	Thread& thread = this->thread;
	Coroutine& current = *thread.current_fiber;

	assert(!thread.delete_me);
	assert(&current != this);

	thread.current_fiber = this;
	swapcontext(&current.context, &context);

	if (thread.delete_me) {
		// This means finish() was called on the coroutine and the pool was full so this coroutine needs
		// to be deleted. We can't delete from inside finish(), because that would deallocate the
		// current stack. However we CAN delete here, we just have to be very careful.
		assert(thread.delete_me == this);
		assert(&current != this);
		thread.delete_me = NULL;
		delete this;
	}
}

void Coroutine::finish(Coroutine& next) {
	assert(&next != this);
	assert(thread.current_fiber == this);
	thread.fiber_did_finish(*this);
	thread.current_fiber = &next;
	swapcontext(&context, &next.context);
}

void* Coroutine::bottom() const {
	return (char*)&stack[0];
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + stack_size;
}

void* Coroutine::get_specific(pthread_key_t key) {
	if (fls_data.size() <= key) {
		return NULL;
	}
	return fls_data[key];
}

void Coroutine::set_specific(pthread_key_t key, const void* data) {
	if (fls_data.size() <= key) {
		fls_data.resize(key + 1);
	}
	fls_data[key] = (void*)data;
}

class Loader {
	public:
	static bool hooks_ready;
	static bool initialized;
	/**
	 * As soon as this library resolves the symbols for pthread_* it will start getting calls.
	 * Unfortunately this happens before malloc is loaded, and it also happens before this library's
	 * static initialization has occurred. This function bootstraps the library enough with primordial
	 * data structures until we can finish in Loader().
	 */
	static void bootstrap() {
		if (hooks_ready) {
			return;
		}
		o_pthread_create = (int(*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*))dlsym(RTLD_NEXT, "pthread_create");
		o_pthread_key_create = (int(*)(pthread_key_t*, pthread_dtor_t))dlsym(RTLD_NEXT, "pthread_key_create");
		o_pthread_key_delete = (int(*)(pthread_key_t))dlsym(RTLD_NEXT, "pthread_key_delete");
		o_pthread_equal = (int(*)(pthread_t, pthread_t))dlsym(RTLD_NEXT, "pthread_equal");
		o_pthread_getspecific = (void*(*)(pthread_key_t))dlsym(RTLD_NEXT, "pthread_getspecific");
		o_pthread_join = (int(*)(pthread_key_t, void**))dlsym(RTLD_NEXT, "pthread_join");
		o_pthread_self = (pthread_t(*)(void))dlsym(RTLD_NEXT, "pthread_self");
		o_pthread_setspecific = (int(*)(pthread_key_t, const void*))dlsym(RTLD_NEXT, "pthread_setspecific");

		o_pthread_key_create(&thread_key, Thread::free);
		prev_synthetic_key = thread_key;
		for (size_t ii = 0; ii < thread_key; ++ii) {
			if (pthread_early_vals[ii]) {
				o_pthread_setspecific(ii, pthread_early_vals[ii]);
			}
		}
		hooks_ready = true;
	}

	/**
	 * Final initialization of this library. By the time we make it here malloc is ready. Also it's
	 * likely the TLS functions have been called, so we need to put the data from the primordial TLS
	 * storage into real FLS.
	 */
	Loader() {
		Loader::bootstrap();

		// Create a real TLS key to store the handle to Thread.
		Thread* thread = new Thread;
		thread->handle = o_pthread_self();
		o_pthread_setspecific(thread_key, thread);

		// Put all the data from the fake pthread_setspecific into FLS
		initialized = true;
		Coroutine& current = Coroutine::current();
		for (size_t ii = thread_key + 1; ii <= prev_synthetic_key; ++ii) {
			pthread_key_t tmp = thread->key_create(pthread_early_dtors[ii]);
			assert(tmp == ii - thread_key - 1);
			if (pthread_early_vals[ii]) {
				current.set_specific(tmp, pthread_early_vals[ii]);
			}
		}

		// Undo fiber-shim so that child processes don't get shimmed as well. This also seems to prevent
		// this library from being loaded multiple times.
		setenv("DYLD_INSERT_LIBRARIES", "", 1);
		setenv("LD_PRELOAD", "", 1);
	}
};
Loader loader;
bool Loader::hooks_ready = false;
bool Loader::initialized = false;

/**
 * TLS hooks
 */
// See comment above MAX_EARLY_KEYS as to why these functions are difficult to hook.
// Note well that in the `!initialized` case there is no heap. Calls to malloc, etc will crash your
// shit.
void* pthread_getspecific(pthread_key_t key) {
	if (!Loader::hooks_ready) {
		return pthread_early_vals[key];
	} else if (thread_key >= key) {
		return o_pthread_getspecific(key);
	} else if (!Loader::initialized) {
		return pthread_early_vals[key];
	} else {
		return Coroutine::current().get_specific(key - thread_key - 1);
	}
}

int pthread_setspecific(pthread_key_t key, const void* data) {
	if (!Loader::hooks_ready) {
		pthread_early_vals[key] = (void*)data;
		return 0;
	} else if (thread_key >= key) {
		return o_pthread_setspecific(key, data);
	} else if (!Loader::initialized) {
		pthread_early_vals[key] = (void*)data;
		return 0;
	} else {
		Coroutine::current().set_specific(key - thread_key - 1, data);
		return 0;
	}
}

int pthread_key_create(pthread_key_t* key, pthread_dtor_t dtor) {
	Loader::bootstrap();
	did_hook_pthreads = true;
	if (Loader::initialized) {
		*key = Thread::current()->key_create(dtor) + thread_key + 1;
		return 0;
	} else {
		*key = ++prev_synthetic_key;
		pthread_early_dtors[*key] = dtor;
		assert(prev_synthetic_key < MAX_EARLY_KEYS);
		return 0;
	}
}

/**
 * Other pthread-related hooks.
 */

// Entry point for pthread_create. We need this to record the Thread in real TLS.
void thread_trampoline(void** args_vector) {
	void* (*entry)(void*) = (void*(*)(void*))args_vector[0];
	void* arg = args_vector[1];
	Thread& thread = *static_cast<Thread*>(args_vector[2]);
	delete[] args_vector;
	o_pthread_setspecific(thread_key, &thread);
	entry(arg);
}

int pthread_create(pthread_t* handle, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
	assert(Loader::initialized);
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
	assert(Loader::initialized);
	if (thread_key >= key) {
		return o_pthread_key_delete(key);
	}
	Thread::current()->key_delete(key - thread_key - 1);
	return 0;
}

#ifndef __USE_EXTERN_INLINES
// If __USE_EXTERN_INLINES is enabled an inline version of this function will be defined, and you
// get duplicate definition errors. Since we only override to make sure the distribution isn't doing
// anything shady we skip it in the inline case.
int pthread_equal(pthread_t left, pthread_t right) {
	return left == right;
}
#endif

int pthread_join(pthread_t thread, void** retval) {
	assert(Loader::initialized);
	// pthread_join should return EDEADLK if you try to join with yourself..
	return pthread_join(reinterpret_cast<Thread*>(thread)->handle, retval);
}

pthread_t pthread_self() {
	Thread* thread = Thread::current();
	if (thread) {
		return (pthread_t)thread->current_fiber;
	} else {
		return o_pthread_self();
	}
}
