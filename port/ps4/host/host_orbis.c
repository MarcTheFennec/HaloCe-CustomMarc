/*
HOST_ORBIS.C

The PS4 platform (OpenOrbis toolchain, running under GoldHEN): the display
through Piglet (the system's OpenGL ES 2.0 / EGL 1.4 driver), the
DualShock 4 through libScePad, sound through libSceAudioOut, and on-screen
notifications for fatal errors.

Shaders. Retail firmware ships Piglet without its GLSL compiler. The only
public way to compile shaders at run time is flatz's (psx-place.com,
"OpenGL ES implementation on the PS4", 2018): load the development kit's
Piglet together with its compiler module, libSceShaccVSH, and patch Piglet
to use it. Those two files are Sony's and are not part of this project: the
player copies them, dumped from their own system, to

	/data/halo/lib/            (checked first), or
	/data/self/system/common/lib/   (where other homebrew expects them)

and the host loads and patches them here. Without them the system's Piglet
is used for the display, and the game stops with a notification explaining
where the files go as soon as it compiles its first shader (host_gl_es2.c
reports compile failures; the check below catches the missing compiler at
start-up).

ASSUMPTION: the patch offsets are flatz's for the 4.74 development kit
modules (the commonly dumped pair); the host checks that they fall inside
Piglet's segments and logs the bytes it replaces so a mismatch shows in the
log.
*/

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>

#define __PIGLET__ 1
#include <EGL/egl.h>

#include <SDL3/SDL_gamepad.h>

/* ---------- piglet's configuration (OpenOrbis' Pigletv2VSH.h, which also
pulls in prototypes for the linked Piglet; the host resolves everything from
the loaded module instead) */

#define PGL_CONFIG_SIZE 0x88
#define PGL_FLAGS_USE_COMPOSITE_EXT 0x8
#define PGL_FLAGS_USE_TILED_TEXTURE 0x40
#define PGL_FLAGS_USE_FLEXIBLE_MEMORY 0x80

struct pgl_config
{
	uint32_t size;                        /* 0x00 */
	uint32_t flags;                       /* 0x04 */
	uint8_t process_order;                /* 0x08 */
	uint8_t pad_09[3];
	uint32_t unknown_0c, unknown_10, unknown_14;
	uint64_t system_shared_memory_size;   /* 0x18 */
	uint32_t unknown_20, unknown_24;
	uint64_t video_shared_memory_size;    /* 0x28 */
	uint64_t max_mapped_flexible_memory;  /* 0x30 */
	uint64_t min_flexible_memory_chunk;   /* 0x38 */
	uint32_t debug_position[4];           /* 0x40 */
	uint8_t debug_position_flag;          /* 0x50 */
	uint8_t pad_51[3];
	uint32_t draw_command_buffer_size;    /* 0x54 */
	uint32_t lcue_resource_buffer_size;   /* 0x58 */
	uint32_t unknown_5c;
	uint64_t unknown_60[5];               /* 0x60-0x87 */
};

_Static_assert(sizeof(struct pgl_config) == PGL_CONFIG_SIZE, "Piglet configuration layout");

/* ---------- modules */

static int piglet_module = -1;
static int shacc_module = -1;
static int shader_compiler;

static int load_module(const char *path)
{
	int handle;

	if (access(path, R_OK) != 0)
		return -1;
	handle = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
	if (handle < 0)
	{
		host_logf(HOST_LOG_WARN, "cannot load %s: 0x%08x", path, (unsigned)handle);
		return -1;
	}
	host_logf(HOST_LOG_INFO, "loaded %s (module %d)", path, handle);
	return handle;
}

static void *module_symbol(int module, const char *name)
{
	void *address = NULL;

	if (module < 0 || sceKernelDlsym(module, name, &address) != 0)
		return NULL;
	return address;
}

/* flatz's patch: Piglet asks whether a compiler is present and which
module it is */
struct piglet_patch
{
	uint32_t offset;
	uint8_t bytes[5];
	uint8_t length;
};

