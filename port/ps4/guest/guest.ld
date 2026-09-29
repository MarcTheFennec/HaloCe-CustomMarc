/*
GUEST.LD

Layout of the PS4 guest image (tools/ps4_build.py): an x32 ELF linked to run
at HALO_GUEST_IMAGE_BASE (port/ps4/include/halo_ps4_abi.h), with the image
header first. The host (port/ps4/host/host_loader.c) copies the loadable
segments to these addresses, clears .bss and makes the code read-only and
executable.
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0x88000000;
	.guest_header : { KEEP(*(.guest_header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.init_array : {
		__guest_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x1000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.fini_array*) *(.eh_frame) *(.eh_frame_hdr) }
}
