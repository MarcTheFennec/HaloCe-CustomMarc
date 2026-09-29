/*
HOST_MEMORY.C

The guest's address space.

Everything the guest touches lies below 4 GB (x32 pointers):

	0x80000000-0x88000000  the Xbox contiguous window (fixed, the game's own
	                       memory map, port/linux/src/platform.h)
	0x88000000-...         the guest image (fixed, tools/ps4_build.py)
	0x90000000-0xf0000000  the pool: malloc arenas, thread stacks, the
	                       guest's anonymous mappings

At start-up the host reserves the three ranges (address space only) and
commits memory into them as the guest asks for it. How memory is reserved
and committed depends on the platform:

- Linux: mmap with MAP_FIXED_NOREPLACE, then MAP_FIXED;
- PS4: the first of three backends that works, probed once at start-up and
  logged ("memory backend: ..."):
    1. direct memory (sceKernelAllocateDirectMemory +
       sceKernelMapDirectMemory at the fixed address), of which a PS4 game
       has several GB;
    2. flexible memory (sceKernelMapNamedFlexibleMemory), whose budget is a
       few hundred MB and shared with Piglet;
    3. plain mmap.
  ASSUMPTION (unverified without a console): fixed mappings below 4 GB are
  allowed for a GoldHEN homebrew process. If none of the backends can map
  the Xbox window the host says so on screen and in the log.

The guest's code must be executable: the image's text is committed writable,
filled, then made read-only and executable with mprotect (under GoldHEN,
whose kernel patches allow executable mappings for homebrew; ASSUMPTION).

Guest memory write tracking (port/linux/src/memory_watch.c's interface) is
implemented here too: the renderer write-protects the pages behind the
textures it caches, and the SIGSEGV handler records the first write to each.
If a signal handler cannot be installed the tracker falls back to sampling
page contents (see "write tracking" below).

Other faults are reported with guest-relative addresses and the ring buffer
of the log is written to crash.log.
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__ORBIS__)
#include <orbis/libkernel.h>
#else
#include <ucontext.h>
#endif

#if defined(__ORBIS__)
/* OpenOrbis musl's signal.h maps sa_sigaction to __sa_handler.sa_sigaction,
but names the member __sa_sigaction */
#undef sa_sigaction
#define sa_sigaction __sa_handler.__sa_sigaction
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#if defined(__linux__) && !defined(MAP_FIXED_NOREPLACE)
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define LOW_LIMIT 0x100000000ULL
#define POOL_BASE 0x90000000ULL
#define POOL_END 0xf0000000ULL
/* the pool's allocation unit: 64 KB (the PS4's direct memory maps in 16 KB
pages; musl asks for page multiples) */
#define GRANULE 0x10000ULL
#define POOL_GRANULES ((POOL_END - POOL_BASE) / GRANULE)

/* the guest's (Linux) mmap flags */
#define LINUX_MAP_SHARED 0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

uint32_t host_page_size = 0x1000;

static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t window_base, window_end;
static uint64_t image_base, image_end;
static uint64_t pool_base = POOL_BASE, pool_end = POOL_END;
/* per granule: 0 free, 1 used, 2 used and the first of its allocation */
static uint8_t granule_state[POOL_GRANULES];
static uint64_t committed_bytes, peak_bytes;