static int patch_bytes(const OrbisKernelModuleInfo *info, uint32_t offset, const void *bytes, size_t length)
{
	uint8_t *base = (uint8_t *)info->segmentInfo[0].address;
	uint8_t *target = base + offset;
	uintptr_t page = (uintptr_t)target & ~(uintptr_t)0x3fff;
	char before[3 * 8 + 1] = "", after[3 * 8 + 1] = "";
	uint32_t segment;
	size_t index;

	for (segment = 0; segment < info->segmentCount; segment++)
	{
		uint8_t *start = (uint8_t *)info->segmentInfo[segment].address;

		if (target >= start && target + length <= start + info->segmentInfo[segment].size)
			break;
	}
	if (segment == info->segmentCount)
	{
		host_logf(HOST_LOG_ERROR, "piglet patch at +0x%x is outside the module: not the expected Piglet build", offset);
		return -1;
	}
	if (sceKernelMprotect((void *)page, 0x8000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot make Piglet writable at +0x%x", offset);
		return -1;
	}
	for (index = 0; index < length && index < 8; index++)
		sprintf(before + index * 3, "%02x ", target[index]);
	memcpy(target, bytes, length);
	for (index = 0; index < length && index < 8; index++)
		sprintf(after + index * 3, "%02x ", target[index]);
	host_logf(HOST_LOG_INFO, "piglet +0x%05x (segment %u): %s-> %s", offset, segment, before, after);
	return 0;
}

static int patch_piglet(void)
{
	static const uint8_t compiler_present[] = { 0x31, 0xc0, 0xff, 0xc0, 0x90 }; /* xor eax,eax; inc eax; nop */
	static const uint8_t zero = 0, one = 1;
	OrbisKernelModuleInfo info;
	int32_t shacc = shacc_module;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetModuleInfo(piglet_module, &info) != 0 || !info.segmentCount)
	{
		host_logf(HOST_LOG_ERROR, "cannot read Piglet's module information");
		return -1;
	}
	host_logf(HOST_LOG_INFO, "piglet module %s at %p, %u segments", info.name, info.segmentInfo[0].address,
		info.segmentCount);
	if (patch_bytes(&info, 0x5451f, compiler_present, sizeof(compiler_present)) != 0 ||
		patch_bytes(&info, 0xb2dec, &zero, 1) != 0 || patch_bytes(&info, 0xb2ded, &zero, 1) != 0 ||
		patch_bytes(&info, 0xb2dee, &one, 1) != 0 || patch_bytes(&info, 0xb2e21, &one, 1) != 0 ||
		patch_bytes(&info, 0xb2e24, &shacc, sizeof(shacc)) != 0)
		return -1;
	return 0;
}

static int load_piglet(void)
{
	static const char *const directories[] = { "/data/halo/lib", "/data/self/system/common/lib" };
	char path[256];
	size_t index;

	for (index = 0; index < sizeof(directories) / sizeof(directories[0]) && piglet_module < 0; index++)
	{
		char piglet_path[256];

		snprintf(path, sizeof(path), "%s/libSceShaccVSH.sprx", directories[index]);
		snprintf(piglet_path, sizeof(piglet_path), "%s/libScePigletv2VSH.sprx", directories[index]);
		if (access(path, R_OK) != 0 || access(piglet_path, R_OK) != 0)
			continue;
		shacc_module = load_module(path);
		if (shacc_module < 0)
			continue;
		piglet_module = load_module(piglet_path);
		if (piglet_module < 0)
			continue;
		if (patch_piglet() == 0)
			shader_compiler = 1;
		else
			host_logf(HOST_LOG_ERROR, "the Piglet in %s could not be patched; shaders will not compile",
				directories[index]);
	}
	if (piglet_module < 0)
	{
		/* the system's own, for the display at least */
		snprintf(path, sizeof(path), "/%s/common/lib/libScePigletv2VSH.sprx", sceKernelGetFsSandboxRandomWord());
		piglet_module = load_module(path);
	}
	if (piglet_module < 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot load Piglet (the system's OpenGL ES driver)");
		return -1;
	}
	if (!shader_compiler)
	{
		host_logf(HOST_LOG_ERROR, "no shader compiler: copy libSceShaccVSH.sprx and libScePigletv2VSH.sprx "
			"(4.74 development kit) to /data/halo/lib; see BUILDING-PS4.md");
	}
	return 0;
}

/* ---------- start-up */

static int user_id = -1;

int host_platform_initialize(void)
{
	int result;

	result = sceUserServiceInitialize(NULL);
	if (result < 0 && (unsigned)result != 0x80960003u /* already initialised */)
		host_logf(HOST_LOG_WARN, "sceUserServiceInitialize: 0x%08x", (unsigned)result);
	if (sceUserServiceGetInitialUser(&user_id) < 0)
	{
		host_logf(HOST_LOG_WARN, "no initial user; using the system user");
		user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
	}
	result = scePadInit();
	if (result < 0)
		host_logf(HOST_LOG_WARN, "scePadInit: 0x%08x", (unsigned)result);
	result = sceAudioOutInit();
	if (result < 0 && (unsigned)result != 0x8026000eu /* already initialised */)
		host_logf(HOST_LOG_WARN, "sceAudioOutInit: 0x%08x", (unsigned)result);
	host_logf(HOST_LOG_INFO, "user %d; %s", user_id, sceKernelIsNeoMode() ? "PS4 Pro" : "PS4");
	if (load_piglet() != 0)
		return -1;
	sceSystemServiceHideSplashScreen();
	return 0;
}

void host_platform_shutdown(void)
{
	host_platform_audio_close();
}

const char *host_platform_name(void)
{
	return "PS4";
}

void host_platform_message(const char *title, const char *message)
{
	OrbisNotificationRequest request;

	memset(&request, 0, sizeof(request));
	request.type = NotificationRequest;
	request.targetId = -1;
	snprintf(request.message, sizeof(request.message), "%s: %s", title, message);
	sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
	/* long enough to be read before the process ends */
	sleep(8);
}

/* ---------- the display */

typedef EGLDisplay (*egl_get_display_function)(EGLNativeDisplayType);
typedef EGLBoolean (*egl_initialize_function)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*egl_bind_api_function)(EGLenum);
typedef EGLBoolean (*egl_choose_config_function)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*egl_create_window_surface_function)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
typedef EGLContext (*egl_create_context_function)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*egl_make_current_function)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*egl_swap_buffers_function)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*egl_swap_interval_function)(EGLDisplay, EGLint);
typedef EGLint (*egl_get_error_function)(void);
typedef void *(*egl_get_proc_address_function)(const char *);
typedef int (*pgl_set_configuration_function)(const struct pgl_config *);

