/*
SM64_PORT_HALO.C

The game side of the libsm64 module: settings, console commands, the frame
hook, the collision built out of the level's BSP, the debug drawing, and
driving the player's biped with Mario's movement when asked to.

It talks to the portable half of the module through include/sm64_port.h only,
which keeps that half testable on its own (see ../tests).

Notes

  - The module never changes how the game plays unless the player asks it to:
    it is off unless "sm64.enable" is set in config.toml, and an actor only
    exists after "sm64_spawn".
  - libsm64 is loaded at run time.  When the library or a ROM is missing, the
    module logs one line and stays off: the game plays normally.
  - Mario's collision is a window of the level around him, rebuilt as he
    walks (see rebuild_collision()).  libsm64 searches its surfaces linearly,
    so a whole Halo BSP, of tens of thousands of surfaces, would be far too
    slow to hand over whole.

CC0 1.0 Universal, like the rest of the port.
*/

#include "sm64_port_halo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm64_port.h"
#include "sm64_port_abi.h"

#include "cseries.h"
#include "math/real_math.h"

#include "camera/observer.h"
#include "game/player_control.h"
#include "game/players.h"
#include "input/input.h"
#include "main/console.h"
#include "objects/objects.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "render/render_debug.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

/* The settings file (config.toml), shared by every platform
   (port/linux/src/port_config.c). */
#include "port_config.h"

/* Every port logs through platform_log(), and every port finds its data
   through platform_data_root() (port/linux/src/xbox_files.c, which the
   Windows and Android ports share); only the Linux port has a header for
   either, so they are declared here. */
void platform_log(const char *format, ...);
const char *platform_data_root(void);

/* ---------- constants ---------- */

/* How many collision triangles the debug drawing shows at most. */
#define MAXIMUM_DEBUG_TRIANGLES 512

/* The default size, in Halo units, of the window of level geometry that
   becomes Mario's collision. */
#define DEFAULT_COLLISION_RADIUS 40.0f

/* Mario's body, in Halo units at the default scale: his capsule is 160 SM64
   units tall and his cylinder is 37 SM64 units in radius. */
#define MARIO_HEIGHT_HALO (SM64_PORT_MARIO_HEIGHT_SM64 * SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT)
#define MARIO_RADIUS_HALO (SM64_PORT_MARIO_RADIUS_SM64 * SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT)

/* ---------- module state ---------- */

/* A triangle in Halo space, kept for the collision wireframe. */
struct debug_triangle
{
	real_point3d points[3];
};

static struct
{
	boolean started;             /* the module was initialised */
	boolean attempted;           /* we tried, so do not try every frame */
	boolean enabled;             /* the settings ask for the module */
	boolean collision_ready;
	short collision_bsp_index;
	boolean debug_draw;
	boolean show_collision;
	boolean draw_mesh;
	boolean possess_player;
	long max_surfaces;
	SM64PortActorId actor;
	SM64PortSurfaceSet *surfaces;
	SM64PortVector3 collision_center;
	struct debug_triangle debug_triangles[MAXIMUM_DEBUG_TRIANGLES];
	long debug_triangle_count;
	long ticks;
	boolean f3_down;             /* F3 switches the module on and off */
} g_state;

/* ---------- settings ---------- */

static boolean setting_boolean(const char *name, boolean fallback)
{
	int value = config_boolean(name);

	return value < 0 ? fallback : (value != 0);
}

static real setting_real(const char *name, real fallback)
{
	double value = config_real(name);

	return value == 0.0 ? fallback : (real)value;
}

