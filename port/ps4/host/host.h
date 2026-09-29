/*
HOST.H

Internals of the PS4 port's host application (eboot.bin), a 64-bit
OpenOrbis program. See port/ps4/PLAN.md for the design and
port/ps4/include/halo_ps4_abi.h for the guest contract.

Milestone 0 (this file set): the host boots on the console, finds the data
root, logs, opens a Piglet (OpenGL ES 2) context, clears the screen, reads
the pads, and probes the fixed memory ranges the guest will need. The
guest loader, syscall layer and the rest arrive with M1/M2.
*/

#ifndef __HALO_PS4_HOST_H
#define __HALO_PS4_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_ps4_abi.h"

/* ---------- logging (host_debug.c)

Every line goes to the kernel log (readable with GoldHEN's klog server on
port 3232), to debug.txt in the data root once it is known, and, when
HALO_PS4_LOG_ADDRESS names a UDP "host:port", to that address. */

#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6

void host_log_initialize(void);
/* starts writing to <data_root>/debug.txt (truncated) */
void host_log_open_file(const char *data_root);
/* sends every line to a UDP address as well ("192.168.1.10:9999") */
int host_log_open_udp(const char *address_and_port);
void host_log(int priority, const char *text);
void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void host_log_shutdown(void);

/* logs, shows the message to the player (message dialog) and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void host_exit(int code) __attribute__((noreturn));

/* ---------- system (host_main.c) */

/* the data root in use ("/data/halo" unless another one has maps/) */
extern char host_data_root[256];
/* the initial user; pads and audio are opened for this user */
extern int32_t host_user_id;

/* ---------- video (host_video.c)

sceVideoOut through Piglet's EGL. Piglet and the shader compiler are
modules that are not on retail firmware; the player copies them to
<data_root>/modules (port/ps4/README.md). */

struct host_video
{
	int width, height;   /* the back buffer, in pixels */
	int vsync;
	const char *gl_version, *gl_renderer, *gl_vendor;
	int has_shader_compiler; /* libSceShaccVSH loaded: glShaderSource works */
};

extern struct host_video host_video;

/* loads the modules and creates the context; returns 0 or -1 (logged) */
int host_video_initialize(int width, int height);
void host_video_swap(void);
void host_video_set_swap_interval(int interval);
void host_video_shutdown(void);

/* ---------- pads (host_pad.c)

scePad for up to four controllers, port 0 being the initial user's pad.
Layout of struct host_pad_state follows the Xbox controller the game
expects; the mapping is the table in port/ps4/README.md. */

#define HOST_PAD_COUNT 4

struct host_pad_state
{
	int connected;
	uint32_t buttons;         /* ORBIS_PAD_BUTTON_* bits */
	uint8_t left_x, left_y;   /* 0..255, 128 centre */
	uint8_t right_x, right_y;
	uint8_t l2, r2;           /* 0..255 */
};

int host_pad_initialize(void);
/* refreshes and returns the state of pad 0..3 */
const struct host_pad_state *host_pad_read(int index);
int host_pad_rumble(int index, uint8_t large, uint8_t small);
void host_pad_shutdown(void);

/* ---------- memory (host_memory.c)

M0 only probes: can the fixed ranges the guest needs be mapped? The result
decides whether M2 keeps the fixed addresses or relocates the image
(PLAN.md section 6). */

struct host_memory_probe
{
	int window_ok;   /* HALO_GUEST_WINDOW_BASE, 128 MB, fixed */
	int image_ok;    /* HALO_GUEST_IMAGE_BASE, 64 MB, fixed */
	int low_pool_ok; /* any 256 MB below 4 GB */
	uint64_t low_pool_address;
	uint64_t flexible_available;
};

int host_memory_probe(struct host_memory_probe *result);

/* ---------- time */

uint64_t host_ticks_ms(void);
void host_sleep_ms(uint32_t milliseconds);

#endif