static struct
{
	egl_make_current_function make_current;
	egl_swap_buffers_function swap_buffers;
	egl_swap_interval_function swap_interval;
	egl_get_error_function get_error;
	egl_get_proc_address_function get_proc_address;
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	OrbisPglWindow window;
} egl;

void host_platform_display_size(int *width, int *height)
{
	*width = host_settings.video_height == 720 ? 1280 : 1920;
	*height = host_settings.video_height;
}

#define EGL_SYMBOL(type, name) type name = (type)module_symbol(piglet_module, #name)

int host_platform_gl_create(void)
{
	EGL_SYMBOL(pgl_set_configuration_function, scePigletSetConfigurationVSH);
	EGL_SYMBOL(egl_get_display_function, eglGetDisplay);
	EGL_SYMBOL(egl_initialize_function, eglInitialize);
	EGL_SYMBOL(egl_bind_api_function, eglBindAPI);
	EGL_SYMBOL(egl_choose_config_function, eglChooseConfig);
	EGL_SYMBOL(egl_create_window_surface_function, eglCreateWindowSurface);
	EGL_SYMBOL(egl_create_context_function, eglCreateContext);
	static const EGLint config_attributes[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE,
	};
	static const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct pgl_config config;
	EGLConfig egl_config;
	EGLint major = 0, minor = 0, count = 0;
	int width, height;

	if (egl.context)
		return 0;
	egl.make_current = (egl_make_current_function)module_symbol(piglet_module, "eglMakeCurrent");
	egl.swap_buffers = (egl_swap_buffers_function)module_symbol(piglet_module, "eglSwapBuffers");
	egl.swap_interval = (egl_swap_interval_function)module_symbol(piglet_module, "eglSwapInterval");
	egl.get_error = (egl_get_error_function)module_symbol(piglet_module, "eglGetError");
	egl.get_proc_address = (egl_get_proc_address_function)module_symbol(piglet_module, "eglGetProcAddress");
	if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglCreateWindowSurface || !eglCreateContext ||
		!egl.make_current || !egl.swap_buffers || !egl.get_error)
	{
		host_logf(HOST_LOG_ERROR, "Piglet lacks EGL entry points");
		return -1;
	}
	memset(&config, 0, sizeof(config));
	config.size = sizeof(config);
	/* COMPOSITE_EXT | FLEXIBLE_MEMORY | 0x60, as OpenOrbis' piglet sample */
	config.flags = PGL_FLAGS_USE_COMPOSITE_EXT | PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
	config.process_order = 1;
	config.system_shared_memory_size = 250u << 20;
	config.video_shared_memory_size = 512u << 20;
	config.max_mapped_flexible_memory = 170u << 20;
	config.draw_command_buffer_size = 1u << 20;
	config.lcue_resource_buffer_size = 1u << 20;
	/* as OpenOrbis' piglet sample (samples/piglet/PigletApplication.cpp) */
	host_platform_display_size(&width, &height);
	config.debug_position[0] = (uint32_t)width;
	config.debug_position[1] = (uint32_t)height;
	config.unknown_5c = 2;
	if (scePigletSetConfigurationVSH && !scePigletSetConfigurationVSH(&config))
		host_logf(HOST_LOG_WARN, "scePigletSetConfigurationVSH failed; using Piglet's defaults");

	egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (egl.display == EGL_NO_DISPLAY || !eglInitialize(egl.display, &major, &minor))
	{
		host_logf(HOST_LOG_ERROR, "EGL initialisation failed: 0x%x (is the application's CATEGORY gde?)",
			egl.get_error());
		return -1;
	}
	if (eglBindAPI)
		eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(egl.display, config_attributes, &egl_config, 1, &count) || count < 1)
	{
		host_logf(HOST_LOG_ERROR, "no RGBA8 D24S8 EGL configuration: 0x%x", egl.get_error());
		return -1;
	}
	egl.window.uID = 0;
	egl.window.uWidth = (khronos_uint32_t)width;
	egl.window.uHeight = (khronos_uint32_t)height;
	egl.surface = eglCreateWindowSurface(egl.display, egl_config, &egl.window, NULL);
	if (egl.surface == EGL_NO_SURFACE)
	{
		host_logf(HOST_LOG_ERROR, "eglCreateWindowSurface %dx%d: 0x%x", width, height, egl.get_error());
		return -1;
	}
	egl.context = eglCreateContext(egl.display, egl_config, EGL_NO_CONTEXT, context_attributes);
	if (egl.context == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_ERROR, "eglCreateContext: 0x%x", egl.get_error());
		return -1;
	}
	if (host_platform_gl_make_current() != 0)
		return -1;
	host_platform_gl_set_swap_interval(host_settings.vsync ? 1 : 0);
	host_logf(HOST_LOG_INFO, "EGL %d.%d, %dx%d window surface, vsync %s, shader compiler %s", major, minor, width,
		height, host_settings.vsync ? "on" : "off", shader_compiler ? "loaded" : "MISSING");
	if (!shader_compiler)
	{
		host_fatal("Halo needs Piglet's shader compiler.\n\nCopy libSceShaccVSH.sprx and libScePigletv2VSH.sprx "
			"to /data/halo/lib (see BUILDING-PS4.md), then start the game again.");
	}
	return 0;
}

