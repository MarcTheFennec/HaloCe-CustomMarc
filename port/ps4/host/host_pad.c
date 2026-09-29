/*
HOST_PAD.C

Controllers through scePad. Pad 0 is the initial user's controller; pads
1 to 3 are the controllers of the other logged-in users, for split screen
(a second DualShock paired to the console logs a guest user in when its PS
button is pushed, which is what the PS4 expects for local multiplayer).

The game sees an Xbox controller; the guest's input layer (M4) reads
struct host_pad_state through the host_sdl_* gamepad functions.
*/

#include "host.h"

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <string.h>

struct pad
{
	int32_t user_id;
	int32_t handle;
	struct host_pad_state state;
};

static struct pad pads[HOST_PAD_COUNT];
static int initialized;

int host_pad_initialize(void)
{
	OrbisUserServiceLoginUserIdList users;
	int index;

	if (scePadInit() != 0)
	{
		host_logf(HOST_LOG_ERROR, "scePadInit failed");
		return -1;
	}
	memset(pads, 0, sizeof(pads));
	for (index = 0; index < HOST_PAD_COUNT; index++)
	{
		pads[index].user_id = -1;
		pads[index].handle = -1;
	}
	pads[0].user_id = host_user_id;
	if (sceUserServiceGetLoginUserIdList(&users) == 0)
	{
		int slot = 1, user;

		for (user = 0; user < 4 && slot < HOST_PAD_COUNT; user++)
			if (users.userId[user] >= 0 && users.userId[user] != host_user_id)
				pads[slot++].user_id = users.userId[user];
	}
	for (index = 0; index < HOST_PAD_COUNT; index++)
	{
		if (pads[index].user_id < 0)
			continue;
		pads[index].handle = scePadOpen(pads[index].user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
		if (pads[index].handle < 0)
			host_logf(HOST_LOG_WARN, "pad %d (user %d): scePadOpen failed (0x%x)", index, pads[index].user_id,
				(unsigned)pads[index].handle);
		else
			host_logf(HOST_LOG_INFO, "pad %d: user %d, handle %d", index, pads[index].user_id, pads[index].handle);
	}
	initialized = 1;
	return 0;
}

const struct host_pad_state *host_pad_read(int index)
{
	OrbisPadData data;
	struct pad *pad;

	if (index < 0 || index >= HOST_PAD_COUNT || !initialized)
		return NULL;
	pad = &pads[index];
	if (pad->handle < 0 || scePadReadState(pad->handle, &data) != 0 || !data.connected)
	{
		memset(&pad->state, 0, sizeof(pad->state));
		return &pad->state;
	}
	pad->state.connected = 1;
	pad->state.buttons = data.buttons;
	pad->state.left_x = data.leftStick.x;
	pad->state.left_y = data.leftStick.y;
	pad->state.right_x = data.rightStick.x;
	pad->state.right_y = data.rightStick.y;
	pad->state.l2 = data.analogButtons.l2;
	pad->state.r2 = data.analogButtons.r2;
	return &pad->state;
}

int host_pad_rumble(int index, uint8_t large, uint8_t small)
{
	OrbisPadVibeParam vibration;

	if (index < 0 || index >= HOST_PAD_COUNT || pads[index].handle < 0)
		return -1;
	vibration.lgMotor = large;
	vibration.smMotor = small;
	return scePadSetVibration(pads[index].handle, &vibration) == 0 ? 0 : -1;
}

void host_pad_shutdown(void)
{
	int index;

	for (index = 0; index < HOST_PAD_COUNT; index++)
	{
		if (pads[index].handle >= 0)
		{
			host_pad_rumble(index, 0, 0);
			scePadClose(pads[index].handle);
			pads[index].handle = -1;
		}
	}
	initialized = 0;
}
