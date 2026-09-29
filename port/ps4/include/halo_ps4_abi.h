/*
HALO_PS4_ABI.H

The contract between the two halves of the PS4 port (see
port/ps4/README.md and PORTING.md):

- the guest: the game, the platform layer shared with the Linux and Android
  ports (port/linux/src) and a small C runtime (port/ps4/guest), compiled as
  x32 code (x86-64 instructions, 32-bit pointers: the game's data formats
  need ILP32) and linked into a static ELF image that runs in the low 4 GB
  of the process;
- the host: the PS4 executable (eboot.bin, an ordinary LP64 Orbis program
  built with the OpenOrbis toolchain) that loads the image, owns the process
  (libkernel, Piglet, libScePad, libSceAudioOut) and serves the guest's
  requests. The same host code also builds as a headless Linux x86-64
  program, used to test the loader and system call layer without a console.

The guest calls the host through import stubs that jump through a table of
64-bit function pointers the host fills in at load time. x32 and the host's
SysV x86-64 ABI share registers, callee-saved sets and 8-byte stack slots;
only types whose treatment agrees cross this boundary: 32-bit integers,
64-bit integers, floats and doubles, and pointers (which clang's x32 target
passes zero-extended). Structures shared here have fixed-width members only.

This header is included by both halves. It has the same layout as
port/android/include/halo_android_abi.h so that the guest runtime is shared.
*/

#ifndef __HALO_PS4_ABI_H
#define __HALO_PS4_ABI_H

#include <stdint.h>

/* the guest image is linked to run here, just above the Xbox window */
#define HALO_GUEST_IMAGE_BASE 0x88000000u

/* the Xbox contiguous memory window (port/linux/src/platform.h) */
#define HALO_GUEST_WINDOW_BASE 0x80000000u
#define HALO_GUEST_WINDOW_SIZE 0x08000000u

#define HALO_GUEST_MAGIC 0x4f4c4148u /* 'HALO' */
#define HALO_GUEST_ABI_VERSION 1
/* marks an x32 image (the Android image is AArch64) */
#define HALO_GUEST_MACHINE_X32 0x3e

/* the directory the guest sees its data under; the host maps it onto the
content roots (/data/halo, /mnt/usb0/halo, /app0) in host_files.c */
#define HALO_GUEST_DATA_ROOT "/halo"

/* at HALO_GUEST_IMAGE_BASE */
struct halo_guest_header
{
	uint32_t magic;
	uint32_t abi_version;
	uint32_t image_end;          /* end of .bss */
	uint32_t import_table;       /* uint64_t[import_count], filled by the host */
	uint32_t import_names;       /* import_count NUL-terminated names */
	uint32_t import_count;       /* address of a uint32_t holding the count */
	uint32_t start;              /* void __guest_start(struct halo_guest_boot *) */
	uint32_t thread_start;       /* void __guest_thread_start(uint32_t thread) */
	uint32_t thread_attach;      /* uint32_t __guest_thread_attach(void) */
	uint32_t init_array_start;   /* void (*)(void) entries, 4 bytes each */
	uint32_t init_array_end;
};

/* the host's description of the process, handed to __guest_start */
struct halo_guest_boot
{
	uint32_t argc;
	uint32_t argv;               /* char ** in guest memory */
	uint32_t environment;        /* char ** in guest memory, NULL-terminated */
	uint32_t page_size;
};

#endif
