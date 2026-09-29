/*
HOST_MAIN.C

Entry point of the PS4 port (eboot.bin).

Milestone 0: the host starts, finds the data root (the folder with maps/,
which the player copies to the console: port/ps4/README.md), opens the
log, brings up the system services, probes the memory layout the guest
needs, creates the Piglet context, and runs a small loop that clears the
screen and reads the pads. Options + Circle on pad 0 for a second exits.

From M1 the loop is replaced by the guest: the image is loaded
(host_loader.c), and the game's main() runs on a thread whose stack is in
guest memory (host_thread.c), as on Android (port/android/host/host_main.c).
*/

#include "host.h"
#include "host_config.h" /* generated: HALO_PS4_VERSION, HALO_PS4_LOG_ADDRESS */

#include <orbis/AudioOut.h>
#include <orbis/Net.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DISPLAY_WIDTH 1920
#define DISPLAY_HEIGHT 1080

char host_data_root[256] = HALO_PS4_DEFAULT_DATA_ROOT;
int32_t host_user_id = -1;

void host_video_clear(float red, float green, float blue);

/* ---------- the data root */

static int is_directory(const char *path)
{
	struct stat status;

	return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static void make_directory(const char *root, const char *name)
{
	char path[300];

	snprintf(path, sizeof(path), "%s/%s", root, name);
	mkdir(path, 0777);
}

/* the first candidate with maps/ in it; failing that, the default, made if
necessary, so that the log and the settings have a place */
static int find_data_root(void)
{
	static const char *const candidates[] = {HALO_PS4_DATA_ROOTS};
	char path[300];
	size_t index;

	for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++)
	{
		snprintf(path, sizeof(path), "%s/maps", candidates[index]);
		if (is_directory(path))
		{
			snprintf(host_data_root, sizeof(host_data_root), "%s", candidates[index]);
			return 1;
		}
	}
	snprintf(host_data_root, sizeof(host_data_root), "%s", HALO_PS4_DEFAULT_DATA_ROOT);
	mkdir(host_data_root, 0777);
	return 0;
}

/* ---------- system services */

static void initialize_services(void)
{
	OrbisUserServiceInitializeParams user_parameters;

	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
	sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);

	memset(&user_parameters, 0, sizeof(user_parameters));
	user_parameters.priority = 700; /* ORBIS_KERNEL_PRIO_FIFO_DEFAULT */
	if (sceUserServiceInitialize(&user_parameters) != 0)
		host_logf(HOST_LOG_WARN, "sceUserServiceInitialize failed");
	if (sceUserServiceGetInitialUser(&host_user_id) != 0)
	{
		host_logf(HOST_LOG_WARN, "sceUserServiceGetInitialUser failed");
		host_user_id = 0;
	}
	sceNetInit();
	sceNetPoolCreate("halo", 64 * 1024, 0);
	sceAudioOutInit();
}

/* ---------- main */

int main(int argc, char **argv)
{
	struct host_memory_probe memory;
	int has_maps;
	uint64_t exit_hold_start = 0, frame = 0;

	(void)argc;
	(void)argv;
	host_log_initialize();
	host_logf(HOST_LOG_INFO, "halo ps4 host %s (milestone 0)", HALO_PS4_VERSION);

	has_maps = find_data_root();
	host_log_open_file(host_data_root);
	/* a UDP "host:port" that receives the log (configure.py --ps4-log-address) */
	if (HALO_PS4_LOG_ADDRESS[0])
		host_log_open_udp(HALO_PS4_LOG_ADDRESS);
	host_logf(HOST_LOG_INFO, "data root %s (%s)", host_data_root, has_maps ? "maps/ found" : "no maps/");
	make_directory(host_data_root, "save");
	make_directory(host_data_root, "modules");

	initialize_services();
	host_logf(HOST_LOG_INFO, "user %d", host_user_id);

	/* the memory layout: the deciding question for M2 */
	if (host_memory_probe(&memory) != 0)
		host_logf(HOST_LOG_WARN, "memory: the fixed layout is not available; M2 must relocate (PLAN.md 6)");

	if (host_pad_initialize() != 0)
		host_logf(HOST_LOG_WARN, "no controllers");

	if (host_video_initialize(DISPLAY_WIDTH, DISPLAY_HEIGHT) != 0)
		host_fatal("The game cannot open the display.\n\n"
			"Copy libScePigletv2VSH.sprx and libSceShaccVSH.sprx to\n%s/modules\n\n"
			"See debug.txt in that folder.", host_data_root);
	if (!host_video.has_shader_compiler)
		host_logf(HOST_LOG_WARN, "no shader compiler: the game's shaders cannot compile (M3 needs libSceShaccVSH.sprx)");

	if (!has_maps)
		host_logf(HOST_LOG_WARN, "no game data: copy maps/ to %s/maps", host_data_root);

	/* M0's loop: a colour that breathes, the pads logged when they change */
	{
		uint32_t last_buttons[HOST_PAD_COUNT] = {0};

		for (;;)
		{
			const struct host_pad_state *pad;
			float phase = (float)(frame % 240) / 240.0f;
			float pulse = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
			int index;

			host_video_clear(0.05f + 0.15f * pulse, 0.25f + 0.35f * pulse, 0.10f + 0.10f * pulse);
			host_video_swap();
			frame++;

			for (index = 0; index < HOST_PAD_COUNT; index++)
			{
				pad = host_pad_read(index);
				if (pad && pad->buttons != last_buttons[index])
				{
					host_logf(HOST_LOG_INFO, "pad %d buttons %08x sticks %3u,%3u %3u,%3u triggers %3u,%3u", index,
						pad->buttons, pad->left_x, pad->left_y, pad->right_x, pad->right_y, pad->l2, pad->r2);
					last_buttons[index] = pad->buttons;
					if (pad->buttons & ORBIS_PAD_BUTTON_CROSS)
						host_pad_rumble(index, 0, 128);
					else
						host_pad_rumble(index, 0, 0);
				}
			}

			pad = host_pad_read(0);
			if (pad && (pad->buttons & (ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_CIRCLE))
				== (ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_CIRCLE))
			{
				if (!exit_hold_start)
					exit_hold_start = host_ticks_ms();
				else if (host_ticks_ms() - exit_hold_start > 1000)
					break;
			}
			else
				exit_hold_start = 0;
		}
	}

	host_logf(HOST_LOG_INFO, "%llu frames", (unsigned long long)frame);
	host_pad_shutdown();
	host_video_shutdown();
	host_exit(0);
}
