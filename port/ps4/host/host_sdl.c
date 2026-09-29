/*
HOST_SDL.C

The SDL3 functions the guest's platform layer uses (through
port/android/guest/runtime/guest_sdl.c), implemented on the platform
interface of host.h: there is no SDL on the PS4. SDL's headers supply the
types and constants only (SDL_Event, SDL_GamepadButton, SDL_AudioSpec), so
what the guest reads has SDL's layout and meaning.

- window and context: the display (1920x1080 or 1280x720) and the EGL
  context (host_orbis.c); the guest's handles are always 1;
- events: SDL_EVENT_GAMEPAD_ADDED/REMOVED when a controller connects or goes
  away (read on each poll), SDL_EVENT_QUIT when the system asks the
  application to close;
- gamepads: controller n (0..3) is joystick id n + 1; its state is read
  once per poll and served from there;
- audio: a dedicated thread (the "halo audio" thread) asks the guest's
  stream callback for each block and hands it to the audio output, which
  blocks until the hardware wants the next one; the guest's format must be
  32-bit float or 16-bit stereo at 48 kHz (dsound_sdl.c uses float).
*/

#include "host.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAD_COUNT 4

static char last_error[256] = "";
static pthread_mutex_t sdl_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(const char *text)
{
	snprintf(last_error, sizeof(last_error), "%s", text);
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	(void)flags;
	return 1;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	host_logf(HOST_LOG_DEBUG, "SDL hint %s = %s", name, value ? value : "(null)");
	return 1;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	if (size)
		snprintf(buffer, size, "%s", last_error);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)(host_time_microseconds() / 1000);
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)(uintptr_t)pthread_self();
}

/* ---------- video */

static int window_created, context_created;

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	int display_width, display_height;

	(void)flags;
	host_platform_display_size(&display_width, &display_height);
	host_logf(HOST_LOG_INFO, "window \"%s\" (%dx%d requested): the display, %dx%d", title ? title : "", width, height,
		display_width, display_height);
	window_created = 1;
	return 1;
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	(void)window;
	host_platform_display_size(width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)window;
	(void)enabled;
	return 1;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	host_logf(HOST_LOG_DEBUG, "GL attribute %d = %d", attribute, value);
	return 1;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	if (!window_created || window != 1)
	{
		set_error("no window");
		return 0;
	}
	if (context_created)
		return 1;
	if (host_platform_gl_create() != 0)
	{
		set_error("cannot create the OpenGL ES context (see host.log)");
		return 0;
	}
	if (host_gl_initialize() != 0)
	{
		set_error("the OpenGL ES driver lacks what the renderer needs (see host.log)");
		return 0;
	}
	context_created = 1;
	return 1;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	(void)window;
	if (!context)
		return 1;
	return host_platform_gl_make_current() == 0;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return host_platform_gl_set_swap_interval(interval) == 0;
}

int host_sdl_gl_swap_window(uint32_t window)
{
	(void)window;
	host_gl_frame_end();
	host_monitor_frame();
	return host_platform_gl_swap() == 0;
}

/* ---------- gamepads */

static struct host_pad_state pads[PAD_COUNT];
static int pad_open[PAD_COUNT];
static int pad_reported[PAD_COUNT];

