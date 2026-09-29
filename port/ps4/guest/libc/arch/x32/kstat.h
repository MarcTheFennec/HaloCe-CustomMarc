/* the x86-64 Linux kernel's struct stat (fstat, newfstatat), spelled with
fixed-width types; the PS4 host fills it in from its own stat
(port/ps4/host/host_syscall.c) */
struct kstat {
	unsigned long long st_dev;
	unsigned long long st_ino;
	unsigned long long st_nlink;
	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	unsigned long long st_rdev;
	long long st_size;
	long long st_blksize;
	long long st_blocks;
	long long st_atime_sec;
	long long st_atime_nsec;
	long long st_mtime_sec;
	long long st_mtime_nsec;
	long long st_ctime_sec;
	long long st_ctime_nsec;
	long long __unused[3];
};
