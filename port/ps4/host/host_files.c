/*
HOST_FILES.C

Where the guest's files are.

The PKG carries no game data. The player copies it (maps/, bitmaps/,
sounds/, ...) to the console over FTP; the guest sees it under /halo
(HALO_DATA_ROOT) and this file maps each guest path onto the first content
root that has it:

	1. /data/halo/<rel>      (internal storage, the write root)
	2. /mnt/usb0/halo/<rel>  (a USB drive)
	3. /app0/<rel>           (the PKG's own files)

On Linux HALO_PS4_ROOTS (colon-separated) replaces the list, for testing.

- '\\' becomes '/', "." and ".." are resolved, relative paths are taken from
  the guest's current directory (/halo at start);
- when the exact name is missing, each path component is looked up
  case-insensitively (the game's data names vary in case between copies);
- settings and logs have their own directories: /halo/config.toml is
  <write root>/config/config.toml, /halo/debug.txt is
  <write root>/logs/debug.txt; saves (/halo/save/...) are
  <write root>/save/...;
- everything written goes to the write root;
- the first resolution of each file logs the root that served it; every
  miss logs the full paths tried (repeats of the same miss are counted and
  logged at debug level);
- resolutions are cached (positive results only; a write drops the entry).

Paths outside /halo are left alone.
*/

#include "host.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT_MAXIMUM 8
#define PATH_SIZE 1024
#define CACHE_BUCKETS 4096

static char *roots[ROOT_MAXIMUM];
static int root_count;
static char guest_cwd[PATH_SIZE] = HALO_GUEST_DATA_ROOT;
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;

struct cache_entry
{
	struct cache_entry *next;
	char *relative;
	char *host_path;
	int root;
};

struct miss_entry
{
	struct miss_entry *next;
	char *relative;
	unsigned count;
};

static struct cache_entry *cache[CACHE_BUCKETS];
static struct miss_entry *misses[CACHE_BUCKETS];
static unsigned long served_count[ROOT_MAXIMUM], miss_count;

static void add_root(const char *path)
{
	size_t length = strlen(path);

	if (!length || root_count == ROOT_MAXIMUM)
		return;
	while (length > 1 && path[length - 1] == '/')
		length--;
	roots[root_count] = malloc(length + 1);
	memcpy(roots[root_count], path, length);
	roots[root_count][length] = 0;
	root_count++;
}

