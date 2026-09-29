/*
HOST_PLATFORM_NULL.C

The headless platform of the host's Linux build: no display, controller or
sound, so that the loader, the system calls, threads, file resolution and the
game's startup can be run and debugged on a PC with the very image the PS4
runs (tools/ps4_build.py, target ps4_host_linux):

	build/ps4/halo_ps4_headless build/ps4/halo_guest.elf

Environment:
	HALO_PS4_ROOTS       content roots, colon-separated (host_files.c)
	HALO_PS4_QUIT_AFTER  seconds after which the "system" asks the game to
	                     quit, as the PS4's close-application does (tests
	                     the clean-exit path unattended)

The display reports 1920x1080 so the game picks the same render size as on
the console, but no GL context can be created: the game runs its no-window
path (d3d8_gl.c without gl_ready). Sound is paced in real time and discarded
so the game's mixer callback runs on its thread as it would on the console.
*/

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int host_platform_audio_block = 256;

static double quit_after;

int host_platform_initialize(void)
{
	const char *text = getenv("HALO_PS4_QUIT_AFTER");

	if (text && *text)
	{
		quit_after = atof(text);
		host_logf(HOST_LOG_INFO, "the application will be asked to quit after %g s", quit_after);
	}
	return 0;
}

void host_platform_shutdown(void)
{
}

const char *host_platform_name(void)
{
	return "Linux (headless)";
}

void host_platform_message(const char *title, const char *message)
{
	fprintf(stderr, "%s: %s\n", title, message);
}

void host_platform_display_size(int *width, int *height)
{
	*width = host_settings.video_height == 720 ? 1280 : 1920;
	*height = host_settings.video_height;
}

int host_platform_gl_create(void)
{
	host_logf(HOST_LOG_INFO, "headless: no GL context");
	return -1;
}

int host_platform_gl_make_current(void)
{
	return -1;
}

int host_platform_gl_swap(void)
{
	return -1;
}

int host_platform_gl_set_swap_interval(int interval)
{
	(void)interval;
	return -1;
}

void *host_platform_gl_proc(const char *name)
{
	(void)name;
	return NULL;
}

int host_platform_pad_read(int index, struct host_pad_state *state)
{
	(void)index;
	state->connected = 0;
	return 0;
}

int host_platform_pad_rumble(int index, uint16_t low, uint16_t high)
{
	(void)index;
	(void)low;
	(void)high;
	return -1;
}

int host_platform_pad_count(void)
{
	return 0;
}

static struct timespec audio_next;

int host_platform_audio_open(void)
{
	clock_gettime(CLOCK_MONOTONIC, &audio_next);
	return 0;
}

/* waits until the block would have finished playing */
int host_platform_audio_output(const float *samples)
{
	(void)samples;
	audio_next.tv_nsec += (long)((int64_t)host_platform_audio_block * 1000000000 / HOST_AUDIO_RATE);
	while (audio_next.tv_nsec >= 1000000000)
	{
		audio_next.tv_nsec -= 1000000000;
		audio_next.tv_sec++;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &audio_next, NULL) != 0)
	{
	}
	return 0;
}

void host_platform_audio_close(void)
{
}

int host_platform_quit_requested(void)
{
	return quit_after > 0.0 && host_time_microseconds() >= (uint64_t)(quit_after * 1e6);
}
