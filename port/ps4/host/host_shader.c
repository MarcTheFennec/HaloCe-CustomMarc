/*
HOST_SHADER.C

Translation of the renderer's GLSL ES 3.00 shaders into GLSL ES 1.00, the
only language the PS4's OpenGL ES 2 (Piglet) compiles.

The shaders come from two generators only: nv2a_vsh.c (vertex programs) and
nv2a_psh.c (pixel shaders), both in port/linux/src, plus the few fixed
shaders of d3d8_gl.c. They use a small, regular subset of ES 3.00, which is
all this handles:

- "#version 300 es" / "310 es"                 -> "#version 100"
- "layout(location = N) in vec4 vN_in;"        -> "attribute vec4 vN_in;"
  (the location is bound before linking, host_gl_es2.c)
- "layout(location = N) in uint vN_packed;"    -> "attribute vec4 vN_packed;"
  (NORMPACKED3: the host feeds the 32-bit word as four unsigned bytes and
  unpack_normpacked3 is rewritten to rebuild the fields arithmetically)
- global "out" (vertex) / "in" (fragment)      -> "varying"
- "layout(location = 0) out vec4 fragment_color;" -> gl_FragColor
- texture(sampler, ...)                        -> texture2D / textureCube
- sampler3D                                    -> sampler2D, sampled at .xy
  (ES 2 has no 3D textures; host_gl_es2.c uploads slice 0)
- c[clamp(a0 + N, 0, M)]                       -> float clamp (no integer
  clamp in 1.00)
- (int(E) & 1) != 0                            -> mod(floor(E), 2.0) >= 0.5
- atomic counter / early_fragment_tests lines  -> dropped (never generated:
  the host reports no ES 3.1)

Attributes a vertex shader never reads are dropped, so a program never needs
more attribute slots than it uses (GL_MAX_VERTEX_ATTRIBS may be 8 or 16).

This file has no dependency on GL or the host, so the shader harness
(tools/ps4_shader_harness.c) builds it natively as is.
*/

#include "host_shader.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- text */

struct text
{
	char *buffer;
	size_t length;
	size_t capacity;
};

static void text_reserve(struct text *text, size_t more)
{
	if (text->length + more + 1 > text->capacity)
	{
		size_t capacity = text->capacity ? text->capacity : 4096;

		while (text->length + more + 1 > capacity)
			capacity *= 2;
		text->buffer = realloc(text->buffer, capacity);
		text->capacity = capacity;
	}
}

static void text_append_length(struct text *text, const char *data, size_t length)
{
	text_reserve(text, length);
	memcpy(text->buffer + text->length, data, length);
	text->length += length;
	text->buffer[text->length] = 0;
}

static void text_append(struct text *text, const char *data)
{
	text_append_length(text, data, strlen(data));
}

