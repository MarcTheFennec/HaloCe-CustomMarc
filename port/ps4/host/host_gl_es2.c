/*
HOST_GL_ES2.C

OpenGL ES 3.0, as the guest's renderer (port/linux/src/d3d8_gl.c with
HALO_ANDROID) uses it, over the PS4's OpenGL ES 2.0 (Piglet).

tools/ps4_gl_host.py routes every import named in its EMULATED set here
(es2_<function>); everything else goes to Piglet directly. What differs:

- framebuffers: separate READ/DRAW bindings are tracked; glBlitFramebuffer
  becomes glCopyTexSubImage2D when it copies 1:1 into a texture, and a
  textured quad otherwise (the presentation blit, which scales and flips);
- depth/stencil "textures" (render targets' depth, never sampled) are
  renderbuffers (GL_DEPTH24_STENCIL8_OES), attached to DEPTH and STENCIL;
- sampler objects are stored and applied to the bound textures at draw
  time (ES 2 has texture parameters only), with the ES 2 rules enforced:
  non-power-of-two textures without GL_OES_texture_npot are clamped and not
  mipmapped, and a texture whose mip chain is incomplete is sampled without
  mipmaps rather than as black;
- textures: sized internal formats become unsized; the R/B swizzle the
  renderer sets on textures converted to BGRA is done by uploading them as
  GL_BGRA_EXT (or swapping bytes); a chain that stops before 1x1 (ES 3's
  GL_TEXTURE_MAX_LEVEL) is completed by box-filtering the last level on the
  CPU; 3D textures keep their first slice;
- buffers bound to ES 3 targets (the renderer's COPY_WRITE mirror) are
  updated through GL_ARRAY_BUFFER;
- NORMPACKED3 attributes (glVertexAttribIPointer, one 32-bit integer) are
  fed as four unsigned bytes that host_shader.c's shader code reassembles;
- shaders are translated to GLSL ES 1.00 (host_shader.c) and their
  attribute locations bound before linking;
- queries use GL_EXT_occlusion_query_boolean when the driver has it and
  report "visible" otherwise; vertex arrays objects are a no-op (one
  implicit VAO, which is all the renderer uses).
*/

#include "host_gl.h"
#include "host_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_UNITS 16
#define MAXIMUM_ATTRIBUTES 16

/* ---------- a small map of GL names */

struct map_entry
{
	GLuint key;
	void *value;
};

struct map
{
	struct map_entry *entries;
	size_t capacity;
	size_t count;
};

static size_t map_slot(const struct map *map, GLuint key)
{
	return (size_t)(key * 2654435761u) & (map->capacity - 1);
}

static void *map_get(const struct map *map, GLuint key)
{
	size_t slot;

	if (!map->capacity || !key)
		return NULL;
	for (slot = map_slot(map, key);; slot = (slot + 1) & (map->capacity - 1))
	{
		if (map->entries[slot].key == key)
			return map->entries[slot].value;
		if (!map->entries[slot].key)
			return NULL;
	}
}

static void map_put(struct map *map, GLuint key, void *value);

static void map_grow(struct map *map)
{
	struct map old = *map;
	size_t index;

	map->capacity = old.capacity ? old.capacity * 2 : 256;
	map->entries = calloc(map->capacity, sizeof(*map->entries));
	map->count = 0;
	for (index = 0; index < old.capacity; index++)
	{
		if (old.entries[index].key && old.entries[index].value)
			map_put(map, old.entries[index].key, old.entries[index].value);
	}
	free(old.entries);
}

static void map_put(struct map *map, GLuint key, void *value)
{
	size_t slot;

	if (!key)
		return;
	if ((map->count + 1) * 2 > map->capacity)
		map_grow(map);
	for (slot = map_slot(map, key);; slot = (slot + 1) & (map->capacity - 1))
	{
		if (map->entries[slot].key == key)
		{
			map->entries[slot].value = value;
			return;
		}
		if (!map->entries[slot].key)
		{
			map->entries[slot].key = key;
			map->entries[slot].value = value;
			map->count++;
			return;
		}
	}
}

/* removal leaves the key with no value (a tombstone), so probing still
works; map_grow drops them */
static void *map_take(struct map *map, GLuint key)
{
	size_t slot;

	if (!map->capacity || !key)
		return NULL;
	for (slot = map_slot(map, key);; slot = (slot + 1) & (map->capacity - 1))
	{
		if (map->entries[slot].key == key)
		{
			void *value = map->entries[slot].value;

			map->entries[slot].value = NULL;
			return value;
		}
		if (!map->entries[slot].key)
			return NULL;
	}
}

/* ---------- state */

struct sampling
{
	GLint min_filter, mag_filter, wrap_s, wrap_t;
	GLfloat anisotropy;
};

struct texture
{
	GLuint name;
	GLenum target;          /* GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP */
	GLsizei width, height;  /* level 0 */
	uint32_t level_mask[6]; /* levels defined, per cube face */
	GLint base_level, max_level;
	int swap_red_blue;      /* ES 3 swizzle R<->B requested */
	int uploaded_bgra;      /* the storage is GL_BGRA_EXT */
	int depth;              /* a renderbuffer instead */
	GLuint renderbuffer;
	struct sampling own;    /* glTexParameter */
	struct sampling applied; /* what Piglet has now */
	int applied_valid;
};

struct framebuffer
{
	GLuint color;
	GLint color_level;
	int color_virtual;     /* level > 0 without GL_OES_fbo_render_mipmap */
	GLuint depth;
};

struct sampler
{
	struct sampling sampling;
};

struct attribute
{
	int enabled;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;
	const void *pointer;
	GLuint buffer;
};

struct shader_info
{
	int vertex;
	int attribute_count;
	struct host_shader_attribute attributes[HOST_SHADER_MAXIMUM_ATTRIBUTES];
};

static struct
{
	struct map textures, framebuffers, shaders, programs;
	struct sampler *samplers;
	GLuint sampler_count, sampler_capacity;
	GLuint next_vertex_array;
	GLuint next_query;

	GLuint active_unit;
	GLuint unit_2d[MAXIMUM_UNITS], unit_cube[MAXIMUM_UNITS], unit_sampler[MAXIMUM_UNITS];
	uint32_t bound_units;

	GLuint draw_framebuffer, read_framebuffer;
	GLuint array_buffer, element_buffer;
	struct { GLenum target; GLuint buffer; } other_buffers[12];

	GLuint program;
	struct attribute attributes[MAXIMUM_ATTRIBUTES];
	int enabled_blend, enabled_cull, enabled_depth, enabled_scissor, enabled_stencil, enabled_offset;

	GLint unpack_alignment;

	/* the presentation/scaling blit */
	GLuint blit_program;
	GLint blit_sampler_location;
	GLuint scratch_framebuffer;

	/* GL_EXT_occlusion_query_boolean */
	void (GL_APIENTRY *gen_queries)(GLsizei, GLuint *);
	void (GL_APIENTRY *begin_query)(GLenum, GLuint);
	void (GL_APIENTRY *end_query)(GLenum);
	void (GL_APIENTRY *get_query_uiv)(GLuint, GLenum, GLuint *);
	void (GL_APIENTRY *discard_framebuffer)(GLenum, GLsizei, const GLenum *);
} es2;

static struct
{
	unsigned long blits_drawn, blits_copied, samplings_applied, shaders_translated, links, link_failures;
	unsigned long cpu_levels, swapped_uploads, depth_renderbuffers, incomplete_samplings, npot_clamps;
} statistics;

static void warn_once(int *flag, const char *format, const char *detail)
{
	if (*flag)
		return;
	*flag = 1;
	host_logf(HOST_LOG_WARN, format, detail);
}

