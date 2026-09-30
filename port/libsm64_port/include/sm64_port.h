/*
SM64_PORT.H

The public interface of the optional libsm64 module of the Halo: Combat
Evolved port (``port/libsm64_port``).  The module runs Super Mario 64's
movement, collision and animation code (libsm64) inside a Halo level and
drives an actor with it.

The header is deliberately self-contained: it declares no Halo type and no
libsm64 type, so that

  - the game includes it and calls it (``port/libsm64_port/src/sm64_port_halo.c``
    is the only file that knows about Halo), and
  - the host-side tests compile and run it without the game.

Everything here is C89-compatible apart from ``bool``/``stdint.h``, which the
platform layer has.  Call every function from one thread: libsm64 keeps
global state, so the module is not thread-safe (see README).

Licence: CC0 1.0 Universal, like the rest of the port (LICENSE.md).  libsm64
itself is CC0 too; see third_party/libsm64/LICENSE.md.

Constants marked "measured" were determined by running libsm64, not from
documentation: see port/libsm64_port/TEST_RESULTS.md.
*/

#ifndef SM64_PORT_H
#define SM64_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- version ---------- */

#define SM64_PORT_VERSION_MAJOR 1
#define SM64_PORT_VERSION_MINOR 0
#define SM64_PORT_VERSION_PATCH 0

/* ---------- measured constants ---------- */

/* libsm64's native tick rate: the N64 ran the game at 30 frames a second and
   Mario's physics is written for that step.  Halo ticks at the same rate
   (TICKS_PER_SECOND, source/cseries/cseries.h), which is why one Halo tick
   is normally one Mario tick. */
#define SM64_PORT_TICKS_PER_SECOND 30
#define SM64_PORT_TICK_SECONDS (1.0 / (double)SM64_PORT_TICKS_PER_SECOND)

/* One Halo world unit is 100 SM64 units by default.  Measured: Mario's
   collision capsule is 160 SM64 units tall, so he stands 1.6 Halo units
   tall, about the height of a Spartan.  Change it with "sm64.scale". */
#define SM64_PORT_DEFAULT_HALO_UNITS_PER_SM64_UNIT 0.01f

/* Mario's collision capsule, in SM64 units: from his feet (his position) up
   to his head (source: libsm64's hack_allocate_mario()). */
#define SM64_PORT_MARIO_HEIGHT_SM64 160.0f
/* Half the width of his collision cylinder. */
#define SM64_PORT_MARIO_RADIUS_SM64 37.0f

/* Largest number of actors the module tracks at once. */
#define SM64_PORT_MAX_ACTORS 16
/* Guard against a stalled frame turning into hundreds of ticks. */
#define SM64_PORT_DEFAULT_MAX_SUBSTEPS 4

/* ---------- types ---------- */

typedef struct SM64PortVector3
{
	float x, y, z;
} SM64PortVector3;

/* Movement intent and buttons, in Halo's frame of reference.  move_x and
   move_y are the horizontal movement the player asks for, already dead-zoned
   and scaled to [-1, 1] by the caller: (move_x, move_y) is the direction in
   Halo's ground plane (X/Y, Z up). */
typedef struct SM64PortInput
{
	float move_x;
	float move_y;
	uint8_t button_a; /* jump */
	uint8_t button_b; /* punch / swim */
	uint8_t button_z; /* crouch / slide */
} SM64PortInput;

/* An actor's state in Halo's frame of reference.  The position is
   interpolated between the last two libsm64 ticks, so it is smooth at any
   frame rate; see SM64Port_GetState(). */
typedef struct SM64PortState
{
	SM64PortVector3 position;         /* Halo units, Z up */
	SM64PortVector3 velocity;         /* Halo units a second */
	float yaw;                        /* radians, Halo convention: 0 = +X */
	float forward_velocity;           /* Halo units a second */
	int16_t health;
	uint32_t action;                  /* libsm64 action id */
	int32_t anim_id;
	int16_t anim_frame;
	uint32_t flags;
	uint32_t particle_flags;
	bool grounded;                    /* action is not an airborne one */
} SM64PortState;

/* A triangle in libsm64's own format, mirrored here so that callers need
   neither libsm64 nor its header.  The layout must stay identical to
   struct SM64Surface: (int16 type, int16 force, uint16 terrain,
   int32 vertices[3][3]).  sm64_port_core.c static-asserts this. */
typedef struct SM64PortSurface
{
	int16_t type;
	int16_t force;
	uint16_t terrain;
	int32_t vertices[3][3];
} SM64PortSurface;