int host_platform_gl_make_current(void)
{
	if (!egl.make_current || !egl.make_current(egl.display, egl.surface, egl.surface, egl.context))
	{
		host_logf(HOST_LOG_ERROR, "eglMakeCurrent: 0x%x", egl.get_error ? egl.get_error() : 0);
		return -1;
	}
	return 0;
}

int host_platform_gl_swap(void)
{
	return egl.swap_buffers && egl.swap_buffers(egl.display, egl.surface) ? 0 : -1;
}

int host_platform_gl_set_swap_interval(int interval)
{
	return egl.swap_interval && egl.swap_interval(egl.display, interval) ? 0 : -1;
}

void *host_platform_gl_proc(const char *name)
{
	void *address = module_symbol(piglet_module, name);

	if (!address && egl.get_proc_address)
		address = egl.get_proc_address(name);
	return address;
}

/* ---------- the controller */

#define PAD_ALREADY_OPENED 0x80920004u
#define PAD_RETRY_MICROSECONDS 1000000u

static struct
{
	int handle;
	int connected;
	int open_error_logged;
	uint64_t last_attempt;
} pad = { -1, 0, 0, 0 };

static void pad_open(void)
{
	uint64_t now = host_time_microseconds();
	int handle;

	if (pad.handle >= 0 || (pad.last_attempt && now - pad.last_attempt < PAD_RETRY_MICROSECONDS))
		return;
	pad.last_attempt = now;
	handle = scePadOpen(user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
	if ((unsigned)handle == PAD_ALREADY_OPENED)
		handle = scePadGetHandle(user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
	if (handle >= 0)
	{
		pad.handle = handle;
		host_logf(HOST_LOG_INFO, "controller opened (handle %d)", handle);
	}
	else if (!pad.open_error_logged)
	{
		/* retried every PAD_RETRY_MICROSECONDS; logged once */
		pad.open_error_logged = 1;
		host_logf(HOST_LOG_WARN, "scePadOpen: 0x%08x (retrying)", (unsigned)handle);
	}
}

static int16_t stick_axis(uint8_t value)
{
	int scaled = ((int)value - 128) * 32767 / 127;

	return (int16_t)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
}

int host_platform_pad_read(int index, struct host_pad_state *state)
{
	OrbisPadData data;
	uint32_t buttons;

	memset(state, 0, sizeof(*state));
	if (index != 0)
		return 0;
	pad_open();
	if (pad.handle < 0)
		return 0;
	memset(&data, 0, sizeof(data));
	if (scePadReadState(pad.handle, &data) != 0 || !data.connected)
	{
		if (pad.connected)
			host_logf(HOST_LOG_INFO, "controller disconnected");
		pad.connected = 0;
		return 0;
	}
	if (!pad.connected)
		host_logf(HOST_LOG_INFO, "controller connected");
	pad.connected = 1;
	state->connected = 1;
	state->axes[SDL_GAMEPAD_AXIS_LEFTX] = stick_axis(data.leftStick.x);
	state->axes[SDL_GAMEPAD_AXIS_LEFTY] = stick_axis(data.leftStick.y);
	state->axes[SDL_GAMEPAD_AXIS_RIGHTX] = stick_axis(data.rightStick.x);
	state->axes[SDL_GAMEPAD_AXIS_RIGHTY] = stick_axis(data.rightStick.y);
	state->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (int16_t)(data.analogButtons.l2 * 32767 / 255);
	state->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (int16_t)(data.analogButtons.r2 * 32767 / 255);
	buttons = data.buttons;
	state->buttons[SDL_GAMEPAD_BUTTON_SOUTH] = (buttons & ORBIS_PAD_BUTTON_CROSS) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_EAST] = (buttons & ORBIS_PAD_BUTTON_CIRCLE) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_WEST] = (buttons & ORBIS_PAD_BUTTON_SQUARE) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_NORTH] = (buttons & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = (buttons & ORBIS_PAD_BUTTON_L1) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = (buttons & ORBIS_PAD_BUTTON_R1) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = (buttons & ORBIS_PAD_BUTTON_L3) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = (buttons & ORBIS_PAD_BUTTON_R3) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_START] = (buttons & ORBIS_PAD_BUTTON_OPTIONS) != 0;
	/* the touch pad's click is the Xbox's Back (scoreboard, menu back) */
	state->buttons[SDL_GAMEPAD_BUTTON_BACK] = (buttons & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = (buttons & ORBIS_PAD_BUTTON_UP) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = (buttons & ORBIS_PAD_BUTTON_DOWN) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = (buttons & ORBIS_PAD_BUTTON_LEFT) != 0;
	state->buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = (buttons & ORBIS_PAD_BUTTON_RIGHT) != 0;
	return 1;
}