void es2_initialize(void)
{
	GLint units = 0;

	memset(&es2, 0, sizeof(es2));
	es2.next_vertex_array = 1;
	es2.next_query = 1;
	es2.unpack_alignment = 4;
	if (host_gl_capabilities.occlusion_query)
	{
		es2.gen_queries = (void (GL_APIENTRY *)(GLsizei, GLuint *))host_gl_proc("glGenQueriesEXT");
		es2.begin_query = (void (GL_APIENTRY *)(GLenum, GLuint))host_gl_proc("glBeginQueryEXT");
		es2.end_query = (void (GL_APIENTRY *)(GLenum))host_gl_proc("glEndQueryEXT");
		es2.get_query_uiv = (void (GL_APIENTRY *)(GLuint, GLenum, GLuint *))host_gl_proc("glGetQueryObjectuivEXT");
		if (!es2.gen_queries || !es2.begin_query || !es2.end_query || !es2.get_query_uiv)
		{
			host_logf(HOST_LOG_WARN, "gl: GL_EXT_occlusion_query_boolean is listed but its functions are missing");
			es2.gen_queries = NULL;
		}
	}
	if (host_gl_capabilities.discard_framebuffer)
		es2.discard_framebuffer = (void (GL_APIENTRY *)(GLenum, GLsizei, const GLenum *))
			host_gl_proc("glDiscardFramebufferEXT");
	hostreal_glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
	host_logf(HOST_LOG_INFO, "gl: ES 3 emulation ready (queries %s)",
		es2.gen_queries ? "GL_EXT_occlusion_query_boolean" : "always visible");
}

void es2_log_statistics(void)
{
	host_logf(HOST_LOG_INFO,
		"gl: blits %lu drawn / %lu copied; %lu sampler applications (%lu without mipmaps: incomplete, %lu clamped: npot); "
		"%lu shaders translated, %lu programs linked (%lu failed); %lu CPU mip levels, %lu byte-swapped uploads, "
		"%lu depth renderbuffers",
		statistics.blits_drawn, statistics.blits_copied, statistics.samplings_applied, statistics.incomplete_samplings,
		statistics.npot_clamps, statistics.shaders_translated, statistics.links, statistics.link_failures,
		statistics.cpu_levels, statistics.swapped_uploads, statistics.depth_renderbuffers);
}

/* ---------- textures */