typedef enum SM64PortResult
{
	SM64_PORT_OK = 0,
	SM64_PORT_ERROR_LIBRARY = 1,        /* libsm64 could not be loaded */
	SM64_PORT_ERROR_ASSETS = 2,         /* no ROM, or an unreadable one */
	SM64_PORT_ERROR_NOT_INITIALIZED = 3,
	SM64_PORT_ERROR_BAD_ACTOR = 4,
	SM64_PORT_ERROR_LIMIT = 5,          /* too many actors or surfaces */
	SM64_PORT_ERROR_MEMORY = 6
} SM64PortResult;

/* Configuration.  ConfigDefaults() fills in the values below; the game
   overrides them from config.toml (see port/linux/src/port_config.c). */
typedef struct SM64PortConfig
{
	/* Halo units per SM64 unit.  0.01 makes Mario human-sized. */
	float halo_units_per_sm64_unit;
	/* libsm64 ticks a second; 30 is what its physics is written for. */
	double tick_rate;
	/* most ticks one Update() call may run (a stalled frame must not
	   run hundreds of them) */
	int max_substeps;
	/* stick and key tuning */
	float deadzone;
	float sensitivity;
	bool invert_y;
	/* how much of the level around an actor becomes collision surfaces,
	   in Halo units; the module rebuilds them as actors move */
	float collision_radius;
	/* hard ceiling on the surfaces handed to libsm64 in one rebuild */
	uint32_t max_surfaces;
	/* drawing */
	bool debug_draw;
	bool show_collision;
	bool draw_mesh;
	/* 1: drive the player's biped instead of a free-flying actor */
	bool possess_player;
	/* path of the libsm64 shared library; NULL searches the defaults */
	const char *library_path;
	/* path of an SM64 (US) ROM; NULL searches the defaults.  The user
	   supplies their own legally obtained copy; none is distributed. */
	const char *rom_path;
	/* A directory to look in first for both of the above: the game's data
	   folder, which is where a player would put a ROM they want found
	   wherever the game is started from. NULL or empty skips it. */
	const char *search_directory;
} SM64PortConfig;

/* Services the module needs from its host (the game, or the test harness).
   Injecting them keeps the core free of Halo. */
typedef struct SM64PortHost
{
	void *context;
	/* a line of the log; may be NULL */
	/* Not called `log`: the game's headers define that as a macro (port/include/halo_math.h). */
	void (*log_message)(void *context, const char *message);
	/* reads a whole file into a buffer the module frees with free_file;
	   returns 0 on failure.  May be NULL, which disables the ROM search. */
	bool (*read_file)(void *context, const char *path, uint8_t **out_data, size_t *out_size);
	void (*free_file)(void *context, uint8_t *data);
} SM64PortHost;

/* ---------- configuration ---------- */

void SM64Port_ConfigDefaults(SM64PortConfig *out_config);

/* ---------- life cycle ---------- */

/* Loads libsm64, reads the ROM and starts the library.  A failure leaves the
   module inert: every other call then returns its error and changes nothing,
   so a player without a ROM plays the normal game.  Calling it again after a
   failure retries. */
SM64PortResult SM64Port_Init(const SM64PortConfig *config, const SM64PortHost *host);
void SM64Port_Shutdown(void);

/* 1 when libsm64 is loaded and an actor can be created. */
bool SM64Port_IsReady(void);
/* Why the module is not ready, or NULL when it is; for the log and the
   console.  The string is owned by the module. */
const char *SM64Port_StatusMessage(void);

/* ---------- actors ---------- */

typedef int SM64PortActorId;
#define SM64_PORT_INVALID_ACTOR ((SM64PortActorId)-1)

/* Spawns an actor at a Halo position, facing halo_yaw.  The position must be
   above collision geometry Mario can stand on, as libsm64 refuses to create
   a Mario with no floor under him. */
SM64PortResult SM64Port_CreateActor(SM64PortVector3 halo_position, float halo_yaw,
	SM64PortActorId *out_id);
SM64PortResult SM64Port_DestroyActor(SM64PortActorId id);
void SM64Port_DestroyAllActors(void);
uint32_t SM64Port_ActorCount(void);

/* Queues an actor's input for the next Update(). */
SM64PortResult SM64Port_SetInput(SM64PortActorId id, const SM64PortInput *input);
/* Teleports an actor (used by "sm64_reset" and by the collision rebuild). */
SM64PortResult SM64Port_SetPosition(SM64PortActorId id, SM64PortVector3 halo_position, float halo_yaw);

/* ---------- the frame ---------- */

/* Runs libsm64's fixed steps for delta_time_seconds and interpolates.
   Returns the number of ticks it ran, or -1 when the module is not ready. */
int SM64Port_Update(double delta_time_seconds);

/* The interpolated state (position, velocity, yaw) of an actor, for
   rendering and for writing back into a Halo object. */
SM64PortResult SM64Port_GetState(SM64PortActorId id, SM64PortState *out_state);
/* The state of the last completed tick, uninterpolated. */
SM64PortResult SM64Port_GetTickState(SM64PortActorId id, SM64PortState *out_state);

