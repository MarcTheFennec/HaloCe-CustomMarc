/*
HOST_MAIN.C

Startup and shutdown of the PS4 host: brings up the platform (host_orbis.c
or host_platform_null.c), finds the content roots (host_files.c), reads the
host's settings, loads the game image and runs the game's main() on a thread
of its own with a large stack in guest memory (host_thread.c).

Also the monitor: frame-rate statistics from every swap and a periodic
memory watermark, logged so that long runs (PORTING.md, A10/A11) can be
measured from the log alone.

The host's settings are in <write root>/config/ps4.toml (the game's own
settings stay in <write root>/config.toml, which the game reads itself):

	[log]
	level = "info"          # debug, info, warn, error
	[monitor]
	seconds = 60            # memory watermark interval, 0 = off
	fps_seconds = 10        # frame statistics interval, 0 = off
	[video]
	height = 1080           # 1080 or 720: the window surface
	vsync = true
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tomlc17.h"

struct host_settings host_settings = {
	HOST_LOG_INFO, /* log_level */
	60,            /* monitor_seconds */
	10,            /* fps_seconds */
	1080,          /* video_height */
	1,             /* vsync */
};

/* ---------- time */

static struct timespec start_time;

uint64_t host_time_microseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)(now.tv_sec - start_time.tv_sec) * 1000000u + (now.tv_nsec - start_time.tv_nsec) / 1000;
}

/* ---------- ending */

static volatile int exiting;

void host_exit(int code)
{
	if (__sync_lock_test_and_set(&exiting, 1))
		_exit(code);
	host_logf(HOST_LOG_INFO, "the game exited with code %d after %.1f s", code, host_time_microseconds() / 1e6);
	host_files_log_statistics();
	host_memory_log_watermark("exit");
	host_log_flush();
	host_platform_shutdown();
	_exit(code);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_FATAL, "%s", message);
	host_log_dump_ring("fatal error");
	host_log_flush();
	if (!exiting)
		host_platform_message("Halo", message);
	host_exit(1);
}

void host_abort(const char *reason)
{
	host_fatal("the game stopped: %s", reason && *reason ? reason : "abort");
}

/* ---------- the monitor */

static struct
{
	uint64_t interval_start;
	uint64_t last_frame;
	uint32_t frames;
	uint64_t longest;
	uint64_t total_frames;
} frames;

void host_monitor_frame(void)
{
	uint64_t now = host_time_microseconds();
	uint64_t interval;

	if (frames.last_frame && now - frames.last_frame > frames.longest)
		frames.longest = now - frames.last_frame;
	frames.last_frame = now;
	frames.frames++;
	frames.total_frames++;
	if (!frames.interval_start)
	{
		frames.interval_start = now;
		host_logf(HOST_LOG_INFO, "first frame presented at %.2f s", now / 1e6);
		return;
	}
	interval = now - frames.interval_start;
	if (host_settings.fps_seconds > 0 && interval >= (uint64_t)host_settings.fps_seconds * 1000000u)
	{
		host_logf(HOST_LOG_INFO, "fps %.1f (longest frame %.1f ms, %llu frames in total)",
			frames.frames * 1e6 / interval, frames.longest / 1e3, (unsigned long long)frames.total_frames);
		frames.interval_start = now;
		frames.frames = 0;
		frames.longest = 0;
	}
}

static void *monitor_main(void *unused)
{
	(void)unused;
	for (;;)
	{
		sleep((unsigned)host_settings.monitor_seconds);
		host_memory_log_watermark("periodic");
		host_log_flush();
	}
	return NULL;
}

/* ---------- settings */

static void read_settings(void)
{
	char path[512];
	toml_result_t result;
	toml_datum_t value;
	struct stat information;

	snprintf(path, sizeof(path), "%s/config/ps4.toml", host_files_write_root());
	if (stat(path, &information) != 0)
	{
		host_logf(HOST_LOG_INFO, "no %s; using the default host settings", path);
		return;
	}
	result = toml_parse_file_ex(path);
	if (!result.ok)
	{
		host_logf(HOST_LOG_WARN, "%s: %s; using the default host settings", path, result.errmsg);
		return;
	}
	value = toml_seek(result.toptab, "log.level");
	if (value.type == TOML_STRING)
	{
		static const char *const names[] = { "debug", "info", "warn", "error" };
		int index;

		for (index = 0; index < 4; index++)
			if (!strcmp(value.u.s, names[index]))
				host_settings.log_level = HOST_LOG_DEBUG + index;
	}
	value = toml_seek(result.toptab, "monitor.seconds");
	if (value.type == TOML_INT64 && value.u.int64 >= 0)
		host_settings.monitor_seconds = (int)value.u.int64;
	value = toml_seek(result.toptab, "monitor.fps_seconds");
	if (value.type == TOML_INT64 && value.u.int64 >= 0)
		host_settings.fps_seconds = (int)value.u.int64;
	value = toml_seek(result.toptab, "video.height");
	if (value.type == TOML_INT64)
	{
		if (value.u.int64 == 720 || value.u.int64 == 1080)
			host_settings.video_height = (int)value.u.int64;
		else
			host_logf(HOST_LOG_WARN, "%s: video.height must be 720 or 1080", path);
	}
	value = toml_seek(result.toptab, "video.vsync");
	if (value.type == TOML_BOOLEAN)
		host_settings.vsync = value.u.boolean;
	toml_free(result);
	host_logf(HOST_LOG_INFO, "settings from %s: log level %d, monitor %d s, fps %d s, %dp, vsync %s", path,
		host_settings.log_level, host_settings.monitor_seconds, host_settings.fps_seconds,
		host_settings.video_height, host_settings.vsync ? "on" : "off");
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	char *entry = malloc(strlen(name) + strlen(value) + 2);

	if (!entry || environment->count >= ENVIRONMENT_MAXIMUM)
	{
		free(entry);
		return;
	}
	sprintf(entry, "%s=%s", name, value);
	environment->entries[environment->count++] = entry;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv, *environ_list;
	char *strings;
	int index;

	if (!memory)
		host_fatal("cannot allocate the game's environment");
	argv = (uint32_t *)(memory + sizeof(*boot));
	environ_list = argv + 2;
	strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = host_page_size;
	return (uint32_t)(uintptr_t)boot;
}

