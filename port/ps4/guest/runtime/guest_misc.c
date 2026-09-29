/*
GUEST_MISC.C

Runtime support functions the PS4 guest's code (clang's x32 target, the
platform layer) expects, which neither the game nor musl provides.
*/

#include "guest_host.h"

#include <stddef.h>

void __stack_chk_fail(void)
{
	host_abort("stack smashing detected");
}

/* <execinfo.h>: the guest has no unwinder (the host logs guest-relative
fault addresses instead, host_memory.c) */
int backtrace(void **frames, int size)
{
	(void)frames;
	(void)size;
	return 0;
}

void backtrace_symbols_fd(void *const *frames, int size, int fd)
{
	(void)frames;
	(void)size;
	(void)fd;
}
