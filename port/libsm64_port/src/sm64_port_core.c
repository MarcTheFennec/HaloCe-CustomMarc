/*
SM64_PORT_CORE.C

The portable half of the libsm64 module: loading the library, running the
fixed-step clock, converting between Halo's and Super Mario 64's frames of
reference, mapping input, building the collision triangles, and holding the
actors.  Nothing here includes a Halo header, so the host-side tests compile
and run it on its own (see port/libsm64_port/tests).

The library is loaded at run time, never linked: a build with the module
enabled but no libsm64 shared library next to the executable, or no ROM,
behaves like a build with the module switched off.

Numbered notes refer to port/libsm64_port/INTEGRATION_DESIGN.md.
*/

#include "sm64_port.h"
#include "sm64_port_abi.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- platform: loading a shared library ---------- */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE sm64_port_library;
#else
#include <dlfcn.h>
typedef void *sm64_port_library;
#endif

static sm64_port_library port_library_open(const char *path)
{
#if defined(_WIN32)
	return LoadLibraryA(path);
#else
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *port_library_symbol(sm64_port_library handle, const char *name)
{
#if defined(_WIN32)
	return (void *)GetProcAddress(handle, name);
#else
	return dlsym(handle, name);
#endif
}

static void port_library_close(sm64_port_library handle)
{
	if (!handle)
		return;
#if defined(_WIN32)
	FreeLibrary(handle);
#else
	dlclose(handle);
#endif
}

/* ---------- the layout of the public surface struct must match ---------- */

/* A compiler that changes the layout of either struct breaks the build here
   rather than feeding libsm64 differently shaped triangles. */
typedef char sm64_port_surface_size_check[
	sizeof(SM64PortSurface) == sizeof(struct SM64PortAbiSurface) ? 1 : -1];
typedef char sm64_port_surface_type_offset_check[
	offsetof(SM64PortSurface, type) == offsetof(struct SM64PortAbiSurface, type) ? 1 : -1];
typedef char sm64_port_surface_terrain_offset_check[
	offsetof(SM64PortSurface, terrain) == offsetof(struct SM64PortAbiSurface, terrain) ? 1 : -1];
typedef char sm64_port_surface_vertices_offset_check[
	offsetof(SM64PortSurface, vertices) == offsetof(struct SM64PortAbiSurface, vertices) ? 1 : -1];

/* ---------- libsm64 entry points ---------- */

typedef void (*sm64_port_fn_global_init)(const uint8_t *rom, uint8_t *out_texture);
typedef void (*sm64_port_fn_global_terminate)(void);
typedef void (*sm64_port_fn_static_surfaces_load)(const struct SM64PortAbiSurface *surfaces, uint32_t num);
typedef int32_t (*sm64_port_fn_mario_create)(float x, float y, float z);
typedef void (*sm64_port_fn_mario_tick)(int32_t mario_id, const struct SM64PortAbiMarioInputs *inputs,
	struct SM64PortAbiMarioState *out_state, struct SM64PortAbiMarioGeometryBuffers *out_buffers);
typedef void (*sm64_port_fn_mario_delete)(int32_t mario_id);
typedef void (*sm64_port_fn_set_position)(int32_t mario_id, float x, float y, float z);
typedef void (*sm64_port_fn_set_angle)(int32_t mario_id, float x, float y, float z);
typedef void (*sm64_port_fn_set_velocity)(int32_t mario_id, float x, float y, float z);
typedef void (*sm64_port_fn_set_forward_velocity)(int32_t mario_id, float vel);
typedef void (*sm64_port_fn_set_health)(int32_t mario_id, uint16_t health);
typedef void (*sm64_port_fn_set_action)(int32_t mario_id, uint32_t action);
typedef uint32_t (*sm64_port_fn_surface_object_create)(const struct SM64PortAbiSurfaceObject *object);
typedef void (*sm64_port_fn_surface_object_move)(uint32_t object_id,
	const struct SM64PortAbiObjectTransform *transform);
typedef void (*sm64_port_fn_surface_object_delete)(uint32_t object_id);
typedef void (*sm64_port_fn_register_debug_print)(void (*callback)(const char *));
typedef void (*sm64_port_fn_register_play_sound)(void (*callback)(uint32_t bits, float *position));

/* ---------- module state ---------- */

typedef struct SM64PortActor
{
	bool used;
	int32_t mario_id;
	SM64PortInput input;
	struct SM64PortAbiMarioState previous;   /* the tick before the last one */
	struct SM64PortAbiMarioState current;    /* the last completed tick */
	bool have_previous;
	struct SM64PortAbiMarioGeometryBuffers geometry;
	float *geometry_storage;
} SM64PortActor;

typedef enum SM64PortPhase
{
	SM64_PORT_PHASE_OFF = 0,       /* Init() was never called, or Shutdown() ran */
	SM64_PORT_PHASE_FAILED,        /* tried and gave up; see g_status */
	SM64_PORT_PHASE_LIBRARY,       /* libsm64 loaded, no ROM yet */
	SM64_PORT_PHASE_READY          /* libsm64 initialised with a ROM */
} SM64PortPhase;

static SM64PortPhase g_phase = SM64_PORT_PHASE_OFF;
static char g_status[256];
static sm64_port_library g_library;
static SM64PortConfig g_config;
static SM64PortHost g_host;
static uint8_t *g_texture_atlas;
static uint8_t *g_rom;
static size_t g_rom_size;
static SM64PortActor g_actors[SM64_PORT_MAX_ACTORS];
static SM64PortClock g_clock;
static double g_alpha;
static uint32_t g_surface_count;
static SM64PortStats g_stats;

/* libsm64's entry points, resolved when the library is loaded. */
static sm64_port_fn_global_init g_global_init;
static sm64_port_fn_global_terminate g_global_terminate;
static sm64_port_fn_static_surfaces_load g_static_surfaces_load;
static sm64_port_fn_mario_create g_mario_create;
static sm64_port_fn_mario_tick g_mario_tick;
static sm64_port_fn_mario_delete g_mario_delete;
static sm64_port_fn_set_position g_set_position;
static sm64_port_fn_set_angle g_set_angle;
static sm64_port_fn_set_velocity g_set_velocity;
static sm64_port_fn_set_forward_velocity g_set_forward_velocity;
static sm64_port_fn_set_health g_set_health;
static sm64_port_fn_set_action g_set_action;
static sm64_port_fn_surface_object_create g_surface_object_create;
static sm64_port_fn_surface_object_move g_surface_object_move;
static sm64_port_fn_surface_object_delete g_surface_object_delete;
static sm64_port_fn_register_debug_print g_register_debug_print;
static sm64_port_fn_register_play_sound g_register_play_sound;

/* ---------- logging ---------- */

static void port_log(const char *format, ...)
{
	char message[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (g_host.log_message)
		g_host.log_message(g_host.context, message);
}

static void port_set_status(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(g_status, sizeof(g_status), format, arguments);
	va_end(arguments);
}

const char *SM64Port_StatusMessage(void)
{
	return g_status[0] ? g_status : NULL;
}

/* ---------- configuration ---------- */

void SM64Port_ConfigDefaults(SM64PortConfig *out_config)
{
	if (!out_config)
		return;

	out_config->halo_units_per_sm64_unit = SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT;
	out_config->tick_rate = (double)SM64_PORT_TICKS_PER_SECOND;
	out_config->max_substeps = SM64_PORT_DEFAULT_MAX_SUBSTEPS;
	out_config->deadzone = 0.08f;
	out_config->sensitivity = 1.0f;
	out_config->invert_y = false;
	out_config->collision_radius = 40.0f;
	out_config->max_surfaces = 4096;
	out_config->debug_draw = true;
	out_config->show_collision = false;
	out_config->draw_mesh = false;
	out_config->possess_player = false;
	out_config->library_path = NULL;
	out_config->rom_path = NULL;
	out_config->search_directory = NULL;
}

/* ---------- coordinate conversions (design note 2) ---------- */

/* Halo: right-handed, Z up, yaw 0 along +X, forward = (cos yaw, sin yaw, 0).
   libsm64: right-handed, Y up, yaw 0 along +Z, forward = (sin yaw, 0,
   cos yaw) - measured, see port/libsm64_port/TEST_RESULTS.md.

   The mapping that keeps both systems right-handed (a left-handed one would
   mirror the level and reverse every turn) is

       Halo = (k * sm64.x, -k * sm64.z, k * sm64.y)
       sm64 = (halo.x / k, halo.z / k, -halo.y / k)

   with k the number of Halo units in one SM64 unit.  Angles then differ by a
   quarter turn: halo yaw = sm64 yaw - pi/2. */

SM64PortVector3 SM64Port_HaloToSM64Position(SM64PortVector3 halo, float halo_units_per_sm64_unit)
{
	SM64PortVector3 sm64;
	float scale = halo_units_per_sm64_unit > 0.0f ? 1.0f / halo_units_per_sm64_unit : 100.0f;

	sm64.x = halo.x * scale;
	sm64.y = halo.z * scale;
	sm64.z = -halo.y * scale;
	return sm64;
}

SM64PortVector3 SM64Port_SM64ToHaloPosition(SM64PortVector3 sm64, float halo_units_per_sm64_unit)
{
	SM64PortVector3 halo;
	float scale = halo_units_per_sm64_unit > 0.0f ? halo_units_per_sm64_unit : 0.01f;

	halo.x = sm64.x * scale;
	halo.y = -sm64.z * scale;
	halo.z = sm64.y * scale;
	return halo;
}

/* Directions are the same mapping without the scale: a direction stays a
   direction. */
SM64PortVector3 SM64Port_HaloToSM64Direction(SM64PortVector3 halo)
{
	SM64PortVector3 sm64;

	sm64.x = halo.x;
	sm64.y = halo.z;
	sm64.z = -halo.y;
	return sm64;
}

SM64PortVector3 SM64Port_SM64ToHaloDirection(SM64PortVector3 sm64)
{
	SM64PortVector3 halo;

	halo.x = sm64.x;
	halo.y = -sm64.z;
	halo.z = sm64.y;
	return halo;
}

/* Wrap an angle into (-pi, pi]. */
static float port_wrap_pi(float angle)
{
	const float two_pi = 6.28318530717958647692f;

	angle = fmodf(angle, two_pi);
	if (angle > 3.14159265358979323846f)
		angle -= two_pi;
	else if (angle <= -3.14159265358979323846f)
		angle += two_pi;
	return angle;
}

float SM64Port_HaloToSM64Yaw(float halo_yaw)
{
	return port_wrap_pi(halo_yaw + 1.57079632679489661923f);
}

float SM64Port_SM64ToHaloYaw(float sm64_yaw)
{
	return port_wrap_pi(sm64_yaw - 1.57079632679489661923f);
}

/* ---------- input mapping (design note 4) ---------- */

/* libsm64 works out where Mario goes from the stick and from the camera: the
   stick is a direction *relative to the camera*, and the camera is given as
   a vector, "camLook", which the library turns into a yaw with
   atan2(camLookZ, camLookX).

   Measured with the real library: pushing the stick forward with camLook = c
   walks Mario along -c, exactly, for every direction.  So to walk in a world
   direction d, hand the library camLook = -d and keep the stick on the
   forward axis, whose length is the analogue magnitude the player asked for.
   That gives us the direction and the speed in one pair of values, and Mario
   turns to face where he is going. */
void SM64Port_HaloInputToSM64Input(const SM64PortInput *input, float deadzone, float sensitivity,
	bool invert_y, float *out_stick_x, float *out_stick_y, float *out_cam_look_x, float *out_cam_look_z)
{
	float x, y, magnitude;
	float scale = sensitivity > 0.0f ? sensitivity : 1.0f;

	if (!input)
	{
		if (out_stick_x) *out_stick_x = 0.0f;
		if (out_stick_y) *out_stick_y = 0.0f;
		if (out_cam_look_x) *out_cam_look_x = 0.0f;
		if (out_cam_look_z) *out_cam_look_z = -1.0f;
		return;
	}

	x = input->move_x * scale;
	y = input->move_y * scale;
	if (invert_y)
		y = -y;

	/* Halo's ground plane is X/Y; libsm64 wants the same direction in X/Z,
	   through the direction mapping above. */
	magnitude = sqrtf(x * x + y * y);

	/* A circular dead zone: below it the player meant to stand still, above
	   it the stick's full range is spread over what is left. */
	if (magnitude <= deadzone || magnitude < 1.0e-6f)
	{
		if (out_stick_x) *out_stick_x = 0.0f;
		if (out_stick_y) *out_stick_y = 0.0f;
		if (out_cam_look_x) *out_cam_look_x = 0.0f;
		if (out_cam_look_z) *out_cam_look_z = -1.0f;
		return;
	}

	{
		float wanted_x = x;            /* Halo space */
		float wanted_y = y;
		float wanted_length = magnitude;
		float normalised_x = wanted_x / wanted_length;
		float normalised_y = wanted_y / wanted_length;
		float sm64_x = normalised_x;   /* direction mapping: (x, y) -> (x, y, -y) */
		float sm64_z = -normalised_y;
		float clamped = (magnitude - deadzone) / (1.0f - deadzone);

		if (clamped > 1.0f)
			clamped = 1.0f;

		/* camLook is minus the direction we want to travel in. */
		if (out_cam_look_x) *out_cam_look_x = -sm64_x;
		if (out_cam_look_z) *out_cam_look_z = -sm64_z;
		if (out_stick_x) *out_stick_x = 0.0f;
		if (out_stick_y) *out_stick_y = clamped;
	}
}

/* ---------- the fixed-step clock (design note 3) ---------- */

void SM64Port_ClockInit(SM64PortClock *clock, double step_seconds, int max_steps)
{
	if (!clock)
		return;
	clock->step = step_seconds > 0.0 ? step_seconds : SM64_PORT_TICK_SECONDS;
	clock->accumulator = 0.0;
	clock->max_steps = max_steps > 0 ? max_steps : SM64_PORT_DEFAULT_MAX_SUBSTEPS;
	clock->total_steps = 0;
}

void SM64Port_ClockReset(SM64PortClock *clock)
{
	if (!clock)
		return;
	clock->accumulator = 0.0;
	clock->total_steps = 0;
}

int SM64Port_ClockAdvance(SM64PortClock *clock, double dt, double *out_alpha)
{
	int steps = 0;

	if (!clock)
	{
		if (out_alpha) *out_alpha = 0.0;
		return 0;
	}

	/* A frame that took longer than the budget (a level load, a breakpoint)
	   must not be paid back with hundreds of ticks: drop the excess.  Time
	   that is dropped is real time the actor does not move in, which is the
	   usual and least bad answer. */
	if (!(dt > 0.0))
	{
		if (out_alpha) *out_alpha = (double)clock->accumulator / clock->step;
		return 0;
	}
	if (dt > clock->step * clock->max_steps)
		dt = clock->step * clock->max_steps;

	clock->accumulator += dt;
	while (clock->accumulator >= clock->step && steps < clock->max_steps)
	{
		clock->accumulator -= clock->step;
		clock->total_steps++;
		steps++;
	}

	/* Guard against a step so small that floating point refuses to make
	   progress. */
	if (clock->accumulator < 0.0)
		clock->accumulator = 0.0;

	if (out_alpha)
		*out_alpha = clock->accumulator / clock->step;
	return steps;
}

/* ---------- collision surface set (design note 5) ---------- */

struct SM64PortSurfaceSet
{
	SM64PortSurface *surfaces;
	uint32_t capacity;
	uint32_t count;
};

SM64PortSurfaceSet *SM64Port_SurfaceSetCreate(uint32_t capacity)
{
	SM64PortSurfaceSet *set;

	if (capacity == 0)
		capacity = 256;
	set = (SM64PortSurfaceSet *)calloc(1, sizeof(SM64PortSurfaceSet));
	if (!set)
		return NULL;
	set->surfaces = (SM64PortSurface *)calloc(capacity, sizeof(SM64PortSurface));
	if (!set->surfaces)
	{
		free(set);
		return NULL;
	}
	set->capacity = capacity;
	set->count = 0;
	return set;
}

void SM64Port_SurfaceSetDestroy(SM64PortSurfaceSet *set)
{
	if (!set)
		return;
	free(set->surfaces);
	free(set);
}

void SM64Port_SurfaceSetClear(SM64PortSurfaceSet *set)
{
	if (!set)
		return;
	set->count = 0;
}

/* libsm64 keeps the surface vertices as 32-bit integers, so a position is
   rounded; at the default scale one integer is a hundredth of a Halo unit,
   well below anything the player can see.  Out-of-range coordinates are
   rejected instead of wrapping around. */
/* A Halo-space point becomes a libsm64 vertex: the same rotation and scale as
   every other position (Halo's Z is libsm64's Y, and Halo's Y is minus
   libsm64's Z), rounded to the 32-bit integers libsm64 keeps.  The mapping
   preserves orientation, so a triangle whose winding was corrected here keeps
   the normal it was given. */
static bool port_vertex_to_sm64(const float point[3], float units_per_sm64_unit, int32_t out[3])
{
	SM64PortVector3 halo, sm64;
	double values[3];
	int axis;

	halo.x = point[0];
	halo.y = point[1];
	halo.z = point[2];
	sm64 = SM64Port_HaloToSM64Position(halo, units_per_sm64_unit);

	values[0] = sm64.x;
	values[1] = sm64.y;
	values[2] = sm64.z;
	for (axis = 0; axis < 3; axis++)
	{
		if (values[axis] < -2000000000.0 || values[axis] > 2000000000.0)
			return false;
		out[axis] = (int32_t)(values[axis] >= 0.0 ? values[axis] + 0.5 : values[axis] - 0.5);
	}
	return true;
}

bool SM64Port_SurfaceSetAddTriangle(SM64PortSurfaceSet *set, const float a[3], const float b[3],
	const float c[3], const float *normal, uint16_t terrain)
{
	SM64PortSurface *surface;
	int32_t vertices[3][3];
	const float *ordered[3];
	int axis;

	if (!set || !a || !b || !c)
		return false;
	if (set->count >= set->capacity)
		return false;

	/* Order the triangle the way the caller's normal asks for. */
	{
		float u[3], v[3], cross[3], dot;

		ordered[0] = a; ordered[1] = b; ordered[2] = c;
		if (normal)
		{
			for (axis = 0; axis < 3; axis++)
			{
				u[axis] = b[axis] - a[axis];
				v[axis] = c[axis] - a[axis];
			}
			cross[0] = u[1] * v[2] - u[2] * v[1];
			cross[1] = u[2] * v[0] - u[0] * v[2];
			cross[2] = u[0] * v[1] - u[1] * v[0];
			dot = cross[0] * normal[0] + cross[1] * normal[1] + cross[2] * normal[2];
			if (dot < 0.0f)
			{
				const float *swap = ordered[1];
				ordered[1] = ordered[2];
				ordered[2] = swap;
			}
		}
	}

	for (axis = 0; axis < 3; axis++)
	{
		if (!port_vertex_to_sm64(ordered[axis], g_config.halo_units_per_sm64_unit, vertices[axis]))
			return false;
	}

	surface = &set->surfaces[set->count];
	surface->type = 0;    /* SURFACE_DEFAULT */
	surface->force = 0;
	surface->terrain = terrain;
	for (axis = 0; axis < 3; axis++)
	{
		int component;
		for (component = 0; component < 3; component++)
			surface->vertices[axis][component] = vertices[axis][component];
	}
	set->count++;
	return true;
}

bool SM64Port_SurfaceSetAddPolygon(SM64PortSurfaceSet *set, const float (*points)[3], int point_count,
	const float *normal, uint16_t terrain)
{
	int i;
	bool added = false;

	if (!set || !points || point_count < 3)
		return false;

	/* Halo's collision surfaces are convex, so a fan from the first point
	   covers the polygon exactly. */
	for (i = 1; i + 1 < point_count; i++)
	{
		if (SM64Port_SurfaceSetAddTriangle(set, points[0], points[i], points[i + 1], normal, terrain))
			added = true;
	}
	return added;
}

const SM64PortSurface *SM64Port_SurfaceSetSurfaces(const SM64PortSurfaceSet *set, uint32_t *out_count)
{
	if (out_count)
		*out_count = set ? set->count : 0;
	return set ? set->surfaces : NULL;
}

/* A room to walk around in when there is no level: the main menu, a map whose
   collision is not loaded yet, or the host tests.  All coordinates are Halo
   units, Z up.  It is laid out so that nothing overlaps:

       +---------------------------+   half_extent (20 by default)
       |  ceiling (z = 4.5)        |
       |  over x in [-20, -10]     |
       |                           |
       |      ramp  x in [-5, 5]   |   rises from z = 0 to z = 2
       |                           |
       |                 ledge     |   top at z = 2, x in [14, 19]
       +---------------------------+

   That is: a floor to walk on, four walls to stop at, a ramp that can be
   walked up (about 27 degrees), a ledge to jump onto, and a ceiling low
   enough to hit.  Everything the integration tests exercise. */
bool SM64Port_BuildTestRoom(SM64PortSurfaceSet *set, float half_extent)
{
	const float floor_normal[3] = { 0.0f, 0.0f, 1.0f };
	const float ceiling_normal[3] = { 0.0f, 0.0f, -1.0f };
	const float wall_height = 8.0f;
	const float h = half_extent > 0.0f ? half_extent : 20.0f;
	const float ramp_low_x = -h * 0.25f;    /* -5 with the default extent */
	const float ramp_high_x = h * 0.25f;    /* +5 */
	const float ramp_half_width = h * 0.15f;
	const float ramp_height = 2.0f;
	int i;

	/* The floor: two triangles spanning the room. */
	{
		const float a[3] = { -h, -h, 0.0f }, b[3] = { h, -h, 0.0f };
		const float c[3] = { h, h, 0.0f }, d[3] = { -h, h, 0.0f };

		if (!SM64Port_SurfaceSetAddTriangle(set, a, b, c, floor_normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, a, c, d, floor_normal, SM64_PORT_TERRAIN_STONE))
			return false;
	}

	/* Four walls: -X, +X, -Y, +Y, each facing into the room. */
	for (i = 0; i < 4; i++)
	{
		float a[3], b[3], c[3], d[3], n[3];

		n[0] = n[1] = n[2] = 0.0f;
		switch (i)
		{
		case 0: /* -X, facing +X */
			a[0] = b[0] = c[0] = d[0] = -h;
			a[1] = d[1] = -h; b[1] = c[1] = h;
			n[0] = 1.0f;
			break;
		case 1: /* +X, facing -X */
			a[0] = b[0] = c[0] = d[0] = h;
			a[1] = d[1] = h; b[1] = c[1] = -h;
			n[0] = -1.0f;
			break;
		case 2: /* -Y, facing +Y */
			a[1] = b[1] = c[1] = d[1] = -h;
			a[0] = d[0] = h; b[0] = c[0] = -h;
			n[1] = 1.0f;
			break;
		default: /* +Y, facing -Y */
			a[1] = b[1] = c[1] = d[1] = h;
			a[0] = d[0] = -h; b[0] = c[0] = h;
			n[1] = -1.0f;
			break;
		}
		a[2] = b[2] = 0.0f;
		c[2] = d[2] = wall_height;
		if (!SM64Port_SurfaceSetAddTriangle(set, a, b, c, n, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, a, c, d, n, SM64_PORT_TERRAIN_STONE))
			return false;
	}

	/* A ramp across the middle of the room, its low edge flush with the floor
	   so that it can be walked onto, rising to ramp_height at its high edge. */
	{
		const float a[3] = { ramp_low_x, -ramp_half_width, 0.0f };
		const float b[3] = { ramp_high_x, -ramp_half_width, ramp_height };
		const float c[3] = { ramp_high_x, ramp_half_width, ramp_height };
		const float d[3] = { ramp_low_x, ramp_half_width, 0.0f };
		float normal[3];
		float run = ramp_high_x - ramp_low_x;
		float length = sqrtf(run * run + ramp_height * ramp_height);

		normal[0] = -ramp_height / length;   /* up the slope is +X */
		normal[1] = 0.0f;
		normal[2] = run / length;
		if (!SM64Port_SurfaceSetAddTriangle(set, a, b, c, normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, a, c, d, normal, SM64_PORT_TERRAIN_STONE))
			return false;
	}

	/* A ledge against the +X wall: a top at z = 2 and the side that faces the
	   rest of the room, so it can be jumped onto. */
	{
		const float ledge_low_x = h * 0.7f;     /* 14 */
		const float ledge_high_x = h * 0.95f;   /* 19 */
		const float ledge_half_width = h * 0.25f;
		const float top[3] = { ledge_low_x, -ledge_half_width, 2.0f };
		const float top2[3] = { ledge_high_x, -ledge_half_width, 2.0f };
		const float top3[3] = { ledge_high_x, ledge_half_width, 2.0f };
		const float top4[3] = { ledge_low_x, ledge_half_width, 2.0f };
		const float side_normal[3] = { -1.0f, 0.0f, 0.0f };
		const float s0[3] = { ledge_low_x, -ledge_half_width, 0.0f };
		const float s1[3] = { ledge_low_x, ledge_half_width, 0.0f };
		const float s2[3] = { ledge_low_x, ledge_half_width, 2.0f };
		const float s3[3] = { ledge_low_x, -ledge_half_width, 2.0f };

		if (!SM64Port_SurfaceSetAddTriangle(set, top, top2, top3, floor_normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, top, top3, top4, floor_normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, s0, s1, s2, side_normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, s0, s2, s3, side_normal, SM64_PORT_TERRAIN_STONE))
			return false;
	}

	/* A ceiling over the -X half of the room, low enough to hit with a jump. */
	{
		const float ceiling_height = 4.5f;
		const float c0[3] = { -h, -h, ceiling_height };
		const float c1[3] = { -h * 0.5f, -h, ceiling_height };
		const float c2[3] = { -h * 0.5f, h, ceiling_height };
		const float c3[3] = { -h, h, ceiling_height };

		if (!SM64Port_SurfaceSetAddTriangle(set, c0, c2, c1, ceiling_normal, SM64_PORT_TERRAIN_STONE))
			return false;
		if (!SM64Port_SurfaceSetAddTriangle(set, c0, c3, c2, ceiling_normal, SM64_PORT_TERRAIN_STONE))
			return false;
	}

	return true;
}

/* ---------- loading libsm64 ---------- */

static const char *const k_library_names[] = {
#if defined(_WIN32)
	"sm64.dll", "libsm64.dll",
#elif defined(__APPLE__)
	"libsm64.dylib",
#else
	"libsm64.so",
#endif
	NULL
};

static bool port_resolve_symbols(void)
{
	struct { void **target; const char *name; bool optional; } entries[] = {
		{ (void **)&g_global_init, "sm64_global_init", false },
		{ (void **)&g_global_terminate, "sm64_global_terminate", false },
		{ (void **)&g_static_surfaces_load, "sm64_static_surfaces_load", false },
		{ (void **)&g_mario_create, "sm64_mario_create", false },
		{ (void **)&g_mario_tick, "sm64_mario_tick", false },
		{ (void **)&g_mario_delete, "sm64_mario_delete", false },
		{ (void **)&g_set_position, "sm64_set_mario_position", true },
		{ (void **)&g_set_angle, "sm64_set_mario_angle", true },
		{ (void **)&g_set_velocity, "sm64_set_mario_velocity", true },
		{ (void **)&g_set_forward_velocity, "sm64_set_mario_forward_velocity", true },
		{ (void **)&g_set_health, "sm64_set_mario_health", true },
		{ (void **)&g_set_action, "sm64_set_mario_action", true },
		{ (void **)&g_surface_object_create, "sm64_surface_object_create", true },
		{ (void **)&g_surface_object_move, "sm64_surface_object_move", true },
		{ (void **)&g_surface_object_delete, "sm64_surface_object_delete", true },
		{ (void **)&g_register_debug_print, "sm64_register_debug_print_function", true },
		{ (void **)&g_register_play_sound, "sm64_register_play_sound_function", true },
	};
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
	{
		*entries[i].target = port_library_symbol(g_library, entries[i].name);
		if (!*entries[i].target && !entries[i].optional)
		{
			port_set_status("libsm64: %s is missing, so this is not a libsm64 library",
				entries[i].name);
			return false;
		}
	}
	return true;
}

/* dlopen() searches the system's library path, not the working directory, so
   a bare file name is tried there too: that is where a player puts the
   library, next to the executable. */
static sm64_port_library port_library_open_searched(const char *path)
{
	sm64_port_library handle = port_library_open(path);

	if (!handle && path && path[0] && !strchr(path, '/')
#if defined(_WIN32)
		&& !strchr(path, '\\')
#endif
		)
	{
		char relative[260];
		snprintf(relative, sizeof(relative), ".%c%s",
#if defined(_WIN32)
			'\\',
#else
			'/',
#endif
			path);
		handle = port_library_open(relative);
	}
	return handle;
}

/* The path of a file in the search directory (config.search_directory), or
   false when there is none: the ROM and the library are looked for there
   first, then in the working directory. */
static bool port_search_path(const char *name, char *out_path, size_t out_path_size)
{
#if defined(_WIN32)
	const char separator = '\\';
#else
	const char separator = '/';
#endif
	int written;

	if (!g_config.search_directory || !g_config.search_directory[0] || !name || !name[0])
		return false;
	written = snprintf(out_path, out_path_size, "%s%c%s", g_config.search_directory,
		separator, name);
	return written > 0 && (size_t)written < out_path_size;
}

static bool port_load_library(void)
{
	int i;

	if (g_config.library_path && g_config.library_path[0])
	{
		g_library = port_library_open_searched(g_config.library_path);
		if (!g_library)
		{
			port_set_status("libsm64: cannot load \"%s\"", g_config.library_path);
			return false;
		}
	}
	else
	{
		for (i = 0; k_library_names[i]; i++)
		{
			char path[512];

			if (port_search_path(k_library_names[i], path, sizeof(path)))
				g_library = port_library_open(path);
			if (!g_library)
				g_library = port_library_open_searched(k_library_names[i]);
			if (g_library)
				break;
		}
		if (!g_library)
		{
			port_set_status("libsm64: no %s next to the executable; the module stays off "
				"(see port/libsm64_port/README_LIBSM64_PORT.md)",
#if defined(_WIN32)
				"sm64.dll"
#elif defined(__APPLE__)
				"libsm64.dylib"
#else
				"libsm64.so"
#endif
				);
			return false;
		}
	}

	if (!port_resolve_symbols())
	{
		port_library_close(g_library);
		g_library = NULL;
		return false;
	}
	return true;
}

/* ---------- the ROM ---------- */

/* Names the module looks for when config gives no path.  The user supplies
   their own legally obtained ROM: none is part of this repository. */
static const char *const k_rom_names[] = {
	"baserom.us.z64", "sm64.us.z64", "baserom.us.n64", "baserom.z64", "sm64.z64", NULL
};

static bool port_load_rom(const char *path, uint8_t **out_data, size_t *out_size)
{
	if (!g_host.read_file)
		return false;
	return g_host.read_file(g_host.context, path, out_data, out_size) && *out_data && *out_size > 0;
}

static bool port_find_and_load_rom(void)
{
	int i;
	bool loaded = false;

	if (g_config.rom_path && g_config.rom_path[0])
	{
		loaded = port_load_rom(g_config.rom_path, &g_rom, &g_rom_size);
		if (!loaded)
			port_log("libsm64: the ROM \"%s\" could not be read", g_config.rom_path);
	}
	else
	{
		for (i = 0; k_rom_names[i] && !loaded; i++)
		{
			char path[512];

			if (port_search_path(k_rom_names[i], path, sizeof(path)))
				loaded = port_load_rom(path, &g_rom, &g_rom_size);
			if (!loaded)
				loaded = port_load_rom(k_rom_names[i], &g_rom, &g_rom_size);
		}
	}

	if (!loaded)
	{
		port_set_status("libsm64: no SM64 (US) ROM found; put your own baserom.us.z64 in the game's "
			"data folder (or the working directory), or set \"sm64.rom\" in config.toml. The "
			"module stays off.");
		return false;
	}

	/* A US SM64 ROM is 8 MiB; accept anything plausibly sized and let the
	   library deal with the contents. */
	if (g_rom_size < 1024 * 1024 || g_rom_size > 64 * 1024 * 1024)
	{
		port_log("libsm64: the ROM is %lu bytes, which is not the size of an SM64 ROM "
			"(%lu expected); ignoring it", (unsigned long)g_rom_size, (unsigned long)(8 * 1024 * 1024));
		if (g_host.free_file)
			g_host.free_file(g_host.context, g_rom);
		g_rom = NULL;
		g_rom_size = 0;
		port_set_status("libsm64: the ROM found is not an SM64 ROM (wrong size)");
		return false;
	}
	return true;
}

/* ---------- life cycle ---------- */

SM64PortResult SM64Port_Init(const SM64PortConfig *config, const SM64PortHost *host)
{
	if (g_phase == SM64_PORT_PHASE_READY || g_phase == SM64_PORT_PHASE_LIBRARY)
		SM64Port_Shutdown();

	SM64Port_ConfigDefaults(&g_config);
	if (config)
	{
		SM64PortConfig defaults;
		SM64Port_ConfigDefaults(&defaults);
		g_config = *config;
		/* A caller that zeroed the struct gets the defaults for the numbers
		   it left at zero, rather than a module that cannot move. */
		if (g_config.halo_units_per_sm64_unit <= 0.0f)
			g_config.halo_units_per_sm64_unit = defaults.halo_units_per_sm64_unit;
		if (g_config.tick_rate <= 0.0)
			g_config.tick_rate = defaults.tick_rate;
		if (g_config.max_substeps <= 0)
			g_config.max_substeps = defaults.max_substeps;
		if (g_config.collision_radius <= 0.0f)
			g_config.collision_radius = defaults.collision_radius;
		if (g_config.max_surfaces == 0)
			g_config.max_surfaces = defaults.max_surfaces;
	}
	memset(&g_host, 0, sizeof(g_host));
	if (host)
		g_host = *host;

	g_status[0] = '\0';
	SM64Port_ClockInit(&g_clock, 1.0 / g_config.tick_rate, g_config.max_substeps);
	g_alpha = 0.0;
	memset(&g_stats, 0, sizeof(g_stats));

	if (!port_load_library())
	{
		g_phase = SM64_PORT_PHASE_FAILED;
		port_log("%s", g_status);
		return SM64_PORT_ERROR_LIBRARY;
	}
	g_phase = SM64_PORT_PHASE_LIBRARY;

	if (!port_find_and_load_rom())
	{
		g_phase = SM64_PORT_PHASE_FAILED;
		port_log("%s", g_status);
		return SM64_PORT_ERROR_ASSETS;
	}

	g_texture_atlas = (uint8_t *)calloc(4, (size_t)SM64_PORT_TEXTURE_WIDTH * SM64_PORT_TEXTURE_HEIGHT);
	if (!g_texture_atlas)
	{
		port_set_status("libsm64: out of memory for the texture atlas");
		g_phase = SM64_PORT_PHASE_FAILED;
		return SM64_PORT_ERROR_MEMORY;
	}

	if (g_register_debug_print)
		g_register_debug_print(NULL);
	if (g_register_play_sound)
		g_register_play_sound(NULL);

	g_global_init(g_rom, g_texture_atlas);
	g_phase = SM64_PORT_PHASE_READY;
	port_set_status("libsm64 ready: %lu KiB ROM, %g Halo units per SM64 unit",
		(unsigned long)(g_rom_size / 1024), (double)g_config.halo_units_per_sm64_unit);
	port_log("libsm64: %s", g_status);
	return SM64_PORT_OK;
}

void SM64Port_Shutdown(void)
{
	int i;

	if (g_phase == SM64_PORT_PHASE_READY || g_phase == SM64_PORT_PHASE_LIBRARY)
	{
		for (i = 0; i < SM64_PORT_MAX_ACTORS; i++)
		{
			if (g_actors[i].used)
				SM64Port_DestroyActor((SM64PortActorId)i);
		}
		if (g_phase == SM64_PORT_PHASE_READY && g_global_terminate)
			g_global_terminate();
	}

	free(g_texture_atlas);
	g_texture_atlas = NULL;
	if (g_rom)
	{
		if (g_host.free_file)
			g_host.free_file(g_host.context, g_rom);
		else
			free(g_rom);
		g_rom = NULL;
	}
	g_rom_size = 0;
	port_library_close(g_library);
	g_library = NULL;
	g_phase = SM64_PORT_PHASE_OFF;
	g_surface_count = 0;
}

bool SM64Port_IsReady(void)
{
	return g_phase == SM64_PORT_PHASE_READY;
}

/* ---------- actors ---------- */

static SM64PortActor *port_actor(SM64PortActorId id)
{
	if (id < 0 || id >= SM64_PORT_MAX_ACTORS)
		return NULL;
	if (!g_actors[id].used)
		return NULL;
	return &g_actors[id];
}

SM64PortResult SM64Port_CreateActor(SM64PortVector3 halo_position, float halo_yaw,
	SM64PortActorId *out_id)
{
	SM64PortVector3 sm64;
	int index;
	int32_t mario_id;
	size_t storage_floats;

	if (g_phase != SM64_PORT_PHASE_READY)
		return SM64_PORT_ERROR_NOT_INITIALIZED;

	for (index = 0; index < SM64_PORT_MAX_ACTORS; index++)
	{
		if (!g_actors[index].used)
			break;
	}
	if (index >= SM64_PORT_MAX_ACTORS)
	{
		port_set_status("libsm64: %d actors already; that is the limit", SM64_PORT_MAX_ACTORS);
		return SM64_PORT_ERROR_LIMIT;
	}

	sm64 = SM64Port_HaloToSM64Position(halo_position, g_config.halo_units_per_sm64_unit);

	/* libsm64 refuses to create a Mario with no floor under him: it returns
	   -1.  Try the position, then a little above it, then straight up. */
	mario_id = g_mario_create(sm64.x, sm64.y, sm64.z);
	if (mario_id < 0)
		mario_id = g_mario_create(sm64.x, sm64.y + 100.0f, sm64.z);
	if (mario_id < 0)
		mario_id = g_mario_create(sm64.x, sm64.y + 500.0f, sm64.z);
	if (mario_id < 0)
	{
		port_set_status("libsm64: no collision surface under (%.1f, %.1f, %.1f); "
			"Mario needs a floor to spawn on", (double)halo_position.x, (double)halo_position.y,
			(double)halo_position.z);
		return SM64_PORT_ERROR_LIMIT;
	}

	{
		SM64PortActor *actor = &g_actors[index];
		memset(actor, 0, sizeof(*actor));

		/* libsm64 writes Mario's triangles into these buffers without
		   checking their size, so they are always the full 1024 triangles. */
		storage_floats = (size_t)SM64_PORT_GEO_MAX_TRIANGLES * (9 + 9 + 9 + 6);
		actor->geometry_storage = (float *)calloc(storage_floats, sizeof(float));
		if (!actor->geometry_storage)
		{
			g_mario_delete(mario_id);
			port_set_status("libsm64: out of memory for an actor");
			return SM64_PORT_ERROR_MEMORY;
		}
		actor->geometry.position = actor->geometry_storage;
		actor->geometry.normal = actor->geometry.position + SM64_PORT_GEO_MAX_TRIANGLES * 9;
		actor->geometry.color = actor->geometry.normal + SM64_PORT_GEO_MAX_TRIANGLES * 9;
		actor->geometry.uv = actor->geometry.color + SM64_PORT_GEO_MAX_TRIANGLES * 9;
		actor->geometry.numTrianglesUsed = 0;

		actor->used = true;
		actor->mario_id = mario_id;
		actor->have_previous = false;
		memset(&actor->input, 0, sizeof(actor->input));
		memset(&actor->previous, 0, sizeof(actor->previous));
		memset(&actor->current, 0, sizeof(actor->current));

		if (out_id)
			*out_id = (SM64PortActorId)index;
		g_stats.actors++;
		return SM64_PORT_OK;
	}
}

SM64PortResult SM64Port_DestroyActor(SM64PortActorId id)
{
	SM64PortActor *actor = port_actor(id);

	if (!actor)
		return SM64_PORT_ERROR_BAD_ACTOR;

	if (g_phase == SM64_PORT_PHASE_READY)
		g_mario_delete(actor->mario_id);
	free(actor->geometry_storage);
	memset(actor, 0, sizeof(*actor));
	actor->mario_id = -1;
	if (g_stats.actors > 0)
		g_stats.actors--;
	return SM64_PORT_OK;
}

void SM64Port_DestroyAllActors(void)
{
	int i;

	for (i = 0; i < SM64_PORT_MAX_ACTORS; i++)
	{
		if (g_actors[i].used)
			SM64Port_DestroyActor((SM64PortActorId)i);
	}
}

uint32_t SM64Port_ActorCount(void)
{
	return g_stats.actors;
}

SM64PortResult SM64Port_SetInput(SM64PortActorId id, const SM64PortInput *input)
{
	SM64PortActor *actor = port_actor(id);

	if (!actor)
		return SM64_PORT_ERROR_BAD_ACTOR;
	if (input)
		actor->input = *input;
	else
		memset(&actor->input, 0, sizeof(actor->input));
	return SM64_PORT_OK;
}

SM64PortResult SM64Port_SetPosition(SM64PortActorId id, SM64PortVector3 halo_position, float halo_yaw)
{
	SM64PortActor *actor = port_actor(id);
	SM64PortVector3 sm64;

	if (!actor)
		return SM64_PORT_ERROR_BAD_ACTOR;
	if (g_phase != SM64_PORT_PHASE_READY)
		return SM64_PORT_ERROR_NOT_INITIALIZED;

	sm64 = SM64Port_HaloToSM64Position(halo_position, g_config.halo_units_per_sm64_unit);
	if (g_set_position)
		g_set_position(actor->mario_id, sm64.x, sm64.y, sm64.z);
	if (g_set_angle)
	{
		float sm64_yaw = SM64Port_HaloToSM64Yaw(halo_yaw);
		g_set_angle(actor->mario_id, 0.0f, sm64_yaw, 0.0f);
	}
	if (g_set_velocity)
		g_set_velocity(actor->mario_id, 0.0f, 0.0f, 0.0f);

	/* A teleport must not be interpolated across. */
	actor->have_previous = false;
	return SM64_PORT_OK;
}

/* ---------- the frame ---------- */

static void port_tick_actor(SM64PortActor *actor)
{
	struct SM64PortAbiMarioInputs inputs;
	float stick_x, stick_y, cam_look_x, cam_look_z;

	SM64Port_HaloInputToSM64Input(&actor->input, g_config.deadzone, g_config.sensitivity,
		g_config.invert_y, &stick_x, &stick_y, &cam_look_x, &cam_look_z);

	memset(&inputs, 0, sizeof(inputs));
	inputs.camLookX = cam_look_x;
	inputs.camLookZ = cam_look_z;
	inputs.stickX = stick_x;
	inputs.stickY = stick_y;
	inputs.buttonA = actor->input.button_a ? 1 : 0;
	inputs.buttonB = actor->input.button_b ? 1 : 0;
	inputs.buttonZ = actor->input.button_z ? 1 : 0;

	actor->previous = actor->current;
	g_mario_tick(actor->mario_id, &inputs, &actor->current, &actor->geometry);
	actor->have_previous = true;
}

int SM64Port_Update(double delta_time_seconds)
{
	int steps;
	int i;
	int step;
	clock_t started;

	if (g_phase != SM64_PORT_PHASE_READY)
		return -1;

	started = clock();
	steps = SM64Port_ClockAdvance(&g_clock, delta_time_seconds, &g_alpha);

	for (step = 0; step < steps; step++)
	{
		for (i = 0; i < SM64_PORT_MAX_ACTORS; i++)
		{
			if (g_actors[i].used)
				port_tick_actor(&g_actors[i]);
		}
	}

	g_stats.ticks_total += steps;
	g_stats.frames_total++;
	g_stats.ticks_last_frame = steps;
	g_stats.last_update_seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;
	return steps;
}

/* The state libsm64 reported, converted into Halo's frame of reference. */
static void port_convert_state(const struct SM64PortAbiMarioState *state, float scale,
	SM64PortState *out_state)
{
	SM64PortVector3 position, velocity;

	position.x = state->position[0];
	position.y = state->position[1];
	position.z = state->position[2];
	velocity.x = state->velocity[0];
	velocity.y = state->velocity[1];
	velocity.z = state->velocity[2];

	out_state->position = SM64Port_SM64ToHaloPosition(position, scale);
	out_state->velocity = SM64Port_SM64ToHaloDirection(velocity);
	out_state->velocity.x *= scale * (float)g_config.tick_rate;
	out_state->velocity.y *= scale * (float)g_config.tick_rate;
	out_state->velocity.z *= scale * (float)g_config.tick_rate;
	out_state->yaw = SM64Port_SM64ToHaloYaw(state->faceAngle);
	out_state->forward_velocity = state->forwardVelocity * scale * (float)g_config.tick_rate;
	out_state->health = state->health;
	out_state->action = state->action;
	out_state->anim_id = state->animID;
	out_state->anim_frame = state->animFrame;
	out_state->flags = state->flags;
	out_state->particle_flags = state->particleFlags;
	out_state->grounded = !(((state->action & SM64_PORT_ACT_GROUP_MASK) == SM64_PORT_ACT_GROUP_AIRBORNE)
		|| (state->action & SM64_PORT_ACT_FLAG_AIR));
}

static SM64PortResult port_get_interpolated(SM64PortActorId id, SM64PortState *out_state, bool interpolate)
{
	SM64PortActor *actor = port_actor(id);

	if (!actor || !out_state)
		return SM64_PORT_ERROR_BAD_ACTOR;

	if (!actor->have_previous || !interpolate)
	{
		port_convert_state(&actor->current, g_config.halo_units_per_sm64_unit, out_state);
		return SM64_PORT_OK;
	}

	{
		/* Between the last two ticks, so that an actor looks smooth at any
		   frame rate.  The angle takes the short way round. */
		struct SM64PortAbiMarioState blended = actor->previous;
		float alpha = (float)g_alpha;
		float previous_yaw = actor->previous.faceAngle;
		float delta_yaw = actor->current.faceAngle - previous_yaw;
		int axis;
		const float two_pi = 6.28318530717958647692f;

		if (delta_yaw > 3.14159265358979323846f)
			delta_yaw -= two_pi;
		else if (delta_yaw < -3.14159265358979323846f)
			delta_yaw += two_pi;

		for (axis = 0; axis < 3; axis++)
		{
			blended.position[axis] = (float)(actor->previous.position[axis]
				+ (actor->current.position[axis] - actor->previous.position[axis]) * (double)alpha);
			blended.velocity[axis] = (float)(actor->previous.velocity[axis]
				+ (actor->current.velocity[axis] - actor->previous.velocity[axis]) * (double)alpha);
		}
		blended.faceAngle = previous_yaw + delta_yaw * alpha;
		blended.forwardVelocity = (float)(actor->previous.forwardVelocity
			+ (actor->current.forwardVelocity - actor->previous.forwardVelocity) * (double)alpha);

		port_convert_state(&blended, g_config.halo_units_per_sm64_unit, out_state);
		return SM64_PORT_OK;
	}
}

SM64PortResult SM64Port_GetState(SM64PortActorId id, SM64PortState *out_state)
{
	return port_get_interpolated(id, out_state, true);
}

SM64PortResult SM64Port_GetTickState(SM64PortActorId id, SM64PortState *out_state)
{
	return port_get_interpolated(id, out_state, false);
}

/* ---------- collision ---------- */

SM64PortResult SM64Port_SetStaticCollision(const SM64PortSurface *surfaces, uint32_t count)
{
	if (g_phase != SM64_PORT_PHASE_READY)
		return SM64_PORT_ERROR_NOT_INITIALIZED;
	if (count > g_config.max_surfaces)
	{
		port_set_status("libsm64: %u collision surfaces is over the limit of %u "
			"(sm64.max_surfaces)", count, g_config.max_surfaces);
		return SM64_PORT_ERROR_LIMIT;
	}

	/* The public struct is laid out exactly like libsm64's own, which the
	   static asserts at the top of this file check. */
	g_static_surfaces_load((const struct SM64PortAbiSurface *)surfaces, count);
	g_surface_count = count;
	g_stats.surfaces = count;
	return SM64_PORT_OK;
}

uint32_t SM64Port_StaticCollisionCount(void)
{
	return g_surface_count;
}

/* ---------- statistics ---------- */

void SM64Port_GetStats(SM64PortStats *out_stats)
{
	if (!out_stats)
		return;
	*out_stats = g_stats;
}