/* ---------- the game image */

#if defined(__ORBIS__)

/* host_image_blob.S */
extern const unsigned char halo_guest_image_start[];
extern const unsigned char halo_guest_image_end[];

static const void *read_image(size_t *size, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	*size = (size_t)(halo_guest_image_end - halo_guest_image_start);
	return halo_guest_image_start;
}

#else

/* the headless host takes the image's path as its first argument */
static const void *read_image(size_t *size, int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : getenv("HALO_PS4_GUEST");
	struct stat information;
	void *image;
	int fd;

	if (!path)
		path = "build/ps4/halo_guest.elf";
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &information) != 0)
		host_fatal("cannot open the game image %s: %s", path, strerror(errno));
	image = mmap(NULL, (size_t)information.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (image == MAP_FAILED)
		host_fatal("cannot read the game image %s: %s", path, strerror(errno));
	*size = (size_t)information.st_size;
	host_logf(HOST_LOG_INFO, "game image %s", path);
	return image;
}

#endif

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static int main_argc;
static char **main_argv;

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	char text[64];
	const void *image;
	size_t image_size;
	int width, height;

	(void)unused;
	image = read_image(&image_size, main_argc, main_argv);
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image; see the log for details");

	environment_set(&environment, "HOME", HALO_GUEST_DATA_ROOT "/save");
	environment_set(&environment, "HALO_DATA_ROOT", HALO_GUEST_DATA_ROOT);
	environment_set(&environment, "HALO_SAVE_ROOT", HALO_GUEST_DATA_ROOT "/save");
	host_platform_display_size(&width, &height);
	if (width > 0 && height > 0)
	{
		/* the game renders 480 lines at the display's aspect ratio unless
		display.screen_width in config.toml says otherwise (d3d8_gl.c) */
		snprintf(text, sizeof(text), "%d", (480 * width / height) & ~1);
		environment_set(&environment, "HALO_DISPLAY_WIDTH", text);
		host_logf(HOST_LOG_INFO, "display %dx%d: rendering %sx480", width, height, text);
	}
	time_zone(text, sizeof(text));
	environment_set(&environment, "TZ", text);
	host_run_guest_main(make_boot(&environment));
}

int main(int argc, char *argv[])
{
	char path[512];

	clock_gettime(CLOCK_MONOTONIC, &start_time);
	main_argc = argc;
	main_argv = argv;
	/* the guest's descriptors are the host's: keep 0-2 from being handed
	out as the game's files if the loader started us without them */
	while (1)
	{
		int fd = open("/dev/null", O_RDWR);

		if (fd < 0)
			break;
		if (fd > 2)
		{
			close(fd);
			break;
		}
	}
	host_log_initialize();
	host_logf(HOST_LOG_INFO, "Halo for %s starting (build " __DATE__ " " __TIME__ ")", host_platform_name());
	if (host_platform_initialize() != 0)
		host_fatal("the %s platform layer failed to start; see the log", host_platform_name());
	host_install_signal_handlers();
	host_files_initialize();
	snprintf(path, sizeof(path), "%s/logs", host_files_write_root());
	mkdir(path, 0777);
	host_log_open_file(path);
	read_settings();
	if (!host_files_exists(HALO_GUEST_DATA_ROOT "/maps"))
	{
		host_fatal("The Halo game data was not found.\n\nCopy the Xbox game data (build 01.01.14.2342), "
			"the folder that contains maps, to %s on the console (GoldHEN FTP, port 2121) "
			"or to /mnt/usb0/halo. See BUILDING-PS4.md.", host_files_write_root());
	}
	if (host_settings.monitor_seconds > 0)
		host_native_thread_create(monitor_main, NULL, 0, "halo monitor");
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE, "halo main") != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit); the main thread
	watches for the system asking the application to close */
	for (;;)
	{
		usleep(100000);
		if (host_platform_quit_requested())
		{
			/* host_sdl_poll_event hands the game SDL_EVENT_QUIT; give it
			time to end cleanly before ending it here */
			host_logf(HOST_LOG_INFO, "the system asked the application to close");
			sleep(5);
			host_logf(HOST_LOG_WARN, "the game did not exit within 5 s of the quit request");
			host_exit(0);
		}
	}
}
