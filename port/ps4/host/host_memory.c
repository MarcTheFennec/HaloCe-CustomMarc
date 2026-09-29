/*
HOST_MEMORY.C

Guest memory below 4 GB.

M0 has only the probe. The game is linked to fixed addresses (the Xbox
window at 0x80000000, the image at 0x88000000); this reports whether the
Orbis kernel lets a process map those addresses, and how much flexible
memory a homebrew process has, before M2 builds the allocator on the
answer (port/ps4/PLAN.md, section 6). The probe's mappings are released
again.

The full version (M2) follows port/android/host/host_memory.c: the fixed
ranges reserved at start-up, and pools below 4 GB for the guest's malloc,
its thread stacks and its anonymous mappings.
*/

#include "host.h"

#include <orbis/libkernel.h>
#include <string.h>

/* sceKernelMapFlexibleMemory flags (Sony's SCE_KERNEL_MAP_*) */
#define MAP_FLAG_FIXED 0x0010
#define MAP_FLAG_NO_OVERWRITE 0x0080

#define PROBE_WINDOW_SIZE HALO_GUEST_WINDOW_SIZE /* the whole window */
#define PROBE_IMAGE_SIZE 0x04000000u             /* 64 MB, more than the image */
#define PROBE_POOL_SIZE 0x10000000u              /* 256 MB, anywhere below 4 GB */

static int map_fixed(uint64_t address, size_t size)
{
	void *pointer = (void *)(uintptr_t)address;
	int result = sceKernelMapFlexibleMemory(&pointer, size, ORBIS_KERNEL_PROT_CPU_RW, MAP_FLAG_FIXED | MAP_FLAG_NO_OVERWRITE);

	if (result != 0)
	{
		host_logf(HOST_LOG_WARN, "memory: fixed map of %llx (%zu MB) failed: 0x%08x", (unsigned long long)address,
			size >> 20, (unsigned)result);
		return 0;
	}
	if ((uintptr_t)pointer != address)
	{
		host_logf(HOST_LOG_WARN, "memory: fixed map of %llx came back at %p", (unsigned long long)address, pointer);
		sceKernelMunmap(pointer, size);
		return 0;
	}
	/* touch it: the pages must really be usable */
	memset(pointer, 0xa5, 0x1000);
	memset((char *)pointer + size - 0x1000, 0x5a, 0x1000);
	sceKernelMunmap(pointer, size);
	host_logf(HOST_LOG_INFO, "memory: fixed map of %llx (%zu MB) ok", (unsigned long long)address, size >> 20);
	return 1;
}

int host_memory_probe(struct host_memory_probe *result)
{
	size_t available = 0;
	uint64_t address;

	memset(result, 0, sizeof(*result));
	if (sceKernelAvailableFlexibleMemorySize(&available) == 0)
		result->flexible_available = available;
	host_logf(HOST_LOG_INFO, "memory: %llu MB of flexible memory available",
		(unsigned long long)(result->flexible_available >> 20));

	result->window_ok = map_fixed(HALO_GUEST_WINDOW_BASE, PROBE_WINDOW_SIZE);
	result->image_ok = map_fixed(HALO_GUEST_IMAGE_BASE, PROBE_IMAGE_SIZE);

	/* somewhere below 4 GB for the pools: try from 16 MB up */
	for (address = 0x01000000ull; address + PROBE_POOL_SIZE <= 0x100000000ull; address += PROBE_POOL_SIZE)
	{
		void *pointer = (void *)(uintptr_t)address;

		if (address >= HALO_GUEST_WINDOW_BASE && address < HALO_GUEST_IMAGE_BASE + PROBE_IMAGE_SIZE)
			continue;
		if (sceKernelMapFlexibleMemory(&pointer, PROBE_POOL_SIZE, ORBIS_KERNEL_PROT_CPU_RW,
				MAP_FLAG_FIXED | MAP_FLAG_NO_OVERWRITE) != 0)
			continue;
		if ((uintptr_t)pointer == address)
		{
			result->low_pool_ok = 1;
			result->low_pool_address = address;
		}
		sceKernelMunmap(pointer, PROBE_POOL_SIZE);
		if (result->low_pool_ok)
			break;
	}
	host_logf(result->low_pool_ok ? HOST_LOG_INFO : HOST_LOG_WARN, "memory: 256 MB pool below 4 GB %s%s%llx",
		result->low_pool_ok ? "ok at " : "not available", result->low_pool_ok ? "" : " ",
		(unsigned long long)result->low_pool_address);
	return result->window_ok && result->image_ok && result->low_pool_ok ? 0 : -1;
}