int host_platform_pad_rumble(int index, uint16_t low, uint16_t high)
{
	OrbisPadVibeParam vibration;

	if (index != 0 || pad.handle < 0 || !pad.connected)
		return -1;
	vibration.lgMotor = (uint8_t)(low >> 8);
	vibration.smMotor = (uint8_t)(high >> 8);
	return scePadSetVibration(pad.handle, &vibration) == 0 ? 0 : -1;
}

int host_platform_pad_count(void)
{
	pad_open();
	return pad.handle >= 0 ? 1 : 0;
}

/* ---------- sound */

int host_platform_audio_block = 256;
static int audio_handle = -1;

int host_platform_audio_open(void)
{
	if (audio_handle >= 0)
		return 0;
	audio_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
		(uint32_t)host_platform_audio_block, HOST_AUDIO_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO);
	if (audio_handle < 0)
	{
		host_logf(HOST_LOG_ERROR, "sceAudioOutOpen: 0x%08x", (unsigned)audio_handle);
		audio_handle = -1;
		return -1;
	}
	return 0;
}

int host_platform_audio_output(const float *samples)
{
	if (audio_handle < 0)
		return -1;
	return sceAudioOutOutput(audio_handle, samples) < 0 ? -1 : 0;
}

void host_platform_audio_close(void)
{
	if (audio_handle >= 0)
	{
		sceAudioOutOutput(audio_handle, NULL);
		sceAudioOutClose(audio_handle);
		audio_handle = -1;
	}
}

/* the system ends the process itself when the player closes the
application; there is no request to answer */
int host_platform_quit_requested(void)
{
	return 0;
}
