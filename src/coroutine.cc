#include "coroutine.h"
#include "v8-version.h"
#include <assert.h>
#ifndef WINDOWS
#include <pthread.h>
#else
#include <windows.h>
#include <intrin.h>
// Stub pthreads into Windows approximations
#define pthread_t HANDLE
#define pthread_create(thread, attr, fn, arg) !((*thread)=CreateThread(NULL, 0, &(fn), arg, 0, NULL))
#define pthread_join(thread, arg) WaitForSingleObject((thread), INFINITE)
#define pthread_key_t DWORD
#define pthread_key_create(key, dtor) (*key)=TlsAlloc()
#define pthread_setspecific(key, val) TlsSetValue((key), (val))
#define pthread_getspecific(key) TlsGetValue((key))
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdexcept>
#include <stack>
#include <vector>
using namespace std;

static pthread_key_t coro_thread_key = 0;
static pthread_key_t isolate_key = 0x7777;
static pthread_key_t thread_id_key = 0x7777;
static pthread_key_t thread_data_key = 0x7777;

#if defined(__GNUC__)
__attribute__((noreturn, cold, format(printf, 5, 6)))
#endif
static void fibers_check_failed(const char* file, int line, const char* func, const char* cond, const char* fmt, ...) {
	fprintf(stderr, "fibers: %s:%d: %s: check `%s` failed: ", file, line, func, cond);
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	fflush(stderr);
	abort();
}

// Unlike assert(), survives NDEBUG: these guard memory safety, not debugging.
#define FIBERS_CHECK(cond, ...) \
	do { \
		if (!(cond)) fibers_check_failed(__FILE__, __LINE__, __func__, #cond, __VA_ARGS__); \
	} while (0)

// From v8 9.0 on some of v8's thread identity lives only in `thread_local`s, so failing to read the
// executable's symbols is fatal. Before that the pthread key scan finds all of it.
static const bool tls_symbols_required = V8_AT_LEAST(9, 0);

// v8's per-thread identity, swapped on every fiber switch so that each fiber looks like its own
// thread to v8. Older v8 keeps these in pthread keys; newer v8 keeps them in C++ `thread_local`s of
// the node executable, which are located through the executable's symbol table.
struct TlsSlot {
	bool is_key;
	pthread_key_t key;
	size_t offset;
	size_t size;
};
static TlsSlot slots[Coroutine::max_tls_slots];
static size_t slot_count = 0;
static long stack_start_slot = -1;

#ifdef __ELF__
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
struct tls_index_t { unsigned long module; unsigned long offset; };
extern "C" void* __tls_get_addr(tls_index_t*);

static char* exe_tls_base() {
	// The main executable is always TLS module 1
	tls_index_t index = { 1, 0 };
	return static_cast<char*>(__tls_get_addr(&index));
}

static char* tls_slot_address(char* base, size_t offset) { return base + offset; }

// Finds `thread_local` variables of the node executable by mangled name, leaving `found` false for
// the ones which don't exist in this build (or all of them, if the executable is stripped).
//
// The names are internal to v8, so the dynamic linker can't resolve them: they are only in the
// full symbol table (.symtab), which is not loaded into memory. Hence reading the file on disk.
static void find_exe_tls_symbols(const char* const* names, size_t count, bool* found, size_t* offsets, size_t* sizes) {
	memset(found, 0, count * sizeof(bool));
	int fd = open("/proc/self/exe", O_RDONLY);
	if (fd < 0) {
		FIBERS_CHECK(!tls_symbols_required, "open(/proc/self/exe): %s", strerror(errno));
		return;
	}
	struct stat st;
	if (fstat(fd, &st) != 0) {
		FIBERS_CHECK(!tls_symbols_required, "fstat(/proc/self/exe): %s", strerror(errno));
		close(fd);
		return;
	}
	void* map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		FIBERS_CHECK(!tls_symbols_required, "mmap(/proc/self/exe): %s", strerror(errno));
		return;
	}
	const char* base = static_cast<const char*>(map);
	// The ELF header gives the section header table; each section header has a type
	const ElfW(Ehdr)* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(base);
	const ElfW(Shdr)* shdr = reinterpret_cast<const ElfW(Shdr)*>(base + ehdr->e_shoff);
	for (size_t ii = 0; ii < ehdr->e_shnum; ++ii) {
		if (shdr[ii].sh_type != SHT_SYMTAB) continue;
		// A symbol table is an array of fixed-size entries; `sh_link` names the section holding
		// their names, which each entry points into by offset
		const ElfW(Sym)* sym = reinterpret_cast<const ElfW(Sym)*>(base + shdr[ii].sh_offset);
		const char* strtab = base + shdr[shdr[ii].sh_link].sh_offset;
		size_t sym_count = shdr[ii].sh_size / sizeof(ElfW(Sym));
		for (size_t jj = 0; jj < sym_count; ++jj) {
			// For a `thread_local` (STT_TLS), `st_value` is not an address but the variable's offset
			// in the executable's TLS block, the same for every thread
			if (ELF64_ST_TYPE(sym[jj].st_info) != STT_TLS) continue;
			for (size_t kk = 0; kk < count; ++kk) {
				if (strcmp(strtab + sym[jj].st_name, names[kk]) == 0) {
					found[kk] = true;
					offsets[kk] = sym[jj].st_value;
					sizes[kk] = sym[jj].st_size;
				}
			}
		}
	}
	munmap(map, st.st_size);
}
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
// A Mach-O `thread_local` symbol names a descriptor whose thunk returns the variable's address
// for the calling thread.
struct TlvDescriptor {
	void* (*thunk)(TlvDescriptor*);
	unsigned long key;
	unsigned long offset;
};

