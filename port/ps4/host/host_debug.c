/*
HOST_DEBUG.C

Logging and termination for the PS4 host.

Lines go to three places: the kernel log (stdout of a homebrew process,
which GoldHEN's klog server shows on TCP port 3232), <data_root>/debug.txt
once the data root is known, and, optionally, a UDP address from the
HALO_PS4_LOG_ADDRESS build setting or config.toml (M2), for a computer on
the same network (nc -ulk 9999).

host_fatal shows the message in a system message dialog so that a player
without any of those sees why the game stopped.
*/

#include "host.h"

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *log_file;
static int log_socket = -1;
static OrbisNetSockaddr log_address;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t log_start_ms;

void host_log_initialize(void)
{
	log_start_ms = host_ticks_ms();
	setvbuf(stdout, NULL, _IOLBF, 0);
}

void host_log_open_file(const char *data_root)
{
	char path[300];

	snprintf(path, sizeof(path), "%s/debug.txt", data_root);
	pthread_mutex_lock(&log_lock);
	if (log_file)
		fclose(log_file);
	log_file = fopen(path, "w");
	pthread_mutex_unlock(&log_lock);
	if (!log_file)
		host_logf(HOST_LOG_WARN, "cannot write %s", path);
	else
		host_logf(HOST_LOG_INFO, "log file %s", path);
}

int host_log_open_udp(const char *address_and_port)
{
	char host[64];
	const char *colon = strrchr(address_and_port, ':');
	int port;
	struct
	{
		uint8_t len;
		uint8_t family;
		uint16_t port;
		uint32_t address;
		uint8_t zero[8];
	} in;

	if (!colon || (size_t)(colon - address_and_port) >= sizeof(host))
		return -1;
	memcpy(host, address_and_port, colon - address_and_port);
	host[colon - address_and_port] = 0;
	port = atoi(colon + 1);
	if (port <= 0 || port > 65535)
		return -1;

	memset(&in, 0, sizeof(in));
	in.len = sizeof(in);
	in.family = ORBIS_NET_AF_INET;
	in.port = sceNetHtons((uint16_t)port);
	if (sceNetInetPton(ORBIS_NET_AF_INET, host, &in.address) != 1)
		return -1;
	log_socket = sceNetSocket("halo_log", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, 0);
	if (log_socket < 0)
		return -1;
	memcpy(&log_address, &in, sizeof(log_address));
	host_logf(HOST_LOG_INFO, "log copies to udp %s", address_and_port);
	return 0;
}

void host_log(int priority, const char *text)
{
	static const char *const names[] = {"", "", "", "", "info", "warn", "error"};
	char line[1200];
	uint64_t now = host_ticks_ms() - log_start_ms;
	int length;

	if (priority < 0 || priority > HOST_LOG_ERROR)
		priority = HOST_LOG_INFO;
	length = snprintf(line, sizeof(line), "[%6llu.%03llu] %-5s %s\n", (unsigned long long)(now / 1000),
		(unsigned long long)(now % 1000), names[priority], text);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = sizeof(line) - 1;

	pthread_mutex_lock(&log_lock);
	fputs(line, stdout);
	if (log_file)
	{
		fputs(line, log_file);
		fflush(log_file);
	}
	if (log_socket >= 0)
		sceNetSendto(log_socket, line, (size_t)length, 0, &log_address, sizeof(log_address));
	pthread_mutex_unlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_log(priority, text);
}

void host_log_shutdown(void)
{
	pthread_mutex_lock(&log_lock);
	if (log_file)
	{
		fclose(log_file);
		log_file = NULL;
	}
	if (log_socket >= 0)
	{
		sceNetSocketClose(log_socket);
		log_socket = -1;
	}
	pthread_mutex_unlock(&log_lock);
}

/* ---------- termination */

static void show_message(const char *message)
{
	OrbisMsgDialogParam param;
	OrbisMsgDialogUserMessageParam user;

	if (sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG) != 0)
		return;
	if (sceCommonDialogInitialize() != 0)
		return;
	if (sceMsgDialogInitialize() != 0)
		return;
	memset(&param, 0, sizeof(param));
	memset(&user, 0, sizeof(user));
	param.baseParam.size = sizeof(param.baseParam);
	param.size = sizeof(param);
	param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &user;
	param.userId = host_user_id;
	user.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
	user.msg = message;
	if (sceMsgDialogOpen(&param) != 0)
		return;
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
		host_sleep_ms(16);
	sceMsgDialogTerminate();
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	show_message(message);
	host_exit(1);
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "exit %d", code);
	host_log_shutdown();
	fflush(stdout);
	sceSystemServiceLoadExec("exit", NULL);
	_Exit(code);
}

/* ---------- time */

uint64_t host_ticks_ms(void)
{
	/* microseconds of process time; monotonic */
	return sceKernelGetProcessTime() / 1000;
}

void host_sleep_ms(uint32_t milliseconds)
{
	sceKernelUsleep(milliseconds * 1000);
}
