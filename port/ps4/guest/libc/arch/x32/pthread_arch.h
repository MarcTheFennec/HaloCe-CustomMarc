/* musl's arch/x32/pthread_arch.h for the PS4 guest.

%fs belongs to the host (libkernel keeps its own thread control block
there), so the guest never reads it: each thread's struct pthread is kept by
the host (host_get_tp) and handed out by the shared guest runtime
(port/android/guest/runtime/guest_thread.c). */
uintptr_t __guest_get_tp(void);

static inline uintptr_t __get_tp()
{
	return __guest_get_tp();
}

#define MC_PC gregs[REG_RIP]

#define CANARY_PAD

#define tls_mod_off_t unsigned long long
