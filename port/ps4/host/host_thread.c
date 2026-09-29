/*
HOST_THREAD.C

Threads that run guest code.

x32 code keeps the stack pointer in 32-bit registers, so guest code must run
on a stack below 4 GB. Rather than handing pthreads stacks of our own
(libkernel's thread library may want its own), each host thread keeps its
ordinary stack and gets a second one in guest memory, which host_call_guest
switches to (host_call_on_stack below). Guest code that calls back into the
host stays on the guest stack, so host functions called by the guest must
not need more than the guest stack provides; the guest stacks are generous
(1 MB and up, with a guard page) for that reason.

The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS (host_get_tp/host_set_tp).

On the PS4 every thread is limited to the six cores a game owns (mask
0x3f).
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__ORBIS__)
#include <orbis/libkernel.h> /* scePthreadSetaffinity is declared without a prototype there */
#endif

#define GUARD_SIZE 0x10000
#define DEFAULT_GUEST_STACK (1024 * 1024)
#define MINIMUM_GUEST_STACK (256 * 1024)

static __thread uint32_t guest_tp;
static __thread uint64_t guest_stack_low, guest_stack_high;
static __thread size_t guest_stack_wanted;
static __thread void *guest_stack_mapping;
static __thread size_t guest_stack_mapping_size;
static volatile uint32_t thread_count;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

uint32_t host_thread_count(void)
{
	return thread_count;
}

void host_thread_guest_stack(uint64_t *low, uint64_t *high)
{
	*low = guest_stack_low;
	*high = guest_stack_high;
}

/* ---------- switching stacks */

/* calls function(argument) with the stack pointer at top (16-byte aligned)
and returns on the original stack */
void host_call_on_stack(uint64_t top, void (*function)(void *), void *argument);

__asm__(
	".text\n"
	".globl host_call_on_stack\n"
	".type host_call_on_stack, @function\n"
	"host_call_on_stack:\n"
	"\tpushq %rbp\n"
	"\tmovq %rsp, %rbp\n"
	"\tmovq %rdi, %rsp\n"
	"\tmovq %rdx, %rdi\n"
	"\tcallq *%rsi\n"
	"\tmovq %rbp, %rsp\n"
	"\tpopq %rbp\n"
	"\tretq\n"
	".size host_call_on_stack, .-host_call_on_stack\n");

int host_in_guest(void)
{
	uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);

	return sp < 0x100000000ULL;
}

static int ensure_guest_stack(void)
{
	size_t size, total;
	void *base;

	if (guest_stack_high)
		return 0;
	size = guest_stack_wanted ? guest_stack_wanted : DEFAULT_GUEST_STACK;
	if (size < MINIMUM_GUEST_STACK)
		size = MINIMUM_GUEST_STACK;
	size = (size + 0xffff) & ~(size_t)0xffff;
	total = size + GUARD_SIZE;
	base = host_low_map(total, PROT_READ | PROT_WRITE);
	if (!base)
		return -1;
	/* guard at the bottom */
	host_guest_mprotect((uint64_t)(uintptr_t)base, GUARD_SIZE, PROT_NONE);
	guest_stack_mapping = base;
	guest_stack_mapping_size = total;
	guest_stack_low = (uint64_t)(uintptr_t)base + GUARD_SIZE;
	guest_stack_high = (uint64_t)(uintptr_t)base + total;
	return 0;
}

static void release_guest_stack(void)
{
	if (!guest_stack_mapping)
		return;
	host_low_unmap(guest_stack_mapping, guest_stack_mapping_size);
	guest_stack_mapping = NULL;
	guest_stack_low = guest_stack_high = 0;
}

/* ---------- calling into the guest */

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

struct guest_call
{
	uint32_t function, a, b, c, d;
	uint32_t result;
};

static void call_thunk(void *context)
{
	struct guest_call *call = context;

	/* a host thread entering guest code needs a guest thread (TLS, errno)
	first; __guest_thread_start sets up its own */
	if (!guest_tp && call->function != host_image.header->thread_start)
		((uint32_t (*)(void))(uintptr_t)host_image.header->thread_attach)();
	call->result = ((guest_function)(uintptr_t)call->function)(call->a, call->b, call->c, call->d);
}

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	struct guest_call call = { function, a, b, c, d, 0 };

	if (host_in_guest())
	{
		/* a host function the guest called is calling back */
		call_thunk(&call);
		return call.result;
	}
	if (ensure_guest_stack() != 0)
		host_fatal("out of memory for a guest thread stack");
	host_call_on_stack(guest_stack_high - 64, call_thunk, &call);
	return call.result;
}

static void guest_main_thunk(void *context)
{
	uint32_t boot = (uint32_t)(uintptr_t)context;

	((void (*)(uint32_t))(uintptr_t)host_image.header->start)(boot);
}

void host_run_guest_main(uint32_t boot)
{
	if (ensure_guest_stack() != 0)
		host_fatal("out of memory for the game's stack");
	host_call_on_stack(guest_stack_high - 64, guest_main_thunk, (void *)(uintptr_t)boot);
	host_fatal("the game returned from __guest_start");
}

/* ---------- host threads that can run guest code */

struct thread_start
{
	void *(*function)(void *);
	void *argument;
	size_t guest_stack_size;
	char name[32];
};

static void *thread_main(void *context)
{
	struct thread_start start = *(struct thread_start *)context;

	free(context);
	__sync_add_and_fetch(&thread_count, 1);
	guest_stack_wanted = start.guest_stack_size;
#if defined(__ORBIS__)
	scePthreadRename(scePthreadSelf(), start.name);
	scePthreadSetaffinity(scePthreadSelf(), 0x3f);
#elif defined(__linux__)
	pthread_setname_np(pthread_self(), start.name);
#endif
	start.function(start.argument);
	guest_tp = 0;
	release_guest_stack();
	__sync_sub_and_fetch(&thread_count, 1);
	return NULL;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t guest_stack_size, const char *name)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	pthread_attr_t attributes;
	pthread_t thread;
	int error;

	if (!start)
		return ENOMEM;
	start->function = function;
	start->argument = argument;
	start->guest_stack_size = guest_stack_size;
	strncpy(start->name, name ? name : "halo", sizeof(start->name) - 1);
	pthread_attr_init(&attributes);
	/* host frames are small, but libkernel and Piglet calls can be deep */
	pthread_attr_setstacksize(&attributes, 512 * 1024);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&thread, &attributes, thread_main, start);
	pthread_attr_destroy(&attributes);
	if (error)
	{
		host_logf(HOST_LOG_ERROR, "cannot start thread %s: %s", start->name, strerror(error));
		free(start);
	}
	return error;
}

/* ---------- the guest's threads (guest_thread.c) */

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	/* the game's threads ask for Xbox-sized stacks (64 KB and less); the
	port's code (the renderer, the platform layer) needs more */
	size_t size = stack_size < DEFAULT_GUEST_STACK ? DEFAULT_GUEST_STACK : stack_size;

	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, size, "halo guest");
}