static void load_settings(SM64PortConfig *out_config)
{
	SM64Port_ConfigDefaults(out_config);

	out_config->halo_units_per_sm64_unit =
		setting_real("sm64.scale", (real)SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT);
	if (out_config->halo_units_per_sm64_unit <= 0.0f)
		out_config->halo_units_per_sm64_unit = SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT;

	out_config->tick_rate = (double)setting_real("sm64.tick_rate", (real)SM64_PORT_TICKS_PER_SECOND);
	if (out_config->tick_rate <= 0.0)
		out_config->tick_rate = (double)SM64_PORT_TICKS_PER_SECOND;

	out_config->deadzone = setting_real("sm64.deadzone", 0.08f);
	out_config->sensitivity = setting_real("sm64.sensitivity", 1.0f);
	out_config->invert_y = setting_boolean("sm64.invert_y", FALSE);
	out_config->collision_radius = setting_real("sm64.collision_radius",
		(real)DEFAULT_COLLISION_RADIUS);
	out_config->debug_draw = setting_boolean("sm64.debug", TRUE);
	out_config->show_collision = setting_boolean("sm64.show_collision", FALSE);
	out_config->draw_mesh = setting_boolean("sm64.draw_mesh", FALSE);
	out_config->possess_player = setting_boolean("sm64.possess_player", FALSE);

	/* Where the module looks first: the folder the game's maps are in, which
	   is where a player would put a ROM. */
	out_config->search_directory = platform_data_root();

	/* Empty strings: the module then searches for the library and the ROM
	   itself. */
	out_config->library_path = NULL;
	out_config->rom_path = NULL;
	{
		const char *path = config_string("sm64.rom");

		if (path && path[0])
			out_config->rom_path = path;
		path = config_string("sm64.library");
		if (path && path[0])
			out_config->library_path = path;
	}
}

/* ---------- the host callbacks the module needs ---------- */

static void host_log(void *context, const char *message)
{
	(void)context;
	platform_log("sm64: %s", message);
}

/* Reads a file the way the platform layer does: through libc, which every
   port has.  The ROM is read once, at start-up. */
static bool host_read_file(void *context, const char *path, uint8_t **out_data, size_t *out_size)
{
	FILE *file;
	long size;
	uint8_t *data;

	(void)context;
	if (!path || !out_data || !out_size)
		return FALSE;

	file = fopen(path, "rb");
	if (!file)
		return FALSE;
	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return FALSE;
	}
	size = ftell(file);
	if (size <= 0)
	{
		fclose(file);
		return FALSE;
	}
	rewind(file);
	data = (uint8_t *)malloc((size_t)size);
	if (!data)
	{
		fclose(file);
		return FALSE;
	}
	if (fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		free(data);
		fclose(file);
		return FALSE;
	}
	fclose(file);

	*out_data = data;
	*out_size = (size_t)size;
	return TRUE;
}

static void host_free_file(void *context, uint8_t *data)
{
	(void)context;
	free(data);
}

/* ---------- starting and stopping ---------- */

static boolean ensure_started(void)
{
	SM64PortConfig config;
	SM64PortHost host;

	if (g_state.started)
		return TRUE;
	if (g_state.attempted)
		return FALSE;
	g_state.attempted = TRUE;

	if (!g_state.enabled)
		return FALSE;

	load_settings(&config);
	memset(&host, 0, sizeof(host));
	host.log_message = host_log;
	host.read_file = host_read_file;
	host.free_file = host_free_file;

	if (SM64Port_Init(&config, &host) != SM64_PORT_OK)
	{
		/* The module says why in the log (missing library, missing ROM). */
		return FALSE;
	}

	g_state.started = TRUE;
	g_state.surfaces = SM64Port_SurfaceSetCreate((uint32_t)g_state.max_surfaces);
	return TRUE;
}

/* ---------- collision out of the level ---------- */

static void remember_debug_triangle(const float a[3], const float b[3], const float c[3])
{
	struct debug_triangle *triangle;

	if (g_state.debug_triangle_count >= MAXIMUM_DEBUG_TRIANGLES)
		return;
	triangle = &g_state.debug_triangles[g_state.debug_triangle_count++];
	triangle->points[0].x = a[0]; triangle->points[0].y = a[1]; triangle->points[0].z = a[2];
	triangle->points[1].x = b[0]; triangle->points[1].y = b[1]; triangle->points[1].z = b[2];
	triangle->points[2].x = c[0]; triangle->points[2].y = c[1]; triangle->points[2].z = c[2];
}

/* Adds one surface of the level's collision BSP, as triangles in Halo space.
   Halo stores a surface as a polygon of up to eight points and a plane; the
   module fans the polygon and fixes the winding from the plane, which is what
   tells libsm64 which side of a triangle Mario can stand on. */