/* ---------- conversions (unit tested; see README_LIBSM64_PORT.md) ---------- */

/* Halo is right-handed with Z up and yaw 0 along +X; libsm64 is
   right-handed with Y up and yaw 0 along +Z.  These functions are the whole
   mapping, and they are exact inverses of each other. */
SM64PortVector3 SM64Port_HaloToSM64Position(SM64PortVector3 halo, float halo_units_per_sm64_unit);
SM64PortVector3 SM64Port_SM64ToHaloPosition(SM64PortVector3 sm64, float halo_units_per_sm64_unit);
SM64PortVector3 SM64Port_HaloToSM64Direction(SM64PortVector3 halo);
SM64PortVector3 SM64Port_SM64ToHaloDirection(SM64PortVector3 sm64);
float SM64Port_HaloToSM64Yaw(float halo_yaw);
float SM64Port_SM64ToHaloYaw(float sm64_yaw);

/* Maps a Halo movement intent (move_x, move_y in [-1, 1]) into the stick and
   camera-look values libsm64 wants.  Measured: with the camera-look vector
   set to minus the wanted direction and the stick pushed forward, Mario
   walks exactly in the wanted direction. */
void SM64Port_HaloInputToSM64Input(const SM64PortInput *input, float deadzone, float sensitivity,
	bool invert_y, float *out_stick_x, float *out_stick_y, float *out_cam_look_x, float *out_cam_look_z);

/* ---------- collision ---------- */

/* An opaque builder that turns Halo-space polygons into the SM64-space
   triangle soup libsm64 collides against. */
typedef struct SM64PortSurfaceSet SM64PortSurfaceSet;

SM64PortSurfaceSet *SM64Port_SurfaceSetCreate(uint32_t capacity);
void SM64Port_SurfaceSetDestroy(SM64PortSurfaceSet *set);
void SM64Port_SurfaceSetClear(SM64PortSurfaceSet *set);

/* Adds one triangle, in Halo space.  The winding is corrected so that the
   result faces the same way as the given normal; without a normal (NULL) the
   triangle's own winding decides.  Returns 1 when it was added. */
bool SM64Port_SurfaceSetAddTriangle(SM64PortSurfaceSet *set, const float a[3], const float b[3],
	const float c[3], const float *normal, uint16_t terrain);
/* Fan-triangulates a convex polygon, in Halo space: Halo collision surfaces
   are convex, so a fan from the first point is exact. */
bool SM64Port_SurfaceSetAddPolygon(SM64PortSurfaceSet *set, const float (*points)[3], int point_count,
	const float *normal, uint16_t terrain);

/* The finished surfaces, for SM64Port_SetStaticCollision(). */
const SM64PortSurface *SM64Port_SurfaceSetSurfaces(const SM64PortSurfaceSet *set, uint32_t *out_count);

/* Replaces the collision libsm64 uses with count surfaces. */
SM64PortResult SM64Port_SetStaticCollision(const SM64PortSurface *surfaces, uint32_t count);
/* The number of surfaces currently loaded into libsm64. */
uint32_t SM64Port_StaticCollisionCount(void);

/* Fills a set with a room to walk around in (a floor, four walls, a ramp, a
   ledge and a low ceiling), for places with no level: the main menu, a map
   whose collision has not loaded, and the host tests.  half_extent is in
   Halo units. */
bool SM64Port_BuildTestRoom(SM64PortSurfaceSet *set, float half_extent);

/* ---------- fixed-step accumulator (unit tested) ---------- */

/* A clock that turns variable frame times into a whole number of fixed
   steps plus the fraction of a step left over, for interpolation. */
typedef struct SM64PortClock
{
	double step;          /* seconds a tick lasts */
	double accumulator;   /* seconds of unfinished time */
	int max_steps;        /* most steps one advance may run */
	long total_steps;     /* steps since the clock was made, for drift tests */
} SM64PortClock;

void SM64Port_ClockInit(SM64PortClock *clock, double step_seconds, int max_steps);
/* Consumes dt seconds and returns how many whole steps to run; *out_alpha is
   the fraction of a step already simulated, for interpolation between the
   last two ticks. */
int SM64Port_ClockAdvance(SM64PortClock *clock, double dt, double *out_alpha);
void SM64Port_ClockReset(SM64PortClock *clock);

/* ---------- statistics, for the console and the tests ---------- */

typedef struct SM64PortStats
{
	long ticks_total;         /* libsm64 ticks run since Init() */
	long frames_total;        /* Update() calls since Init() */
	uint32_t actors;
	uint32_t surfaces;
	double last_update_seconds; /* wall time of the last Update() */
	int ticks_last_frame;
} SM64PortStats;

void SM64Port_GetStats(SM64PortStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* SM64_PORT_H */
