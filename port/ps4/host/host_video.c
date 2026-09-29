/*
HOST_VIDEO.C

The display: an OpenGL ES 2 context from Piglet (libScePigletv2VSH) on the
console's video output.

Piglet is Sony's GLES 2.0 / EGL 1.4 implementation. Retail firmware has a
copy in the system library folder, but not the runtime shader compiler
(libSceShaccVSH) that glShaderSource needs; the player copies both modules
to <data_root>/modules (port/ps4/README.md), and this file prefers that
folder. The modules are loaded by path and their functions resolved with
sceKernelDlsym, never linked as imports: an import that the loader cannot
find stops the application before main(), without a message. The same
table is what the guest's GL calls go through later (host_gl.c, M3).
*/

#include "host.h"

#include <stdbool.h> /* the Piglet header uses bool without including it */
#include <orbis/Pigletv2VSH.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <stdio.h>
#include <string.h>

struct host_video host_video;

static int piglet_module = -1, shacc_module = -1;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static OrbisPglWindow window;

/* ---------- entry points, resolved from the loaded module */

#define EGL_FUNCTIONS(X) \
	X(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType)) \
	X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) \
	X(EGLBoolean, eglBindAPI, (EGLenum)) \
	X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
	X(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)) \
	X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
	X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
	X(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint)) \
	X(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface)) \
	X(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface)) \
	X(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext)) \
	X(EGLBoolean, eglTerminate, (EGLDisplay)) \
	X(EGLint, eglGetError, (void)) \
	X(void *, eglGetProcAddress, (const char *)) \
	X(const GLubyte *, glGetString, (GLenum)) \
	X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, glClear, (GLbitfield)) \
	X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, glEnable, (GLenum)) \
	X(void, glDisable, (GLenum)) \
	X(void, glFinish, (void)) \
	X(GLenum, glGetError, (void))

#define DECLARE(type, name, arguments) static type (*p_##name) arguments;
EGL_FUNCTIONS(DECLARE)
#undef DECLARE

typedef bool (*set_configuration_t)(const OrbisPglConfig *);

static void *resolve(int module, const char *name)
{
	void *address = NULL;

	if (sceKernelDlsym(module, name, &address) != 0)
		return NULL;
	return address;
}