static int power_of_two(GLsizei value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

static int levels_for(GLsizei width, GLsizei height)
{
	GLsizei largest = width > height ? width : height;
	int levels = 1;

	while (largest > 1)
	{
		largest >>= 1;
		levels++;
	}
	return levels;
}

static struct texture *texture_get(GLuint name)
{
	return map_get(&es2.textures, name);
}

static struct texture *texture_bound(GLenum target)
{
	GLuint unit = es2.active_unit < MAXIMUM_UNITS ? es2.active_unit : 0;

	if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
		return texture_get(es2.unit_cube[unit]);
	return texture_get(es2.unit_2d[unit]);
}

static int face_of(GLenum target)
{
	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
		return (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
	return 0;
}

static GLenum image_target(GLenum target)
{
	return target == ES3_TEXTURE_3D || target == ES3_TEXTURE_2D_ARRAY ? GL_TEXTURE_2D : target;
}

static int mip_complete(const struct texture *texture)
{
	uint32_t wanted;
	int faces = texture->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, face, levels;

	if (!texture->width || !texture->height)
		return 0;
	levels = levels_for(texture->width, texture->height);
	wanted = levels >= 32 ? 0xffffffffu : (1u << levels) - 1;
	for (face = 0; face < faces; face++)
	{
		if ((texture->level_mask[face] & wanted) != wanted)
			return 0;
	}
	return 1;
}

static void sampling_default(struct sampling *sampling)
{
	sampling->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	sampling->mag_filter = GL_LINEAR;
	sampling->wrap_s = GL_REPEAT;
	sampling->wrap_t = GL_REPEAT;
	sampling->anisotropy = 1.0f;
}

void es2_glGenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	hostreal_glGenTextures(count, names);
	for (index = 0; index < count; index++)
	{
		struct texture *texture = calloc(1, sizeof(*texture));

		texture->name = names[index];
		texture->target = GL_TEXTURE_2D;
		texture->max_level = 1000;
		sampling_default(&texture->own);
		map_put(&es2.textures, names[index], texture);
	}
}

void es2_glDeleteTextures(GLsizei count, const GLuint *names)
{
	GLsizei index;
	int unit;

	for (index = 0; index < count; index++)
	{
		struct texture *texture = map_take(&es2.textures, names[index]);

		if (texture)
		{
			if (texture->renderbuffer)
				hostreal_glDeleteRenderbuffers(1, &texture->renderbuffer);
			free(texture);
		}
		for (unit = 0; unit < MAXIMUM_UNITS; unit++)
		{
			if (es2.unit_2d[unit] == names[index])
				es2.unit_2d[unit] = 0;
			if (es2.unit_cube[unit] == names[index])
				es2.unit_cube[unit] = 0;
		}
	}
	hostreal_glDeleteTextures(count, names);
}

void es2_glActiveTexture(GLenum unit)
{
	es2.active_unit = unit - GL_TEXTURE0;
	hostreal_glActiveTexture(unit);
}

void es2_glBindTexture(GLenum target, GLuint name)
{
	GLuint unit = es2.active_unit < MAXIMUM_UNITS ? es2.active_unit : 0;
	struct texture *texture;

	target = image_target(target);
	if (!name)
	{
		/* textures the guest never generated are created on first bind */
	}
	else if (!(texture = texture_get(name)))
	{
		texture = calloc(1, sizeof(*texture));
		texture->name = name;
		texture->max_level = 1000;
		sampling_default(&texture->own);
		texture->target = target;
		map_put(&es2.textures, name, texture);
	}
	else
	{
		texture->target = target;
	}
	if (target == GL_TEXTURE_CUBE_MAP)
		es2.unit_cube[unit] = name;
	else
		es2.unit_2d[unit] = name;
	if (es2.unit_2d[unit] || es2.unit_cube[unit])
		es2.bound_units |= 1u << unit;
	else
		es2.bound_units &= ~(1u << unit);
	hostreal_glBindTexture(target, name);
}

static void texture_parameter(GLenum target, GLenum name, GLint value, GLfloat value_float, int is_float)
{
	struct texture *texture = texture_bound(target);

	if (!texture)
		return;
	switch (name)
	{
	case GL_TEXTURE_MIN_FILTER: texture->own.min_filter = is_float ? (GLint)value_float : value; break;
	case GL_TEXTURE_MAG_FILTER: texture->own.mag_filter = is_float ? (GLint)value_float : value; break;
	case GL_TEXTURE_WRAP_S: texture->own.wrap_s = is_float ? (GLint)value_float : value; break;
	case GL_TEXTURE_WRAP_T: texture->own.wrap_t = is_float ? (GLint)value_float : value; break;
	case ES3_TEXTURE_MAX_ANISOTROPY: texture->own.anisotropy = is_float ? value_float : (GLfloat)value; break;
	case ES3_TEXTURE_BASE_LEVEL: texture->base_level = is_float ? (GLint)value_float : value; break;
	case ES3_TEXTURE_MAX_LEVEL: texture->max_level = is_float ? (GLint)value_float : value; break;
	case ES3_TEXTURE_SWIZZLE_R: texture->swap_red_blue = (is_float ? (GLint)value_float : value) == ES3_BLUE; break;
	default:
		/* WRAP_R, LOD range and bias, swizzles G/B/A, compare mode:
		ES 2 has none (the pixel shader applies the LOD bias) */
		break;
	}
}

void es2_glTexParameteri(GLenum target, GLenum name, GLint value)
{
	texture_parameter(target, name, value, 0.0f, 0);
}

void es2_glTexParameteriv(GLenum target, GLenum name, const GLint *values)
{
	if (values)
		texture_parameter(target, name, values[0], 0.0f, 0);
}

void es2_glTexParameterf(GLenum target, GLenum name, GLfloat value)
{
	texture_parameter(target, name, 0, value, 1);
}

void es2_glTexParameterfv(GLenum target, GLenum name, const GLfloat *values)
{
	if (values && name != ES3_TEXTURE_BORDER_COLOR)
		texture_parameter(target, name, 0, values[0], 1);
}

void es2_glPixelStorei(GLenum name, GLint value)
{
	if (name == GL_UNPACK_ALIGNMENT)
		es2.unpack_alignment = value;
	if (name == GL_UNPACK_ALIGNMENT || name == GL_PACK_ALIGNMENT)
		hostreal_glPixelStorei(name, value);
	else if (value != 0)
	{
		static int warned;

		warn_once(&warned, "gl: pixel store parameter %s is not emulated", "(ES 3 row/skip)");
	}
}

/* the row pitch of client pixel data at the current alignment */
static size_t row_bytes(GLsizei width, size_t pixel_bytes)
{
	size_t alignment = es2.unpack_alignment > 0 ? (size_t)es2.unpack_alignment : 4;
	size_t bytes = (size_t)width * pixel_bytes;

	return (bytes + alignment - 1) / alignment * alignment;
}

/* RGBA8 pixels with red and blue exchanged, tightly packed */
static unsigned char *swapped_copy(const void *pixels, GLsizei width, GLsizei height)
{
	size_t pitch = row_bytes(width, 4);
	unsigned char *copy = malloc((size_t)width * (size_t)height * 4);
	GLsizei x, y;

	if (!copy)
		return NULL;
	for (y = 0; y < height; y++)
	{
		const unsigned char *source = (const unsigned char *)pixels + (size_t)y * pitch;
		unsigned char *destination = copy + (size_t)y * (size_t)width * 4;

		for (x = 0; x < width; x++)
		{
			destination[x * 4 + 0] = source[x * 4 + 2];
			destination[x * 4 + 1] = source[x * 4 + 1];
			destination[x * 4 + 2] = source[x * 4 + 0];
			destination[x * 4 + 3] = source[x * 4 + 3];
		}
	}
	statistics.swapped_uploads++;
	return copy;
}

/* the next mip level of tightly packed RGBA8 pixels, box filtered */
static unsigned char *downsample(const unsigned char *pixels, size_t pitch, GLsizei width, GLsizei height,
	GLsizei *next_width, GLsizei *next_height)
{
	GLsizei w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1, x, y;
	unsigned char *result = malloc((size_t)w * (size_t)h * 4);
	int channel;

	if (!result)
		return NULL;
	for (y = 0; y < h; y++)
	{
		GLsizei y0 = height > 1 ? y * 2 : 0, y1 = height > 1 ? y * 2 + 1 : 0;

		for (x = 0; x < w; x++)
		{
			GLsizei x0 = width > 1 ? x * 2 : 0, x1 = width > 1 ? x * 2 + 1 : 0;

			for (channel = 0; channel < 4; channel++)
			{
				unsigned sum = pixels[(size_t)y0 * pitch + (size_t)x0 * 4 + channel] +
					pixels[(size_t)y0 * pitch + (size_t)x1 * 4 + channel] +
					pixels[(size_t)y1 * pitch + (size_t)x0 * 4 + channel] +
					pixels[(size_t)y1 * pitch + (size_t)x1 * 4 + channel];

				result[((size_t)y * (size_t)w + (size_t)x) * 4 + channel] = (unsigned char)((sum + 2) / 4);
			}
		}
	}
	*next_width = w;
	*next_height = h;
	return result;
}

static void depth_storage(struct texture *texture, GLsizei width, GLsizei height)
{
	GLenum format = host_gl_capabilities.packed_depth_stencil ? GL_DEPTH24_STENCIL8_OES :
		host_gl_capabilities.depth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;

	texture->depth = 1;
	texture->width = width;
	texture->height = height;
	if (!texture->renderbuffer)
	{
		hostreal_glGenRenderbuffers(1, &texture->renderbuffer);
		statistics.depth_renderbuffers++;
	}
	hostreal_glBindRenderbuffer(GL_RENDERBUFFER, texture->renderbuffer);
	hostreal_glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
	hostreal_glBindRenderbuffer(GL_RENDERBUFFER, 0);
	if (!host_gl_capabilities.packed_depth_stencil)
	{
		static int warned;

		warn_once(&warned, "gl: no packed depth/stencil (%s); render targets have no stencil",
			"GL_OES_packed_depth_stencil");
	}
}

void es2_glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const void *pixels)
{
	struct texture *texture = texture_bound(target);
	unsigned char *converted = NULL;
	const unsigned char *data = pixels;
	GLenum upload_format = format;
	int face = face_of(target);

	(void)border;
	target = image_target(target);
	if (internal_format == ES3_DEPTH24_STENCIL8 || format == ES3_DEPTH_STENCIL || format == GL_DEPTH_COMPONENT)
	{
		if (texture)
			depth_storage(texture, width, height);
		return;
	}
	if (format == ES3_BGRA_EXT && !host_gl_capabilities.bgra && data)
	{
		/* desktop-style BGRA data */
		converted = swapped_copy(data, width, height);
		data = converted;
		upload_format = GL_RGBA;
	}
	else if (texture && texture->swap_red_blue && format == GL_RGBA && type == GL_UNSIGNED_BYTE)
	{
		if (host_gl_capabilities.bgra)
			upload_format = ES3_BGRA_EXT;
		else if (data)
		{
			converted = swapped_copy(data, width, height);
			data = converted;
		}
	}
	if (texture)
	{
		texture->uploaded_bgra = upload_format == ES3_BGRA_EXT;
		if (level == 0)
		{
			texture->width = width;
			texture->height = height;
			if (target == GL_TEXTURE_2D)
				memset(texture->level_mask, 0, sizeof(texture->level_mask));
		}
		if (level < 32)
			texture->level_mask[face] |= 1u << level;
	}
	/* ES 2: the internal format is the format */
	if (converted)
		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	hostreal_glTexImage2D(target, level, (GLint)upload_format, width, height, 0, upload_format, type, data);

	/* the chain ends here (GL_TEXTURE_MAX_LEVEL) before 1x1: ES 2 samples
	such a texture as black, so complete it */
	if (texture && data && type == GL_UNSIGNED_BYTE && (format == GL_RGBA || format == ES3_BGRA_EXT) &&
		level == texture->max_level && (width > 1 || height > 1) && level < 30 &&
		(host_gl_capabilities.npot || (power_of_two(texture->width) && power_of_two(texture->height))))
	{
		const unsigned char *source = data;
		size_t pitch = converted ? (size_t)width * 4 : row_bytes(width, 4);
		unsigned char *previous = NULL;
		GLsizei w = width, h = height;
		GLint next = level;

		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		while (w > 1 || h > 1)
		{
			GLsizei nw, nh;
			unsigned char *smaller = downsample(source, pitch, w, h, &nw, &nh);

			if (!smaller)
				break;
			next++;
			hostreal_glTexImage2D(target, next, (GLint)upload_format, nw, nh, 0, upload_format, type, smaller);
			if (next < 32)
				texture->level_mask[face] |= 1u << next;
			statistics.cpu_levels++;
			free(previous);
			previous = smaller;
			source = smaller;
			pitch = (size_t)nw * 4;
			w = nw;
			h = nh;
		}
		free(previous);
		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, es2.unpack_alignment);
	}
	else if (converted)
	{
		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, es2.unpack_alignment);
	}
	free(converted);
}

