/*
HALO_PS4_ABI.H

The contract between the PS4 host (port/ps4/host, a 64-bit OpenOrbis
application) and the guest image (the game built as x32 code: x86-64
instructions with 32-bit pointers, see port/ps4/PLAN.md section 2.1).

This header is shared by both sides, so it uses only fixed-width types
and no pointers. It mirrors port/android/include/halo_android_abi.h; the
constants that both ports have in common must stay equal, because the
game's data is linked to the Xbox addresses.

Parameter rules for host functions the guest imports (as on Android):
32-bit values are int or unsigned int, 64-bit values long long, and
pointers are passed as they are (the x32 psABI keeps them zero-extended in
registers, so the host receives a valid 64-bit pointer below 4 GB).
*/

#ifndef __HALO_PS4_ABI_H
#define __HALO_PS4_ABI_H

#include <stdint.h>

/* ---------- addresses (all below 4 GB) */

/* the guest image is linked here; the host copies its segments to this
address (host_loader.c) */
#define HALO_GUEST_IMAGE_BASE 0x88000000u
/* the Xbox contiguous memory window (port/linux/src/platform.h): tag cache,
game state, textures; cache files are linked to these addresses */
#define HALO_GUEST_WINDOW_BASE 0x80000000u
#define HALO_GUEST_WINDOW_SIZE 0x08000000u

/* ---------- the image header */

#define HALO_GUEST_MAGIC 0x4f4c4148u /* 'HALO' */
/* bump when the import table or the header changes */
#define HALO_GUEST_ABI_VERSION 1u

/* at HALO_GUEST_IMAGE_BASE, filled by the guest's link (M1) */
struct halo_guest_header
{
	uint32_t magic;
	uint32_t abi_version;
	/* address of an array of 64-bit slots, one for each import, in the order
	of import_names; the host writes the host function's address */
	uint32_t import_table;
	/* address of the NUL-separated import names */
	uint32_t import_names;
	/* address of a uint32_t: the number of imports */
	uint32_t import_count;
	/* address of the guest's entry point, __guest_start(boot) */
	uint32_t entry;
	uint32_t reserved[2];
};

/* ---------- storage (port/ps4/README.md)

The data root holds maps/ (copied to the console by the user), save/,
config.toml, debug.txt and modules/. The host looks in these places, in
this order, for a folder with maps/ in it. */

#define HALO_PS4_DATA_ROOTS \
	"/data/halo", \
	"/mnt/usb0/halo", \
	"/mnt/usb1/halo", \
	"/mnt/usb2/halo", \
	"/mnt/usb3/halo"
#define HALO_PS4_DEFAULT_DATA_ROOT "/data/halo"
#define HALO_PS4_TITLE_ID "HALO00001"

#endif