static void text_printf(struct text *text, const char *format, ...)
{
	char buffer[1024];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(buffer, sizeof(buffer), format, arguments);
	va_end(arguments);
	if (length > 0)
		text_append_length(text, buffer, (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1);
}

static int is_identifier_character(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* occurrences of word as a whole identifier */
static int count_word(const char *text, const char *word)
{
	size_t length = strlen(word);
	const char *at = text;
	int count = 0;

	while ((at = strstr(at, word)) != NULL)
	{
		if ((at == text || !is_identifier_character(at[-1])) && !is_identifier_character(at[length]))
			count++;
		at += length;
	}
	return count;
}

/* the position just past the parenthesis that closes the one at open */
static const char *matching_parenthesis(const char *open)
{
	int depth = 0;
	const char *at;

	for (at = open; *at; at++)
	{
		if (*at == '(')
			depth++;
		else if (*at == ')' && --depth == 0)
			return at + 1;
	}
	return NULL;
}

static const char *skip_spaces(const char *at)
{
	while (*at == ' ' || *at == '\t')
		at++;
	return at;
}

/* ---------- declarations */

static const char normpacked3_es2[] =
	"vec4 unpack_normpacked3(vec4 b)\n"
	"{\n"
	"\tb = floor(b + 0.5);\n"
	"\tfloat x = b.x + mod(b.y, 8.0) * 256.0;\n"
	"\tfloat y = floor(b.y / 8.0) + mod(b.z, 64.0) * 32.0;\n"
	"\tfloat z = floor(b.z / 64.0) + b.w * 4.0;\n"
	"\tif (x >= 1024.0) x -= 2048.0;\n"
	"\tif (y >= 1024.0) y -= 2048.0;\n"
	"\tif (z >= 512.0) z -= 1024.0;\n"
	"\treturn vec4(x / 1023.0, y / 1023.0, z / 511.0, 1.0);\n"
	"}\n";

static void sampler_record(struct host_shader_translation *result, const char *name, int type)
{
	if (result->sampler_count < HOST_SHADER_MAXIMUM_SAMPLERS)
	{
		snprintf(result->samplers[result->sampler_count].name, sizeof(result->samplers[0].name), "%s", name);
		result->samplers[result->sampler_count].type = type;
		result->sampler_count++;
	}
}

static int sampler_type(const struct host_shader_translation *result, const char *name, size_t length)
{
	int index;

	for (index = 0; index < result->sampler_count; index++)
	{
		if (strlen(result->samplers[index].name) == length && !strncmp(result->samplers[index].name, name, length))
			return result->samplers[index].type;
	}
	return HOST_SHADER_SAMPLER_2D;
}

/* one line of the declarations and statements, rewritten; returns 0 when the
line is dropped */
static int translate_line(const char *line, size_t length, int vertex, struct host_shader_translation *result,
	struct text *output, int *in_packed_function)
{
	char copy[1024];
	char type[64], name[64];
	unsigned location;

	if (length >= sizeof(copy))
	{
		text_append_length(output, line, length);
		text_append(output, "\n");
		return 1;
	}
	memcpy(copy, line, length);
	copy[length] = 0;

	/* the packed attribute decoder: replaced as a whole */
	if (*in_packed_function)
	{
		if (!strcmp(copy, "}"))
			*in_packed_function = 0;
		return 0;
	}
	if (!strncmp(copy, "vec4 unpack_normpacked3(uint", 28))
	{
		*in_packed_function = 1;
		text_append(output, normpacked3_es2);
		return 1;
	}

	if (!strncmp(copy, "#version", 8))
	{
		text_append(output, "#version 100\n");
		return 1;
	}
	if (!strcmp(copy, "precision highp sampler3D;") || !strncmp(copy, "layout(early_fragment_tests)", 28) ||
		strstr(copy, "atomic_uint") || strstr(copy, "atomicCounter"))
	{
		return 0;
	}
	if (sscanf(copy, "layout(location = %u) in %63s %63[A-Za-z0-9_];", &location, type, name) == 3)
	{
		if (vertex && result->attribute_count < HOST_SHADER_MAXIMUM_ATTRIBUTES)
		{
			struct host_shader_attribute *attribute = &result->attributes[result->attribute_count++];

			snprintf(attribute->name, sizeof(attribute->name), "%s", name);
			attribute->location = (int)location;
			attribute->packed = !strcmp(type, "uint");
			attribute->used = 1;
			if (attribute->packed)
				result->packed_mask |= 1u << (location & 31);
		}
		text_printf(output, "attribute %s %s;\n", !strcmp(type, "uint") ? "vec4" : type, name);
		return 1;
	}
	if (sscanf(copy, "layout(location = %u) out %63s %63[A-Za-z0-9_];", &location, type, name) == 3)
	{
		/* the one colour output */
		text_printf(output, "#define %s gl_FragColor\n", name);
		return 1;
	}
	if (!strncmp(copy, "out ", 4) && vertex)
	{
		text_printf(output, "varying %s\n", copy + 4);
		return 1;
	}
	if (!strncmp(copy, "in ", 3) && !vertex)
	{
		text_printf(output, "varying %s\n", copy + 3);
		return 1;
	}
	if (sscanf(copy, "uniform %63s %63[A-Za-z0-9_];", type, name) == 2 && !strncmp(type, "sampler", 7))
	{
		int kind = !strcmp(type, "samplerCube") ? HOST_SHADER_SAMPLER_CUBE :
			!strcmp(type, "sampler3D") ? HOST_SHADER_SAMPLER_3D : HOST_SHADER_SAMPLER_2D;

		sampler_record(result, name, kind);
		text_printf(output, "uniform %s %s;\n", kind == HOST_SHADER_SAMPLER_CUBE ? "samplerCube" : "sampler2D", name);
		return 1;
	}
	text_append_length(output, line, length);
	text_append(output, "\n");
	return 1;
}

/* ---------- expressions */

/* texture(s, coordinates[, bias]) -> texture2D/textureCube; the sampler's
type comes from its declaration */
static void rewrite_texture_calls(const char *input, struct text *output, const struct host_shader_translation *result)
{
	const char *at = input;

	for (;;)
	{
		const char *found = strstr(at, "texture(");
		const char *close, *name, *name_end, *comma;
		int kind;

		if (!found)
			break;
		if (found != input && is_identifier_character(found[-1]))
		{
			text_append_length(output, at, (size_t)(found + 8 - at));
			at = found + 8;
			continue;
		}
		close = matching_parenthesis(found + 7);
		if (!close)
			break;
		name = skip_spaces(found + 8);
		name_end = name;
		while (is_identifier_character(*name_end))
			name_end++;
		kind = sampler_type(result, name, (size_t)(name_end - name));
		comma = skip_spaces(name_end);
		text_append_length(output, at, (size_t)(found - at));
		if (kind == HOST_SHADER_SAMPLER_3D && *comma == ',')
		{
			/* the coordinates argument: up to the next top-level comma */
			const char *argument = comma + 1, *end;
			int depth = 0;

			for (end = argument; end < close - 1; end++)
			{
				if (*end == '(')
					depth++;
				else if (*end == ')')
					depth--;
				else if (*end == ',' && depth == 0)
					break;
			}
			text_append(output, "texture2D(");
			text_append_length(output, name, (size_t)(name_end - name));
			text_append(output, ", (");
			{
				/* the generator writes (coordinates).xyz; sample (coordinates).xyz.xy */
				struct text inner = { 0 };

				text_append_length(&inner, argument, (size_t)(end - argument));
				rewrite_texture_calls(inner.buffer, output, result);
				free(inner.buffer);
			}
			text_append(output, ").xy");
			{
				struct text rest = { 0 };

				text_append_length(&rest, end, (size_t)(close - end));
				rewrite_texture_calls(rest.buffer, output, result);
				free(rest.buffer);
			}
		}
		else
		{
			struct text inner = { 0 };

			text_append(output, kind == HOST_SHADER_SAMPLER_CUBE ? "textureCube(" : "texture2D(");
			text_append_length(&inner, found + 8, (size_t)(close - (found + 8)));
			rewrite_texture_calls(inner.buffer, output, result);
			free(inner.buffer);
		}
		at = close;
	}
	text_append(output, at);
}

/* clamp(a0 + N, 0, M) -> int(clamp(float(a0 + N), 0.0, M.0)) */
static void rewrite_relative_addressing(const char *input, struct text *output)
{
	const char *at = input;

	for (;;)
	{
		const char *found = strstr(at, "clamp(a0 + ");
		unsigned long offset, maximum;
		int consumed = 0;

		if (!found)
			break;
		text_append_length(output, at, (size_t)(found - at));
		if (sscanf(found, "clamp(a0 + %lu, 0, %lu)%n", &offset, &maximum, &consumed) == 2 && consumed > 0)
		{
			text_printf(output, "int(clamp(float(a0 + %lu), 0.0, %lu.0))", offset, maximum);
			at = found + consumed;
		}
		else
		{
			text_append_length(output, found, 6);
			at = found + 6;
		}
	}
	text_append(output, at);
}

/* (int(E) & 1) != 0 -> (mod(floor(E), 2.0) >= 0.5) */
static void rewrite_parity_tests(const char *input, struct text *output)
{
	const char *at = input;

	for (;;)
	{
		const char *found = strstr(at, "(int(");
		const char *close, *tail;

		if (!found)
			break;
		close = matching_parenthesis(found + 4);
		tail = close ? skip_spaces(close) : NULL;
		text_append_length(output, at, (size_t)(found - at));
		if (tail && !strncmp(tail, "& 1) != 0", 9))
		{
			text_append(output, "(mod(floor(");
			text_append_length(output, found + 5, (size_t)(close - 1 - (found + 5)));
			text_append(output, "), 2.0) >= 0.5)");
			at = tail + 9;
		}
		else
		{
			text_append_length(output, found, 1);
			at = found + 1;
		}
	}
	text_append(output, at);
}

/* attributes the shader never reads: their declaration goes, and the local
copy is initialised to the default attribute value instead */
static char *drop_unused_attributes(char *source, struct host_shader_translation *result)
{
	int index;

	for (index = 0; index < result->attribute_count; index++)
	{
		struct host_shader_attribute *attribute = &result->attributes[index];
		char local[64], declaration[160], copy[200], replacement[200];
		char *found;
		size_t name_length = strlen(attribute->name);

		/* vN_in / vN_packed -> vN */
		if (name_length > 3 && !strcmp(attribute->name + name_length - 3, "_in"))
			snprintf(local, sizeof(local), "%.*s", (int)(name_length - 3), attribute->name);
		else if (name_length > 7 && !strcmp(attribute->name + name_length - 7, "_packed"))
			snprintf(local, sizeof(local), "%.*s", (int)(name_length - 7), attribute->name);
		else
			continue;
		/* the local's own declaration is its one use when unread */
		if (count_word(source, local) > 1)
			continue;
		attribute->used = 0;
		snprintf(declaration, sizeof(declaration), "attribute vec4 %s;\n", attribute->name);
		found = strstr(source, declaration);
		if (found)
			memmove(found, found + strlen(declaration), strlen(found + strlen(declaration)) + 1);
		if (attribute->packed)
			snprintf(copy, sizeof(copy), "vec4 %s = unpack_normpacked3(%s);", local, attribute->name);
		else
			snprintf(copy, sizeof(copy), "vec4 %s = %s;", local, attribute->name);
		snprintf(replacement, sizeof(replacement), "vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);", local);
		found = strstr(source, copy);
		if (found)
		{
			size_t before = (size_t)(found - source);
			size_t after_length = strlen(found + strlen(copy));
			char *rebuilt = malloc(before + strlen(replacement) + after_length + 1);

			memcpy(rebuilt, source, before);
			memcpy(rebuilt + before, replacement, strlen(replacement));
			memcpy(rebuilt + before + strlen(replacement), found + strlen(copy), after_length + 1);
			free(source);
			source = rebuilt;
		}
	}
	return source;
}

/* ---------- interface */

int host_shader_needs_translation(const char *source)
{
	const char *at = skip_spaces(source);

	while (*at == '\n' || *at == '\r' || *at == ' ' || *at == '\t')
		at++;
	return !strncmp(at, "#version 300 es", 15) || !strncmp(at, "#version 310 es", 15) ||
		!strncmp(at, "#version 320 es", 15);
}

int host_shader_translate(int vertex, const char *source, struct host_shader_translation *result)
{
	struct text declarations = { 0 }, pass1 = { 0 }, pass2 = { 0 }, pass3 = { 0 };
	const char *line = source;
	int in_packed_function = 0;

	memset(result, 0, sizeof(*result));
	if (!host_shader_needs_translation(source))
	{
		result->source = strdup(source);
		return 1;
	}
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);

		if (length && line[length - 1] == '\r')
			translate_line(line, length - 1, vertex, result, &declarations, &in_packed_function);
		else
			translate_line(line, length, vertex, result, &declarations, &in_packed_function);
		line += length + (end ? 1 : 0);
	}
	if (!declarations.buffer)
		return 0;
	rewrite_texture_calls(declarations.buffer, &pass1, result);
	rewrite_relative_addressing(pass1.buffer, &pass2);
	rewrite_parity_tests(pass2.buffer, &pass3);
	free(declarations.buffer);
	free(pass1.buffer);
	free(pass2.buffer);
	result->source = vertex ? drop_unused_attributes(pass3.buffer, result) : pass3.buffer;
	result->translated = 1;
	return result->source != NULL;
}

void host_shader_translation_free(struct host_shader_translation *result)
{
	free(result->source);
	result->source = NULL;
}