static int make_directories(const char *path)
{
	char buffer[PATH_SIZE];
	size_t index;

	snprintf(buffer, sizeof(buffer), "%s", path);
	for (index = 1; buffer[index]; index++)
	{
		if (buffer[index] != '/')
			continue;
		buffer[index] = 0;
		mkdir(buffer, 0777);
		buffer[index] = '/';
	}
	return mkdir(buffer, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

void host_files_initialize(void)
{
	static const char *const writable[] = { "", "/config", "/logs", "/save", "/maps" };
	const char *list = getenv("HALO_PS4_ROOTS");
	char path[PATH_SIZE];
	unsigned index;

#if !defined(__ORBIS__)
	if (list && *list)
	{
		char *copy = strdup(list), *token, *state = NULL;

		for (token = strtok_r(copy, ":", &state); token; token = strtok_r(NULL, ":", &state))
			add_root(token);
		free(copy);
	}
#else
	(void)list;
#endif
	if (!root_count)
	{
		add_root("/data/halo");
		add_root("/mnt/usb0/halo");
		add_root("/app0");
	}
	/* the write root and its directories */
	for (index = 0; index < sizeof(writable) / sizeof(writable[0]); index++)
	{
		snprintf(path, sizeof(path), "%s%s", roots[0], writable[index]);
		if (make_directories(path) != 0)
			host_logf(HOST_LOG_ERROR, "cannot create %s: %s", path, strerror(errno));
	}
	snprintf(path, sizeof(path), "%s/logs", roots[0]);
	host_log_open_file(path);
	for (index = 0; index < (unsigned)root_count; index++)
	{
		struct stat st;
		int present = stat(roots[index], &st) == 0 && S_ISDIR(st.st_mode);

		host_logf(HOST_LOG_INFO, "content root %u: %s%s%s", index + 1, roots[index],
			index == 0 ? " (write root)" : "", present ? "" : " (not present)");
	}
}

const char *host_files_write_root(void)
{
	return roots[0];
}

/* ---------- guest paths */

/* normalizes a guest path into an absolute one: separators, ".", "..",
the current directory */
static void normalize(const char *path, char *result, size_t size)
{
	char joined[PATH_SIZE * 2];
	char *component, *state = NULL;
	size_t length = 0;

	if (path[0] == '/' || path[0] == '\\')
		snprintf(joined, sizeof(joined), "%s", path);
	else
		snprintf(joined, sizeof(joined), "%s/%s", guest_cwd, path);
	for (component = joined; *component; component++)
	{
		if (*component == '\\')
			*component = '/';
	}
	result[0] = 0;
	for (component = strtok_r(joined, "/", &state); component; component = strtok_r(NULL, "/", &state))
	{
		if (!strcmp(component, "."))
			continue;
		if (!strcmp(component, ".."))
		{
			char *slash = strrchr(result, '/');

			if (slash)
				*slash = 0;
			length = strlen(result);
			continue;
		}
		length += (size_t)snprintf(result + length, size - length, "/%s", component);
		if (length >= size)
		{
			length = size - 1;
			break;
		}
	}
	if (!result[0])
		snprintf(result, size, "/");
}

/* the path relative to the data root, or NULL if outside it; with the
settings' and logs' own directories */
static const char *relative_of(const char *absolute, char *buffer, size_t size)
{
	size_t root_length = strlen(HALO_GUEST_DATA_ROOT);
	const char *relative;

	if (strncmp(absolute, HALO_GUEST_DATA_ROOT, root_length) ||
		(absolute[root_length] && absolute[root_length] != '/'))
		return NULL;
	relative = absolute + root_length;
	while (*relative == '/')
		relative++;
	if (!strcasecmp(relative, "config.toml"))
		relative = "config/config.toml";
	else if (!strcasecmp(relative, "debug.txt"))
		relative = "logs/debug.txt";
	snprintf(buffer, size, "%s", relative);
	return buffer;
}

/* ---------- lookups */

static unsigned hash_of(const char *text)
{
	unsigned hash = 2166136261u;

	while (*text)
		hash = (hash ^ (unsigned char)tolower((unsigned char)*text++)) * 16777619u;
	return hash & (CACHE_BUCKETS - 1);
}

static int exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* finds root/relative with each component matched case-insensitively
where the exact name is missing; writes the real path into result */
static int find_case_insensitive(const char *root, const char *relative, char *result, size_t size)
{
	char copy[PATH_SIZE];
	char *component, *state = NULL;
	size_t length;

	snprintf(result, size, "%s", root);
	snprintf(copy, sizeof(copy), "%s", relative);
	length = strlen(result);
	for (component = strtok_r(copy, "/", &state); component; component = strtok_r(NULL, "/", &state))
	{
		DIR *directory;
		struct dirent *entry;
		int found = 0;

		snprintf(result + length, size - length, "/%s", component);
		if (exists(result))
		{
			length = strlen(result);
			continue;
		}
		result[length] = 0;
		directory = opendir(result);
		if (!directory)
			return 0;
		while ((entry = readdir(directory)) != NULL)
		{
			if (!strcasecmp(entry->d_name, component))
			{
				snprintf(result + length, size - length, "/%s", entry->d_name);
				found = 1;
				break;
			}
		}
		closedir(directory);
		if (!found)
			return 0;
		length = strlen(result);
	}
	return 1;
}

static struct cache_entry *cache_find(const char *relative)
{
	struct cache_entry *entry;

	for (entry = cache[hash_of(relative)]; entry; entry = entry->next)
	{
		if (!strcmp(entry->relative, relative))
			return entry;
	}
	return NULL;
}

static void cache_drop(const char *relative)
{
	struct cache_entry **link = &cache[hash_of(relative)];

	while (*link)
	{
		struct cache_entry *entry = *link;

		if (!strcmp(entry->relative, relative))
		{
			*link = entry->next;
			free(entry->relative);
			free(entry->host_path);
			free(entry);
			return;
		}
		link = &entry->next;
	}
}

static void cache_add(const char *relative, const char *host_path, int root)
{
	struct cache_entry *entry = calloc(1, sizeof(*entry));
	unsigned hash = hash_of(relative);

	entry->relative = strdup(relative);
	entry->host_path = strdup(host_path);
	entry->root = root;
	entry->next = cache[hash];
	cache[hash] = entry;
}

static void log_miss(const char *guest_path, const char *relative)
{
	struct miss_entry *entry;
	unsigned hash = hash_of(relative);
	char tried[PATH_SIZE * ROOT_MAXIMUM];
	size_t length = 0;
	int index;

	miss_count++;
	for (entry = misses[hash]; entry; entry = entry->next)
	{
		if (!strcmp(entry->relative, relative))
		{
			entry->count++;
			host_logf(HOST_LOG_DEBUG, "file not found (again, %u times): %s", entry->count, guest_path);
			return;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->relative = strdup(relative);
	entry->count = 1;
	entry->next = misses[hash];
	misses[hash] = entry;
	tried[0] = 0;
	for (index = 0; index < root_count; index++)
		length += (size_t)snprintf(tried + length, sizeof(tried) - length, "%s%s/%s", index ? ", " : "",
			roots[index], relative);
	host_logf(HOST_LOG_WARN, "file not found: %s (tried %s)", guest_path, tried);
}

char *host_files_resolve(const char *guest_path, char *buffer, size_t size, int for_write)
{
	char absolute[PATH_SIZE], relative_buffer[PATH_SIZE], candidate[PATH_SIZE];
	const char *relative;
	struct cache_entry *entry;
	int index;

	normalize(guest_path, absolute, sizeof(absolute));
	relative = relative_of(absolute, relative_buffer, sizeof(relative_buffer));
	if (!relative)
	{
		snprintf(buffer, size, "%s", absolute);
		return buffer;
	}
	if (!*relative)
	{
		snprintf(buffer, size, "%s", roots[0]);
		return buffer;
	}

	pthread_mutex_lock(&files_lock);
	if (for_write)
	{
		/* written files live in the write root, whatever served them
		before */
		cache_drop(relative);
		snprintf(buffer, size, "%s/%s", roots[0], relative);
		if (!exists(buffer) && find_case_insensitive(roots[0], relative, candidate, sizeof(candidate)))
			snprintf(buffer, size, "%s", candidate);
		pthread_mutex_unlock(&files_lock);
		return buffer;
	}
	entry = cache_find(relative);
	if (entry)
	{
		snprintf(buffer, size, "%s", entry->host_path);
		pthread_mutex_unlock(&files_lock);
		return buffer;
	}
	for (index = 0; index < root_count; index++)
	{
		snprintf(candidate, sizeof(candidate), "%s/%s", roots[index], relative);
		if (exists(candidate) || find_case_insensitive(roots[index], relative, candidate, sizeof(candidate)))
		{
			cache_add(relative, candidate, index);
			served_count[index]++;
			host_logf(HOST_LOG_INFO, "file %s: root %d (%s)", relative, index + 1, candidate);
			snprintf(buffer, size, "%s", candidate);
			pthread_mutex_unlock(&files_lock);
			return buffer;
		}
	}
	log_miss(guest_path, relative);
	pthread_mutex_unlock(&files_lock);
	snprintf(buffer, size, "%s/%s", roots[0], relative);
	return buffer;
}

int host_files_exists(const char *guest_path)
{
	char absolute[PATH_SIZE], relative_buffer[PATH_SIZE], candidate[PATH_SIZE];
	const char *relative;
	int index;

	normalize(guest_path, absolute, sizeof(absolute));
	relative = relative_of(absolute, relative_buffer, sizeof(relative_buffer));
	if (!relative)
		return exists(absolute);
	for (index = 0; index < root_count; index++)
	{
		snprintf(candidate, sizeof(candidate), "%s/%s", roots[index], relative);
		if (exists(candidate) || find_case_insensitive(roots[index], relative, candidate, sizeof(candidate)))
			return 1;
	}
	return 0;
}

const char *host_files_guest_cwd(void)
{
	return guest_cwd;
}

int host_files_set_guest_cwd(const char *guest_path)
{
	char absolute[PATH_SIZE];

	if (!guest_path)
		return -1;
	normalize(guest_path, absolute, sizeof(absolute));
	pthread_mutex_lock(&files_lock);
	snprintf(guest_cwd, sizeof(guest_cwd), "%s", absolute);
	pthread_mutex_unlock(&files_lock);
	return 0;
}

void host_files_log_statistics(void)
{
	char text[256];
	size_t length = 0;
	int index;

	for (index = 0; index < root_count; index++)
		length += (size_t)snprintf(text + length, sizeof(text) - length, "%sroot %d: %lu", index ? ", " : "",
			index + 1, served_count[index]);
	host_logf(HOST_LOG_INFO, "files served: %s; misses: %lu", text, miss_count);
}

/* ---------- port/linux/src/posix_files.c, built into the host with
host_fs_redirect.h: its path arguments are guest paths */

#include <sys/statvfs.h>

int host_fs_stat(const char *path, struct stat *st)
{
	char buffer[PATH_SIZE];

	return stat(host_files_resolve(path, buffer, sizeof(buffer), 0), st);
}

int host_fs_chmod(const char *path, mode_t mode)
{
	char buffer[PATH_SIZE];

	return chmod(host_files_resolve(path, buffer, sizeof(buffer), 1), mode);
}

int host_fs_mkdir(const char *path, mode_t mode)
{
	char buffer[PATH_SIZE];

	return mkdir(host_files_resolve(path, buffer, sizeof(buffer), 1), mode);
}

DIR *host_fs_opendir(const char *path)
{
	char buffer[PATH_SIZE];

	return opendir(host_files_resolve(path, buffer, sizeof(buffer), 0));
}

int host_fs_utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
	char buffer[PATH_SIZE];

	(void)dirfd;
	return utimensat(AT_FDCWD, host_files_resolve(path, buffer, sizeof(buffer), 1), times, flags);
}

int host_fs_statvfs(const char *path, struct statvfs *st)
{
#if defined(__ORBIS__)
	/* no statvfs on Orbis: report plenty of room (saves are small) */
	(void)path;
	memset(st, 0, sizeof(*st));
	st->f_bsize = st->f_frsize = 4096;
	st->f_blocks = (fsblkcnt_t)(64ULL << 30) / 4096;
	st->f_bavail = st->f_bfree = (fsblkcnt_t)(16ULL << 30) / 4096;
	return 0;
#else
	char buffer[PATH_SIZE];

	return statvfs(host_files_resolve(path, buffer, sizeof(buffer), 1), st);
#endif
}
