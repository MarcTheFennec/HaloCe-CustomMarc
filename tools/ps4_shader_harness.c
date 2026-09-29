/*
PS4_SHADER_HARNESS.C

Checks the PS4 host's GLSL ES 3.00 -> 1.00 translation (port/ps4/host/
host_shader.c) against the real shader generators (port/linux/src/
nv2a_vsh.c, nv2a_psh.c). It generates vertex programs from random NV2A
instruction words and pixel shaders from random combiner keys, as the
Android/PS4 guest does (HALO_ANDROID), translates each, and writes both the
ES 3.00 original and the ES 1.00 translation to a directory, where
tools/ps4_shader_check.sh runs glslangValidator on every file.

Built natively (x86_64) by tools/ps4_shader_check.sh; not part of any game
build.

Usage: ps4_shader_harness <output directory> [count] [seed]
*/

#include "xgpu.h"
#include "port_config.h"
#include "host_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xgpu_capabilities xgpu_capabilities;

int config_boolean(const char *name)
{
	(void)name;
	return 0;
}

long config_integer(const char *name)
{
	(void)name;
	return 0;
}

const char *config_string(const char *name)
{
	(void)name;
	return "";
}

static unsigned long long random_state;

static unsigned long random_word(void)
{
	random_state = random_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (unsigned long)((random_state >> 32) & 0xffffffffUL);
}

static int write_file(const char *directory, const char *name, const char *text)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", directory, name);
	file = fopen(path, "w");
	if (!file)
	{
		perror(path);
		return 0;
	}
	fputs(text, file);
	fclose(file);
	return 1;
}

static int emit(const char *directory, const char *stem, int vertex, const char *source)
{
	struct host_shader_translation translation;
	char name[256];
	int index;

	snprintf(name, sizeof(name), "%s.es3.%s", stem, vertex ? "vert" : "frag");
	write_file(directory, name, source);
	if (!host_shader_translate(vertex, source, &translation))
	{
		fprintf(stderr, "%s: translation failed\n", stem);
		return 0;
	}
	snprintf(name, sizeof(name), "%s.es2.%s", stem, vertex ? "vert" : "frag");
	write_file(directory, name, translation.source);
	if (vertex)
	{
		/* attributes must fit in 16 slots and keep their locations */
		for (index = 0; index < translation.attribute_count; index++)
		{
			if (translation.attributes[index].location < 0 || translation.attributes[index].location > 15)
			{
				fprintf(stderr, "%s: attribute %s at location %d\n", stem, translation.attributes[index].name,
					translation.attributes[index].location);
				host_shader_translation_free(&translation);
				return 0;
			}
		}
	}
	host_shader_translation_free(&translation);
	return 1;
}

int main(int argc, char **argv)
{
	const char *directory;
	long count = 200, index;
	int failures = 0;

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <output directory> [count] [seed]\n", argv[0]);
		return 2;
	}
	directory = argv[1];
	if (argc > 2)
		count = atol(argv[2]);
	random_state = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x5eed;

	/* the host reports OpenGL ES 3.0 (host_gl.c) */
	xgpu_capabilities.shading_language = "300 es";

	for (index = 0; index < count; index++)
	{
		DWORD instructions[4 * 24];
		unsigned long packed = (index % 3) == 0 ? random_word() & 0xffff : 0;
		char stem[64];
		char *source = NULL;
		int attempt;

		/* random words also name temporaries r13-r15, which the NV2A does
		not have (the generator declares r0-r11 and oPos); draw again */
		for (attempt = 0; attempt < 1000; attempt++)
		{
			unsigned long instruction_count = 1 + random_word() % 24, word;

			for (word = 0; word < instruction_count * 4; word++)
				instructions[word] = (DWORD)random_word();
			/* the last instruction ends the program; programs address the
			192 constant registers only */
			for (word = 0; word < instruction_count; word++)
			{
				DWORD constant = (instructions[word * 4 + 1] >> 13) & 0xff;

				instructions[word * 4 + 1] = (instructions[word * 4 + 1] & ~((DWORD)0xff << 13)) |
					((constant % XGPU_VERTEX_CONSTANT_COUNT) << 13);
				instructions[word * 4 + 3] &= ~(DWORD)1;
			}
			instructions[(instruction_count - 1) * 4 + 3] |= 1;
			free(source);
			source = nv2a_vertex_shader_to_glsl(instructions, instruction_count, packed);
			if (!strstr(source, "r13") && !strstr(source, "r14") && !strstr(source, "r15"))
				break;
		}
		snprintf(stem, sizeof(stem), "vs%04ld", index);
		failures += !emit(directory, stem, 1, source);
		free(source);
	}

	for (index = 0; index < count; index++)
	{
		struct nv2a_pixel_shader_key key;
		unsigned long word;
		char stem[64];
		char *source;
		int stage;

		memset(&key, 0, sizeof(key));
		for (word = 0; word < sizeof(key.combiner_state) / sizeof(key.combiner_state[0]); word++)
			key.combiner_state[word] = (DWORD)random_word();
		key.combiner_state[D3DRS_PSCOMBINERCOUNT] = (DWORD)((random_word() % 9) | (random_word() & 0x11100));
		if (index % 5 == 0)
		{
			key.combiner_state[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0;
			key.combiner_state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0;
		}
		key.texture_modes = (DWORD)random_word();
		for (stage = 0; stage < 4; stage++)
		{
			key.sampler_type[stage] = (unsigned char)(random_word() % 4);
			key.alpha_kill[stage] = (unsigned char)(random_word() % 2);
			key.color_sign[stage] = (unsigned char)(random_word() % 16);
		}
		key.alpha_test_function = random_word() % 9;
		key.fog_enable = (unsigned char)(random_word() % 2);
		key.fog_table_mode = (unsigned char)(random_word() % 4);
		source = nv2a_pixel_shader_to_glsl(&key);
		snprintf(stem, sizeof(stem), "ps%04ld", index);
		failures += !emit(directory, stem, 0, source);
		free(source);
	}
	printf("%ld vertex and %ld pixel shaders written to %s; %d translation failures\n", count, count, directory,
		failures);
	return failures ? 1 : 0;
}
