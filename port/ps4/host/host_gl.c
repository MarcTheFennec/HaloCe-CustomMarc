/*
HOST_GL.C

OpenGL ES for the guest on the PS4: the context's capabilities, the strings
the guest reads (host_gl_get_string) and the buffer helpers of guest_host.h.

The guest's renderer (port/linux/src/d3d8_gl.c, built with HALO_ANDROID) is
written for OpenGL ES 3.0. Piglet is OpenGL ES 2.0; host_gl_es2.c emulates
the difference, so the guest is told it has ES 3.0 with GLSL ES 3.00, and
only the extensions the emulation passes through are reported.
*/

#include "host_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_gl_capabilities host_gl_capabilities;

static char *driver_extensions;
static int gl_ready;

/* the extensions the guest may use (d3d8_gl.c gl_initialize): the emulation
reports ES 3.0, so it must not claim copy_image or border clamp, which
it does not emulate */
static const char *const guest_extensions[] =
{
	"GL_EXT_texture_compression_s3tc",
	"GL_EXT_texture_compression_dxt1",
	"GL_ANGLE_texture_compression_dxt3",
	"GL_ANGLE_texture_compression_dxt5",
	"GL_EXT_texture_filter_anisotropic",
};

void *host_gl_proc(const char *name)
{
	return host_platform_gl_proc(name);
}

int host_gl_driver_has_extension(const char *name)
{
	size_t length = strlen(name);
	const char *at = driver_extensions;

	if (!at)
		return 0;
	while ((at = strstr(at, name)) != NULL)
	{
		if ((at == driver_extensions || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0))
			return 1;
		at += length;
	}
	return 0;
}

static int guest_extension_count(void)
{
	int count = 0;
	size_t index;

	for (index = 0; index < sizeof(guest_extensions) / sizeof(guest_extensions[0]); index++)
		count += host_gl_driver_has_extension(guest_extensions[index]);
	return count;
}

static const char *guest_extension(int wanted)
{
	int count = 0;
	size_t index;

	for (index = 0; index < sizeof(guest_extensions) / sizeof(guest_extensions[0]); index++)
	{
		if (host_gl_driver_has_extension(guest_extensions[index]) && count++ == wanted)
			return guest_extensions[index];
	}
	return NULL;
}

int host_gl_has_extension(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(guest_extensions) / sizeof(guest_extensions[0]); index++)
	{
		if (!strcmp(guest_extensions[index], name))
			return host_gl_driver_has_extension(name);
	}
	return 0;
}

int host_gl_initialize(void)
{
	struct host_gl_capabilities *caps = &host_gl_capabilities;
	const char *extensions;
	int missing;

	missing = host_gl_load_real(host_gl_proc);
	if (!hostreal_glGetString || !hostreal_glGetIntegerv)
	{
		host_logf(HOST_LOG_ERROR, "gl: the driver has no glGetString; no rendering");
		return -1;
	}
	extensions = (const char *)hostreal_glGetString(GL_EXTENSIONS);
	driver_extensions = strdup(extensions ? extensions : "");
	host_logf(HOST_LOG_INFO, "gl: %s / %s / %s / %s", (const char *)hostreal_glGetString(GL_VENDOR),
		(const char *)hostreal_glGetString(GL_RENDERER), (const char *)hostreal_glGetString(GL_VERSION),
		(const char *)hostreal_glGetString(GL_SHADING_LANGUAGE_VERSION));
	host_logf(HOST_LOG_INFO, "gl: extensions: %s", driver_extensions);
	if (missing)
		host_logf(HOST_LOG_WARN, "gl: %d OpenGL ES 2 functions are missing", missing);

	caps->npot = host_gl_driver_has_extension("GL_OES_texture_npot") ||
		host_gl_driver_has_extension("GL_ARB_texture_non_power_of_two");
	caps->bgra = host_gl_driver_has_extension("GL_EXT_texture_format_BGRA8888");
	caps->packed_depth_stencil = host_gl_driver_has_extension("GL_OES_packed_depth_stencil");
	caps->depth24 = host_gl_driver_has_extension("GL_OES_depth24");
	caps->anisotropy = host_gl_driver_has_extension("GL_EXT_texture_filter_anisotropic");
	caps->s3tc = host_gl_driver_has_extension("GL_EXT_texture_compression_s3tc") ||
		(host_gl_driver_has_extension("GL_EXT_texture_compression_dxt1") &&
		host_gl_driver_has_extension("GL_ANGLE_texture_compression_dxt3") &&
		host_gl_driver_has_extension("GL_ANGLE_texture_compression_dxt5"));
	caps->occlusion_query = host_gl_driver_has_extension("GL_EXT_occlusion_query_boolean");
	caps->fbo_render_mipmap = host_gl_driver_has_extension("GL_OES_fbo_render_mipmap");
	caps->discard_framebuffer = host_gl_driver_has_extension("GL_EXT_discard_framebuffer");
	caps->element_index_uint = host_gl_driver_has_extension("GL_OES_element_index_uint");
	hostreal_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &caps->max_vertex_attributes);
	hostreal_glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &caps->max_vertex_uniform_vectors);
	hostreal_glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &caps->max_fragment_uniform_vectors);
	hostreal_glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &caps->max_texture_units);
	hostreal_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps->max_texture_size);
	host_logf(HOST_LOG_INFO,
		"gl: npot %d, BGRA %d, packed depth/stencil %d, depth24 %d, anisotropy %d, S3TC %d, occlusion queries %d, "
		"fbo mipmaps %d; %d attributes, %d vertex / %d fragment uniform vectors, %d texture units, textures up to %d",
		caps->npot, caps->bgra, caps->packed_depth_stencil, caps->depth24, caps->anisotropy, caps->s3tc,
		caps->occlusion_query, caps->fbo_render_mipmap, (int)caps->max_vertex_attributes,
		(int)caps->max_vertex_uniform_vectors, (int)caps->max_fragment_uniform_vectors, (int)caps->max_texture_units,
		(int)caps->max_texture_size);
	/* the vertex programs' 192 constants and 4 more uniforms */
	if (caps->max_vertex_uniform_vectors && caps->max_vertex_uniform_vectors < 196)
		host_logf(HOST_LOG_ERROR, "gl: only %d vertex uniform vectors; the game's vertex programs need 196 "
			"and will fail to link", (int)caps->max_vertex_uniform_vectors);
	if (!caps->s3tc)
		host_logf(HOST_LOG_WARN, "gl: no S3TC; compressed textures are decoded by the CPU (slower loads)");
	es2_initialize();
	gl_ready = 1;
	return 0;
}

