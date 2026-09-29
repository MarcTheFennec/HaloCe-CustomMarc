/* musl's arch/x32/syscall_arch.h for the PS4 guest.

The guest never executes the syscall instruction: the PS4 kernel is not
Linux, and the structures musl would pass have x32 layouts. Every system call
goes to the runtime's dispatcher (port/android/guest/runtime/guest_syscall.c,
shared with the Android port), which hands it to the host
(port/ps4/host/host_syscall.c). The numbers are musl's x32 ones (with
__X32_SYSCALL_BIT), which the host decodes. */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define __scc(X) sizeof(1?(X):0ULL) < 8 ? (unsigned long) (X) : (long long) (X)
typedef long long syscall_arg_t;

long __guest_syscall(long long, long long, long long, long long, long long, long long, long long);

static __inline long __syscall0(long long n)
{
	return __guest_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long long n, long long a)
{
	return __guest_syscall(n, a, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long long n, long long a, long long b)
{
	return __guest_syscall(n, a, b, 0, 0, 0, 0);
}

static __inline long __syscall3(long long n, long long a, long long b, long long c)
{
	return __guest_syscall(n, a, b, c, 0, 0, 0);
}

static __inline long __syscall4(long long n, long long a, long long b, long long c, long long d)
{
	return __guest_syscall(n, a, b, c, d, 0, 0);
}

static __inline long __syscall5(long long n, long long a, long long b, long long c, long long d, long long e)
{
	return __guest_syscall(n, a, b, c, d, e, 0);
}

static __inline long __syscall6(long long n, long long a, long long b, long long c, long long d, long long e, long long f)
{
	return __guest_syscall(n, a, b, c, d, e, f);
}

#undef SYS_futimesat

/* No SYS_*_time64 aliases (unlike musl's x32): the guest's time_t is 32-bit
(bits/alltypes.h.in), so musl passes 32-bit timespec and timeval structures,
which the host converts, as the Android guest's do. */

#define SYSCALL_NO_TLS 1
#define IPC_64 0
