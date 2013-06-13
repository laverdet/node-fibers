#include "coroutine.h"
#include <assert.h>
#ifndef WINDOWS
#include <pthread.h>
#else
#include <windows.h>
// Stub pthreads into Windows approximations
#define pthread_t HANDLE
#define pthread_create(thread, attr, fn, arg) !((*thread)=CreateThread(NULL, 0, &(fn), arg, 0, NULL))
#define pthread_join(thread, arg) WaitForSingleObject((thread), INFINITE)
#define pthread_key_t DWORD
#define pthread_key_create(key, dtor) (*key)=TlsAlloc()
#define pthread_setspecific(key, val) TlsSetValue((key), (val))
#define pthread_getspecific(key) TlsGetValue((key))
#endif

#include <stdexcept>
#include <stack>
#include <vector>
using namespace std;

const size_t v8_tls_keys = 3;
static pthread_key_t floor_thread_key = 0;
static pthread_key_t ceil_thread_key = 0;
static pthread_key_t coro_thread_key = 0;

static size_t stack_size = 0;
static size_t coroutines_created_ = 0;
static vector<Coroutine*> fiber_pool;
static Coroutine* delete_me = NULL;
size_t Coroutine::pool_size = 120;

#ifndef WINDOWS
static void* find_thread_id_key(void* arg)
#else
static DWORD __stdcall find_thread_id_key(LPVOID arg)
#endif
{
	v8::Isolate* isolate = static_cast<v8::Isolate*>(arg);
	v8::Locker locker(isolate);
	assert(isolate != NULL);
	floor_thread_key = 0x7777;
	for (pthread_key_t ii = coro_thread_key - 1; ii >= (coro_thread_key >= 20 ? coro_thread_key - 20 : 0); --ii) {
		if (pthread_getspecific(ii) == isolate) {
			floor_thread_key = ii;
			break;
		}
	}
	assert(floor_thread_key != 0x7777);
	ceil_thread_key = floor_thread_key + v8_tls_keys - 1;
	return NULL;
}

/**
 * Coroutine class definition
 */
void Coroutine::init(v8::Isolate* isolate) {
	v8::Unlocker unlocker(isolate);
	pthread_key_create(&coro_thread_key, NULL);
	pthread_setspecific(coro_thread_key, &current());
	pthread_t thread;
	pthread_create(&thread, NULL, find_thread_id_key, isolate);
	pthread_join(thread, NULL);
}

Coroutine& Coroutine::current() {
	Coroutine* current = static_cast<Coroutine*>(pthread_getspecific(coro_thread_key));
	if (!current) {
		current = new Coroutine;
		pthread_setspecific(coro_thread_key, current);
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
	pthread_setspecific(coro_thread_key, that);
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
	fls_data(v8_tls_keys),
	entry(NULL),
	arg(NULL) {
	stack.sptr = NULL;
	coro_create(&context, NULL, NULL, NULL, 0);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	fls_data(v8_tls_keys),
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
		for (pthread_key_t ii = floor_thread_key; ii <= ceil_thread_key; ++ii) {
			// Swap TLS keys with fiber locals
			fls_data[ii - floor_thread_key] = pthread_getspecific(ii);
			pthread_setspecific(ii, next.fls_data[ii - floor_thread_key]);
		}
	}
	pthread_setspecific(coro_thread_key, &next);
#endif
	coro_transfer(&context, &next.context);
#ifndef CORO_PTHREAD
	pthread_setspecific(coro_thread_key, this);
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