void es2_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
	GLenum format, GLenum type, const void *pixels)
{
	struct texture *texture = texture_bound(target);
	unsigned char *converted = NULL;
	const void *data = pixels;
	GLenum upload_format = format;

	target = image_target(target);
	if (texture && format == GL_RGBA && type == GL_UNSIGNED_BYTE && (texture->swap_red_blue || format == ES3_BGRA_EXT))
	{
		if (texture->uploaded_bgra)
			upload_format = ES3_BGRA_EXT;
		else if (data)
		{
			converted = swapped_copy(data, width, height);
			data = converted;
		}
	}
	if (converted)
		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	hostreal_glTexSubImage2D(target, level, x, y, width, height, upload_format, type, data);
	if (converted)
		hostreal_glPixelStorei(GL_UNPACK_ALIGNMENT, es2.unpack_alignment);
	free(converted);
}

static GLsizei compressed_block_bytes(GLenum format)
{
	return format == ES3_COMPRESSED_RGBA_S3TC_DXT1_EXT || format == ES3_COMPRESSED_RGB_S3TC_DXT1_EXT ? 8 : 16;
}

void es2_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal_format, GLsizei width, GLsizei height,
	GLint border, GLsizei size, const void *data)
{
	struct texture *texture = texture_bound(target);
	int face = face_of(target);

	(void)border;
	target = image_target(target);
	if (texture)
	{
		if (level == 0)
		{
			texture->width = width;
			texture->height = height;
			if (target == GL_TEXTURE_2D)
				memset(texture->level_mask, 0, sizeof(texture->level_mask));
		}
		if (level < 32)
			texture->level_mask[face] |= 1u << level;
	}
	hostreal_glCompressedTexImage2D(target, level, internal_format, width, height, 0, size, data);
	/* complete a chain that stops early with the last level's first block */
	if (texture && data && level == texture->max_level && (width > 1 || height > 1) && level < 30 &&
		size >= compressed_block_bytes(internal_format) &&
		(host_gl_capabilities.npot || (power_of_two(texture->width) && power_of_two(texture->height))))
	{
		GLsizei w = width, h = height, block = compressed_block_bytes(internal_format);
		GLint next = level;

		/* each smaller level is the last level's first block, repeated */
		while (w > 1 || h > 1)
		{
			GLsizei blocks, index;
			unsigned char *filler;

			w = w > 1 ? w / 2 : 1;
			h = h > 1 ? h / 2 : 1;
			next++;
			blocks = ((w + 3) / 4) * ((h + 3) / 4);
			filler = malloc((size_t)blocks * (size_t)block);
			if (!filler)
				break;
			for (index = 0; index < blocks; index++)
				memcpy(filler + (size_t)index * (size_t)block, data, (size_t)block);
			hostreal_glCompressedTexImage2D(target, next, internal_format, w, h, 0, blocks * block, filler);
			free(filler);
			if (next < 32)
				texture->level_mask[face] |= 1u << next;
			statistics.cpu_levels++;
		}
	}
}

void es2_glTexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	static int warned;

	(void)depth;
	warn_once(&warned, "gl: %s: 3D textures keep their first slice (ES 2)", "glTexImage3D");
	es2_glTexImage2D(image_target(target), level, internal_format, width, height, border, format, type, pixels);
}

void es2_glCompressedTexImage3D(GLenum target, GLint level, GLenum internal_format, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLsizei size, const void *data)
{
	static int warned;

	warn_once(&warned, "gl: %s: 3D textures keep their first slice (ES 2)", "glCompressedTexImage3D");
	es2_glCompressedTexImage2D(image_target(target), level, internal_format, width, height, border,
		depth > 0 ? size / depth : size, data);
}

void es2_glGenerateMipmap(GLenum target)
{
	struct texture *texture = texture_bound(target);
	static int warned_base;

	target = image_target(target);
	if (texture && texture->base_level > 0)
	{
		/* levels below a rendered base (d3d8_gl.c mip composites): ES 2
		can only generate from level 0 */
		warn_once(&warned_base, "gl: %s from a base level above 0 is skipped (ES 2)", "glGenerateMipmap");
		return;
	}
	if (texture && !host_gl_capabilities.npot && (!power_of_two(texture->width) || !power_of_two(texture->height)))
		return;
	hostreal_glGenerateMipmap(target);
	if (texture && texture->width && texture->height)
	{
		int levels = levels_for(texture->width, texture->height), face;
		uint32_t mask = levels >= 32 ? 0xffffffffu : (1u << levels) - 1;

		for (face = 0; face < 6; face++)
			texture->level_mask[face] |= mask;
	}
}

/* ---------- samplers, applied at draw time */

void es2_glGenSamplers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		if (es2.sampler_count + 1 >= es2.sampler_capacity)
		{
			es2.sampler_capacity = es2.sampler_capacity ? es2.sampler_capacity * 2 : 64;
			es2.samplers = realloc(es2.samplers, es2.sampler_capacity * sizeof(*es2.samplers));
		}
		es2.sampler_count++;
		sampling_default(&es2.samplers[es2.sampler_count].sampling);
		names[index] = es2.sampler_count;
	}
}

static struct sampler *sampler_get(GLuint name)
{
	return name && name <= es2.sampler_count ? &es2.samplers[name] : NULL;
}

void es2_glBindSampler(GLuint unit, GLuint sampler)
{
	if (unit < MAXIMUM_UNITS)
		es2.unit_sampler[unit] = sampler;
}

void es2_glSamplerParameteri(GLuint name, GLenum parameter, GLint value)
{
	struct sampler *sampler = sampler_get(name);

	if (!sampler)
		return;
	switch (parameter)
	{
	case GL_TEXTURE_MIN_FILTER: sampler->sampling.min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER: sampler->sampling.mag_filter = value; break;
	case GL_TEXTURE_WRAP_S: sampler->sampling.wrap_s = value == ES3_CLAMP_TO_BORDER ? GL_CLAMP_TO_EDGE : value; break;
	case GL_TEXTURE_WRAP_T: sampler->sampling.wrap_t = value == ES3_CLAMP_TO_BORDER ? GL_CLAMP_TO_EDGE : value; break;
	case ES3_TEXTURE_MAX_ANISOTROPY: sampler->sampling.anisotropy = (GLfloat)value; break;
	default: break;
	}
}

void es2_glSamplerParameterf(GLuint name, GLenum parameter, GLfloat value)
{
	struct sampler *sampler = sampler_get(name);

	if (!sampler)
		return;
	if (parameter == ES3_TEXTURE_MAX_ANISOTROPY)
		sampler->sampling.anisotropy = value;
	else if (parameter != ES3_TEXTURE_MIN_LOD && parameter != ES3_TEXTURE_MAX_LOD && parameter != ES3_TEXTURE_LOD_BIAS)
		es2_glSamplerParameteri(name, parameter, (GLint)value);
}

void es2_glSamplerParameterfv(GLuint name, GLenum parameter, const GLfloat *values)
{
	if (values && parameter != ES3_TEXTURE_BORDER_COLOR)
		es2_glSamplerParameterf(name, parameter, values[0]);
}

static GLint without_mipmaps(GLint filter)
{
	switch (filter)
	{
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return GL_NEAREST;
	case GL_LINEAR_MIPMAP_NEAREST:
	case GL_LINEAR_MIPMAP_LINEAR:
		return GL_LINEAR;
	default:
		return filter;
	}
}