static char* exe_tls_base() { return NULL; }
static char* tls_slot_address(char*, size_t descriptor) {
	TlvDescriptor* tlv = reinterpret_cast<TlvDescriptor*>(descriptor);
	return static_cast<char*>(tlv->thunk(tlv));
}

// As on ELF, v8's internal names are only in the full symbol table. Unlike ELF, dyld maps that
// table into memory with the rest of the image (the __LINKEDIT segment), so nothing is read from
// disk.
static void find_exe_tls_symbols(const char* const* names, size_t count, bool* found, size_t* offsets, size_t* sizes) {
	memset(found, 0, count * sizeof(bool));
	// Image 0 is the main executable
	const struct mach_header_64* header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(0));
	FIBERS_CHECK(header && header->magic == MH_MAGIC_64, "the node executable is not a 64-bit Mach-O image");
	// How far the image was moved from the addresses recorded in the file (ASLR)
	intptr_t slide = _dyld_get_image_vmaddr_slide(0);
	// The load commands follow the header. Two are needed: the __LINKEDIT segment, which says
	// where the link-edit data is mapped, and LC_SYMTAB, which says where in that data the symbol
	// and string tables are.
	const struct segment_command_64* linkedit = NULL;
	const struct symtab_command* symtab = NULL;
	const struct load_command* cmd = reinterpret_cast<const struct load_command*>(header + 1);
	for (uint32_t ii = 0; ii < header->ncmds; ++ii) {
		if (cmd->cmd == LC_SEGMENT_64) {
			const struct segment_command_64* segment = reinterpret_cast<const struct segment_command_64*>(cmd);
			if (strcmp(segment->segname, SEG_LINKEDIT) == 0) linkedit = segment;
		} else if (cmd->cmd == LC_SYMTAB) {
			symtab = reinterpret_cast<const struct symtab_command*>(cmd);
		}
		cmd = reinterpret_cast<const struct load_command*>(reinterpret_cast<const char*>(cmd) + cmd->cmdsize);
	}
	if (!linkedit || !symtab) {
		FIBERS_CHECK(!tls_symbols_required, "the node executable has no symbol table");
		return;
	}
	// LC_SYMTAB's offsets are file offsets; this is the address file offset 0 would have if the
	// whole file were mapped the way __LINKEDIT is
	const char* base = reinterpret_cast<const char*>(slide + linkedit->vmaddr - linkedit->fileoff);
	const struct nlist_64* sym = reinterpret_cast<const struct nlist_64*>(base + symtab->symoff);
	const char* strtab = base + symtab->stroff;
	for (uint32_t jj = 0; jj < symtab->nsyms; ++jj) {
		// Skip debugger entries and anything not defined in a section of this image
		if ((sym[jj].n_type & N_STAB) || (sym[jj].n_type & N_TYPE) != N_SECT) continue;
		// Mach-O prefixes every C symbol with an underscore
		const char* name = strtab + sym[jj].n_un.n_strx;
		if (name[0] != '_') continue;
		for (size_t kk = 0; kk < count; ++kk) {
			if (strcmp(name + 1, names[kk]) == 0) {
				found[kk] = true;
				// The address of the variable's TlvDescriptor, not of the variable
				offsets[kk] = sym[jj].n_value + slide;
				// The symbol table has no sizes: the thread id is an int, the rest are pointers
				sizes[kk] = strstr(names[kk], "thread_idE") ? sizeof(int) : sizeof(void*);
			}
		}
	}
}
#else
#error "fibers only knows how to find v8's thread-locals in ELF (Linux) and Mach-O (macOS) executables"
#endif

