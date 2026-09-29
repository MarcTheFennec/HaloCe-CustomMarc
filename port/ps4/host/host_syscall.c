/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl (its syscall_arch.h sends every
call here, numbered as Linux x32 calls).

The host is not Linux (on the PS4 it is FreeBSD-derived Orbis), so nothing
passes through: each call is implemented with the host's POSIX functions,
translating what differs between the Linux ABI the guest was built for and
the host's

- flag values (open, *at, fcntl), clock ids and errno values;
- structures: stat (the x86-64 kernel layout, kstat.h), timespec/timeval
  (the guest's time_t is 32-bit, as in the MSVC runtime), iovec (32-bit);
- paths: the guest sees its data under /halo; host_files.c maps each path
  onto the content roots;
- futexes, which Orbis lacks: emulated with mutexes and condition variables
  (musl's locks, condition variables and thread joins are built on them);
- memory mappings, which must stay below 4 GB (host_memory.c);
- the standard output and error streams, which go to the log;
- process exit and signals, which the host owns.
*/

#include "host.h"
#include "host_linux_syscalls.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

/* ---------- the Linux ABI's constants */

enum
{
	L_EPERM = 1, L_ENOENT = 2, L_ESRCH = 3, L_EINTR = 4, L_EIO = 5, L_ENXIO = 6, L_E2BIG = 7, L_EBADF = 9,
	L_ECHILD = 10, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EBUSY = 16, L_EEXIST = 17,
	L_EXDEV = 18, L_ENODEV = 19, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENFILE = 23, L_EMFILE = 24,
	L_ENOTTY = 25, L_EFBIG = 27, L_ENOSPC = 28, L_ESPIPE = 29, L_EROFS = 30, L_EMLINK = 31, L_EPIPE = 32,
	L_EDOM = 33, L_ERANGE = 34, L_EDEADLK = 35, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39,
	L_ELOOP = 40, L_ETIMEDOUT = 110, L_ECONNREFUSED = 111, L_EINPROGRESS = 115, L_EALREADY = 114,
	L_ENOTSOCK = 88, L_EADDRINUSE = 98, L_ECONNRESET = 104, L_ENOTCONN = 107, L_EOPNOTSUPP = 95,
};

#define L_O_ACCMODE 03
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_NOCTTY 0400
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000
#define L_O_DIRECTORY 0200000
#define L_O_NOFOLLOW 0400000
#define L_O_CLOEXEC 02000000

#define L_AT_FDCWD (-100)
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR 0x200
#define L_AT_EMPTY_PATH 0x1000

#define L_F_DUPFD 0
#define L_F_GETFD 1
#define L_F_SETFD 2
#define L_F_GETFL 3
#define L_F_SETFL 4
#define L_F_GETLK 5
#define L_F_SETLK 6
#define L_F_SETLKW 7
#define L_F_DUPFD_CLOEXEC 1030

#define L_FUTEX_WAIT 0
#define L_FUTEX_WAKE 1
#define L_FUTEX_REQUEUE 3
#define L_FUTEX_CMP_REQUEUE 4
#define L_FUTEX_WAIT_BITSET 9
#define L_FUTEX_WAKE_BITSET 10
#define L_FUTEX_PRIVATE 128
#define L_FUTEX_CLOCK_REALTIME 256

/* Linux errno for the host's */
int host_linux_errno(int error)
{
#if defined(__linux__)
	return error;
#else
	switch (error)
	{
	case 0: return 0;
	case EPERM: return L_EPERM;
	case ENOENT: return L_ENOENT;
	case ESRCH: return L_ESRCH;
	case EINTR: return L_EINTR;
	case EIO: return L_EIO;
	case ENXIO: return L_ENXIO;
	case E2BIG: return L_E2BIG;
	case EBADF: return L_EBADF;
	case ECHILD: return L_ECHILD;
	case EAGAIN: return L_EAGAIN;
	case ENOMEM: return L_ENOMEM;
	case EACCES: return L_EACCES;
	case EFAULT: return L_EFAULT;
	case EBUSY: return L_EBUSY;
	case EEXIST: return L_EEXIST;
	case EXDEV: return L_EXDEV;
	case ENODEV: return L_ENODEV;
	case ENOTDIR: return L_ENOTDIR;
	case EISDIR: return L_EISDIR;
	case EINVAL: return L_EINVAL;
	case ENFILE: return L_ENFILE;
	case EMFILE: return L_EMFILE;
	case ENOTTY: return L_ENOTTY;
	case EFBIG: return L_EFBIG;
	case ENOSPC: return L_ENOSPC;
	case ESPIPE: return L_ESPIPE;
	case EROFS: return L_EROFS;
	case EMLINK: return L_EMLINK;
	case EPIPE: return L_EPIPE;
	case EDOM: return L_EDOM;
	case ERANGE: return L_ERANGE;
	case EDEADLK: return L_EDEADLK;
	case ENAMETOOLONG: return L_ENAMETOOLONG;
	case ENOSYS: return L_ENOSYS;
	case ENOTEMPTY: return L_ENOTEMPTY;
	case ELOOP: return L_ELOOP;
	case ETIMEDOUT: return L_ETIMEDOUT;
	case ECONNREFUSED: return L_ECONNREFUSED;
	case EINPROGRESS: return L_EINPROGRESS;
	case EALREADY: return L_EALREADY;
	case ENOTSOCK: return L_ENOTSOCK;
	case EADDRINUSE: return L_EADDRINUSE;
	case ECONNRESET: return L_ECONNRESET;
	case ENOTCONN: return L_ENOTCONN;
	case EOPNOTSUPP: return L_EOPNOTSUPP;
	default: return L_EIO;
	}
#endif
}

static long result_of(long value)
{
	return value == -1 ? -host_linux_errno(errno) : value;
}

static int open_flags_in(int flags)
{
	int result = flags & L_O_ACCMODE;

	if (flags & L_O_CREAT) result |= O_CREAT;
	if (flags & L_O_EXCL) result |= O_EXCL;
	if (flags & L_O_NOCTTY) result |= O_NOCTTY;
	if (flags & L_O_TRUNC) result |= O_TRUNC;
	if (flags & L_O_APPEND) result |= O_APPEND;
	if (flags & L_O_NONBLOCK) result |= O_NONBLOCK;
	if (flags & L_O_DIRECTORY) result |= O_DIRECTORY;
	if (flags & L_O_CLOEXEC) result |= O_CLOEXEC;
	return result;
}

static int open_flags_out(int flags)
{
	int result = flags & O_ACCMODE;

	if (flags & O_APPEND) result |= L_O_APPEND;
	if (flags & O_NONBLOCK) result |= L_O_NONBLOCK;
	return result;
}

static int clock_in(int clock)
{
	switch (clock)
	{
	case 0: /* CLOCK_REALTIME */
	case 5: /* CLOCK_REALTIME_COARSE */
		return CLOCK_REALTIME;
	case 2: /* CLOCK_PROCESS_CPUTIME_ID */
	case 3: /* CLOCK_THREAD_CPUTIME_ID */
	default: /* MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE, BOOTTIME */
		return CLOCK_MONOTONIC;
	}
}

/* ---------- the guest's structures */

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

struct guest_kstat
{
	uint64_t st_dev, st_ino, st_nlink;
	uint32_t st_mode, st_uid, st_gid, pad0;
	uint64_t st_rdev;
	int64_t st_size, st_blksize, st_blocks;
	int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
	int64_t unused[3];
};

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static void stat_out(uint64_t address, const struct stat *st)
{
	struct guest_kstat *k = GUEST(struct guest_kstat *, address);

	memset(k, 0, sizeof(*k));
	k->st_dev = (uint64_t)st->st_dev;
	k->st_ino = (uint64_t)st->st_ino;
	k->st_nlink = (uint64_t)st->st_nlink;
	/* the file type and permission bits agree */
	k->st_mode = (uint32_t)st->st_mode;
	k->st_size = (int64_t)st->st_size;
	k->st_blksize = 4096;
	k->st_blocks = ((int64_t)st->st_size + 511) / 512;
	k->st_atime_sec = (int64_t)st->st_atime;
	k->st_mtime_sec = (int64_t)st->st_mtime;
	k->st_ctime_sec = (int64_t)st->st_ctime;
}

/* ---------- paths */

#define PATH_SIZE 1024

/* the host path for a guest path relative to dirfd (only AT_FDCWD is
supported for relative paths) */
static const char *path_in(int dirfd, uint64_t address, char *buffer, int for_write)
{
	const char *path = GUEST(const char *, address);

	if (!path)
		return NULL;
	if (path[0] != '/' && path[0] != '\\' && dirfd != L_AT_FDCWD)
	{
		host_logf(HOST_LOG_WARN, "guest path %s relative to a directory descriptor", path);
		return NULL;
	}
	return host_files_resolve(path, buffer, PATH_SIZE, for_write);
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	pthread_mutex_lock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_logf(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, "[game] %s", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	pthread_mutex_unlock(&log_lock);
}

static long guest_rw_vector(int fd, uint64_t vector, int count, int64_t offset, int positional, int writing)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -L_EINVAL;
	for (index = 0; index < count; index++)
	{
		char *base = GUEST(char *, guest_vector[index].base);
		size_t length = guest_vector[index].length;
		ssize_t done;

		if (!length)
			continue;
		if (writing && (fd == 1 || fd == 2))
		{
			log_bytes(fd, base, length);
			done = (ssize_t)length;
		}
		else if (positional)
		{
			done = writing ? pwrite(fd, base, length, (off_t)(offset + total)) : pread(fd, base, length, (off_t)(offset + total));
		}
		else
		{
			done = writing ? write(fd, base, length) : read(fd, base, length);
		}
		if (done < 0)
			return total ? total : -host_linux_errno(errno);
		total += done;
		if ((size_t)done < length)
			break;
	}
	return total;
}

/* ---------- futexes

Waiters queue on one of 256 buckets (by address), each with its own
condition variable; a wake marks up to n waiters of the address as woken. */

struct futex_waiter
{
	struct futex_waiter *next;
	uint64_t address;
	pthread_cond_t condition;
	int woken;
};

struct futex_bucket
{
	pthread_mutex_t lock;
	struct futex_waiter *waiters;
};

#define FUTEX_BUCKETS 256

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];
static pthread_once_t futex_once = PTHREAD_ONCE_INIT;

static void futex_initialize(void)
{
	int index;

	for (index = 0; index < FUTEX_BUCKETS; index++)
		pthread_mutex_init(&futex_buckets[index].lock, NULL);
}

static struct futex_bucket *futex_bucket_of(uint64_t address)
{
	return &futex_buckets[((address >> 2) * 2654435761u >> 8) & (FUTEX_BUCKETS - 1)];
}

static void futex_unlink(struct futex_bucket *bucket, struct futex_waiter *waiter)
{
	struct futex_waiter **link;

	for (link = &bucket->waiters; *link; link = &(*link)->next)
	{
		if (*link == waiter)
		{
			*link = waiter->next;
			return;
		}
	}
}

static long futex_wait(uint64_t address, uint32_t value, const struct timespec *timeout, int absolute, int realtime)
{
	struct futex_bucket *bucket = futex_bucket_of(address);
	struct futex_waiter waiter;
	struct timespec deadline;
	long result = 0;

	if (timeout)
	{
		struct timespec now;

		/* condition variables time out on CLOCK_REALTIME */
		clock_gettime(CLOCK_REALTIME, &now);
		if (absolute)
		{
			deadline = *timeout;
			if (!realtime)
			{
				/* a CLOCK_MONOTONIC deadline, moved to CLOCK_REALTIME */
				struct timespec monotonic;
				int64_t delta;

				clock_gettime(CLOCK_MONOTONIC, &monotonic);
				delta = (timeout->tv_sec - monotonic.tv_sec) * 1000000000LL + (timeout->tv_nsec - monotonic.tv_nsec);
				if (delta < 0)
					delta = 0;
				deadline.tv_sec = now.tv_sec + (time_t)(delta / 1000000000LL);
				deadline.tv_nsec = now.tv_nsec + (long)(delta % 1000000000LL);
			}
		}
		else
		{
			deadline.tv_sec = now.tv_sec + timeout->tv_sec;
			deadline.tv_nsec = now.tv_nsec + timeout->tv_nsec;
		}
		if (deadline.tv_nsec >= 1000000000L)
		{
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}
	pthread_mutex_lock(&bucket->lock);
	if (__atomic_load_n(GUEST(volatile uint32_t *, address), __ATOMIC_SEQ_CST) != value)
	{
		pthread_mutex_unlock(&bucket->lock);
		return -L_EAGAIN;
	}
	waiter.address = address;
	waiter.woken = 0;
	pthread_cond_init(&waiter.condition, NULL);
	waiter.next = bucket->waiters;
	bucket->waiters = &waiter;
	while (!waiter.woken)
	{
		int error = timeout ? pthread_cond_timedwait(&waiter.condition, &bucket->lock, &deadline) :
			pthread_cond_wait(&waiter.condition, &bucket->lock);

		if (error == ETIMEDOUT && !waiter.woken)
		{
			result = -L_ETIMEDOUT;
			break;
		}
	}
	/* a requeue may have moved the waiter to another bucket */
	if (!waiter.woken)
		futex_unlink(bucket, &waiter);
	pthread_mutex_unlock(&bucket->lock);
	pthread_cond_destroy(&waiter.condition);
	return result;
}

static long futex_wake(uint64_t address, int count)
{
	struct futex_bucket *bucket = futex_bucket_of(address);
	struct futex_waiter **link;
	long woken = 0;

	pthread_mutex_lock(&bucket->lock);
	link = &bucket->waiters;
	while (*link && woken < count)
	{
		struct futex_waiter *waiter = *link;

		if (waiter->address == address)
		{
			*link = waiter->next;
			waiter->woken = 1;
			pthread_cond_signal(&waiter->condition);
			woken++;
			continue;
		}
		link = &waiter->next;
	}
	pthread_mutex_unlock(&bucket->lock);
	return woken;
}

/* wakes count waiters of address and moves up to requeue others to
address2. Moving a waiter between buckets would leave it waiting on the
wrong bucket's lock, so across buckets the others are woken instead (a
spurious wake-up, which musl's users of FUTEX_REQUEUE tolerate) */
static long futex_requeue(uint64_t address, int count, int requeue, uint64_t address2, int compare, uint32_t value)
{
	struct futex_bucket *bucket = futex_bucket_of(address);
	struct futex_bucket *bucket2 = futex_bucket_of(address2);
	struct futex_waiter **link;
	long done = 0;
	int moved = 0;

	pthread_mutex_lock(&bucket->lock);
	if (compare && __atomic_load_n(GUEST(volatile uint32_t *, address), __ATOMIC_SEQ_CST) != value)
	{
		pthread_mutex_unlock(&bucket->lock);
		return -L_EAGAIN;
	}
	link = &bucket->waiters;
	while (*link)
	{
		struct futex_waiter *waiter = *link;

		if (waiter->address != address)
		{
			link = &waiter->next;
			continue;
		}
		if (done < count || (bucket2 != bucket && moved < requeue))
		{
			*link = waiter->next;
			waiter->woken = 1;
			pthread_cond_signal(&waiter->condition);
			if (done < count)
				done++;
			else
				moved++;
			continue;
		}
		if (moved < requeue)
		{
			waiter->address = address2;
			moved++;
		}
		link = &waiter->next;
	}
	pthread_mutex_unlock(&bucket->lock);
	return done + moved;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	int command = operation & ~(L_FUTEX_PRIVATE | L_FUTEX_CLOCK_REALTIME);
	struct timespec host_timeout;

	(void)value3;
	pthread_once(&futex_once, futex_initialize);
	switch (command)
	{
	case L_FUTEX_WAIT:
		return futex_wait(address, value, timespec_in(timeout, &host_timeout) ? &host_timeout : NULL, 0, 0);
	case L_FUTEX_WAIT_BITSET:
		return futex_wait(address, value, timespec_in(timeout, &host_timeout) ? &host_timeout : NULL, 1,
			(operation & L_FUTEX_CLOCK_REALTIME) != 0);
	case L_FUTEX_WAKE:
	case L_FUTEX_WAKE_BITSET:
		return futex_wake(address, (int)value);
	case L_FUTEX_REQUEUE:
		return futex_requeue(address, (int)value, (int)(uint32_t)timeout, address2, 0, 0);
	case L_FUTEX_CMP_REQUEUE:
		return futex_requeue(address, (int)value, (int)(uint32_t)timeout, address2, 1, value3);
	default:
		host_logf(HOST_LOG_WARN, "guest futex operation %d is not supported", operation);
		return -L_ENOSYS;
	}
}

/* ---------- thread ids */

static __thread long thread_id;
static long next_thread_id = 1000;

static long guest_gettid(void)
{
	if (!thread_id)
		thread_id = __sync_add_and_fetch(&next_thread_id, 1);
	return thread_id;
}

/* ---------- files */

static long guest_open(int dirfd, uint64_t path, int flags, int mode)
{
	char buffer[PATH_SIZE];
	int for_write = (flags & L_O_ACCMODE) != 0 || (flags & L_O_CREAT);
	const char *host_path = path_in(dirfd, path, buffer, for_write);
	int fd;

	if (!host_path)
		return -L_ENOENT;
	fd = open(host_path, open_flags_in(flags), mode);
	if (fd < 0)
	{
		int error = errno;

		if (for_write || error != ENOENT)
			host_logf(HOST_LOG_WARN, "open %s (%s): %s", GUEST(const char *, path), host_path, strerror(error));
		return -host_linux_errno(error);
	}
	return fd;
}

static long guest_stat_path(int dirfd, uint64_t path, uint64_t result, int flags)
{
	char buffer[PATH_SIZE];
	const char *host_path;
	struct stat st;

	if ((flags & L_AT_EMPTY_PATH) && GUEST(const char *, path) && !*GUEST(const char *, path))
	{
		if (fstat(dirfd, &st) != 0)
			return -host_linux_errno(errno);
		stat_out(result, &st);
		return 0;
	}
	host_path = path_in(dirfd, path, buffer, 0);
	if (!host_path)
		return -L_ENOENT;
	if (((flags & L_AT_SYMLINK_NOFOLLOW) ? lstat(host_path, &st) : stat(host_path, &st)) != 0)
		return -host_linux_errno(errno);
	stat_out(result, &st);
	return 0;
}

static long guest_fcntl(int fd, int command, uint64_t argument)
{
	switch (command)
	{
	case L_F_DUPFD:
		return result_of(fcntl(fd, F_DUPFD, (int)argument));
	case L_F_DUPFD_CLOEXEC:
		return result_of(fcntl(fd, F_DUPFD_CLOEXEC, (int)argument));
	case L_F_GETFD:
		return result_of(fcntl(fd, F_GETFD));
	case L_F_SETFD:
		return result_of(fcntl(fd, F_SETFD, (argument & 1) ? FD_CLOEXEC : 0));
	case L_F_GETFL:
	{
		int flags = fcntl(fd, F_GETFL);

		return flags < 0 ? -host_linux_errno(errno) : open_flags_out(flags);
	}
	case L_F_SETFL:
		return result_of(fcntl(fd, F_SETFL, open_flags_in((int)argument & (L_O_APPEND | L_O_NONBLOCK))));
	case L_F_GETLK:
	case L_F_SETLK:
	case L_F_SETLKW:
		/* one process: record locks always succeed */
		return 0;
	default:
		return -L_EINVAL;
	}
}

/* ---------- dispatch */

static int logged_unsupported[1024];

static long long dispatch(long long number, long long a, long long b, long long c, long long d, long long e,
	long long f);

/* HALO_PS4_TRACE_SYSCALLS=1 logs every system call the guest makes (debug
level; see DEBUG.md) */
static int trace_state = -1;

long long host_syscall(long long number, long long a, long long b, long long c, long long d, long long e, long long f)
{
	long long result;

	if (trace_state < 0)
	{
		const char *text = getenv("HALO_PS4_TRACE_SYSCALLS");

		trace_state = text && *text && *text != '0';
	}
	result = dispatch(number, a, b, c, d, e, f);
	if (trace_state)
	{
		host_logf(HOST_LOG_INFO, "syscall %lld(%llx, %llx, %llx, %llx, %llx, %llx) = %lld (%llx)",
			number & ~(long long)LINUX_X32_SYSCALL_BIT, a, b, c, d, e, f, result, result);
	}
	return result;
}

static long long dispatch(long long number, long long a, long long b, long long c, long long d, long long e,
	long long f)
{
	char buffer[PATH_SIZE], buffer2[PATH_SIZE];

	number &= ~(long long)LINUX_X32_SYSCALL_BIT;
	switch (number)
	{
	/* ----- input and output */
	case LX_read:
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case LX_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case LX_pread64:
		return result_of(pread((int)a, GUEST(void *, b), (size_t)(uint32_t)c, (off_t)d));
	case LX_pwrite64:
		return result_of(pwrite((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, (off_t)d));
	case LX_readv:
		return guest_rw_vector((int)a, (uint64_t)b, (int)c, 0, 0, 0);
	case LX_writev:
		return guest_rw_vector((int)a, (uint64_t)b, (int)c, 0, 0, 1);
	case LX_preadv:
		return guest_rw_vector((int)a, (uint64_t)b, (int)c, d, 1, 0);
	case LX_pwritev:
		return guest_rw_vector((int)a, (uint64_t)b, (int)c, d, 1, 1);
	case LX_lseek:
		return result_of((long)lseek((int)a, (off_t)b, (int)c));
	case LX_close:
		/* the standard streams are the log's */
		if (a <= 2)
			return 0;
		return result_of(close((int)a));
	case LX_ioctl:
		/* no terminals (musl asks, to line-buffer stdout) */
		return -L_ENOTTY;
	case LX_fcntl:
		return guest_fcntl((int)a, (int)b, (uint64_t)c);
	case LX_dup:
		return result_of(dup((int)a));
	case LX_dup2:
		return result_of(dup2((int)a, (int)b));
	case LX_dup3:
	{
		long result = result_of(dup2((int)a, (int)b));

		if (result >= 0 && (c & L_O_CLOEXEC))
			fcntl((int)b, F_SETFD, FD_CLOEXEC);
		return result;
	}
	case LX_fsync:
	case LX_fdatasync:
		return result_of(fsync((int)a));
	case LX_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case LX_flock:
		return 0;
	case LX_fstat:
	{
		struct stat st;

		if (fstat((int)a, &st) != 0)
			return -host_linux_errno(errno);
		stat_out((uint64_t)b, &st);
		return 0;
	}

	/* ----- paths */
	case LX_open:
		return guest_open(L_AT_FDCWD, (uint64_t)a, (int)b, (int)c);
	case LX_openat:
		return guest_open((int)a, (uint64_t)b, (int)c, (int)d);
	case LX_creat:
		return guest_open(L_AT_FDCWD, (uint64_t)a, L_O_CREAT | 1 | L_O_TRUNC, (int)b);
	case LX_stat:
		return guest_stat_path(L_AT_FDCWD, (uint64_t)a, (uint64_t)b, 0);
	case LX_lstat:
		return guest_stat_path(L_AT_FDCWD, (uint64_t)a, (uint64_t)b, L_AT_SYMLINK_NOFOLLOW);
	case LX_newfstatat:
		return guest_stat_path((int)a, (uint64_t)b, (uint64_t)c, (int)d);
	case LX_access:
	case LX_faccessat:
	case LX_faccessat2:
	{
		int dirfd = number == LX_access ? L_AT_FDCWD : (int)a;
		uint64_t path = number == LX_access ? (uint64_t)a : (uint64_t)b;
		int mode = number == LX_access ? (int)b : (int)c;
		const char *host_path = path_in(dirfd, path, buffer, (mode & W_OK) != 0);

		if (!host_path)
			return -L_ENOENT;
		return result_of(access(host_path, mode));
	}
	case LX_mkdir:
	case LX_mkdirat:
	{
		int dirfd = number == LX_mkdir ? L_AT_FDCWD : (int)a;
		uint64_t path = number == LX_mkdir ? (uint64_t)a : (uint64_t)b;
		int mode = number == LX_mkdir ? (int)b : (int)c;
		const char *host_path = path_in(dirfd, path, buffer, 1);

		if (!host_path)
			return -L_ENOENT;
		return result_of(mkdir(host_path, (mode_t)mode));
	}
	case LX_rmdir:
	case LX_unlink:
	case LX_unlinkat:
	{
		int dirfd = number == LX_unlinkat ? (int)a : L_AT_FDCWD;
		uint64_t path = number == LX_unlinkat ? (uint64_t)b : (uint64_t)a;
		int directory = number == LX_rmdir || (number == LX_unlinkat && (c & L_AT_REMOVEDIR));
		const char *host_path = path_in(dirfd, path, buffer, 1);

		if (!host_path)
			return -L_ENOENT;
		return result_of(directory ? rmdir(host_path) : unlink(host_path));
	}
	case LX_rename:
	case LX_renameat:
	case LX_renameat2:
	{
		int renameat_form = number != LX_rename;
		const char *from = path_in(renameat_form ? (int)a : L_AT_FDCWD, renameat_form ? (uint64_t)b : (uint64_t)a,
			buffer, 1);
		const char *to = path_in(renameat_form ? (int)c : L_AT_FDCWD, renameat_form ? (uint64_t)d : (uint64_t)b,
			buffer2, 1);

		if (!from || !to)
			return -L_ENOENT;
		return result_of(rename(from, to));
	}
	case LX_chmod:
	case LX_fchmodat:
	{
		const char *host_path = number == LX_chmod ? path_in(L_AT_FDCWD, (uint64_t)a, buffer, 1) :
			path_in((int)a, (uint64_t)b, buffer, 1);

		if (!host_path)
			return -L_ENOENT;
		return result_of(chmod(host_path, (mode_t)(number == LX_chmod ? b : c)));
	}
	case LX_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case LX_utimensat:
	{
		struct timespec times[2];
		const char *host_path;

		if (!b)
			return -L_EINVAL;
		host_path = path_in((int)a, (uint64_t)b, buffer, 1);
		if (!host_path)
			return -L_ENOENT;
		if (c)
		{
			timespec_in((uint64_t)c, &times[0]);
			timespec_in((uint64_t)c + sizeof(struct guest_timespec), &times[1]);
		}
		return result_of(utimensat(AT_FDCWD, host_path, c ? times : NULL, 0));
	}
	case LX_getcwd:
	{
		const char *cwd = host_files_guest_cwd();
		size_t length = strlen(cwd) + 1;

		if (length > (size_t)(uint32_t)b)
			return -L_ERANGE;
		memcpy(GUEST(char *, a), cwd, length);
		return (long)length;
	}
	case LX_chdir:
		return host_files_set_guest_cwd(GUEST(const char *, a)) == 0 ? 0 : -L_ENOENT;
	case LX_readlink:
	case LX_readlinkat:
		return -L_EINVAL;
	case LX_umask:
		return 022;
	case LX_getdents64:
	case LX_getdents:
	case LX_statx:
	case LX_statfs:
	case LX_fstatfs:
		return -L_ENOSYS;

	/* ----- time */
	case LX_clock_gettime:
	case LX_clock_getres:
	{
		struct timespec value;
		int result = number == LX_clock_gettime ? clock_gettime(clock_in((int)a), &value) :
			clock_getres(clock_in((int)a), &value);

		if (result != 0)
			return -host_linux_errno(errno);
		timespec_out((uint64_t)b, &value);
		return 0;
	}
	case LX_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case LX_time:
	{
		time_t now = time(NULL);

		if (a)
			*GUEST(int32_t *, a) = (int32_t)now;
		return (long)now;
	}
	case LX_nanosleep:
	{
		struct timespec request;

		if (!timespec_in((uint64_t)a, &request))
			return -L_EFAULT;
		while (nanosleep(&request, &request) != 0 && errno == EINTR)
			;
		return 0;
	}
	case LX_clock_nanosleep:
	{
		struct timespec request;

		if (!timespec_in((uint64_t)c, &request))
			return -L_EFAULT;
		if (b & 1)
		{
			/* TIMER_ABSTIME: sleep until the deadline on that clock */
			struct timespec now;
			int64_t delta;

			clock_gettime(clock_in((int)a), &now);
			delta = (request.tv_sec - now.tv_sec) * 1000000000LL + (request.tv_nsec - now.tv_nsec);
			if (delta <= 0)
				return 0;
			request.tv_sec = (time_t)(delta / 1000000000LL);
			request.tv_nsec = (long)(delta % 1000000000LL);
		}
		while (nanosleep(&request, &request) != 0 && errno == EINTR)
			;
		return 0;
	}
	case LX_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case LX_sched_yield:
		sched_yield();
		return 0;
	case LX_pause:
		for (;;)
			sleep(3600);

	/* ----- memory */
	case LX_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case LX_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case LX_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case LX_madvise:
	case LX_msync:
		return 0;
	case LX_mremap:
	case LX_brk:
		/* musl then falls back to mmap and copying */
		return -L_ENOMEM;

	/* ----- the process */
	case LX_exit:
	case LX_exit_group:
		host_exit((int)a);
	case LX_getpid:
		return 1000;
	case LX_getppid:
		return 1;
	case LX_gettid:
	case LX_set_tid_address:
		return guest_gettid();
	case LX_getuid:
	case LX_geteuid:
	case LX_getgid:
	case LX_getegid:
		return 0;
	case LX_set_robust_list:
	case LX_rt_sigaction:
	case LX_rt_sigprocmask:
	case LX_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case LX_kill:
	case LX_tkill:
	case LX_tgkill:
	{
		int signal_number = (int)(number == LX_tgkill ? c : b);

		if (signal_number == 0)
			return 0;
		host_logf(HOST_LOG_FATAL, "the game raised signal %d", signal_number);
		host_abort("guest signal");
	}
	case LX_sched_getaffinity:
	{
		/* the cores a PS4 game owns (six); as many as the host has on
		Linux, up to 64 */
		long cores = 6;
		uint64_t mask;

#if !defined(__ORBIS__)
		cores = sysconf(_SC_NPROCESSORS_ONLN);
		if (cores < 1)
			cores = 1;
		if (cores > 64)
			cores = 64;
#endif
		mask = cores == 64 ? ~0ULL : (1ULL << cores) - 1;
		if ((uint32_t)b < sizeof(mask))
			return -L_EINVAL;
		memcpy(GUEST(void *, c), &mask, sizeof(mask));
		return sizeof(mask);
	}
	case LX_getrlimit:
	case LX_prlimit64:
	{
		/* unlimited (the kernel's x32 struct rlimit has 64-bit fields) */
		uint64_t address = number == LX_getrlimit ? (uint64_t)b : (uint64_t)d;
		uint64_t *limits = GUEST(uint64_t *, address);

		if (number == LX_prlimit64 && c)
			return 0;
		if (limits)
		{
			limits[0] = ~0ULL;
			limits[1] = ~0ULL;
		}
		return 0;
	}
	case LX_uname:
	{
		/* six 65-byte fields */
		char *fields = GUEST(char *, a);
		static const char *const values[6] = { "Linux", "halo", "5.0.0", "#1", "x86_64", "" };
		int index;

		memset(fields, 0, 6 * 65);
		for (index = 0; index < 6; index++)
			strncpy(fields + index * 65, values[index], 64);
		return 0;
	}
	case LX_getrandom:
	{
		static uint64_t state;
		unsigned char *out = GUEST(unsigned char *, a);
		size_t index;

		if (!state)
			state = host_time_microseconds() * 6364136223846793005ULL + 1442695040888963407ULL;
		for (index = 0; index < (size_t)(uint32_t)b; index++)
		{
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			out[index] = (unsigned char)state;
		}
		return (uint32_t)b;
	}
	case LX_sysinfo:
	default:
		if (number >= 0 && number < 1024 && !logged_unsupported[number])
		{
			logged_unsupported[number] = 1;
			host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		}
		return -L_ENOSYS;
	}
}