static void apply_texture(GLuint unit, struct texture *texture, const struct sampling *requested)
{
	struct sampling wanted = *requested;
	GLenum target = texture->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

	if (texture->depth)
		return;
	if (!host_gl_capabilities.npot && (!power_of_two(texture->width) || !power_of_two(texture->height)))
	{
		if (wanted.wrap_s != GL_CLAMP_TO_EDGE || wanted.wrap_t != GL_CLAMP_TO_EDGE ||
			wanted.min_filter != without_mipmaps(wanted.min_filter))
		{
			statistics.npot_clamps++;
		}
		wanted.wrap_s = GL_CLAMP_TO_EDGE;
		wanted.wrap_t = GL_CLAMP_TO_EDGE;
		wanted.min_filter = without_mipmaps(wanted.min_filter);
	}
	if (wanted.min_filter != without_mipmaps(wanted.min_filter) && !mip_complete(texture))
	{
		wanted.min_filter = without_mipmaps(wanted.min_filter);
		statistics.incomplete_samplings++;
	}
	if (!host_gl_capabilities.anisotropy)
		wanted.anisotropy = 1.0f;
	if (texture->applied_valid && !memcmp(&wanted, &texture->applied, sizeof(wanted)))
		return;
	if (es2.active_unit != unit)
		hostreal_glActiveTexture(GL_TEXTURE0 + unit);
	if (!texture->applied_valid || texture->applied.min_filter != wanted.min_filter)
		hostreal_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, wanted.min_filter);
	if (!texture->applied_valid || texture->applied.mag_filter != wanted.mag_filter)
		hostreal_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, wanted.mag_filter);
	if (!texture->applied_valid || texture->applied.wrap_s != wanted.wrap_s)
		hostreal_glTexParameteri(target, GL_TEXTURE_WRAP_S, wanted.wrap_s);
	if (!texture->applied_valid || texture->applied.wrap_t != wanted.wrap_t)
		hostreal_glTexParameteri(target, GL_TEXTURE_WRAP_T, wanted.wrap_t);
	if (host_gl_capabilities.anisotropy && (!texture->applied_valid || texture->applied.anisotropy != wanted.anisotropy))
		hostreal_glTexParameterf(target, ES3_TEXTURE_MAX_ANISOTROPY, wanted.anisotropy);
	if (es2.active_unit != unit)
		hostreal_glActiveTexture(GL_TEXTURE0 + es2.active_unit);
	texture->applied = wanted;
	texture->applied_valid = 1;
	statistics.samplings_applied++;
}

static void apply_samplers(void)
{
	uint32_t units = es2.bound_units;
	GLuint unit;

	for (unit = 0; units && unit < MAXIMUM_UNITS; unit++, units >>= 1)
	{
		struct sampler *sampler;
		struct texture *texture;

		if (!(units & 1))
			continue;
		sampler = sampler_get(es2.unit_sampler[unit]);
		if ((texture = texture_get(es2.unit_2d[unit])) != NULL)
			apply_texture(unit, texture, sampler ? &sampler->sampling : &texture->own);
		if ((texture = texture_get(es2.unit_cube[unit])) != NULL)
			apply_texture(unit, texture, sampler ? &sampler->sampling : &texture->own);
	}
}

/* ---------- framebuffers */

static struct framebuffer *framebuffer_get(GLuint name, int create)
{
	struct framebuffer *framebuffer = map_get(&es2.framebuffers, name);

	if (!framebuffer && create && name)
	{
		framebuffer = calloc(1, sizeof(*framebuffer));
		map_put(&es2.framebuffers, name, framebuffer);
	}
	return framebuffer;
}

void es2_glDeleteFramebuffers(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		free(map_take(&es2.framebuffers, names[index]));
		if (es2.draw_framebuffer == names[index])
			es2.draw_framebuffer = 0;
		if (es2.read_framebuffer == names[index])
			es2.read_framebuffer = 0;
	}
	hostreal_glDeleteFramebuffers(count, names);
}

void es2_glBindFramebuffer(GLenum target, GLuint name)
{
	if (target == ES3_READ_FRAMEBUFFER)
	{
		es2.read_framebuffer = name;
		return;
	}
	if (target == GL_FRAMEBUFFER)
		es2.read_framebuffer = name;
	es2.draw_framebuffer = name;
	hostreal_glBindFramebuffer(GL_FRAMEBUFFER, name);
}

/* runs with the framebuffer of target bound for real */
static GLuint framebuffer_select(GLenum target)
{
	GLuint name = target == ES3_READ_FRAMEBUFFER ? es2.read_framebuffer : es2.draw_framebuffer;

	if (name != es2.draw_framebuffer)
		hostreal_glBindFramebuffer(GL_FRAMEBUFFER, name);
	return name;
}

static void framebuffer_restore(GLuint selected)
{
	if (selected != es2.draw_framebuffer)
		hostreal_glBindFramebuffer(GL_FRAMEBUFFER, es2.draw_framebuffer);
}

void es2_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum texture_target, GLuint name, GLint level)
{
	GLuint selected = framebuffer_select(target);
	struct framebuffer *framebuffer = framebuffer_get(selected, 1);
	struct texture *texture = texture_get(name);

	if (attachment == ES3_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT ||
		attachment == GL_STENCIL_ATTACHMENT)
	{
		if (framebuffer)
			framebuffer->depth = name;
		if (texture && texture->depth)
		{
			if (attachment != GL_STENCIL_ATTACHMENT)
				hostreal_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
					texture->renderbuffer);
			if (attachment != GL_DEPTH_ATTACHMENT && host_gl_capabilities.packed_depth_stencil)
				hostreal_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
					texture->renderbuffer);
		}
		else if (!name)
		{
			if (attachment != GL_STENCIL_ATTACHMENT)
				hostreal_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
			if (attachment != GL_DEPTH_ATTACHMENT)
				hostreal_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
		}
		else
		{
			hostreal_glFramebufferTexture2D(GL_FRAMEBUFFER, attachment == ES3_DEPTH_STENCIL_ATTACHMENT ?
				GL_DEPTH_ATTACHMENT : attachment, image_target(texture_target), name, level);
		}
		framebuffer_restore(selected);
		return;
	}
	if (framebuffer && attachment == GL_COLOR_ATTACHMENT0)
	{
		framebuffer->color = name;
		framebuffer->color_level = level;
		framebuffer->color_virtual = level != 0 && !host_gl_capabilities.fbo_render_mipmap;
	}
	if (level != 0 && !host_gl_capabilities.fbo_render_mipmap)
	{
		/* only glBlitFramebuffer writes to it (copy_level_by_blit), which
		becomes glCopyTexSubImage2D into the level */
		framebuffer_restore(selected);
		return;
	}
	hostreal_glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, image_target(texture_target), name, level);
	framebuffer_restore(selected);
}

GLenum es2_glCheckFramebufferStatus(GLenum target)
{
	GLuint selected = framebuffer_select(target);
	struct framebuffer *framebuffer = framebuffer_get(selected, 0);
	GLenum status;

	if (framebuffer && framebuffer->color_virtual)
		status = GL_FRAMEBUFFER_COMPLETE;
	else
		status = hostreal_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		host_logf(HOST_LOG_WARN, "gl: framebuffer %u is incomplete: 0x%04x (colour %u, depth %u)", selected,
			(unsigned)status, framebuffer ? framebuffer->color : 0, framebuffer ? framebuffer->depth : 0);
	framebuffer_restore(selected);
	return status;
}

void es2_glDrawBuffers(GLsizei count, const GLenum *buffers)
{
	(void)count;
	(void)buffers;
}

void es2_glReadBuffer(GLenum buffer)
{
	(void)buffer;
}

void es2_glInvalidateFramebuffer(GLenum target, GLsizei count, const GLenum *attachments)
{
	GLenum converted[4];
	GLsizei index, used = 0;
	GLuint selected;

	if (!es2.discard_framebuffer || target == ES3_READ_FRAMEBUFFER)
		return;
	for (index = 0; index < count && used < 3; index++)
	{
		if (attachments[index] == ES3_DEPTH_STENCIL_ATTACHMENT)
		{
			converted[used++] = GL_DEPTH_ATTACHMENT;
			converted[used++] = GL_STENCIL_ATTACHMENT;
		}
		else
		{
			converted[used++] = attachments[index];
		}
	}
	selected = framebuffer_select(target);
	es2.discard_framebuffer(GL_FRAMEBUFFER, used, converted);
	framebuffer_restore(selected);
}

