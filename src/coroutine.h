#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif

#include <stdlib.h>
#include <ucontext.h>
#include <ext/pool_allocator.h>
#include <vector>

class Coroutine {
	public:
		friend class Thread;
		typedef void(entry_t)(void*);

	private:
		// vector<char> will 0 out the memory first which is not necessary; this hack lets us get
		// around that, as there is no constructor.
		struct char_noinit { char x; };
		class Thread& thread;
		ucontext_t context;
		std::vector<char_noinit, __gnu_cxx::__pool_alloc<char_noinit> > stack;
		std::vector<void*> fls_data;
		entry_t* entry;
		void* arg;
		static size_t stack_size;

		static void trampoline(Coroutine& that);
		~Coroutine() {}

		/**
		 * Constructor for currently running "fiber". This is really just original thread, but we
		 * need a way to get back into the main thread after yielding to a fiber. Basically this
		 * shouldn't be called from anywhere.
		 */
		Coroutine(Thread& t);

		/**
		 * This constructor will actually create a new fiber context. Execution does not begin
		 * until you call run() for the first time.
		 */
		Coroutine(Thread& t, entry_t& entry, void* arg);

		/**
		 * Resets the context of this coroutine from the start. Used to recyle old coroutines.
		 */
		void reset(entry_t* entry, void* arg);

	public:
		/**
		 * Returns the currently-running fiber.
		 */
		static Coroutine& current();

		/**
		 * Create a new fiber.
		 */
		static Coroutine& create_fiber(entry_t* entry, void* arg = NULL);

		/**
		 * Is Coroutine-local storage via pthreads enabled? The Coroutine library should work fine
		 * without this, but programs that are not aware of coroutines may panic if they make
		 * assumptions about the stack. In order to enable this you must LD_PRELOAD (or equivalent)
		 * this library.
		 */
		static const bool is_local_storage_enabled();

		/**
		 * Set the size of coroutines created by this library. Since coroutines are pooled the stack
		 * size is global instead of per-coroutine.
		 */
		static void set_stack_size(size_t size);

		/**
		 * Access to fiber-local storage.
		 */
		void* get_specific(pthread_key_t key);
		void set_specific(pthread_key_t key, const void* data);

		/**
		 * Start or resume execution in this fiber. Note there is no explicit yield() function,
		 * you must manually run another fiber.
		 */
		void run();

		/**
		 * Finish this coroutine.. This will halt execution of this coroutine and resume execution
		 * of `next`. If you do not call this function, and instead just return from `entry` the
		 * application will exit. This function may or may not actually return.
		 */
		void finish(Coroutine& next);

		/**
		 * Returns address of the lowest usable byte in this Coroutine's stack.
		 */
		void* bottom() const;

		/**
		 * Returns the size this Coroutine takes up in the heap.
		 */
		size_t size() const;
};
