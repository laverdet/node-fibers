#include "coroutine.h"
#include <assert.h>
#ifndef WINDOWS
#include <pthread.h>
#else
#include <windows.h>
// Pretend Windows TLS is pthreads. Note that pthread_key_create() skips the dtor, but this doesn't
// matter for our application.
#define pthread_key_t DWORD
#define pthread_key_create(x,y) (*x)=TlsAlloc()
#define pthread_setspecific(x,y) TlsSetValue((x), (y))
#define pthread_getspecific(x) TlsGetValue((x))
#endif

#include <stdexcept>
#include <stack>
#include <vector>

#include <iostream>
using namespace std;

static pthread_key_t floor_thread_key = 0;
static pthread_key_t ceil_thread_key = 0;

static size_t stack_size = 0;
static size_t coroutines_created_ = 0;
static vector<Coroutine*> fiber_pool;
static Coroutine* delete_me = NULL;
size_t Coroutine::pool_size = 120;

/**
 * Coroutine class definition
 */
void Coroutine::init() {
	pthread_key_create(&ceil_thread_key, NULL);
	pthread_setspecific(ceil_thread_key, &current());
	// Assume that v8 registered their TLS keys within the past 5 keys.. if not there's trouble.
	if (ceil_thread_key > 5) {
		floor_thread_key = ceil_thread_key - 5;
	} else {
		floor_thread_key = 0;
	}
}

Coroutine& Coroutine::current() {
	Coroutine* current = static_cast<Coroutine*>(pthread_getspecific(ceil_thread_key));
	if (!current) {
		current = new Coroutine;
		pthread_setspecific(ceil_thread_key, current);
	}
	return *current;
}

void Coroutine::set_stack_size(unsigned int size) {
	assert(!stack_size);
	stack_size = size;
}

size_t Coroutine::coroutines_created() {
	return coroutines_created_;
}

void Coroutine::trampoline(void* that) {
#ifdef CORO_PTHREAD
	pthread_setspecific(ceil_thread_key, that);
#endif
#ifdef CORO_FIBER
	// I can't figure out how to get the precise base of the stack in Windows. Since CreateFiber
	// creates the stack automatically we don't have access to the base. We can however grab the
	// current esp position, and use that as an approximation. Padding is added for safety since the
	// base is slightly different.
	static_cast<Coroutine*>(that)->stack_base = (size_t*)_AddressOfReturnAddress() - stack_size + 16;
#endif
	while (true) {
		static_cast<Coroutine*>(that)->entry(const_cast<void*>(static_cast<Coroutine*>(that)->arg));
	}
}

Coroutine::Coroutine() :
	entry(NULL),
	arg(NULL) {
	stack.sptr = NULL;
	coro_create(&context, NULL, NULL, NULL, 0);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	entry(entry),
	arg(arg) {
	coro_stack_alloc(&stack, stack_size);
	coro_create(&context, trampoline, this, stack.sptr, stack.ssze);
}

Coroutine::~Coroutine() {
	if (stack.sptr) {
		coro_stack_free(&stack);
	}
	(void)coro_destroy(&context);
}

Coroutine& Coroutine::create_fiber(entry_t* entry, void* arg) {
	if (!fiber_pool.empty()) {
		Coroutine& fiber = *fiber_pool.back();
		fiber_pool.pop_back();
		fiber.reset(entry, arg);
		return fiber;
	}
	++coroutines_created_;
	return *new Coroutine(*entry, arg);
}

void Coroutine::reset(entry_t* entry, void* arg) {
	assert(entry != NULL);
	this->entry = entry;
	this->arg = arg;
}

void Coroutine::transfer(Coroutine& next) {
	assert(this != &next);
#ifndef CORO_PTHREAD
	{
		for (pthread_key_t ii = ceil_thread_key - 1; ii > floor_thread_key; --ii) {
			// Capture current thread specifics
			void* data = pthread_getspecific(ii);
			if (fls_data.size() >= ii - floor_thread_key) {
				fls_data[ii - floor_thread_key - 1] = data;
			} else if (data) {
				fls_data.resize(ii - floor_thread_key);
				fls_data[ii - floor_thread_key - 1] = data;
			}

			// Replace current thread specifics
			if (next.fls_data.size() >= ii - floor_thread_key) {
				if (data != next.fls_data[ii - floor_thread_key - 1]) {
					pthread_setspecific(ii, next.fls_data[ii - floor_thread_key - 1]);
				}
			} else if (data) {
				pthread_setspecific(ii, NULL);
			}
		}
	}
	pthread_setspecific(ceil_thread_key, &next);
#endif
	coro_transfer(&context, &next.context);
#ifndef CORO_PTHREAD
	pthread_setspecific(ceil_thread_key, this);
#endif
}

void Coroutine::run() {
	Coroutine& current = Coroutine::current();
	assert(!delete_me);
	assert(&current != this);
	current.transfer(*this);

	if (delete_me) {
		// This means finish() was called on the coroutine and the pool was full so this coroutine needs
		// to be deleted. We can't delete from inside finish(), because that would deallocate the
		// current stack. However we CAN delete here, we just have to be very careful.
		assert(delete_me == this);
		assert(&current != this);
		delete_me = NULL;
		delete this;
	}
}

void Coroutine::finish(Coroutine& next) {
	{
		assert(&next != this);
		assert(&current() == this);
		if (fiber_pool.size() < pool_size) {
			fiber_pool.push_back(this);
		} else {
			// TODO?: This assumes that we didn't capture any keys with dtors. This may not always be
			// true, and if it is not there will be memory leaks.

			// Can't delete right now because we're currently on this stack!
			assert(delete_me == NULL);
			delete_me = this;
		}
	}
	this->transfer(next);
}

void* Coroutine::bottom() const {
#ifdef CORO_FIBER
	return stack_base;
#else
	return stack.sptr;
#endif
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + stack_size * sizeof(void*);
}