void host_gl_missing(const char *name)
{
	static char reported[64][48];
	static int count;
	int index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(reported[index], name))
			return;
	}
	if (count < 64)
		snprintf(reported[count++], sizeof(reported[0]), "%s", name);
	host_logf(HOST_LOG_ERROR, "gl: the guest called %s, which the driver lacks", name);
}

void host_gl_frame_end(void)
{
	static uint64_t frames;
	static unsigned errors_reported;
	GLenum error;

	if (!gl_ready)
		return;
	frames++;
	/* errors the frame left, a few times (glGetError is cheap but not free) */
	if ((frames & 63) == 0 && errors_reported < 32)
	{
		while ((error = hostreal_glGetError()) != GL_NO_ERROR && errors_reported < 32)
		{
			errors_reported++;
			host_logf(HOST_LOG_WARN, "gl: error 0x%04x during frame %llu", (unsigned)error,
				(unsigned long long)frames);
		}
	}
	if (frames % 3600 == 0)
		es2_log_statistics();
}

/* ---------- the guest's imports (guest_host.h) */

static void copy_string(char *buffer, uint32_t size, const char *text)
{
	if (!buffer || !size)
		return;
	snprintf(buffer, size, "%s", text ? text : "");
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	if (!gl_ready)
	{
		copy_string(buffer, size, "");
		return;
	}
	if (index >= 0)
	{
		copy_string(buffer, size, name == GL_EXTENSIONS ? guest_extension(index) : NULL);
		return;
	}
	switch (name)
	{
	case GL_VERSION:
		copy_string(buffer, size, "OpenGL ES 3.0 (PS4 Piglet OpenGL ES 2 with emulation)");
		break;
	case GL_SHADING_LANGUAGE_VERSION:
		copy_string(buffer, size, "OpenGL ES GLSL ES 3.00");
		break;
	case GL_EXTENSIONS:
	{
		char list[1024] = "";
		int count = guest_extension_count(), item;

		for (item = 0; item < count; item++)
		{
			if (item)
				strncat(list, " ", sizeof(list) - strlen(list) - 1);
			strncat(list, guest_extension(item), sizeof(list) - strlen(list) - 1);
		}
		copy_string(buffer, size, list);
		break;
	}
	default:
		copy_string(buffer, size, (const char *)hostreal_glGetString(name));
		break;
	}
}

/* only with atomic counters, which the emulation does not report */
uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	(void)buffer;
	(void)offset;
	return 0;
}

void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	es2_glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
}

/* ES 2 has no fences; the driver orders buffer updates against the draws
that read them (glBufferSubData) */
void host_gl_fence_frame(uint32_t slot)
{
	(void)slot;
}

void host_gl_wait_frame(uint32_t slot)
{
	(void)slot;
}
