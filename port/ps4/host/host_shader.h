/*
HOST_SHADER.H

GLSL ES 3.00 -> GLSL ES 1.00 translation for the PS4's OpenGL ES 2
(host_shader.c). Independent of GL so the shader harness can build it.
*/

#ifndef __HALO_PS4_HOST_SHADER_H
#define __HALO_PS4_HOST_SHADER_H

#define HOST_SHADER_MAXIMUM_ATTRIBUTES 16
#define HOST_SHADER_MAXIMUM_SAMPLERS 8

enum
{
	HOST_SHADER_SAMPLER_2D,
	HOST_SHADER_SAMPLER_3D,
	HOST_SHADER_SAMPLER_CUBE,
};

struct host_shader_attribute
{
	char name[32];
	int location;
	/* NORMPACKED3, fed as four unsigned bytes */
	int packed;
	/* read by the shader; unread ones are removed */
	int used;
};

struct host_shader_translation
{
	/* malloc'd GLSL ES 1.00 (or the input, copied, when it needed nothing) */
	char *source;
	int translated;
	int attribute_count;
	struct host_shader_attribute attributes[HOST_SHADER_MAXIMUM_ATTRIBUTES];
	unsigned packed_mask;
	int sampler_count;
	struct
	{
		char name[32];
		int type;
	} samplers[HOST_SHADER_MAXIMUM_SAMPLERS];
};

int host_shader_needs_translation(const char *source);
/* vertex: nonzero for a vertex shader; returns 0 on failure */
int host_shader_translate(int vertex, const char *source, struct host_shader_translation *result);
void host_shader_translation_free(struct host_shader_translation *result);

#endif