static void pads_update(void)
{
	int index;

	for (index = 0; index < PAD_COUNT; index++)
	{
		struct host_pad_state state;

		memset(&state, 0, sizeof(state));
		host_platform_pad_read(index, &state);
		pthread_mutex_lock(&sdl_lock);
		pads[index] = state;
		pthread_mutex_unlock(&sdl_lock);
	}
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;

	for (index = 0; index < PAD_COUNT && count < capacity; index++)
	{
		if (pads[index].connected)
			ids[count++] = (uint32_t)index + 1;
	}
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	if (id < 1 || id > PAD_COUNT || !pads[id - 1].connected)
		return 0;
	if (!pad_open[id - 1])
		host_logf(HOST_LOG_INFO, "controller %u opened", (unsigned)id);
	pad_open[id - 1] = 1;
	return id;
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	if (id < 1 || id > PAD_COUNT || !pad_open[id - 1] || !pads[id - 1].connected)
		return 0;
	return id;
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	int value;

	if (gamepad < 1 || gamepad > PAD_COUNT || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
		return 0;
	pthread_mutex_lock(&sdl_lock);
	value = pads[gamepad - 1].axes[axis];
	pthread_mutex_unlock(&sdl_lock);
	return value;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	int value;

	if (gamepad < 1 || gamepad > PAD_COUNT || button < 0 || button >= (int)sizeof(pads[0].buttons))
		return 0;
	pthread_mutex_lock(&sdl_lock);
	value = pads[gamepad - 1].buttons[button];
	pthread_mutex_unlock(&sdl_lock);
	return value;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	(void)gamepad;
	return SDL_GAMEPAD_TYPE_PS4;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	(void)milliseconds;
	if (gamepad < 1 || gamepad > PAD_COUNT)
		return 0;
	return host_platform_pad_rumble((int)gamepad - 1, (uint16_t)low, (uint16_t)high) == 0;
}

/* ---------- events */

int host_sdl_poll_event(void *event)
{
	SDL_Event result;
	int index;

	memset(&result, 0, sizeof(result));
	if (host_platform_quit_requested())
	{
		static int quit_sent;

		if (!quit_sent)
		{
			quit_sent = 1;
			result.type = SDL_EVENT_QUIT;
			result.quit.timestamp = host_time_microseconds() * 1000;
			memcpy(event, &result, sizeof(result));
			return 1;
		}
	}
	pads_update();
	for (index = 0; index < PAD_COUNT; index++)
	{
		if (pads[index].connected == pad_reported[index])
			continue;
		pad_reported[index] = pads[index].connected;
		if (!pads[index].connected)
			pad_open[index] = 0;
		host_logf(HOST_LOG_INFO, "controller %d %s", index + 1, pads[index].connected ? "connected" : "disconnected");
		result.type = pads[index].connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
		result.gdevice.timestamp = host_time_microseconds() * 1000;
		result.gdevice.which = (SDL_JoystickID)(index + 1);
		memcpy(event, &result, sizeof(result));
		return 1;
	}
	return 0;
}

/* ---------- audio */

struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	SDL_AudioSpec spec;
	int frame_bytes;
	volatile int running;
	pthread_mutex_t lock;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

static struct audio_binding *audio;

static int audio_append(struct audio_binding *binding, const void *data, int length)
{
	if (length <= 0)
		return 1;
	pthread_mutex_lock(&binding->lock);
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = realloc(binding->buffer, (size_t)size);

		if (!buffer)
		{
			pthread_mutex_unlock(&binding->lock);
			return 0;
		}
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	pthread_mutex_unlock(&binding->lock);
	return 1;
}

/* takes one block's frames out of the buffer as float stereo */
static void audio_take(struct audio_binding *binding, float *out, int frames)
{
	int available, index;

	pthread_mutex_lock(&binding->lock);
	available = binding->buffer_length / binding->frame_bytes;
	if (available > frames)
		available = frames;
	if (binding->spec.format == SDL_AUDIO_F32)
	{
		memcpy(out, binding->buffer, (size_t)available * 8);
	}
	else
	{
		const int16_t *samples = (const int16_t *)binding->buffer;

		for (index = 0; index < available * 2; index++)
			out[index] = samples[index] / 32768.0f;
	}
	memset(out + available * 2, 0, (size_t)(frames - available) * 8);
	memmove(binding->buffer, binding->buffer + available * binding->frame_bytes,
		(size_t)(binding->buffer_length - available * binding->frame_bytes));
	binding->buffer_length -= available * binding->frame_bytes;
	pthread_mutex_unlock(&binding->lock);
}

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;
	int frames = host_platform_audio_block;
	float *block = calloc((size_t)frames * 2, sizeof(float));
	uint64_t blocks = 0, late = 0, started = host_time_microseconds();

	while (!binding->running)
	{
		struct timespec pause = { 0, 5000000 };

		nanosleep(&pause, NULL);
	}
	host_logf(HOST_LOG_INFO, "audio: %d-frame blocks at %d Hz", frames, HOST_AUDIO_RATE);
	for (;;)
	{
		int missing;

		pthread_mutex_lock(&binding->lock);
		missing = frames * binding->frame_bytes - binding->buffer_length;
		pthread_mutex_unlock(&binding->lock);
		if (missing > 0)
			host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)missing,
				(uint32_t)missing);
		audio_take(binding, block, frames);
		if (host_platform_audio_output(block) != 0)
			late++;
		if (++blocks % (HOST_AUDIO_RATE / frames * 60) == 0)
		{
			host_logf(HOST_LOG_DEBUG, "audio: %llu blocks in %llu s, %llu output errors", (unsigned long long)blocks,
				(unsigned long long)((host_time_microseconds() - started) / 1000000), (unsigned long long)late);
		}
	}
	return NULL;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec_pointer, uint32_t callback, uint32_t userdata)
{
	const SDL_AudioSpec *spec = spec_pointer;
	struct audio_binding *binding;

	(void)device;
	if (audio)
	{
		set_error("only one audio stream");
		return 0;
	}
	if (!spec || spec->channels != 2 || spec->freq != HOST_AUDIO_RATE ||
		(spec->format != SDL_AUDIO_F32 && spec->format != SDL_AUDIO_S16))
	{
		host_logf(HOST_LOG_ERROR, "audio: unsupported format %#x, %d channels, %d Hz", spec ? (unsigned)spec->format : 0,
			spec ? spec->channels : 0, spec ? spec->freq : 0);
		set_error("unsupported audio format");
		return 0;
	}
	if (host_platform_audio_open() != 0)
	{
		set_error("cannot open the audio output (see host.log)");
		return 0;
	}
	binding = calloc(1, sizeof(*binding));
	binding->handle = 1;
	binding->callback = callback;
	binding->userdata = userdata;
	binding->spec = *spec;
	binding->frame_bytes = spec->format == SDL_AUDIO_F32 ? 8 : 4;
	pthread_mutex_init(&binding->lock, NULL);
	audio = binding;
	if (callback && host_native_thread_create(audio_thread, binding, 1024 * 1024, "halo audio") != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	if (!audio || stream != audio->handle)
		return 0;
	return audio_append(audio, data, length);
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	if (!audio || stream != audio->handle)
		return 0;
	audio->running = 1;
	return 1;
}

/* ---------- the rest: nothing to do on a console */

int host_sdl_set_clipboard_text(const char *text)
{
	host_logf(HOST_LOG_INFO, "clipboard: %s", text);
	return 1;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	if (size)
		buffer[0] = 0;
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "notice: %s", message);
	return 1;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	(void)flags;
	host_logf(HOST_LOG_WARN, "message: %s: %s", title, message);
	host_platform_message(title, message);
	return 1;
}