static void add_key_slot(pthread_key_t key) {
	FIBERS_CHECK(slot_count < Coroutine::max_tls_slots, "%zu slots is one too many", slot_count + 1);
	TlsSlot slot = { true, key, 0, sizeof(void*) };
	slots[slot_count++] = slot;
}

static void add_tls_slot(size_t offset, size_t size) {
	FIBERS_CHECK(slot_count < Coroutine::max_tls_slots, "%zu slots is one too many", slot_count + 1);
	FIBERS_CHECK(size <= sizeof(uint64_t), "a %zu byte thread-local does not fit in a slot", size);
	TlsSlot slot = { false, 0, offset, size };
	slots[slot_count++] = slot;
}

static void save_slots(uint64_t* data) {
	char* base = NULL;
	for (size_t ii = 0; ii < slot_count; ++ii) {
		if (slots[ii].is_key) {
			data[ii] = reinterpret_cast<uint64_t>(pthread_getspecific(slots[ii].key));
		} else {
			if (!base) base = exe_tls_base();
			data[ii] = 0;
			memcpy(&data[ii], tls_slot_address(base, slots[ii].offset), slots[ii].size);
		}
	}
}

static void restore_slots(const uint64_t* data) {
	char* base = NULL;
	for (size_t ii = 0; ii < slot_count; ++ii) {
		if (slots[ii].is_key) {
			pthread_setspecific(slots[ii].key, reinterpret_cast<void*>(data[ii]));
		} else {
			if (!base) base = exe_tls_base();
			memcpy(tls_slot_address(base, slots[ii].offset), &data[ii], slots[ii].size);
		}
	}
}

static size_t stack_size = 0;
static size_t coroutines_created_ = 0;
static vector<Coroutine*> fiber_pool;
static Coroutine* delete_me = NULL;
size_t Coroutine::pool_size = 120;

static bool can_poke(void* addr) {
#ifdef WINDOWS
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQueryEx(GetCurrentProcess(), addr, &mbi, sizeof(mbi))) {
		return false;
	}
	if (!(mbi.State & MEM_COMMIT)) {
		return false;
	}
	return true;
#else
	// TODO?
	return addr > (void*)0x1000;
#endif
}

#ifdef USE_V8_SYMBOLS
// ** This no longer works as of v8 7.3.262; `thread_id_key_` moved to a static variable inside a
// function in an anonymous namespace.

// Some distributions of node, most notably Ubuntu, strip the v8 internal symbols and so we don't
// have access to this stuff. In most cases we will use the more complicated `find_thread_id_key`
// below, since it tends to work on more platforms.
namespace v8 {
	namespace base {
		class Thread {
			public: typedef int32_t LocalStorageKey;
		};
	}

	namespace internal {
		class Isolate {
			public:
				static base::Thread::LocalStorageKey isolate_key_;
				static base::Thread::LocalStorageKey per_isolate_thread_data_key_;
				static base::Thread::LocalStorageKey thread_id_key_;
		};
	}
}
#endif