void es2_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	GLuint selected = framebuffer_select(ES3_READ_FRAMEBUFFER);

	hostreal_glReadPixels(x, y, width, height, format == ES3_BGRA_EXT ? GL_RGBA : format, type, pixels);
	framebuffer_restore(selected);
}

/* ---------- blits */

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = hostreal_glCreateShader(type);
	GLint status = 0;

	hostreal_glShaderSource(shader, 1, &source, NULL);
	hostreal_glCompileShader(shader);
	hostreal_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024] = "";

		hostreal_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		host_logf(HOST_LOG_ERROR, "gl: the blit shader does not compile: %s", log);
		hostreal_glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int blit_program_create(void)
{
	static const char vertex[] =
		"attribute vec2 position;\n"
		"attribute vec2 coordinates;\n"
		"varying vec2 uv;\n"
		"void main()\n"
		"{\n"
		"\tuv = coordinates;\n"
		"\tgl_Position = vec4(position, 0.0, 1.0);\n"
		"}\n";
	static const char fragment[] =
		"precision mediump float;\n"
		"uniform sampler2D image;\n"
		"varying vec2 uv;\n"
		"void main()\n"
		"{\n"
		"\tgl_FragColor = texture2D(image, uv);\n"
		"}\n";
	GLuint vs, fs;
	GLint status = 0;

	if (es2.blit_program)
		return 1;
	vs = compile(GL_VERTEX_SHADER, vertex);
	fs = compile(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs)
		return 0;
	es2.blit_program = hostreal_glCreateProgram();
	hostreal_glAttachShader(es2.blit_program, vs);
	hostreal_glAttachShader(es2.blit_program, fs);
	hostreal_glBindAttribLocation(es2.blit_program, 0, "position");
	hostreal_glBindAttribLocation(es2.blit_program, 1, "coordinates");
	hostreal_glLinkProgram(es2.blit_program);
	hostreal_glDeleteShader(vs);
	hostreal_glDeleteShader(fs);
	hostreal_glGetProgramiv(es2.blit_program, GL_LINK_STATUS, &status);
	if (!status)
	{
		host_logf(HOST_LOG_ERROR, "gl: the blit program does not link");
		hostreal_glDeleteProgram(es2.blit_program);
		es2.blit_program = 0;
		return 0;
	}
	es2.blit_sampler_location = hostreal_glGetUniformLocation(es2.blit_program, "image");
	return 1;
}

static void restore_attribute(GLuint index)
{
	const struct attribute *attribute = &es2.attributes[index];

	if (attribute->size)
	{
		hostreal_glBindBuffer(GL_ARRAY_BUFFER, attribute->buffer);
		hostreal_glVertexAttribPointer(index, attribute->size, attribute->type, attribute->normalized,
			attribute->stride, attribute->pointer);
	}
	if (attribute->enabled)
		hostreal_glEnableVertexAttribArray(index);
	else
		hostreal_glDisableVertexAttribArray(index);
}

static void set_enabled(GLenum capability, int enabled)
{
	if (enabled)
		hostreal_glEnable(capability);
	else
		hostreal_glDisable(capability);
}

/* draws the source texture's rectangle into the draw framebuffer's */
static void blit_draw(struct texture *source, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
	GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter)
{
	GLfloat u0 = (GLfloat)sx0 / (GLfloat)source->width, u1 = (GLfloat)sx1 / (GLfloat)source->width;
	GLfloat v0 = (GLfloat)sy0 / (GLfloat)source->height, v1 = (GLfloat)sy1 / (GLfloat)source->height;
	GLfloat positions[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	GLfloat coordinates[8];
	GLint viewport[4];
	GLboolean color_mask[4];
	GLint x = dx0 < dx1 ? dx0 : dx1, y = dy0 < dy1 ? dy0 : dy1;
	GLsizei width = dx0 < dx1 ? dx1 - dx0 : dx0 - dx1, height = dy0 < dy1 ? dy1 - dy0 : dy0 - dy1;

	if (!blit_program_create() || !source->width || !source->height)
		return;
	/* a mirrored destination rectangle mirrors the picture */
	if (dx1 < dx0)
	{
		GLfloat swap = u0;

		u0 = u1;
		u1 = swap;
	}
	if (dy1 < dy0)
	{
		GLfloat swap = v0;

		v0 = v1;
		v1 = swap;
	}
	coordinates[0] = u0; coordinates[1] = v0;
	coordinates[2] = u1; coordinates[3] = v0;
	coordinates[4] = u0; coordinates[5] = v1;
	coordinates[6] = u1; coordinates[7] = v1;

	hostreal_glGetIntegerv(GL_VIEWPORT, viewport);
	hostreal_glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	hostreal_glDisable(GL_BLEND);
	hostreal_glDisable(GL_CULL_FACE);
	hostreal_glDisable(GL_DEPTH_TEST);
	hostreal_glDisable(GL_SCISSOR_TEST);
	hostreal_glDisable(GL_STENCIL_TEST);
	hostreal_glDisable(GL_POLYGON_OFFSET_FILL);
	hostreal_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	hostreal_glViewport(x, y, width, height);
	hostreal_glUseProgram(es2.blit_program);
	hostreal_glUniform1i(es2.blit_sampler_location, 0);
	hostreal_glActiveTexture(GL_TEXTURE0);
	hostreal_glBindTexture(GL_TEXTURE_2D, source->name);
	hostreal_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
	hostreal_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
	hostreal_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	hostreal_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	source->applied.min_filter = (GLint)filter;
	source->applied.mag_filter = (GLint)filter;
	source->applied.wrap_s = GL_CLAMP_TO_EDGE;
	source->applied.wrap_t = GL_CLAMP_TO_EDGE;
	if (!source->applied_valid)
		source->applied.anisotropy = 1.0f;
	source->applied_valid = 1;
	hostreal_glBindBuffer(GL_ARRAY_BUFFER, 0);
	hostreal_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
	hostreal_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
	hostreal_glEnableVertexAttribArray(0);
	hostreal_glEnableVertexAttribArray(1);
	hostreal_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* everything back as the guest left it */
	restore_attribute(0);
	restore_attribute(1);
	hostreal_glBindBuffer(GL_ARRAY_BUFFER, es2.array_buffer);
	hostreal_glBindTexture(GL_TEXTURE_2D, es2.unit_2d[0]);
	hostreal_glActiveTexture(GL_TEXTURE0 + es2.active_unit);
	hostreal_glUseProgram(es2.program);
	hostreal_glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	hostreal_glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	set_enabled(GL_BLEND, es2.enabled_blend);
	set_enabled(GL_CULL_FACE, es2.enabled_cull);
	set_enabled(GL_DEPTH_TEST, es2.enabled_depth);
	set_enabled(GL_SCISSOR_TEST, es2.enabled_scissor);
	set_enabled(GL_STENCIL_TEST, es2.enabled_stencil);
	set_enabled(GL_POLYGON_OFFSET_FILL, es2.enabled_offset);
	statistics.blits_drawn++;
}

/* copies a rectangle of the framebuffer `from` into a texture level */
static void copy_to_texture(GLuint from, GLuint texture, GLint level, GLint dx, GLint dy, GLint sx, GLint sy,
	GLsizei width, GLsizei height)
{
	if (from != es2.draw_framebuffer)
		hostreal_glBindFramebuffer(GL_FRAMEBUFFER, from);
	hostreal_glActiveTexture(GL_TEXTURE0);
	hostreal_glBindTexture(GL_TEXTURE_2D, texture);
	hostreal_glCopyTexSubImage2D(GL_TEXTURE_2D, level, dx, dy, sx, sy, width, height);
	hostreal_glBindTexture(GL_TEXTURE_2D, es2.unit_2d[0]);
	hostreal_glActiveTexture(GL_TEXTURE0 + es2.active_unit);
	if (from != es2.draw_framebuffer)
		hostreal_glBindFramebuffer(GL_FRAMEBUFFER, es2.draw_framebuffer);
	statistics.blits_copied++;
}

void es2_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
	GLbitfield mask, GLenum filter)
{
	struct framebuffer *read = framebuffer_get(es2.read_framebuffer, 0);
	struct framebuffer *draw = framebuffer_get(es2.draw_framebuffer, 0);
	struct texture *source = read ? texture_get(read->color) : NULL;
	static int warned_source, warned_mask;

	if (!(mask & GL_COLOR_BUFFER_BIT))
	{
		warn_once(&warned_mask, "gl: %s of depth/stencil is not emulated", "glBlitFramebuffer");
		return;
	}
	if (!source || read->color_level != 0)
	{
		warn_once(&warned_source, "gl: %s from a framebuffer without a colour texture is not emulated",
			"glBlitFramebuffer");
		return;
	}
	/* 1:1 into a texture: a copy, which also reaches mip levels ES 2 cannot
	render to */
	if (draw && draw->color && sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0)
	{
		copy_to_texture(es2.read_framebuffer, draw->color, draw->color_level, dx0, dy0, sx0, sy0, sx1 - sx0, sy1 - sy0);
		return;
	}
	if (draw && draw->color_virtual)
	{
		warn_once(&warned_source, "gl: %s scaling into a mip level is not emulated", "glBlitFramebuffer");
		return;
	}
	blit_draw(source, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filter == GL_LINEAR ? GL_LINEAR : GL_NEAREST);
}

