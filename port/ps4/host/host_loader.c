/*
HOST_LOADER.C

Loads the guest image: a statically linked x32 ELF executable
(tools/ps4_build.py, checked by tools/ps4_image_check.py) whose segments are
copied to the addresses it was linked at, below 4 GB. The image starts with a
struct halo_guest_header naming its import table, which is filled with the
host functions of the same names (the generated host_import_table.c).

On the PS4 the image is part of eboot.bin (embedded with .incbin by
host_image_blob.S), so the PKG stays a single executable.
*/

#include "host.h"

#include <elf.h>
#include <string.h>
#include <sys/mman.h>

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the game called a host function that is not available (see host.log)");
}

int host_load_image(const void *file, size_t size)
{
	const Elf32_Ehdr *elf = file;
	const Elf32_Phdr *segments;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != ELFCLASS32 ||
		elf->e_machine != HALO_GUEST_MACHINE_X32 || elf->e_type != ET_EXEC)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an x32 executable");
		return -1;
	}
	segments = (const Elf32_Phdr *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		if (segments[index].p_type != PT_LOAD || !segments[index].p_memsz)
			continue;
		if (segments[index].p_vaddr < low)
			low = segments[index].p_vaddr;
		if ((uint64_t)segments[index].p_vaddr + segments[index].p_memsz > high)
			high = (uint64_t)segments[index].p_vaddr + segments[index].p_memsz;
	}
	low &= ~0xfffULL;
	high = (high + 0xffff) & ~0xffffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;
	if (host_memory_commit_image() != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot commit memory for the guest image");
		return -1;
	}
	memset((void *)(uintptr_t)low, 0, high - low);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf32_Phdr *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if ((uint64_t)segment->p_offset + segment->p_filesz > size)
			return -1;
		memcpy((void *)(uintptr_t)segment->p_vaddr, (const char *)file + segment->p_offset, segment->p_filesz);
	}

	header = (const struct halo_guest_header *)(uintptr_t)low;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	table = (uint64_t *)(uintptr_t)header->import_table;
	name = (const char *)(uintptr_t)header->import_names;
	count = *(const uint32_t *)(uintptr_t)header->import_count;
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf32_Phdr *segment = &segments[index];
		uint64_t start = segment->p_vaddr & ~(uint64_t)(host_page_size - 1);
		uint64_t end = ((uint64_t)segment->p_vaddr + segment->p_memsz + host_page_size - 1) &
			~(uint64_t)(host_page_size - 1);

		if (segment->p_type != PT_LOAD || !(segment->p_flags & PF_X))
			continue;
		if (host_guest_mprotect(start, end - start, PROT_READ | PROT_EXEC) != 0)
		{
			host_logf(HOST_LOG_ERROR, "cannot make the guest's code executable (%08llx-%08llx)",
				(unsigned long long)start, (unsigned long long)end);
			return -1;
		}
	}
	return 0;
}