static int resolve_all(void)
{
	int missing = 0;

#define RESOLVE(type, name, arguments) \
	p_##name = (type (*) arguments)resolve(piglet_module, #name); \
	if (!p_##name) \
	{ \
		host_logf(HOST_LOG_ERROR, "piglet: %s is missing", #name); \
		missing++; \
	}
	EGL_FUNCTIONS(RESOLVE)
#undef RESOLVE
	return missing ? -1 : 0;
}

/* the guest's GL table (M3) resolves through here */
void *host_gl_resolve(const char *name)
{
	void *address = resolve(piglet_module, name);

	if (!address && p_eglGetProcAddress)
		address = p_eglGetProcAddress(name);
	return address;
}

/* ---------- modules */

static int load_module_from(const char *folder, const char *name)
{
	char path[320];
	int result;

	snprintf(path, sizeof(path), "%s/%s", folder, name);
	result = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
	if (result >= 0)
		host_logf(HOST_LOG_INFO, "loaded %s", path);
	return result;
}

static int load_module(const char *name, int required)
{
	char user_folder[300], system_folder[300];
	const char *folders[3];
	int index, result = -1;

	snprintf(user_folder, sizeof(user_folder), "%s/modules", host_data_root);
	/* the process's view of /system/common/lib */
	snprintf(system_folder, sizeof(system_folder), "/%s/common/lib", sceKernelGetFsSandboxRandomWord());
	folders[0] = user_folder;
	folders[1] = system_folder;
	folders[2] = "/system/common/lib";
	for (index = 0; index < 3 && result < 0; index++)
		result = load_module_from(folders[index], name);
	if (result < 0)
	{
		host_logf(required ? HOST_LOG_ERROR : HOST_LOG_WARN, "%s: not found (put it in %s)", name, user_folder);
		return -1;
	}
	return result;
}

/* ---------- context */

int host_video_initialize(int width, int height)
{
	OrbisPglConfig configuration;
	set_configuration_t set_configuration;
	EGLConfig config = NULL;
	EGLint config_count = 0, major = 0, minor = 0;
	static const EGLint config_attributes[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE,
	};
	static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	static const EGLint surface_attributes[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};

	memset(&host_video, 0, sizeof(host_video));
	host_video.width = width;
	host_video.height = height;
	host_video.vsync = 1;

	/* the shader compiler first: Piglet looks it up by name when it compiles */
	shacc_module = load_module("libSceShaccVSH.sprx", 0);
	host_video.has_shader_compiler = shacc_module >= 0;
	piglet_module = load_module("libScePigletv2VSH.sprx", 1);
	if (piglet_module < 0)
		return -1;
	if (resolve_all() != 0)
		return -1;
	set_configuration = (set_configuration_t)resolve(piglet_module, "scePigletSetConfigurationVSH");
	if (!set_configuration)
	{
		host_logf(HOST_LOG_ERROR, "piglet: scePigletSetConfigurationVSH is missing");
		return -1;
	}

	/* the memory budgets follow the OpenOrbis Piglet sample; the game's
	textures (a 128 MB window at most) and buffers fit in the video share */
	memset(&configuration, 0, sizeof(configuration));
	configuration.size = sizeof(configuration);
	configuration.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
	configuration.processOrder = 1;
	configuration.systemSharedMemorySize = 250u * 1024 * 1024;
	configuration.videoSharedMemorySize = 512u * 1024 * 1024;
	configuration.maxMappedFlexibleMemory = 170u * 1024 * 1024;
	configuration.drawCommandBufferSize = 1024 * 1024;
	configuration.lcueResourceBufferSize = 1024 * 1024;
	configuration.dbgPosCmd_0x40 = (uint32_t)width;
	configuration.dbgPosCmd_0x44 = (uint32_t)height;
	configuration.unk_0x5C = 2;
	if (!set_configuration(&configuration))
	{
		host_logf(HOST_LOG_ERROR, "piglet: scePigletSetConfigurationVSH failed");
		return -1;
	}

	display = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY || !p_eglInitialize(display, &major, &minor))
	{
		host_logf(HOST_LOG_ERROR, "egl: no display (0x%x)", p_eglGetError());
		return -1;
	}
	if (!p_eglBindAPI(EGL_OPENGL_ES_API))
	{
		host_logf(HOST_LOG_ERROR, "egl: eglBindAPI failed (0x%x)", p_eglGetError());
		return -1;
	}
	if (!p_eglChooseConfig(display, config_attributes, &config, 1, &config_count) || config_count < 1)
	{
		host_logf(HOST_LOG_ERROR, "egl: no config with depth and stencil (0x%x)", p_eglGetError());
		return -1;
	}
	memset(&window, 0, sizeof(window));
	window.uID = 0;
	window.uWidth = (uint32_t)width;
	window.uHeight = (uint32_t)height;
	surface = p_eglCreateWindowSurface(display, config, &window, surface_attributes);
	if (surface == EGL_NO_SURFACE)
	{
		host_logf(HOST_LOG_ERROR, "egl: eglCreateWindowSurface failed (0x%x)", p_eglGetError());
		return -1;
	}
	context = p_eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (context == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_ERROR, "egl: eglCreateContext failed (0x%x)", p_eglGetError());
		return -1;
	}
	if (!p_eglMakeCurrent(display, surface, surface, context))
	{
		host_logf(HOST_LOG_ERROR, "egl: eglMakeCurrent failed (0x%x)", p_eglGetError());
		return -1;
	}
	p_eglSwapInterval(display, 1);

	host_video.gl_version = (const char *)p_glGetString(GL_VERSION);
	host_video.gl_renderer = (const char *)p_glGetString(GL_RENDERER);
	host_video.gl_vendor = (const char *)p_glGetString(GL_VENDOR);
	host_logf(HOST_LOG_INFO, "egl %d.%d, %s, %s, %s; shader compiler %s", major, minor,
		host_video.gl_version ? host_video.gl_version : "?", host_video.gl_renderer ? host_video.gl_renderer : "?",
		host_video.gl_vendor ? host_video.gl_vendor : "?", host_video.has_shader_compiler ? "present" : "missing");
	{
		const char *extensions = (const char *)p_glGetString(GL_EXTENSIONS);

		if (extensions)
			host_logf(HOST_LOG_INFO, "gl extensions: %s", extensions);
	}
	p_glViewport(0, 0, width, height);
	sceSystemServiceHideSplashScreen();
	return 0;
}

void host_video_set_swap_interval(int interval)
{
	if (display != EGL_NO_DISPLAY)
	{
		p_eglSwapInterval(display, interval);
		host_video.vsync = interval != 0;
	}
}

void host_video_swap(void)
{
	if (display != EGL_NO_DISPLAY && !p_eglSwapBuffers(display, surface))
		host_logf(HOST_LOG_WARN, "egl: eglSwapBuffers failed (0x%x)", p_eglGetError());
}

/* M0's frame: a clear colour that moves, proof the context presents */
void host_video_clear(float red, float green, float blue)
{
	p_glClearColor(red, green, blue, 1.0f);
	p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void host_video_shutdown(void)
{
	if (display == EGL_NO_DISPLAY)
		return;
	p_eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (surface != EGL_NO_SURFACE)
		p_eglDestroySurface(display, surface);
	if (context != EGL_NO_CONTEXT)
		p_eglDestroyContext(display, context);
	p_eglTerminate(display);
	display = EGL_NO_DISPLAY;
	surface = EGL_NO_SURFACE;
	context = EGL_NO_CONTEXT;
	if (piglet_module >= 0)
		sceKernelStopUnloadModule(piglet_module, 0, NULL, 0, NULL, NULL);
	if (shacc_module >= 0)
		sceKernelStopUnloadModule(shacc_module, 0, NULL, 0, NULL, NULL);
	piglet_module = shacc_module = -1;
}
