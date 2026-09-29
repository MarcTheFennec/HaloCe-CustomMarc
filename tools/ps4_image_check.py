#!/usr/bin/env python3
"""Check the PS4 guest image (build/ps4/halo_guest.elf) after linking.

The host loader (port/ps4/host/host_loader.c) expects an x32 executable
whose image header is at the image base, whose segments all lie below 4 GB
and outside the Xbox window, and which has no dynamic linking of any kind.
Catching a violation here is cheaper than on the console.

Usage: ps4_image_check.py image.elf
"""

import struct
import sys

IMAGE_BASE = 0x88000000
WINDOW_BASE, WINDOW_END = 0x80000000, 0x88000000
MAGIC = 0x4F4C4148
EM_X86_64 = 62
PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_TLS = 1, 2, 3, 7


def fail(message):
    raise SystemExit(f"ps4_image_check: {message}")


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        fail(f"{path} is not an ELF file")
    if data[4] != 1:
        fail("not a 32-bit ELF (an x32 image is ELFCLASS32)")
    e_type, e_machine, _, e_entry, e_phoff = struct.unpack_from("<HHIII", data, 16)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    if e_machine != EM_X86_64:
        fail(f"machine {e_machine}, expected x86-64 ({EM_X86_64})")
    if e_type != 2:
        fail("not an executable")
    if e_entry != IMAGE_BASE:
        fail(f"entry {e_entry:#x} is not the image header at {IMAGE_BASE:#x}")
    lowest, highest = None, 0
    for index in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(
            "<IIIIIIII", data, e_phoff + index * e_phentsize)
        if p_type in (PT_DYNAMIC, PT_INTERP):
            fail("the image must be static")
        if p_type == PT_TLS:
            fail("the image has TLS (compile with -femulated-tls)")
        if p_type != PT_LOAD or not p_memsz:
            continue
        end = p_vaddr + p_memsz
        if p_vaddr < WINDOW_END and end > WINDOW_BASE:
            fail(f"segment {p_vaddr:#x}-{end:#x} overlaps the Xbox window")
        lowest = p_vaddr if lowest is None else min(lowest, p_vaddr)
        highest = max(highest, end)
        if p_vaddr == IMAGE_BASE:
            magic = struct.unpack_from("<I", data, p_offset)[0]
            if magic != MAGIC:
                fail("no image header at the image base")
    if lowest != IMAGE_BASE:
        fail(f"the first segment is at {lowest}, not {IMAGE_BASE:#x}")
    print(f"ps4_image_check: {path}: {IMAGE_BASE:#x}-{highest:#x} ({(highest - IMAGE_BASE) / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
