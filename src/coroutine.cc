#include "coroutine.h"
#include <assert.h>
#include <pthread.h>

#include <stdexcept>
#include <stack>
#include <vector>

#define MAX_POOL_SIZE 120

#include <iostream>
using namespace std;

static pthread_key_t floor_thread_key = NULL;
static pthread_key_t ceil_thread_key = NULL;

static size_t stack_size = 0;
static vector<Coroutine*> fiber_pool;
static Coroutine* delete_me = NULL;

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

void Coroutine::set_stack_size(size_t size) {
	assert(!stack_size);
#ifdef CORO_PTHREAD
	size += 1024 * 64;
#endif
	stack_size = size;
}

void Coroutine::trampoline(void* that) {
#ifdef CORO_PTHREAD
	pthread_setspecific(ceil_thread_key, that);
#endif
	while (true) {
		static_cast<Coroutine*>(that)->entry(const_cast<void*>(static_cast<Coroutine*>(that)->arg));
	}
}

Coroutine::Coroutine() : entry(NULL) {
	coro_create(&context, NULL, NULL, NULL, NULL);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	stack(stack_size),
	entry(entry),
	arg(arg) {
	coro_create(&context, trampoline, this, &stack[0], stack_size);
}

Coroutine::~Coroutine() {
	coro_destroy(&context);
}

Coroutine& Coroutine::create_fiber(entry_t* entry, void* arg) {
	if (!fiber_pool.empty()) {
		Coroutine& fiber = *fiber_pool.back();
		fiber_pool.pop_back();
		fiber.reset(entry, arg);
		return fiber;
	}
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
		if (fiber_pool.size() < MAX_POOL_SIZE) {
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
	return (char*)&stack[0];
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + stack_size;
}
