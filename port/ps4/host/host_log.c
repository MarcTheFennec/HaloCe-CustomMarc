/*
HOST_LOG.C

The PS4 host's log. Each line goes to

- the kernel log (sceKernelDebugOutText; standard error on Linux), which
  GoldHEN's klog server streams over the network (`nc <ps4 ip> 3232`);
- <write root>/logs/host.log, once host_files has created it (lines logged
  before that are kept in the ring buffer and written out when it opens);
- a ring buffer of the last 64 KB, written to <write root>/logs/crash.log by
  the crash reporter (host_memory.c) and fatal errors.

The guest's standard output and error (host_syscall.c) and its host_log
calls come here too, so host.log has the whole story of a run.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__ORBIS__)
#include <orbis/libkernel.h>
#endif

#define RING_SIZE 65536

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char ring[RING_SIZE];
static size_t ring_head; /* next byte to write */
static int ring_wrapped;
static FILE *log_file;
static char log_directory[512];
static int console_level = HOST_LOG_DEBUG;

static const char *priority_name(int priority)
{
	switch (priority)
	{
	case HOST_LOG_DEBUG: return "debug";
	case HOST_LOG_INFO: return "info";
	case HOST_LOG_WARN: return "warn";
	case HOST_LOG_ERROR: return "error";
	case HOST_LOG_FATAL: return "fatal";
	default: return "log";
	}
}

static void ring_append(const char *text, size_t length)
{
	while (length)
	{
		size_t chunk = RING_SIZE - ring_head;

		if (chunk > length)
			chunk = length;
		memcpy(ring + ring_head, text, chunk);
		ring_head += chunk;
		text += chunk;
		length -= chunk;
		if (ring_head == RING_SIZE)
		{
			ring_head = 0;
			ring_wrapped = 1;
		}
	}
}

static void ring_write(FILE *file)
{
	if (ring_wrapped)
		fwrite(ring + ring_head, 1, RING_SIZE - ring_head, file);
	fwrite(ring, 1, ring_head, file);
}

static void console_write(const char *line)
{
#if defined(__ORBIS__)
	sceKernelDebugOutText(0, line);
#else
	fputs(line, stderr);
#endif
}

void host_log_initialize(void)
{
	const char *level = getenv("HALO_PS4_LOG_LEVEL");

	if (level)
		console_level = atoi(level);
}

void host_log_open_file(const char *directory)
{
	char path[600];

	pthread_mutex_lock(&log_lock);
	snprintf(log_directory, sizeof(log_directory), "%s", directory);
	snprintf(path, sizeof(path), "%s/host.log", directory);
	if (!log_file)
	{
		/* keep the previous run's log */
		char previous[620];

		snprintf(previous, sizeof(previous), "%s/host.previous.log", directory);
		rename(path, previous);
		log_file = fopen(path, "w");
		if (log_file)
		{
			/* what was logged before the file existed */
			ring_write(log_file);
			fflush(log_file);
		}
	}
	pthread_mutex_unlock(&log_lock);
	if (!log_file)
	{
		char line[700];

		snprintf(line, sizeof(line), "[halo host] cannot create %s: %s\n", path, strerror(errno));
		console_write(line);
	}
}

void host_logv(int priority, const char *format, va_list arguments)
{
	char line[2048];
	uint64_t now = host_time_microseconds();
	int prefix, length;

	prefix = snprintf(line, sizeof(line), "[%5llu.%03llu] %s: ", (unsigned long long)(now / 1000000),
		(unsigned long long)(now / 1000 % 1000), priority_name(priority));
	length = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1, format, arguments);
	if (length < 0)
		length = 0;
	length += prefix;
	if (length > (int)sizeof(line) - 2)
		length = (int)sizeof(line) - 2;
	line[length++] = '\n';
	line[length] = 0;

	pthread_mutex_lock(&log_lock);
	/* the crash ring keeps every level; the file and klog only the
	configured ones (log.level in ps4.toml) */
	ring_append(line, (size_t)length);
	if (priority < host_settings.log_level)
	{
		pthread_mutex_unlock(&log_lock);
		return;
	}
	if (log_file)
	{
		fputs(line, log_file);
		/* the log must survive a crash or a power cycle */
		if (priority >= HOST_LOG_WARN)
			fflush(log_file);
	}
	pthread_mutex_unlock(&log_lock);
	if (priority >= console_level)
		console_write(line);
}

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	host_logv(priority, format, arguments);
	va_end(arguments);
}

void host_log_flush(void)
{
	pthread_mutex_lock(&log_lock);
	if (log_file)
		fflush(log_file);
	pthread_mutex_unlock(&log_lock);
}

void host_log_dump_ring(const char *reason)
{
	char path[600];
	FILE *file;

	if (!log_directory[0])
		return;
	snprintf(path, sizeof(path), "%s/crash.log", log_directory);
	/* no lock: this runs from the crash handler, possibly with the lock
	held by the crashed thread */
	file = fopen(path, "w");
	if (!file)
		return;
	fprintf(file, "Halo PS4 host: %s\n\n", reason);
	ring_write(file);
	fclose(file);
	if (log_file)
		fflush(log_file);
}

/* ---------- the guest's services */

/* the guest passes Android log priorities (android/log.h), which these
match */
void host_log(int priority, const char *text)
{
	host_logf(priority, "[guest] %s", text);
}

/* the guest's errno values are Linux's (host_syscall.c) */
int host_errno(void)
{
	return host_linux_errno(errno);
}