static bool scan_isolate_keys = true;
static bool scan_thread_id_key = true;

#ifndef WINDOWS
static void* find_thread_id_key(void* arg)
#else
static DWORD __stdcall find_thread_id_key(LPVOID arg)
#endif
{
	v8::Isolate* isolate = static_cast<v8::Isolate*>(arg);
	assert(isolate != NULL);
	v8::Locker locker(isolate);
	isolate->Enter();
	int thread_id = 0;
	if (!scan_isolate_keys) goto done;

	// First pass-- find isolate thread key
	{
#ifdef __MUSL__
	// 128 is default max key in musl
	for (pthread_key_t ii = 1; ii < 128; ++ii) {
#else
	for (pthread_key_t ii = coro_thread_key; ii > 0; --ii) {
#endif
		void* tls = pthread_getspecific(ii - 1);
		if (tls == isolate) {
			isolate_key = ii - 1;
			break;
		}
	}
	assert(isolate_key != 0x7777);
	}

	// Second pass-- find data key
	{
#ifdef __MUSL__
	for (pthread_key_t ii = 0; ii < 128; ++ii) {
#else
	for (pthread_key_t ii = isolate_key + 1; ii < coro_thread_key; ++ii) {
#endif
		void* tls = pthread_getspecific(ii);
		if (can_poke(tls) && *(void**)tls == isolate) {
			// First member of per-thread data is the isolate
			thread_data_key = ii;
			// Second member is the thread id
			thread_id = *(int*)((void**)tls + 1);
			break;
		}
	}
	assert(thread_data_key != 0x7777);
	}

	// Third pass-- find thread id key
	if (!scan_thread_id_key) goto done;
	{
#ifdef __MUSL__
	for (pthread_key_t ii = 0; ii < 128; ++ii) {
#else
	for (pthread_key_t ii = isolate_key + 1; ii < coro_thread_key; ++ii) {
#endif
		int tls = static_cast<int>(reinterpret_cast<intptr_t>(pthread_getspecific(ii)));
		if (tls == thread_id) {
			thread_id_key = ii;
			break;
		}
	}
	assert(thread_id_key != 0x7777);
	}

done:
	isolate->Exit();
	return NULL;
}

/**
 * Coroutine class definition
 */
void Coroutine::init(v8::Isolate* isolate) {
	v8::Unlocker unlocker(isolate);
	pthread_key_create(&coro_thread_key, NULL);
	pthread_setspecific(coro_thread_key, &current());
#ifdef USE_V8_SYMBOLS
	isolate_key = v8::internal::Isolate::isolate_key_;
	thread_data_key = v8::internal::Isolate::per_isolate_thread_data_key_;
	thread_id_key = v8::internal::Isolate::thread_id_key_;
#else
	// The two in an anonymous namespace are spelled with an `L` by compilers which mark internal
	// linkage in the name, so each is looked for both ways.
	static const char* const names[] = {
		"_ZN2v88internal18g_current_isolate_E",
		"_ZN2v88internal34g_current_per_isolate_thread_data_E",
		"_ZN2v88internal12_GLOBAL__N_19thread_idE",
		"_ZN2v84base12_GLOBAL__N_118thread_stack_startE",
		"_ZN2v88internal12_GLOBAL__N_1L9thread_idE",
		"_ZN2v84base12_GLOBAL__N_1L18thread_stack_startE",
	};
	bool found[6];
	size_t offsets[6], sizes[6];
	find_exe_tls_symbols(names, 6, found, offsets, sizes);
	for (size_t ii = 2; ii < 4; ++ii) {
		if (!found[ii] && found[ii + 2]) {
			found[ii] = true;
			offsets[ii] = offsets[ii + 2];
			sizes[ii] = sizes[ii + 2];
		}
	}
	// v8 9.0 moved the thread id to a `thread_local`, 11.x the isolate, per-thread data and the
	// cached stack start. Without their symbols every fiber would share one v8 thread identity.
	bool missing = V8_AT_LEAST(11, 0) ? !(found[0] && found[1] && found[2] && found[3]) :
		V8_AT_LEAST(9, 0) ? !found[2] : false;
	if (missing) {
		fprintf(stderr, "fibers: could not find v8's thread-local symbols in the node executable "
			"(isolate=%d thread_data=%d thread_id=%d stack_start=%d). Is the binary stripped?\n",
			found[0], found[1], found[2], found[3]);
		abort();
	}
	scan_isolate_keys = !found[0];
	scan_thread_id_key = !found[0] && !found[2];
	if (scan_isolate_keys) {
		pthread_t thread;
		pthread_create(&thread, NULL, find_thread_id_key, isolate);
		pthread_join(thread, NULL);
		add_key_slot(isolate_key);
		add_key_slot(thread_data_key);
		if (scan_thread_id_key) add_key_slot(thread_id_key);
	}
	for (size_t ii = 0; ii < 3; ++ii) {
		if (found[ii]) add_tls_slot(offsets[ii], sizes[ii]);
	}
	if (found[3]) {
		stack_start_slot = slot_count;
		add_tls_slot(offsets[3], sizes[3]);
	}
	if (getenv("FIBERS_DEBUG_TLS")) {
		fprintf(stderr, "fibers: tls symbols isolate=%d thread_data=%d thread_id=%d stack_start=%d; %zu slots\n",
			found[0], found[1], found[2], found[3], slot_count);
	}
#endif
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
	fls_data(),
	entry(NULL),
	arg(NULL) {
	stack.sptr = NULL;
	coro_create(&context, NULL, NULL, NULL, 0);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	fls_data(),
	entry(entry),
	arg(arg) {
}

Coroutine::~Coroutine() {
	if (stack.sptr) {
		coro_stack_free(&stack);
	}
#ifdef CORO_FIBER
	if (context.fiber)
#endif
	(void)coro_destroy(&context);
}

Coroutine* Coroutine::create_fiber(entry_t* entry, void* arg) {
	if (!fiber_pool.empty()) {
		Coroutine* fiber = fiber_pool.back();
		fiber_pool.pop_back();
		fiber->reset(entry, arg);
		return fiber;
	}
	Coroutine* coro = new Coroutine(*entry, arg);
	if (!coro_stack_alloc(&coro->stack, stack_size)) {
		delete coro;
		return NULL;
	}
	coro_create(&coro->context, trampoline, coro, coro->stack.sptr, coro->stack.ssze);
	if (stack_start_slot >= 0) {
		// v8 caches the thread's stack start; a fiber's stack starts at the top of its own allocation
		coro->fls_data[stack_start_slot] = reinterpret_cast<uint64_t>(coro->stack.sptr) + coro->stack.ssze;
	}
#ifdef CORO_FIBER
	// Stupid hack. libcoro's project structure combined with Windows's CreateFiber functions makes
	// it difficult to catch this error. Sometimes Windows will return `ERROR_NOT_ENOUGH_MEMORY` or
	// `ERROR_COMMITMENT_LIMIT` if it can't make any more fibers. However, `coro_stack_alloc` returns
	// success unconditionally on Windows so we have to detect the error here, after the call to
	// `coro_create`.
	if (!coro->context.fiber) {
		delete coro;
		return NULL;
	}
#endif
	++coroutines_created_;
	return coro;
}

void Coroutine::reset(entry_t* entry, void* arg) {
	assert(entry != NULL);
	this->entry = entry;
	this->arg = arg;
}

void Coroutine::transfer(Coroutine& next) {
	assert(this != &next);
#ifndef CORO_PTHREAD
	save_slots(fls_data);
	restore_slots(next.fls_data);

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

void Coroutine::finish(Coroutine& next, v8::Isolate* isolate) {
	{
		assert(&next != this);
		assert(&current() == this);
		if (fiber_pool.size() < pool_size) {
			fiber_pool.push_back(this);
		} else {
			// Clean up isolate data
			isolate->DiscardThreadSpecificMetadata();
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