void es2_glCopyImageSubData(GLuint source, GLenum source_target, GLint source_level, GLint sx, GLint sy, GLint sz,
	GLuint destination, GLenum destination_target, GLint destination_level, GLint dx, GLint dy, GLint dz,
	GLsizei width, GLsizei height, GLsizei depth)
{
	static int warned;

	(void)sz; (void)dz; (void)depth; (void)destination_target;
	if (source_level != 0 || image_target(source_target) != GL_TEXTURE_2D)
	{
		warn_once(&warned, "gl: %s from a mip level or a non-2D texture is not emulated", "glCopyImageSubData");
		return;
	}
	if (!es2.scratch_framebuffer)
		hostreal_glGenFramebuffers(1, &es2.scratch_framebuffer);
	hostreal_glBindFramebuffer(GL_FRAMEBUFFER, es2.scratch_framebuffer);
	hostreal_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
	hostreal_glActiveTexture(GL_TEXTURE0);
	hostreal_glBindTexture(GL_TEXTURE_2D, destination);
	hostreal_glCopyTexSubImage2D(GL_TEXTURE_2D, destination_level, dx, dy, sx, sy, width, height);
	hostreal_glBindTexture(GL_TEXTURE_2D, es2.unit_2d[0]);
	hostreal_glActiveTexture(GL_TEXTURE0 + es2.active_unit);
	hostreal_glBindFramebuffer(GL_FRAMEBUFFER, es2.draw_framebuffer);
	statistics.blits_copied++;
}

/* ---------- buffers */

static int real_buffer_target(GLenum target)
{
	return target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
}

static GLuint *other_binding(GLenum target, int create)
{
	size_t index;

	for (index = 0; index < sizeof(es2.other_buffers) / sizeof(es2.other_buffers[0]); index++)
	{
		if (es2.other_buffers[index].target == target)
			return &es2.other_buffers[index].buffer;
	}
	if (!create)
		return NULL;
	for (index = 0; index < sizeof(es2.other_buffers) / sizeof(es2.other_buffers[0]); index++)
	{
		if (!es2.other_buffers[index].target)
		{
			es2.other_buffers[index].target = target;
			return &es2.other_buffers[index].buffer;
		}
	}
	return NULL;
}

void es2_glBindBuffer(GLenum target, GLuint buffer)
{
	GLuint *binding;

	if (target == GL_ARRAY_BUFFER)
		es2.array_buffer = buffer;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		es2.element_buffer = buffer;
	if (real_buffer_target(target))
	{
		hostreal_glBindBuffer(target, buffer);
		return;
	}
	if ((binding = other_binding(target, 1)) != NULL)
		*binding = buffer;
}

void es2_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	(void)index;
	es2_glBindBuffer(target, buffer);
}

void es2_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	(void)index; (void)offset; (void)size;
	es2_glBindBuffer(target, buffer);
}

void es2_glDeleteBuffers(GLsizei count, const GLuint *buffers)
{
	GLsizei index;
	size_t other;

	for (index = 0; index < count; index++)
	{
		if (es2.array_buffer == buffers[index])
			es2.array_buffer = 0;
		if (es2.element_buffer == buffers[index])
			es2.element_buffer = 0;
		for (other = 0; other < sizeof(es2.other_buffers) / sizeof(es2.other_buffers[0]); other++)
		{
			if (es2.other_buffers[other].buffer == buffers[index])
				es2.other_buffers[other].buffer = 0;
		}
	}
	hostreal_glDeleteBuffers(count, buffers);
}

/* the buffer an ES 3 target names, bound to GL_ARRAY_BUFFER for the call;
0 if none is bound */
static GLuint other_begin(GLenum target)
{
	GLuint *binding = other_binding(target, 0);

	if (!binding || !*binding)
		return 0;
	hostreal_glBindBuffer(GL_ARRAY_BUFFER, *binding);
	return *binding;
}

static void other_end(void)
{
	hostreal_glBindBuffer(GL_ARRAY_BUFFER, es2.array_buffer);
}

void es2_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	if (real_buffer_target(target))
	{
		hostreal_glBufferData(target, size, data, usage == GL_STATIC_DRAW || usage == GL_DYNAMIC_DRAW ||
			usage == GL_STREAM_DRAW ? usage : GL_DYNAMIC_DRAW);
		return;
	}
	if (!other_begin(target))
		return;
	hostreal_glBufferData(GL_ARRAY_BUFFER, size, data, GL_DYNAMIC_DRAW);
	other_end();
}

void es2_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	if (real_buffer_target(target))
	{
		hostreal_glBufferSubData(target, offset, size, data);
		return;
	}
	if (!other_begin(target))
		return;
	hostreal_glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
	other_end();
}

/* ---------- vertex arrays and attributes */

void es2_glGenVertexArrays(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		names[index] = es2.next_vertex_array++;
}

void es2_glBindVertexArray(GLuint name)
{
	(void)name;
}

void es2_glEnableVertexAttribArray(GLuint index)
{
	if (index < MAXIMUM_ATTRIBUTES)
		es2.attributes[index].enabled = 1;
	hostreal_glEnableVertexAttribArray(index);
}

void es2_glDisableVertexAttribArray(GLuint index)
{
	if (index < MAXIMUM_ATTRIBUTES)
		es2.attributes[index].enabled = 0;
	hostreal_glDisableVertexAttribArray(index);
}

static void attribute_record(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
	const void *pointer)
{
	if (index < MAXIMUM_ATTRIBUTES)
	{
		struct attribute *attribute = &es2.attributes[index];

		attribute->size = size;
		attribute->type = type;
		attribute->normalized = normalized;
		attribute->stride = stride;
		attribute->pointer = pointer;
		attribute->buffer = es2.array_buffer;
	}
}

