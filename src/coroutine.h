#include <stdlib.h>
#include <ext/pool_allocator.h>
#include <vector>
#include "libcoro/coro.h"

class Coroutine {
	public:
		typedef void(entry_t)(void*);

	private:
		// vector<char> will 0 out the memory first which is not necessary; this hack lets us get
		// around that, as there is no constructor.
		struct char_noinit { char x; };
		coro_context context;
		std::vector<char_noinit, __gnu_cxx::__pool_alloc<char_noinit> > stack;
		std::vector<void*> fls_data;
		entry_t* entry;
		void* arg;

		~Coroutine();

		/**
		 * Constructor for currently running "fiber". This is really just original thread, but we
		 * need a way to get back into the main thread after yielding to a fiber. Basically this
		 * shouldn't be called from anywhere.
		 */
		Coroutine();

		/**
		 * This constructor will actually create a new fiber context. Execution does not begin
		 * until you call run() for the first time.
		 */
		Coroutine(entry_t& entry, void* arg);

		/**
		 * Resets the context of this coroutine from the start. Used to recyle old coroutines.
		 */
		void reset(entry_t* entry, void* arg);

		static void trampoline(void* that);
		void transfer(Coroutine& next);

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
		 * Initialize the library.
		 */
		static void init();

		/**
		 * Set the size of coroutines created by this library. Since coroutines are pooled the stack
		 * size is global instead of per-coroutine.
		 */
		static void set_stack_size(size_t size);

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
