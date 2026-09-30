/* See sm64_test_blob.h.  CC0 1.0 Universal. */

#include "sm64_test_blob.h"

#include <stdlib.h>
#include <string.h>

/* The two addresses libsm64 reads, from its src/load_tex_data.c and
   src/load_anim_data.c. */
#define TEST_BLOB_ANIM_DATA_ADDRESS 0x004EC000u
#define TEST_BLOB_MARIO_TEX_ROM_OFFSET 1132368u

/* Enough entries for every MARIO_ANIM_* id (the largest is 0xD0). */
#define TEST_BLOB_ANIM_ENTRIES 256
/* Pairs of u16 retrieve_animation_index() may consume before it runs out. */
#define TEST_BLOB_ANIM_INDEX_WORDS 512
#define TEST_BLOB_ANIM_VALUE_WORDS 64

/* The decoded size of the texture block: it has to cover the highest texture
   offset libsm64 asks for (37008 + 32*64*2 bytes). */
#define TEST_BLOB_TEXTURE_SIZE 65536u

static void put_be32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)((value >> 24) & 0xFF);
	p[1] = (uint8_t)((value >> 16) & 0xFF);
	p[2] = (uint8_t)((value >> 8) & 0xFF);
	p[3] = (uint8_t)(value & 0xFF);
}

uint8_t *sm64_test_blob_create(size_t *out_size)
{
	const size_t entry_bytes = 12 + TEST_BLOB_ANIM_INDEX_WORDS * 2 + TEST_BLOB_ANIM_VALUE_WORDS * 2 + 64;
	const size_t anim_bytes = 8 + (size_t)TEST_BLOB_ANIM_ENTRIES * 8
		+ (size_t)TEST_BLOB_ANIM_ENTRIES * entry_bytes;
	const size_t texture_bytes = (size_t)TEST_BLOB_MARIO_TEX_ROM_OFFSET + 16
		+ TEST_BLOB_TEXTURE_SIZE / 8 + TEST_BLOB_TEXTURE_SIZE + 64;
	size_t size = TEST_BLOB_ANIM_DATA_ADDRESS + anim_bytes;
	uint8_t *rom;
	uint8_t *mio;
	uint8_t *table;
	uint32_t base;
	uint32_t i;

	if (texture_bytes > size)
		size = texture_bytes;

	rom = (uint8_t *)calloc(1, size);
	if (!rom)
		return NULL;

	/* 1. The MIO0 texture block: a header saying "65536 bytes, all of them
	      literals", a bit stream of set bits, and 65536 zero bytes. */
	mio = rom + TEST_BLOB_MARIO_TEX_ROM_OFFSET;
	memcpy(mio, "MIO0", 4);
	put_be32(mio + 4, (uint32_t)TEST_BLOB_TEXTURE_SIZE);
	put_be32(mio + 8, 16u);                                    /* compressed stream */
	put_be32(mio + 12, 16u + TEST_BLOB_TEXTURE_SIZE / 8);      /* literal bytes */
	memset(mio + 16, 0xFF, TEST_BLOB_TEXTURE_SIZE / 8);        /* every bit set */

	/* 2. The animation table. */
	table = rom + TEST_BLOB_ANIM_DATA_ADDRESS;
	put_be32(table, (uint32_t)TEST_BLOB_ANIM_ENTRIES);
	base = 8 + (uint32_t)TEST_BLOB_ANIM_ENTRIES * 8;

	for (i = 0; i < TEST_BLOB_ANIM_ENTRIES; i++)
	{
		uint32_t offset = base + i * (uint32_t)entry_bytes;
		uint8_t *entry;

		put_be32(table + 8 + i * 8, offset);
		put_be32(table + 8 + i * 8 + 4, (uint32_t)entry_bytes);

		entry = table + offset;
		/* s16 flags, s16 animYTransDivisor, s16 startFrame, s16 loopStart,
		   s16 loopEnd, s16 unusedBoneCount: 12 bytes. */
		entry[2] = 0; entry[3] = 1;   /* animYTransDivisor: never zero (division) */
		entry[8] = 0; entry[9] = 2;   /* loopEnd: two frames, so the frame
		                                 counter advances and wraps */
		/* u32 valuesOffset, u32 indexOffset, u32 endOffset */
		put_be32(entry + 12, 12 + TEST_BLOB_ANIM_INDEX_WORDS * 2);
		put_be32(entry + 16, 12);
		put_be32(entry + 20, 12 + TEST_BLOB_ANIM_INDEX_WORDS * 2 + TEST_BLOB_ANIM_VALUE_WORDS * 2);
		/* The index and value arrays are zeros already. */
	}

	if (out_size)
		*out_size = size;
	return rom;
}