static void add_bsp_surface(SM64PortSurfaceSet *set, struct collision_bsp *bsp,
	long surface_index, float radius)
{
	struct collision_surface const *surface;
	real_point3d points[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
	short point_count;
	real_plane3d plane;
	real_point3d centre;
	float normal[3];
	float triangles[MAXIMUM_VERTICES_PER_COLLISION_SURFACE][3];
	short i;
	float dx, dy, dz;

	if (surface_index < 0 || surface_index >= bsp->surfaces.count)
		return;
	surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
	if (!surface)
		return;

	point_count = collision_surface_polygon(bsp, surface_index, points);
	if (point_count < 3 || point_count > MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
		return;

	/* The polygon's centre, to test it against the window around the actor. */
	centre.x = 0.0f; centre.y = 0.0f; centre.z = 0.0f;
	for (i = 0; i < point_count; i++)
	{
		centre.x += points[i].x;
		centre.y += points[i].y;
		centre.z += points[i].z;
	}
	centre.x /= (real)point_count;
	centre.y /= (real)point_count;
	centre.z /= (real)point_count;

	dx = (float)(centre.x - (real)g_state.collision_center.x);
	dy = (float)(centre.y - (real)g_state.collision_center.y);
	dz = (float)(centre.z - (real)g_state.collision_center.z);
	if (dx * dx + dy * dy + dz * dz > radius * radius)
		return;

	bsp3d_get_plane_from_designator(&bsp->bsp3d, surface->plane_designator, &plane);
	normal[0] = plane.n.i;
	normal[1] = plane.n.j;
	normal[2] = plane.n.k;

	for (i = 0; i < point_count; i++)
	{
		triangles[i][0] = (float)points[i].x;
		triangles[i][1] = (float)points[i].y;
		triangles[i][2] = (float)points[i].z;
	}

	SM64Port_SurfaceSetAddPolygon(set, (const float (*)[3])triangles, point_count, normal,
		SM64_PORT_TERRAIN_STONE);

	/* Triangle pairs, for the wireframe. */
	if (g_state.debug_triangle_count < MAXIMUM_DEBUG_TRIANGLES)
	{
		for (i = 1; i + 1 < point_count; i++)
			remember_debug_triangle(triangles[0], triangles[i], triangles[i + 1]);
	}
}

/* Builds the collision around a position.  Without a level (the main menu) it
   builds the module's own room at the world origin instead, so that an actor
   has something to stand on there too. */
static void rebuild_collision(SM64PortVector3 centre)
{
	struct collision_bsp *bsp;
	SM64PortSurfaceSet *set = g_state.surfaces;
	uint32_t count = 0;
	const SM64PortSurface *surfaces;
	long i;
	float radius;

	if (!set)
		return;

	g_state.collision_center = centre;
	g_state.debug_triangle_count = 0;
	SM64Port_SurfaceSetClear(set);

	radius = setting_real("sm64.collision_radius", (real)DEFAULT_COLLISION_RADIUS);
	if (radius <= 0.0f)
		radius = DEFAULT_COLLISION_RADIUS;

	bsp = global_collision_bsp_get();
	if (bsp && bsp->surfaces.count > 0)
	{
		/* Every surface of the BSP is tested against the window; the set
		   stops accepting them once it is full, so the walk ends early in a
		   dense level. */
		for (i = 0; i < bsp->surfaces.count; i++)
		{
			SM64Port_SurfaceSetSurfaces(set, &count);
			if ((long)count >= g_state.max_surfaces)
				break;
			add_bsp_surface(set, bsp, i, radius);
		}
	}
	else
	{
		/* No level: the module's own room, at the world origin. */
		SM64Port_BuildTestRoom(set, radius * 0.5f);
	}

	surfaces = SM64Port_SurfaceSetSurfaces(set, &count);
	if (SM64Port_SetStaticCollision(surfaces, count) == SM64_PORT_OK)
	{
		g_state.collision_ready = TRUE;
		g_state.collision_bsp_index = global_structure_bsp_index_get();
	}
	else
	{
		g_state.collision_ready = FALSE;
	}
}

/* Rebuilds when the actor has walked a quarter of the window's radius, or the
   level's BSP changed under him.  Building is a walk over the whole BSP, so it
   is done as rarely as possible. */
static void update_collision(SM64PortState const *state)
{
	float dx, dy, dz;
	float moved;
	float radius;

	if (!g_state.collision_ready || !g_state.surfaces)
	{
		rebuild_collision(state->position);
		return;
	}
	if (global_structure_bsp_index_get() != g_state.collision_bsp_index)
	{
		rebuild_collision(state->position);
		return;
	}

	radius = setting_real("sm64.collision_radius", (real)DEFAULT_COLLISION_RADIUS);
	if (radius <= 0.0f)
		radius = DEFAULT_COLLISION_RADIUS;
	dx = state->position.x - g_state.collision_center.x;
	dy = state->position.y - g_state.collision_center.y;
	dz = state->position.z - g_state.collision_center.z;
	moved = sqrtf(dx * dx + dy * dy + dz * dz);
	if (moved > radius * 0.25f)
		rebuild_collision(state->position);
}

/* ---------- input ---------- */

/* The player's movement intent, in Halo's ground plane and relative to where
   the player is looking: throttle.x is forwards, throttle.y is sideways. */
static void gather_input(SM64PortInput *out_input)
{
	struct player_control const *control;
	real_vector3d facing;
	real forward_x, forward_y;
	real length;

	memset(out_input, 0, sizeof(*out_input));

	control = player_control_get(0);
	if (!control)
		return;

	/* The direction the player is looking, flattened onto the ground: the
	   movement is relative to it, as it is for the player's own biped. */
	if (!player_control_get_facing_direction(0, &facing))
	{
		facing.i = 1.0f;
		facing.j = 0.0f;
		facing.k = 0.0f;
	}
	else if (fabs((double)facing.i) < 0.0001 && fabs((double)facing.j) < 0.0001)
	{
		/* Looking straight up or down: fall back to +X rather than to a
		   direction of zero length. */
		facing.i = 1.0f;
		facing.j = 0.0f;
		facing.k = 0.0f;
	}
	forward_x = facing.i;
	forward_y = facing.j;
	length = (real)sqrt((double)(forward_x * forward_x + forward_y * forward_y));
	if (length > 0.0001f)
	{
		forward_x /= length;
		forward_y /= length;
	}
	else
	{
		forward_x = 1.0f;
		forward_y = 0.0f;
	}

	/* right = forward turned a quarter turn clockwise (Z up). */
	{
		real right_x = forward_y;
		real right_y = -forward_x;

		out_input->move_x = (float)(forward_x * control->throttle.i + right_x * control->throttle.j);
		out_input->move_y = (float)(forward_y * control->throttle.i + right_y * control->throttle.j);
	}

	/* Buttons: jump, "use", and crouch.  Halo's own keys and gamepad buttons
	   feed these flags, so the module follows the player's bindings. */
	out_input->button_a = (control->control_flags & FLAG(_player_control_jump_bit)) ? 1 : 0;
	out_input->button_b = (control->control_flags & FLAG(_player_control_action_bit)) ? 1 : 0;
	out_input->button_z = (control->control_flags & FLAG(_player_control_back_bit)) ? 1 : 0;
}

/* ---------- driving the player's biped ---------- */

static void possess_player(SM64PortState const *state)
{
	long unit_index = player_control_get_unit_index(0);
	real_point3d position;
	real_vector3d forward, up;

	if (unit_index == NONE)
		return;

	position.x = (real)state->position.x;
	position.y = (real)state->position.y;
	position.z = (real)state->position.z;
	forward.i = (real)cos((double)state->yaw);
	forward.j = (real)sin((double)state->yaw);
	forward.k = 0.0f;
	up.i = 0.0f;
	up.j = 0.0f;
	up.k = 1.0f;

	object_set_position(unit_index, &position, &forward, &up);
}

/* ---------- debug drawing ---------- */

static void draw_debug(SM64PortState const *state)
{
	real_argb_color body = { 1.0f, 1.0f, 0.4f, 0.1f };
	real_argb_color facing = { 1.0f, 0.2f, 1.0f, 0.2f };
	real_argb_color wire = { 0.6f, 0.3f, 0.8f, 1.0f };
	real_argb_color text_colour = { 1.0f, 1.0f, 1.0f, 1.0f };
	real_point3d base, tip, head;
	real_vector3d height;
	char text[128];
	long i;

	if (!g_state.debug_draw)
		return;

	/* The body: a capsule from his feet to the top of his head. */
	base.x = (real)state->position.x;
	base.y = (real)state->position.y;
	base.z = (real)state->position.z;
	height.i = 0.0f;
	height.j = 0.0f;
	height.k = (real)MARIO_HEIGHT_HALO;
	render_debug_pill(FALSE, &base, &height, (real)MARIO_RADIUS_HALO, &body);

	/* Which way he faces. */
	head.x = base.x;
	head.y = base.y;
	head.z = base.z + (real)MARIO_HEIGHT_HALO * 0.5f;
	tip.x = head.x + (real)cos((double)state->yaw) * 1.5f;
	tip.y = head.y + (real)sin((double)state->yaw) * 1.5f;
	tip.z = head.z;
	render_debug_line(FALSE, &head, &tip, &facing);

	/* His velocity, and what he is doing. */
	{
		real_vector3d velocity;

		velocity.i = (real)state->velocity.x;
		velocity.j = (real)state->velocity.y;
		velocity.k = (real)state->velocity.z;
		render_debug_vector(FALSE, &head, &velocity, 0.25f, &facing);
	}

	snprintf(text, sizeof(text), "action 0x%08lx  %.1f u/s  health %d",
		(unsigned long)state->action, (double)state->forward_velocity, (int)state->health);
	head.z += 0.3f;
	render_debug_string_at_point(FALSE, &head, text, &text_colour);

	/* The collision libsm64 is using. */
	if (g_state.show_collision)
	{
		for (i = 0; i < g_state.debug_triangle_count; i++)
			render_debug_polygon_edges(g_state.debug_triangles[i].points, 3, &wire);
	}
}

/* ---------- the frame ---------- */

static boolean spawn_actor(void);

/* F3: on, and an actor in front of the player; F3 again: off. The module is
   off until it is pressed, so that a build with it in is still a normal game
   until the player asks for Mario. (F3 is also one of the editor's debug keys,
   which do nothing outside the editor.) */
static void toggle_module(void)
{
	if (!g_state.enabled)
	{
		g_state.enabled = TRUE;
		g_state.attempted = FALSE;
		if (!ensure_started())
		{
			console_warning("sm64: could not start (%s)", SM64Port_StatusMessage()
				? SM64Port_StatusMessage() : "no reason given");
			g_state.enabled = FALSE;
			return;
		}
		console_printf(FALSE, "sm64: on");
		spawn_actor();
	}
	else
	{
		g_state.enabled = FALSE;
		sm64_port_halo_dispose();
		console_printf(FALSE, "sm64: off");
	}
}

void sm64_port_halo_update(real delta_time_seconds)
{
	SM64PortInput input;
	SM64PortState state;
	boolean f3;
	int ticks;

	/* The key is read every frame, whether the module is on or not, and only
	   acts on the press. */
	f3 = input_key_is_down(_key_f3);
	if (f3 && !g_state.f3_down)
		toggle_module();
	g_state.f3_down = f3;

	if (!g_state.enabled)
		return;
	if (!ensure_started())
		return;
	if (g_state.actor == SM64_PORT_INVALID_ACTOR)
		return;

	/* The player's own frame of movement, so that the module follows the
	   game's clock: dt is the length of this frame in seconds. */
	gather_input(&input);
	SM64Port_SetInput(g_state.actor, &input);

	ticks = SM64Port_Update((double)delta_time_seconds);
	if (ticks > 0)
		g_state.ticks += ticks;
	if (ticks < 0)
		return;

	if (SM64Port_GetState(g_state.actor, &state) != SM64_PORT_OK)
		return;

	update_collision(&state);

	if (g_state.possess_player)
		possess_player(&state);
	draw_debug(&state);
}

/* ---------- console commands ---------- */

static boolean spawn_actor(void)
{
	SM64PortVector3 position;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	struct player_control const *control;

	if (!ensure_started())
	{
		console_warning("sm64: the module is not running (%s)",
			SM64Port_StatusMessage() ? SM64Port_StatusMessage() : "no reason given");
		return TRUE;
	}

	/* A few units in front of the player, at head height. */
	position.x = 0.0f;
	position.y = 0.0f;
	position.z = 2.0f;
	control = player_control_get(0);
	(void)control;
	{
		struct observer_result const *observer = observer_get_camera(0);

		if (observer)
		{
			/* Three units in front of the camera, and a bit above where it
			   is, so that the actor has room to fall onto the floor. */
			position.x = (float)observer->position.x + (float)observer->forward.i * 3.0f;
			position.y = (float)observer->position.y + (float)observer->forward.j * 3.0f;
			position.z = (float)observer->position.z + 2.0f;
		}
	}

	/* The module needs a floor under the spawn point, and it builds its own
	   room at the origin when there is no level. */
	rebuild_collision(position);

	if (SM64Port_CreateActor(position, 0.0f, &id) != SM64_PORT_OK)
	{
		console_warning("sm64: no actor: %s",
			SM64Port_StatusMessage() ? SM64Port_StatusMessage() : "unknown reason");
		return TRUE;
	}

	g_state.actor = id;
	console_printf(FALSE, "sm64: actor %d at %.1f %.1f %.1f", (int)id,
		(double)position.x, (double)position.y, (double)position.z);
	return TRUE;
}

static void print_status(void)
{
	SM64PortStats stats;

	if (!SM64Port_IsReady())
	{
		console_warning("sm64: off (%s)", SM64Port_StatusMessage()
			? SM64Port_StatusMessage() : "not started");
		return;
	}

	SM64Port_GetStats(&stats);
	console_printf(FALSE,
		"sm64: %lu actor(s), %u surface(s), %ld tick(s) in %ld frame(s), %.2f ms in the last frame",
		(unsigned long)stats.actors, stats.surfaces, stats.ticks_total, stats.frames_total,
		stats.last_update_seconds * 1000.0);
}

boolean sm64_port_halo_console_command(const char *command)
{
	float value = 0.0f;
	int number = 0;
	float x = 0.0f, y = 0.0f, z = 0.0f;

	if (!command)
		return FALSE;
	if (strncmp(command, "sm64", 4) != 0)
		return FALSE;

	if (strcmp(command, "sm64_spawn") == 0)
		return spawn_actor();

	/* sm64_spawn x y z: place an actor exactly. */
	if (sscanf(command, "sm64_spawn %f %f %f", &x, &y, &z) == 3)
	{
		SM64PortVector3 position;
		SM64PortActorId id = SM64_PORT_INVALID_ACTOR;

		if (!ensure_started())
		{
			console_warning("sm64: the module is not running (%s)",
				SM64Port_StatusMessage() ? SM64Port_StatusMessage() : "no reason given");
			return TRUE;
		}
		position.x = x;
		position.y = y;
		position.z = z;
		rebuild_collision(position);
		if (g_state.actor != SM64_PORT_INVALID_ACTOR)
			SM64Port_DestroyActor(g_state.actor);
		g_state.actor = SM64_PORT_INVALID_ACTOR;
		if (SM64Port_CreateActor(position, 0.0f, &id) == SM64_PORT_OK)
		{
			g_state.actor = id;
			console_printf(FALSE, "sm64: actor %d at %.1f %.1f %.1f", (int)id,
				(double)x, (double)y, (double)z);
		}
		else
			console_warning("sm64: no actor there: %s", SM64Port_StatusMessage()
				? SM64Port_StatusMessage() : "unknown reason");
		return TRUE;
	}

	if (strcmp(command, "sm64_reset") == 0)
	{
		if (g_state.actor != SM64_PORT_INVALID_ACTOR)
		{
			SM64Port_DestroyActor(g_state.actor);
			g_state.actor = SM64_PORT_INVALID_ACTOR;
		}
		console_printf(FALSE, "sm64: actor removed");
		return TRUE;
	}

	if (sscanf(command, "sm64_enable %d", &number) == 1)
	{
		g_state.enabled = (number != 0);
		g_state.attempted = FALSE;
		if (!g_state.enabled)
			sm64_port_halo_dispose();
		else
			ensure_started();
		console_printf(FALSE, "sm64: %s", g_state.enabled ? "on" : "off");
		return TRUE;
	}

	if (sscanf(command, "sm64_debug %d", &number) == 1)
	{
		g_state.debug_draw = (number != 0);
		console_printf(FALSE, "sm64: debug drawing %s", g_state.debug_draw ? "on" : "off");
		return TRUE;
	}

	if (sscanf(command, "sm64_show_collision %d", &number) == 1)
	{
		g_state.show_collision = (number != 0);
		console_printf(FALSE, "sm64: collision wireframe %s", g_state.show_collision ? "on" : "off");
		return TRUE;
	}

	if (sscanf(command, "sm64_scale %f", &value) == 1)
	{
		if (value <= 0.0f)
		{
			console_warning("sm64: the scale must be greater than zero");
			return TRUE;
		}
		/* The scale is read when the module starts, so it takes effect on the
		   next sm64_enable or on the next start of the game. */
		console_printf(FALSE, "sm64: scale %.4f Halo units per SM64 unit (restart the module to "
			"apply it: sm64_enable 0, sm64_enable 1)", (double)value);
		return TRUE;
	}

	if (strcmp(command, "sm64_status") == 0)
	{
		print_status();
		return TRUE;
	}

	if (strcmp(command, "sm64_help") == 0 || strcmp(command, "sm64") == 0)
	{
		console_printf(FALSE, "sm64_spawn [x y z]  put an actor in front of the player");
		console_printf(FALSE, "sm64_reset          remove it");
		console_printf(FALSE, "sm64_enable 0|1     switch the module on and off");
		console_printf(FALSE, "sm64_debug 0|1      draw the actor's capsule and facing");
		console_printf(FALSE, "sm64_show_collision 0|1  draw the collision it is using");
		console_printf(FALSE, "sm64_scale <value>  Halo units per SM64 unit (0.01 by default)");
		console_printf(FALSE, "sm64_status         ticks, surfaces and the last frame's cost");
		return TRUE;
	}

	return FALSE;
}

/* ---------- life cycle ---------- */

void sm64_port_halo_initialize(void)
{
	g_state.actor = SM64_PORT_INVALID_ACTOR;
	g_state.collision_bsp_index = -1;
	g_state.collision_ready = FALSE;
	g_state.debug_triangle_count = 0;
	g_state.ticks = 0;
	g_state.f3_down = FALSE;
	g_state.enabled = setting_boolean("sm64.enable", FALSE);
	g_state.debug_draw = setting_boolean("sm64.debug", TRUE);
	g_state.show_collision = setting_boolean("sm64.show_collision", FALSE);
	g_state.draw_mesh = setting_boolean("sm64.draw_mesh", FALSE);
	g_state.possess_player = setting_boolean("sm64.possess_player", FALSE);
	g_state.max_surfaces = (long)setting_real("sm64.max_surfaces", 4096.0f);
	if (g_state.max_surfaces <= 0)
		g_state.max_surfaces = 4096;
	g_state.attempted = FALSE;
	g_state.started = FALSE;

	if (!g_state.enabled)
		return;

	ensure_started();
}

void sm64_port_halo_dispose(void)
{
	if (g_state.started)
	{
		SM64Port_DestroyAllActors();
		SM64Port_Shutdown();
	}
	if (g_state.surfaces)
	{
		SM64Port_SurfaceSetDestroy(g_state.surfaces);
		g_state.surfaces = NULL;
	}
	g_state.actor = SM64_PORT_INVALID_ACTOR;
	g_state.started = FALSE;
	g_state.attempted = FALSE;
	g_state.collision_ready = FALSE;
	g_state.debug_triangle_count = 0;
}