void es2_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
	const void *pointer)
{
	attribute_record(index, size, type, normalized, stride, pointer);
	hostreal_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

/* NORMPACKED3: one 32-bit word, read as four bytes (host_shader.c) */
void es2_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	static int warned;

	if (size != 1 || (type != GL_UNSIGNED_INT && type != GL_INT))
		warn_once(&warned, "gl: %s is emulated for one 32-bit integer only", "glVertexAttribIPointer");
	attribute_record(index, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, pointer);
	hostreal_glVertexAttribPointer(index, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, pointer);
}

void es2_glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
	/* as the four bytes of the packed word the shader expects */
	hostreal_glVertexAttrib4f(index, (GLfloat)(x & 0xff), (GLfloat)(y & 0xff), (GLfloat)(z & 0xff), (GLfloat)(w & 0xff));
}

/* ---------- state */

static int *enable_flag(GLenum capability)
{
	switch (capability)
	{
	case GL_BLEND: return &es2.enabled_blend;
	case GL_CULL_FACE: return &es2.enabled_cull;
	case GL_DEPTH_TEST: return &es2.enabled_depth;
	case GL_SCISSOR_TEST: return &es2.enabled_scissor;
	case GL_STENCIL_TEST: return &es2.enabled_stencil;
	case GL_POLYGON_OFFSET_FILL: return &es2.enabled_offset;
	default: return NULL;
	}
}

static int es2_capability(GLenum capability)
{
	switch (capability)
	{
	case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER: case GL_POLYGON_OFFSET_FILL:
	case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_SCISSOR_TEST: case GL_STENCIL_TEST:
		return 1;
	default:
		return 0;
	}
}

void es2_glEnable(GLenum capability)
{
	int *flag = enable_flag(capability);

	if (flag)
		*flag = 1;
	if (es2_capability(capability))
		hostreal_glEnable(capability);
}

void es2_glDisable(GLenum capability)
{
	int *flag = enable_flag(capability);

	if (flag)
		*flag = 0;
	if (es2_capability(capability))
		hostreal_glDisable(capability);
}

void es2_glGetIntegerv(GLenum name, GLint *values)
{
	switch (name)
	{
	case ES3_MAJOR_VERSION: *values = 3; return;
	case ES3_MINOR_VERSION: *values = 0; return;
	case ES3_MAX_FRAGMENT_ATOMIC_COUNTERS: *values = 0; return;
	case ES3_READ_FRAMEBUFFER_BINDING: *values = (GLint)es2.read_framebuffer; return;
	case GL_FRAMEBUFFER_BINDING: *values = (GLint)es2.draw_framebuffer; return;
	case ES3_NUM_EXTENSIONS:
	{
		char name_buffer[64];
		GLint count = 0;

		for (;;)
		{
			host_gl_get_string(GL_EXTENSIONS, count, name_buffer, sizeof(name_buffer));
			if (!name_buffer[0])
				break;
			count++;
		}
		*values = count;
		return;
	}
	default:
		hostreal_glGetIntegerv(name, values);
		return;
	}
}

/* ---------- queries */

void es2_glGenQueries(GLsizei count, GLuint *names)
{
	GLsizei index;

	if (es2.gen_queries)
	{
		es2.gen_queries(count, names);
		return;
	}
	for (index = 0; index < count; index++)
		names[index] = es2.next_query++;
}

void es2_glBeginQuery(GLenum target, GLuint name)
{
	if (es2.gen_queries)
		es2.begin_query(target == ES3_SAMPLES_PASSED ? ES3_ANY_SAMPLES_PASSED : target, name);
}

void es2_glEndQuery(GLenum target)
{
	if (es2.gen_queries)
		es2.end_query(target == ES3_SAMPLES_PASSED ? ES3_ANY_SAMPLES_PASSED : target);
}

void es2_glGetQueryObjectuiv(GLuint name, GLenum parameter, GLuint *value)
{
	if (es2.gen_queries)
	{
		es2.get_query_uiv(name, parameter, value);
		return;
	}
	/* no occlusion queries: everything tested is visible */
	*value = 1;
}

/* ---------- draws */

void es2_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	apply_samplers();
	hostreal_glDrawArrays(mode, first, count);
}

void es2_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	static int warned;

	if (type == GL_UNSIGNED_INT && !host_gl_capabilities.element_index_uint)
	{
		warn_once(&warned, "gl: 32-bit indices need %s", "GL_OES_element_index_uint");
		return;
	}
	apply_samplers();
	hostreal_glDrawElements(mode, count, type, indices);
}

void es2_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base)
{
	static int warned;

	/* not reported (ES 3.2), so the renderer rebases indices itself */
	if (base)
	{
		warn_once(&warned, "gl: %s with a base vertex is not emulated", "glDrawElementsBaseVertex");
		return;
	}
	es2_glDrawElements(mode, count, type, indices);
}

/* ---------- shaders and programs */

void es2_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct host_shader_translation translation;
	struct shader_info *info;
	size_t total = 0, at = 0;
	GLint type = 0;
	GLsizei index;
	char *source;

	for (index = 0; index < count; index++)
		total += lengths && lengths[index] >= 0 ? (size_t)lengths[index] : strlen(strings[index]);
	source = malloc(total + 1);
	if (!source)
		return;
	for (index = 0; index < count; index++)
	{
		size_t length = lengths && lengths[index] >= 0 ? (size_t)lengths[index] : strlen(strings[index]);

		memcpy(source + at, strings[index], length);
		at += length;
	}
	source[at] = 0;
	hostreal_glGetShaderiv(shader, GL_SHADER_TYPE, &type);

	if (!host_shader_translate(type == GL_VERTEX_SHADER, source, &translation))
	{
		host_logf(HOST_LOG_ERROR, "gl: shader %u cannot be translated to GLSL ES 1.00", shader);
		hostreal_glShaderSource(shader, 1, (const GLchar *const *)&source, NULL);
		free(source);
		return;
	}
	info = map_get(&es2.shaders, shader);
	if (!info)
	{
		info = calloc(1, sizeof(*info));
		map_put(&es2.shaders, shader, info);
	}
	info->vertex = type == GL_VERTEX_SHADER;
	info->attribute_count = translation.attribute_count;
	memcpy(info->attributes, translation.attributes, sizeof(info->attributes));
	if (translation.translated)
		statistics.shaders_translated++;
	hostreal_glShaderSource(shader, 1, (const GLchar *const *)&translation.source, NULL);
	host_shader_translation_free(&translation);
	free(source);
}

void es2_glAttachShader(GLuint program, GLuint shader)
{
	struct shader_info *info = map_get(&es2.shaders, shader);

	if (info && info->vertex)
		map_put(&es2.programs, program, (void *)(uintptr_t)shader);
	hostreal_glAttachShader(program, shader);
}

void es2_glLinkProgram(GLuint program)
{
	GLuint vertex = (GLuint)(uintptr_t)map_get(&es2.programs, program);
	struct shader_info *info = vertex ? map_get(&es2.shaders, vertex) : NULL;
	GLint status = 0;
	int index;

	if (info)
	{
		for (index = 0; index < info->attribute_count; index++)
		{
			const struct host_shader_attribute *attribute = &info->attributes[index];

			if (!attribute->used)
				continue;
			if (attribute->location >= host_gl_capabilities.max_vertex_attributes &&
				host_gl_capabilities.max_vertex_attributes > 0)
			{
				host_logf(HOST_LOG_ERROR, "gl: attribute %s at location %d is past the driver's %d",
					attribute->name, attribute->location, (int)host_gl_capabilities.max_vertex_attributes);
				continue;
			}
			hostreal_glBindAttribLocation(program, (GLuint)attribute->location, attribute->name);
		}
	}
	hostreal_glLinkProgram(program);
	statistics.links++;
	hostreal_glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
		statistics.link_failures++;
}

void es2_glUseProgram(GLuint program)
{
	es2.program = program;
	hostreal_glUseProgram(program);
}
