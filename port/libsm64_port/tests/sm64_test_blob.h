/*
SM64_TEST_BLOB.H

A synthetic, copyright-free stand-in for the asset data libsm64 reads out of
an SM64 ROM, so that the module's tests can run against the *real* library on
a machine that has no ROM.

This is not a ROM and contains no Nintendo data whatsoever.  libsm64's
sm64_global_init() reads exactly two things out of the buffer it is given:

  1. Mario's textures: one MIO0-compressed block at ROM offset 1132368, which
     it decodes and copies into an RGBA atlas.  MIO0's format is a 16-byte
     header (magic "MIO0", the decoded size, the offset of the compressed
     stream and the offset of the literal bytes) followed by a bit stream
     where a set bit means "take the next byte from the literal stream".  A
     block whose bits are all set decodes to its literal bytes - here, zeros.

  2. Mario's animations: a table at ROM offset 0x004EC000 of (u32 count, u32
     unused, count x (u32 offset, u32 size)) describing, for each animation,
     a small header and two arrays of u16.  All zeros make every animation a
     static pose, which is fine: animation drives the model, and the tests
     measure movement, collision and timing.

The result: real Super Mario 64 physics with a blank texture atlas and no
animation.  A user who wants Mario to look like Mario supplies their own
legally obtained ROM; this repository ships none, and neither does this file.

CC0 1.0 Universal, like the rest of the port.
*/

#ifndef SM64_TEST_BLOB_H
#define SM64_TEST_BLOB_H

#include <stddef.h>
#include <stdint.h>

/* Allocates the blob with malloc(); the caller frees it. */
uint8_t *sm64_test_blob_create(size_t *out_size);

#endif /* SM64_TEST_BLOB_H */
