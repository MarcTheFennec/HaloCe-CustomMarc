/*
HOST_IMAGE_BLOB.S

The guest image (build/ps4/halo_guest.elf) inside eboot.bin, so that the
executable is self-contained: host_main.c hands these bytes to
host_load_image. HALO_GUEST_IMAGE is the image's path in quotes
(tools/ps4_build.py passes it).
*/

	.section .rodata.halo_guest_image, "a"
	.balign 64
	.globl halo_guest_image_start
halo_guest_image_start:
	.incbin HALO_GUEST_IMAGE
	.globl halo_guest_image_end
halo_guest_image_end:
	.byte 0

	.section .note.GNU-stack, "", @progbits