static uint64_t round_up(uint64_t value, uint64_t unit)
{
	return (value + unit - 1) & ~(unit - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

/* ---------- the platform's reserve and commit */

#if defined(__ORBIS__)

/* libkernel's protection and mapping flags */
#define SCE_PROT_CPU_READ 0x01
#define SCE_PROT_CPU_WRITE 0x02
#define SCE_PROT_CPU_EXEC 0x04
#define SCE_PROT_GPU_READ 0x10
#define SCE_PROT_GPU_WRITE 0x20
#define SCE_MAP_FIXED 0x10
#define SCE_MAP_NO_OVERWRITE 0x0080
#define SCE_WB_ONION 0

enum { _backend_direct, _backend_flexible, _backend_mmap, _backend_none };
static const char *const backend_names[] = { "direct memory", "flexible memory", "mmap", "none" };
static int backend = _backend_none;

/* direct memory allocations, to release them */
#define DIRECT_MAXIMUM 4096
static struct { uint64_t address, length; off_t offset; } direct_blocks[DIRECT_MAXIMUM];

static int sce_protection(int protection)
{
	int result = 0;

	if (protection & PROT_READ)
		result |= SCE_PROT_CPU_READ | SCE_PROT_GPU_READ;
	if (protection & PROT_WRITE)
		result |= SCE_PROT_CPU_WRITE | SCE_PROT_GPU_WRITE;
	if (protection & PROT_EXEC)
		result |= SCE_PROT_CPU_EXEC;
	return result;
}

static int platform_reserve(uint64_t address, uint64_t size)
{
	void *result = (void *)(uintptr_t)address;
	int error = sceKernelReserveVirtualRange(&result, size, SCE_MAP_FIXED | SCE_MAP_NO_OVERWRITE, 0);

	if (error == 0 && result == (void *)(uintptr_t)address)
		return 0;
	host_logf(HOST_LOG_WARN, "sceKernelReserveVirtualRange(%08llx, %llx) = %#x (%p)",
		(unsigned long long)address, (unsigned long long)size, error, result);
	/* a hint only: accepted if the kernel honours it */
	result = mmap((void *)(uintptr_t)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (result == (void *)(uintptr_t)address)
		return 0;
	if (result != MAP_FAILED)
		munmap(result, size);
	return -1;
}

static void direct_forget(uint64_t address, uint64_t length)
{
	int index;

	for (index = 0; index < DIRECT_MAXIMUM; index++)
	{
		if (direct_blocks[index].length && direct_blocks[index].address >= address &&
			direct_blocks[index].address + direct_blocks[index].length <= address + length)
		{
			sceKernelReleaseDirectMemory(direct_blocks[index].offset, direct_blocks[index].length);
			direct_blocks[index].length = 0;
		}
	}
}

static int commit_with(int which, uint64_t address, uint64_t size, int protection)
{
	void *result = (void *)(uintptr_t)address;
	int error = -1;

	switch (which)
	{
	case _backend_direct:
	{
		off_t offset = 0;
		int index;

		for (index = 0; index < DIRECT_MAXIMUM && direct_blocks[index].length; index++)
			;
		if (index == DIRECT_MAXIMUM)
			return -1;
		error = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x4000, SCE_WB_ONION, &offset);
		if (error)
			break;
		error = sceKernelMapDirectMemory(&result, size, sce_protection(protection), SCE_MAP_FIXED, offset, 0x4000);
		if (error || result != (void *)(uintptr_t)address)
		{
			sceKernelReleaseDirectMemory(offset, size);
			error = error ? error : -1;
			break;
		}
		direct_blocks[index].address = address;
		direct_blocks[index].length = size;
		direct_blocks[index].offset = offset;
		break;
	}
	case _backend_flexible:
		error = sceKernelMapNamedFlexibleMemory(&result, size, sce_protection(protection), SCE_MAP_FIXED, "halo guest");
		if (!error && result != (void *)(uintptr_t)address)
			error = -1;
		break;
	case _backend_mmap:
		result = mmap(result, size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		error = result == (void *)(uintptr_t)address ? 0 : -1;
		break;
	}
	if (error)
		host_logf(HOST_LOG_DEBUG, "%s at %08llx+%llx failed (%#x)", backend_names[which],
			(unsigned long long)address, (unsigned long long)size, error);
	return error ? -1 : 0;
}

static int platform_commit(uint64_t address, uint64_t size, int protection)
{
	int which;

	if (backend != _backend_none)
		return commit_with(backend, address, size, protection);
	/* the first commit (the Xbox window) picks the backend */
	for (which = _backend_direct; which < _backend_none; which++)
	{
		if (commit_with(which, address, size, protection) == 0)
		{
			backend = which;
			host_logf(HOST_LOG_INFO, "memory backend: %s", backend_names[which]);
			return 0;
		}
	}
	return -1;
}

static void platform_decommit(uint64_t address, uint64_t size)
{
	void *result = (void *)(uintptr_t)address;

	munmap((void *)(uintptr_t)address, size);
	if (backend == _backend_direct)
		direct_forget(address, size);
	/* keep the address space */
	if (sceKernelReserveVirtualRange(&result, size, SCE_MAP_FIXED, 0) != 0)
		mmap((void *)(uintptr_t)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

static int platform_protect(uint64_t address, uint64_t size, int protection)
{
	if (mprotect((void *)(uintptr_t)address, size, protection) == 0)
		return 0;
	return sceKernelMprotect((void *)(uintptr_t)address, size, sce_protection(protection)) == 0 ? 0 : -1;
}

#else /* Linux */

static int platform_reserve(uint64_t address, uint64_t size)
{
	void *result = mmap((void *)(uintptr_t)address, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

	if (result == (void *)(uintptr_t)address)
		return 0;
	if (result != MAP_FAILED)
		munmap(result, size);
	return -1;
}

static int platform_commit(uint64_t address, uint64_t size, int protection)
{
	return mmap((void *)(uintptr_t)address, size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
		(void *)(uintptr_t)address ? 0 : -1;
}

static void platform_decommit(uint64_t address, uint64_t size)
{
	mmap((void *)(uintptr_t)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

static int platform_protect(uint64_t address, uint64_t size, int protection)
{
	return mprotect((void *)(uintptr_t)address, size, protection);
}

#endif

static void count_commit(int64_t bytes)
{
	committed_bytes += (uint64_t)bytes;
	if (committed_bytes > peak_bytes)
		peak_bytes = committed_bytes;
}

/* ---------- start-up */

int host_memory_initialize(uint32_t base, uint32_t size)
{
	long page = sysconf(_SC_PAGESIZE);

	host_page_size = page > 0 ? (uint32_t)page : 0x1000;
#if defined(__ORBIS__)
	/* libkernel maps in 16 KB pages */
	if (host_page_size < 0x4000)
		host_page_size = 0x4000;
#endif
	window_base = HALO_GUEST_WINDOW_BASE;
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	image_base = base;
	image_end = base + round_up(size, GRANULE);
	if (image_end > POOL_BASE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image (%08llx-%08llx) runs into the pool", (unsigned long long)image_base,
			(unsigned long long)image_end);
		return -1;
	}
	if (platform_reserve(window_base, window_end - window_base) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the Xbox memory window at %08llx (%s)",
			(unsigned long long)window_base, strerror(errno));
		return -1;
	}
	if (platform_reserve(image_base, image_end - image_base) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest image range at %08llx (%s)",
			(unsigned long long)image_base, strerror(errno));
		return -1;
	}
	/* the pool: as much of 0x90000000-0xf0000000 as can be had, halving
	from the top */
	while (pool_end - pool_base >= 0x10000000ULL)
	{
		if (platform_reserve(pool_base, pool_end - pool_base) == 0)
			break;
		pool_end = pool_base + (pool_end - pool_base) / 2;
	}
	if (pool_end - pool_base < 0x10000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve guest memory at %08llx", (unsigned long long)POOL_BASE);
		return -1;
	}
	/* the window is committed whole: the game's allocator hands it out
	itself (port/linux/src/xbox_memory.c) */
	if (platform_commit(window_base, window_end - window_base, PROT_READ | PROT_WRITE) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot commit the Xbox memory window (%s)", strerror(errno));
		return -1;
	}
	count_commit((int64_t)(window_end - window_base));
	host_logf(HOST_LOG_INFO, "guest memory: window %08llx-%08llx, image %08llx-%08llx, pool %08llx-%08llx, page %u",
		(unsigned long long)window_base, (unsigned long long)window_end, (unsigned long long)image_base,
		(unsigned long long)image_end, (unsigned long long)pool_base, (unsigned long long)pool_end, host_page_size);
	return 0;
}

int host_memory_commit_image(void)
{
	if (platform_commit(image_base, image_end - image_base, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 &&
		platform_commit(image_base, image_end - image_base, PROT_READ | PROT_WRITE) != 0)
		return -1;
	count_commit((int64_t)(image_end - image_base));
	return 0;
}

/* ---------- the pool */

static uint64_t pool_take(uint64_t granules)
{
	uint64_t count = (pool_end - pool_base) / GRANULE;
	uint64_t run = 0, index;

	for (index = 0; index < count; index++)
	{
		if (granule_state[index])
		{
			run = 0;
			continue;
		}
		if (++run == granules)
		{
			uint64_t first = index + 1 - granules;

			memset(&granule_state[first], 1, granules);
			granule_state[first] = 2;
			return pool_base + first * GRANULE;
		}
	}
	return 0;
}

static void pool_release(uint64_t address, uint64_t length)
{
	uint64_t first = (address - pool_base) / GRANULE, count = length / GRANULE, index;

	for (index = first; index < first + count; index++)
		granule_state[index] = 0;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t length = round_up(size, GRANULE);
	uint64_t address;

	if (!length)
		return NULL;
	pthread_mutex_lock(&memory_lock);
	address = pool_take(length / GRANULE);
	if (address && platform_commit(address, length, protection) != 0)
	{
		pool_release(address, length);
		address = 0;
	}
	if (address)
		count_commit((int64_t)length);
	pthread_mutex_unlock(&memory_lock);
	if (!address)
	{
		host_logf(HOST_LOG_ERROR, "out of guest memory (%llu KB requested, %llu MB committed)",
			(unsigned long long)(size / 1024), (unsigned long long)(committed_bytes >> 20));
		return NULL;
	}
	return (void *)(uintptr_t)address;
}

void host_low_unmap(void *pointer, size_t size)
{
	uint64_t address = (uint64_t)(uintptr_t)pointer & ~(GRANULE - 1);
	uint64_t length = round_up((uint64_t)(uintptr_t)pointer + size, GRANULE) - address;

	if (!in_range(address, length, pool_base, pool_end))
		return;
	pthread_mutex_lock(&memory_lock);
	platform_decommit(address, length);
	pool_release(address, length);
	count_commit(-(int64_t)length);
	pthread_mutex_unlock(&memory_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	return in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end) ||
		in_range(address, size, pool_base, pool_end);
}

void host_memory_usage(uint64_t *current, uint64_t *peak)
{
	*current = committed_bytes;
	*peak = peak_bytes;
}

void host_memory_log_watermark(const char *reason)
{
	uint64_t used = 0, index, count = (pool_end - pool_base) / GRANULE;

	for (index = 0; index < count; index++)
		used += granule_state[index] != 0;
#if defined(__ORBIS__)
	{
		size_t flexible = 0;

		sceKernelAvailableFlexibleMemorySize(&flexible);
		host_logf(HOST_LOG_INFO, "memory (%s): %llu MB committed, peak %llu MB, pool %llu/%llu MB, "
			"flexible free %llu MB", reason, (unsigned long long)(committed_bytes >> 20),
			(unsigned long long)(peak_bytes >> 20), (unsigned long long)(used * GRANULE >> 20),
			(unsigned long long)((pool_end - pool_base) >> 20), (unsigned long long)(flexible >> 20));
	}
#else
	host_logf(HOST_LOG_INFO, "memory (%s): %llu MB committed, peak %llu MB, pool %llu/%llu MB", reason,
		(unsigned long long)(committed_bytes >> 20), (unsigned long long)(peak_bytes >> 20),
		(unsigned long long)(used * GRANULE >> 20), (unsigned long long)((pool_end - pool_base) >> 20));
#endif
}

/* ---------- the guest's memory system calls */

static int host_protection(int linux_protection)
{
	/* PROT_READ 1, PROT_WRITE 2, PROT_EXEC 4 on both */
	return linux_protection & (PROT_READ | PROT_WRITE | PROT_EXEC);
}

/* a file mapping: memory filled from the file (the guest maps only small
read-only files, such as time zone data) */
static long map_file(uint64_t address, uint64_t length, int fd, int64_t offset)
{
	uint64_t done = 0;

	while (done < length)
	{
		ssize_t count = pread(fd, (char *)(uintptr_t)address + done, (size_t)(length - done), (off_t)(offset + done));

		if (count < 0)
			return -errno;
		if (count == 0)
			break;
		done += (uint64_t)count;
	}
	return 0;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, host_page_size);
	int anonymous = (flags & LINUX_MAP_ANONYMOUS) != 0;
	void *result;

	if (!length)
		return -EINVAL;
	if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
	{
		/* only inside the host's ranges, which are the guest's */
		if (!host_low_owns(address, length))
			return (flags & LINUX_MAP_FIXED_NOREPLACE) ? -EEXIST : -EINVAL;
		if (in_range(address, length, window_base, window_end) || in_range(address, length, image_base, image_end))
		{
			memset((void *)(uintptr_t)address, 0, length);
			platform_protect(address, length, host_protection(protection) | PROT_READ | PROT_WRITE);
			return anonymous ? (long)address : (map_file(address, length, fd, offset) ?: (long)address);
		}
		/* in the pool: the range must have been handed out already */
		memset((void *)(uintptr_t)address, 0, length);
		if (!anonymous)
		{
			long error = map_file(address, length, fd, offset);

			if (error)
				return error;
		}
		return (long)address;
	}
	result = host_low_map(length, PROT_READ | PROT_WRITE);
	if (!result)
		return -ENOMEM;
	if (!anonymous)
	{
		long error = map_file((uint64_t)(uintptr_t)result, length, fd, offset);

		if (error)
		{
			host_low_unmap(result, length);
			return error;
		}
	}
	if ((protection & 7) != (PROT_READ | PROT_WRITE))
		platform_protect((uint64_t)(uintptr_t)result, round_up(length, GRANULE), host_protection(protection));
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size, host_page_size);

	if (address + length > LOW_LIMIT)
		return -EINVAL;
	if (in_range(address, length, window_base, window_end))
	{
		/* the window stays committed; the game's allocator owns it */
		return 0;
	}
	if (in_range(address, length, image_base, image_end))
		return -EINVAL;
	if (in_range(address, length, pool_base, pool_end))
	{
		/* only whole allocations are given back; musl frees whole
		mappings, or trims them (which is left mapped) */
		if ((address & (GRANULE - 1)) == 0 && (length & (GRANULE - 1)) == 0)
			host_low_unmap((void *)(uintptr_t)address, length);
		return 0;
	}
	return -EINVAL;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	if (!host_low_owns(address, size))
		return -EINVAL;
	return platform_protect(address & ~(uint64_t)(host_page_size - 1),
		round_up((address & (host_page_size - 1)) + size, host_page_size), host_protection(protection)) ? -errno : 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c)

Signal mode: the pages behind cached textures are made read-only; the first
write faults and the handler gives the page a new generation.

Sampling mode (if no SIGSEGV handler could be installed, or config.toml
sets ps4.memory_watch = "sample"): nothing is protected; a page's generation
advances when a sample of 64 of its words changed since the last query.
Slower to query and blind to writes between the samples, but safe. */

#define WATCH_MAX_PAGES (HALO_GUEST_WINDOW_SIZE / 0x1000)

static uint8_t page_protected[WATCH_MAX_PAGES];
static uint32_t page_generation[WATCH_MAX_PAGES];
static uint32_t page_sample[WATCH_MAX_PAGES];
static volatile uint32_t current_generation = 1;
static int watch_active;
static int watch_sampling;
static uint32_t watch_page_size = 0x1000;

void host_memory_watch_set_sampling(int sampling)
{
	watch_sampling = sampling;
}

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / watch_page_size;
}

static uint64_t watch_page_count(void)
{
	return HALO_GUEST_WINDOW_SIZE / watch_page_size;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	platform_protect(HALO_GUEST_WINDOW_BASE + page * watch_page_size, watch_page_size, PROT_READ | PROT_WRITE);
}

static uint32_t sample_page(uint64_t page)
{
	const uint32_t *words = (const uint32_t *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * watch_page_size);
	uint32_t count = watch_page_size / 4, step = count / 64, index, hash = 2166136261u;

	for (index = 0; index < count; index += step)
		hash = (hash ^ words[index]) * 16777619u;
	return hash;
}

/* ---------- crashes */

static struct sigaction previous_segv, previous_bus, previous_ill, previous_fpe;
static volatile int crashing;

#if defined(__ORBIS__)
/* the kernel's (FreeBSD amd64) ucontext_t: a 16-byte signal mask, then the
machine context. ASSUMPTION: the layout of FreeBSD 9, which the PS4 kernel
derives from; OpenOrbis's musl headers describe Linux's instead. */
struct orbis_mcontext
{
	uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
	uint32_t trapno;
	uint16_t fs, gs;
	uint64_t addr;
	uint32_t flags;
	uint16_t es, ds;
	uint64_t err, rip, cs, rflags, rsp, ss;
};

static void context_registers(void *context, uint64_t *pc, uint64_t *sp, uint64_t *fp, uint64_t general[16])
{
	const struct orbis_mcontext *m = (const struct orbis_mcontext *)((const char *)context + 16);

	*pc = m->rip;
	*sp = m->rsp;
	*fp = m->rbp;
	general[0] = m->rax; general[1] = m->rbx; general[2] = m->rcx; general[3] = m->rdx;
	general[4] = m->rsi; general[5] = m->rdi; general[6] = m->rbp; general[7] = m->rsp;
	general[8] = m->r8; general[9] = m->r9; general[10] = m->r10; general[11] = m->r11;
	general[12] = m->r12; general[13] = m->r13; general[14] = m->r14; general[15] = m->r15;
}
#else
static void context_registers(void *context, uint64_t *pc, uint64_t *sp, uint64_t *fp, uint64_t general[16])
{
	const greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;

	*pc = (uint64_t)g[REG_RIP];
	*sp = (uint64_t)g[REG_RSP];
	*fp = (uint64_t)g[REG_RBP];
	general[0] = (uint64_t)g[REG_RAX]; general[1] = (uint64_t)g[REG_RBX]; general[2] = (uint64_t)g[REG_RCX];
	general[3] = (uint64_t)g[REG_RDX]; general[4] = (uint64_t)g[REG_RSI]; general[5] = (uint64_t)g[REG_RDI];
	general[6] = (uint64_t)g[REG_RBP]; general[7] = (uint64_t)g[REG_RSP]; general[8] = (uint64_t)g[REG_R8];
	general[9] = (uint64_t)g[REG_R9]; general[10] = (uint64_t)g[REG_R10]; general[11] = (uint64_t)g[REG_R11];
	general[12] = (uint64_t)g[REG_R12]; general[13] = (uint64_t)g[REG_R13]; general[14] = (uint64_t)g[REG_R14];
	general[15] = (uint64_t)g[REG_R15];
}
#endif

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	static const char *const names[16] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
		"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
	uint64_t pc, sp, fp, general[16];
	uint64_t stack_low = 0, stack_high = 0;
	int index;

	if (__sync_lock_test_and_set(&crashing, 1))
		return;
	context_registers(context, &pc, &sp, &fp, general);
	host_logf(HOST_LOG_FATAL, "signal %d, fault address %p, pc %016llx sp %016llx", signal_number,
		information ? information->si_addr : NULL, (unsigned long long)pc, (unsigned long long)sp);
	if (pc >= host_image.base && pc < host_image.end)
		host_logf(HOST_LOG_FATAL, "  in the guest: llvm-addr2line -f -e build/ps4/halo_guest.elf 0x%llx",
			(unsigned long long)pc);
	for (index = 0; index < 16; index += 4)
		host_logf(HOST_LOG_FATAL, "  %-3s %016llx %-3s %016llx %-3s %016llx %-3s %016llx", names[index],
			(unsigned long long)general[index], names[index + 1], (unsigned long long)general[index + 1],
			names[index + 2], (unsigned long long)general[index + 2], names[index + 3],
			(unsigned long long)general[index + 3]);
	/* the guest keeps frame pointers (-fno-omit-frame-pointer): rbp chains
	of 8-byte saved rbp and return address */
	host_thread_guest_stack(&stack_low, &stack_high);
	for (index = 0; index < 32 && fp >= stack_low && fp + 16 <= stack_high && (fp & 7) == 0; index++)
	{
		const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

		host_logf(HOST_LOG_FATAL, "  frame %2d: return %08llx", index, (unsigned long long)frame[1]);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
	host_memory_log_watermark("crash");
	host_log_flush();
	host_log_dump_ring("crash");
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the previous
	(or default) handler */
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr;

	if (watch_active && !watch_sampling && in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	/* FreeBSD reports some protection faults as SIGBUS */
	if (watch_active && !watch_sampling && in_window((uint64_t)(uintptr_t)information->si_addr))
	{
		uint64_t page = watch_page((uint64_t)(uintptr_t)information->si_addr);

		if (page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_ill, signal_number, information, context);
}

static void fpe_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_fpe, signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;
	int failed = 0;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	failed |= sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	failed |= sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
	action.sa_sigaction = fpe_handler;
	sigaction(SIGFPE, &action, &previous_fpe);
	if (failed)
	{
		host_logf(HOST_LOG_WARN, "cannot install the SIGSEGV handler (%s): texture write tracking samples memory",
			strerror(errno));
		watch_sampling = 1;
	}
}

void host_memory_watch_initialize(void)
{
	watch_page_size = host_page_size > 0x1000 ? host_page_size : 0x1000;
	if (!watch_sampling)
	{
		/* self-test: a write to a protected window page must come back
		through the handler */
		uint64_t page = watch_page_count() - 1;
		volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * watch_page_size);
		uint32_t before = current_generation, value = *word;

		watch_active = 1;
		page_protected[page] = 1;
		if (platform_protect(HALO_GUEST_WINDOW_BASE + page * watch_page_size, watch_page_size, PROT_READ) != 0)
		{
			page_protected[page] = 0;
			watch_sampling = 1;
			host_logf(HOST_LOG_WARN, "memory watch: mprotect failed; sampling instead");
		}
		else
		{
			*word = value;
			if (current_generation == before)
			{
				watch_sampling = 1;
				host_logf(HOST_LOG_WARN, "memory watch: the protected write did not fault; sampling instead");
			}
		}
	}
	watch_active = 1;
	host_logf(HOST_LOG_INFO, "memory watch: %s, %u-byte pages", watch_sampling ? "sampling" : "write faults",
		watch_page_size);
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= watch_page_count())
		last = watch_page_count() - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			continue;
		page_protected[page] = 1;
		if (watch_sampling)
			page_sample[page] = sample_page(page);
		else
			platform_protect(HALO_GUEST_WINDOW_BASE + page * watch_page_size, watch_page_size, PROT_READ);
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= watch_page_count())
		last = watch_page_count() - 1;
	for (page = first; page <= last; page++)
	{
		if (watch_sampling && page_protected[page])
		{
			uint32_t sample = sample_page(page);

			if (sample != page_sample[page])
			{
				page_sample[page] = sample;
				page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
			}
		}
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= watch_page_count())
		last = watch_page_count() - 1;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
			continue;
		if (watch_sampling)
		{
			page_protected[page] = 0;
			page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
		}
		else
		{
			mark_written(page);
		}
	}
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= watch_page_count())
		last = watch_page_count() - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] && !watch_sampling)
			platform_protect(HALO_GUEST_WINDOW_BASE + page * watch_page_size, watch_page_size, PROT_READ | PROT_WRITE);
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}
