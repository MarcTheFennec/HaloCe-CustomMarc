/*
HOST.H

Internals of the PS4 port's host (eboot.bin). See port/ps4/README.md for the
design and port/ps4/include/halo_ps4_abi.h for the guest contract.

The host is one program for two platforms:

- __ORBIS__: the PS4 (OpenOrbis toolchain, GoldHEN). libkernel, Piglet
  (OpenGL ES 2), libScePad, libSceAudioOut (host_orbis.c);
- __linux__: a headless x86-64 Linux build (host_platform_null.c) that runs
  the same guest image with no display, sound or controller, for testing
  the loader, system calls, threads and file resolution without a console.

Everything else is shared and platform-neutral POSIX; platform differences
are confined to the host_platform_* interface below.
*/

#ifndef __HALO_PS4_HOST_H
#define __HALO_PS4_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_ps4_abi.h"

/* ---------- logging (host_log.c)

Every line goes to the console's kernel log (klog, readable over the network
with GoldHEN's klog server or `nc <ps4> 3232`), to the log file
<write root>/logs/host.log, and to a ring buffer of the last 64 KB that is
written out next to the log if the process dies. */

#define HOST_LOG_DEBUG 3
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
#define HOST_LOG_FATAL 7

void host_log_initialize(void);
/* opens <directory>/host.log (called once the write root exists) */
void host_log_open_file(const char *directory);
void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void host_logv(int priority, const char *format, va_list arguments);
/* writes the ring buffer to <log directory>/crash.log */
void host_log_dump_ring(const char *reason);
void host_log_flush(void);

/* the guest's view (guest_host.h) */
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message on screen where possible and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- settings (host_main.c, from <write root>/config/ps4.toml) */

struct host_settings
{
	int log_level;       /* HOST_LOG_*: lines below it are not logged */
	int monitor_seconds; /* memory watermark interval, 0 = off */
	int fps_seconds;     /* frame statistics interval, 0 = off */
	int video_height;    /* 720 or 1080 */
	int vsync;
};

extern struct host_settings host_settings;

/* ---------- time (host_main.c) */

/* monotonic microseconds since the host started */
uint64_t host_time_microseconds(void);

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB: the Xbox contiguous
window, the image, and pools the host reserves on demand for everything else
(malloc arenas, thread stacks, anonymous mappings). */

extern uint32_t host_page_size;

int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* commits the image's range (after host_memory_initialize); 0 on success */
int host_memory_commit_image(void);
/* page-granular allocations below 4 GB; NULL on failure. protection is
the host's PROT_* */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise, with Linux flag values */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
/* bytes committed for the guest now, and the most ever */
void host_memory_usage(uint64_t *current, uint64_t *peak);
/* logs a memory watermark line (host_main.c's monitor calls it) */
void host_memory_log_watermark(const char *reason);
void host_install_signal_handlers(void);
/* texture write tracking by sampling rather than write faults */
void host_memory_watch_set_sampling(int sampling);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);
/* the host function for an import name, or NULL (the generated
host_import_table.c) */
void *host_resolve_import(const char *name);

/* ---------- threads (host_thread.c)

Host threads have ordinary host stacks; guest code runs on a second stack in
guest memory, which host_call_guest switches to (x32 code keeps the stack
pointer in 32-bit registers). */

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a host thread running function(argument); guest_stack_size is the
size of the guest stack it gets for host_call_guest. Returns 0 or an errno
value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t guest_stack_size,
	const char *name);
/* runs the guest's __guest_start on the calling thread; does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));
/* 1 while the calling thread is running guest code */
int host_in_guest(void);
uint32_t host_thread_count(void);
/* the calling thread's guest stack bounds, for the crash reporter */
void host_thread_guest_stack(uint64_t *low, uint64_t *high);

/* ---------- files (host_files.c)

The guest sees its data under HALO_GUEST_DATA_ROOT ("/halo"). Reads are
served from the first content root that has the file:

	/data/halo/<rel>  →  /mnt/usb0/halo/<rel>  →  /app0/<rel>

(HALO_PS4_ROOTS, colon-separated, replaces the list on Linux), with '\\'
turned into '/' and a case-insensitive search when the exact name is
missing. Writes (saves, settings, logs) go to the first root, the write
root. Every resolution logs which root served it; every miss logs the paths
tried. */

void host_files_initialize(void);
const char *host_files_write_root(void);
/* the host path for a guest path; for_write selects the write root when
the file does not exist yet. Returns buffer */
char *host_files_resolve(const char *guest_path, char *buffer, size_t size, int for_write);
/* the guest's current directory, as the guest sees it */
const char *host_files_guest_cwd(void);
int host_files_set_guest_cwd(const char *guest_path);
/* 1 if the file exists at any root (no logging) */
int host_files_exists(const char *guest_path);
/* logs how many lookups each root served and how many missed */
void host_files_log_statistics(void);

/* ---------- system calls (host_syscall.c) */

/* the guest's system call: number is x32 Linux's (0x40000000 + n);
returns the result or -errno (Linux values) */
long long host_syscall(long long number, long long a, long long b, long long c, long long d, long long e,
	long long f);
/* a host errno value as Linux's */
int host_linux_errno(int error);
/* the guest thread pointer (%fs base) of the calling thread */
uint32_t host_get_tp(void);
void host_set_tp(uint32_t tp);

/* ---------- the platform (host_orbis.c, host_platform_null.c)

The display, controller, sound and system services the SDL layer
(host_sdl.c) is built on. */

int host_platform_initialize(void);
void host_platform_shutdown(void);
const char *host_platform_name(void);
/* shows a message to the player (fatal errors) */
void host_platform_message(const char *title, const char *message);

/* the display: its size in pixels, and presenting a frame */
void host_platform_display_size(int *width, int *height);
/* creates the EGL window surface and context; 0 on success */
int host_platform_gl_create(void);
int host_platform_gl_make_current(void);
int host_platform_gl_swap(void);
int host_platform_gl_set_swap_interval(int interval);
/* a GL (ES 2) entry point of the driver, or NULL */
void *host_platform_gl_proc(const char *name);

/* the controller: SDL's gamepad state (SDL_GamepadAxis/SDL_GamepadButton
indices, axes in SDL's ranges) */
struct host_pad_state
{
	int connected;
	int16_t axes[6];
	uint8_t buttons[26];
};

/* reads controller `index` (0..3); returns 1 if connected */
int host_platform_pad_read(int index, struct host_pad_state *state);
int host_platform_pad_rumble(int index, uint16_t low, uint16_t high);
int host_platform_pad_count(void);

/* sound: 48 kHz stereo 32-bit float, blocks of host_platform_audio_block
frames; the output call blocks until the hardware wants the next block */
#define HOST_AUDIO_RATE 48000
#define HOST_AUDIO_CHANNELS 2
extern int host_platform_audio_block;
int host_platform_audio_open(void);
int host_platform_audio_output(const float *samples);
void host_platform_audio_close(void);

/* 1 once the system asked the application to quit */
int host_platform_quit_requested(void);

/* ---------- OpenGL ES (host_gl.c, host_gl_es2.c, host_shader.c) */

/* called with the context current, once */
int host_gl_initialize(void);
int host_gl_load_real(void *(*get_proc_address)(const char *name));
void host_gl_missing(const char *name);
/* per frame, before swapping */
void host_gl_frame_end(void);

/* ---------- the monitor (host_main.c) */

/* frame statistics the SDL layer reports on each swap */
void host_monitor_frame(void);

#endif
