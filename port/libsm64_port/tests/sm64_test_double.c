/*
SM64_TEST_DOUBLE.C

A stand-in for libsm64 that implements its interface with simple physics of
the same shape: a 30 Hz tick, gravity of 4 units a tick, a jump that starts at
38 units a tick, camera-relative stick movement, floors, walls, slopes and
ceilings.  It is built as libsm64_test_double.so/.dll and is used

  - by the host tests, so that they run on a machine that has neither
    libsm64 nor an SM64 ROM, and
  - as the "library found but no assets" case of the module's error paths.

It is deliberately not a reimplementation of Super Mario 64: it is there so
that the *module* (clock, conversions, input mapping, collision conversion,
actor life cycle) can be tested anywhere.  Numbers the tests assert come from
the real library and are recorded in port/libsm64_port/TEST_RESULTS.md; the tests
that need the real physics (jump heights, triple jumps, wall kicks) skip
themselves when they are running against this double.

CC0 1.0 Universal, like the rest of the port.
*/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The public interface, from a copy libsm64 installs next to its library
   (dist/include/libsm64.h).  The tests build against the real header when it
   is there; this file needs no other part of libsm64. */
#include "libsm64.h"

#ifndef SM64_LIB_EXPORT
#define SM64_LIB_EXPORT
#endif

#define MAX_MARIOS 16
#define MAX_SURFACES 8192

#define GRAVITY 4.0f
#define JUMP_VELOCITY 38.0f
#define MAX_WALK_SPEED 27.9f      /* units a tick, measured with the real library */
#define TERMINAL_VELOCITY -75.0f
#define MARIO_HEIGHT 160.0f
#define MARIO_RADIUS 37.0f

struct LoadedSurface
{
	struct SM64SurfaceCollisionData data;
};

struct Mario
{
	bool used;
	float position[3];
	float velocity[3];
	float face_angle;      /* radians, libsm64's convention: 0 = +Z */
	float forward_velocity;
	int16_t health;
	uint32_t action;
	bool jump_held;
	int jump_count;
};

static struct SM64SurfaceCollisionData *g_surfaces;
static uint32_t g_surface_count;
static struct Mario g_marios[MAX_MARIOS];
static bool g_initialised;

/* ---------- geometry helpers ---------- */

static void surface_from_lib(const struct SM64Surface *in, struct SM64SurfaceCollisionData *out)
{
	float x1 = (float)in->vertices[0][0], y1 = (float)in->vertices[0][1], z1 = (float)in->vertices[0][2];
	float x2 = (float)in->vertices[1][0], y2 = (float)in->vertices[1][1], z2 = (float)in->vertices[1][2];
	float x3 = (float)in->vertices[2][0], y3 = (float)in->vertices[2][1], z3 = (float)in->vertices[2][2];
	float nx = (y2 - y1) * (z3 - z2) - (z2 - z1) * (y3 - y2);
	float ny = (z2 - z1) * (x3 - x2) - (x2 - x1) * (z3 - z2);
	float nz = (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2);
	float magnitude = sqrtf(nx * nx + ny * ny + nz * nz);
	int i;

	memset(out, 0, sizeof(*out));
	if (magnitude < 0.0001f)
	{
		out->isValid = 0;
		return;
	}
	magnitude = 1.0f / magnitude;
	nx *= magnitude; ny *= magnitude; nz *= magnitude;

	out->vertex1[0] = (int32_t)x1; out->vertex1[1] = (int32_t)y1; out->vertex1[2] = (int32_t)z1;
	out->vertex2[0] = (int32_t)x2; out->vertex2[1] = (int32_t)y2; out->vertex2[2] = (int32_t)z2;
	out->vertex3[0] = (int32_t)x3; out->vertex3[1] = (int32_t)y3; out->vertex3[2] = (int32_t)z3;
	out->normal.x = nx; out->normal.y = ny; out->normal.z = nz;
	out->originOffset = -(nx * x1 + ny * y1 + nz * z1);
	out->lowerY = (int32_t)(fminf(fminf(y1, y2), y3) - 5.0f);
	out->upperY = (int32_t)(fmaxf(fmaxf(y1, y2), y3) + 5.0f);
	out->terrain = in->terrain;
	out->type = in->type;
	out->force = in->force;
	out->isValid = 1;
	(void)i;
}

/* The area of a triangle's footprint in XZ.  A vertical surface, such as a
   wall, projects to a line, so a point can never be strictly "inside" it: the
   callers treat a negligible area as "anywhere along it". */
static float triangle_footprint_area(const struct SM64SurfaceCollisionData *s)
{
	float x1 = (float)s->vertex1[0], z1 = (float)s->vertex1[2];
	float x2 = (float)s->vertex2[0], z2 = (float)s->vertex2[2];
	float x3 = (float)s->vertex3[0], z3 = (float)s->vertex3[2];

	return 0.5f * fabsf((x2 - x1) * (z3 - z1) - (x3 - x1) * (z2 - z1));
}

/* Is the point (x, z) inside the triangle's footprint? */
static bool triangle_contains_xz(const struct SM64SurfaceCollisionData *s, float x, float z)
{
	float x1 = (float)s->vertex1[0], z1 = (float)s->vertex1[2];
	float x2 = (float)s->vertex2[0], z2 = (float)s->vertex2[2];
	float x3 = (float)s->vertex3[0], z3 = (float)s->vertex3[2];
	float d1 = (z1 - z) * (x2 - x1) - (x1 - x) * (z2 - z1);
	float d2 = (z2 - z) * (x3 - x2) - (x2 - x) * (z3 - z2);
	float d3 = (z3 - z) * (x1 - x3) - (x3 - x) * (z1 - z3);
	bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
	bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

	return !(has_neg && has_pos);
}

static float triangle_height_at(const struct SM64SurfaceCollisionData *s, float x, float z)
{
	if (fabsf(s->normal.y) < 0.0001f)
		return 0.0f;
	return -(x * s->normal.x + z * s->normal.z + s->originOffset) / s->normal.y;
}

/* The highest floor at or below y (with the same 78 unit reach the real
   library uses), or -11000 when there is none. */
static float find_floor_height(float x, float y, float z)
{
	float best = -11000.0f;
	uint32_t i;

	for (i = 0; i < g_surface_count; i++)
	{
		const struct SM64SurfaceCollisionData *s = &g_surfaces[i];
		float height;

		if (!s->isValid || s->normal.y <= 0.01f)
			continue;
		if (!triangle_contains_xz(s, x, z))
			continue;
		height = triangle_height_at(s, x, z);
		if (height > best && height <= y + 78.0f)
			best = height;
	}
	return best;
}

/* Mario is pushed out of any near-vertical surface he has walked into. */
static void resolve_walls(struct Mario *mario)
{
	float x = mario->position[0], y = mario->position[1], z = mario->position[2];
	uint32_t i;

	for (i = 0; i < g_surface_count; i++)
	{
		const struct SM64SurfaceCollisionData *s = &g_surfaces[i];
		float height;
		float distance;

		if (!s->isValid)
			continue;
		/* Walls and steep slopes: the real library stops treating a surface
		   as a floor below about 45 degrees. */
		if (s->normal.y > 0.7f)
			continue;

		/* Is Mario's body alongside it? */
		height = (fabsf(s->normal.y) > 0.0001f) ? triangle_height_at(s, x, z) : 0.0f;
		if (fabsf(s->normal.y) > 0.0001f)
		{
			if (y + MARIO_HEIGHT < height || y > height + MARIO_HEIGHT)
				continue;
		}

		/* Distance from the plane, and push back out of it. */
		distance = x * s->normal.x + y * s->normal.y + z * s->normal.z + s->originOffset;
		if (distance < MARIO_RADIUS && distance > -MARIO_RADIUS * 4.0f)
		{
			float push = MARIO_RADIUS - distance;
			bool vertical = triangle_footprint_area(s) < 1.0f;

			/* A wall is a line seen from above, so there is no footprint to
			   be inside of: it stops Mario along its whole length. */
			if (!vertical && !triangle_contains_xz(s, x + s->normal.x * push, z + s->normal.z * push))
				continue;
			if (vertical)
			{
				/* ...but not past its ends: use the surface's own bounds in
				   the two axes it spans. */
				float min_x = fminf((float)s->vertex1[0], fminf((float)s->vertex2[0], (float)s->vertex3[0]));
				float max_x = fmaxf((float)s->vertex1[0], fmaxf((float)s->vertex2[0], (float)s->vertex3[0]));
				float min_z = fminf((float)s->vertex1[2], fminf((float)s->vertex2[2], (float)s->vertex3[2]));
				float max_z = fmaxf((float)s->vertex1[2], fmaxf((float)s->vertex2[2], (float)s->vertex3[2]));

				if (x < min_x - MARIO_RADIUS || x > max_x + MARIO_RADIUS
					|| z < min_z - MARIO_RADIUS || z > max_z + MARIO_RADIUS)
					continue;
			}
			mario->position[0] += s->normal.x * push;
			mario->position[2] += s->normal.z * push;
			/* Kill the velocity going into the wall. */
			{
				float into = mario->velocity[0] * s->normal.x + mario->velocity[2] * s->normal.z;
				if (into < 0.0f)
				{
					mario->velocity[0] -= s->normal.x * into;
					mario->velocity[2] -= s->normal.z * into;
				}
			}
		}
	}
}

/* A ceiling stops an upward-moving Mario. */
static float find_ceiling_height(float x, float y, float z)
{
	float best = 20000.0f;
	uint32_t i;

	for (i = 0; i < g_surface_count; i++)
	{
		const struct SM64SurfaceCollisionData *s = &g_surfaces[i];
		float height;

		if (!s->isValid || s->normal.y >= -0.01f)
			continue;
		if (!triangle_contains_xz(s, x, z))
			continue;
		height = triangle_height_at(s, x, z);
		if (height < best && height >= y - 20.0f)
			best = height;
	}
	return best;
}

/* ---------- the interface ---------- */

SM64_LIB_FN void sm64_register_debug_print_function(SM64DebugPrintFunctionPtr function)
{
	(void)function;
}

SM64_LIB_FN void sm64_register_play_sound_function(SM64PlaySoundFunctionPtr function)
{
	(void)function;
}

SM64_LIB_FN void sm64_global_init(const uint8_t *rom, uint8_t *outTexture)
{
	/* The double needs no assets: a NULL ROM is fine, which is what lets the
	   tests run without one. */
	(void)rom;
	if (outTexture)
		memset(outTexture, 0, 4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
	free(g_surfaces);
	g_surfaces = NULL;
	g_surface_count = 0;
	memset(g_marios, 0, sizeof(g_marios));
	g_initialised = true;
}

SM64_LIB_FN void sm64_global_terminate(void)
{
	free(g_surfaces);
	g_surfaces = NULL;
	g_surface_count = 0;
	memset(g_marios, 0, sizeof(g_marios));
	g_initialised = false;
}

SM64_LIB_FN void sm64_static_surfaces_load(const struct SM64Surface *surfaceArray, uint32_t numSurfaces)
{
	uint32_t i;

	free(g_surfaces);
	g_surfaces = NULL;
	g_surface_count = 0;
	if (numSurfaces == 0 || numSurfaces > MAX_SURFACES)
		return;
	g_surfaces = (struct SM64SurfaceCollisionData *)calloc(numSurfaces, sizeof(*g_surfaces));
	if (!g_surfaces)
		return;
	for (i = 0; i < numSurfaces; i++)
		surface_from_lib(&surfaceArray[i], &g_surfaces[i]);
	g_surface_count = numSurfaces;
}

SM64_LIB_FN int32_t sm64_mario_create(float x, float y, float z)
{
	int index;
	struct Mario *mario;

	if (!g_initialised)
		return -1;
	/* The real library refuses a Mario with no floor under him. */
	if (find_floor_height(x, y, z) <= -10000.0f)
		return -1;

	for (index = 0; index < MAX_MARIOS; index++)
	{
		if (!g_marios[index].used)
			break;
	}
	if (index >= MAX_MARIOS)
		return -1;

	mario = &g_marios[index];
	memset(mario, 0, sizeof(*mario));
	mario->used = true;
	mario->position[0] = x;
	mario->position[1] = y;
	mario->position[2] = z;
	mario->health = 0x880;
	mario->action = 0x0C400201;   /* ACT_IDLE */
	return index;
}

SM64_LIB_FN void sm64_mario_delete(int32_t marioId)
{
	if (marioId >= 0 && marioId < MAX_MARIOS)
		memset(&g_marios[marioId], 0, sizeof(g_marios[marioId]));
}

/* Turn the stick and the camera into a world direction, the way the real
   library does: the stick is relative to the camera, and pushing forward
   walks along -camLook. */
static void stick_to_direction(const struct SM64MarioInputs *inputs, float *out_x, float *out_z,
	float *out_magnitude)
{
	float length = sqrtf(inputs->camLookX * inputs->camLookX + inputs->camLookZ * inputs->camLookZ);
	float forward_x, forward_z, right_x, right_z;
	float stick_x = inputs->stickX, stick_y = inputs->stickY;
	float magnitude = sqrtf(stick_x * stick_x + stick_y * stick_y);

	if (length < 1.0e-6f)
	{
		forward_x = 0.0f; forward_z = -1.0f;
	}
	else
	{
		forward_x = -inputs->camLookX / length;
		forward_z = -inputs->camLookZ / length;
	}
	right_x = -forward_z;
	right_z = forward_x;

	if (magnitude < 0.02f)
		magnitude = 0.0f;
	if (magnitude > 1.0f)
		magnitude = 1.0f;

	*out_x = forward_x * stick_y - right_x * stick_x;
	*out_z = forward_z * stick_y - right_z * stick_x;
	*out_magnitude = magnitude;
}

SM64_LIB_FN void sm64_mario_tick(int32_t marioId, const struct SM64MarioInputs *inputs,
	struct SM64MarioState *outState, struct SM64MarioGeometryBuffers *outBuffers)
{
	struct Mario *mario;
	float wanted_x, wanted_z, magnitude;
	float floor_height, ceiling_height;
	bool grounded;

	if (marioId < 0 || marioId >= MAX_MARIOS || !g_marios[marioId].used || !outState)
		return;
	mario = &g_marios[marioId];

	floor_height = find_floor_height(mario->position[0], mario->position[1], mario->position[2]);
	grounded = mario->position[1] <= floor_height + 0.5f && floor_height > -10000.0f;

	stick_to_direction(inputs, &wanted_x, &wanted_z, &magnitude);

	if (grounded)
	{
		float target = MAX_WALK_SPEED * magnitude;
		float length = sqrtf(wanted_x * wanted_x + wanted_z * wanted_z);
		float speed = sqrtf(mario->velocity[0] * mario->velocity[0]
			+ mario->velocity[2] * mario->velocity[2]);

		/* Accelerate towards the wanted speed, and turn to face it. */
		if (length > 1.0e-6f)
		{
			float nx = wanted_x / length, nz = wanted_z / length;

			if (speed < target)
				speed += (target - speed) * 0.25f + 0.5f;
			else
				speed += (target - speed) * 0.25f;
			mario->velocity[0] = nx * speed;
			mario->velocity[2] = nz * speed;
			mario->face_angle = atan2f(nx, nz);
			mario->action = 0x04000440;   /* ACT_WALKING */
		}
		else
		{
			speed *= 0.7f;
			if (speed < 0.5f)
				speed = 0.0f;
			if (speed > 0.0f)
			{
				float old = sqrtf(mario->velocity[0] * mario->velocity[0]
					+ mario->velocity[2] * mario->velocity[2]);
				if (old > 1.0e-6f)
				{
					mario->velocity[0] *= speed / old;
					mario->velocity[2] *= speed / old;
				}
			}
			else
			{
				mario->velocity[0] = 0.0f;
				mario->velocity[2] = 0.0f;
			}
			mario->action = 0x0C400201;   /* ACT_IDLE */
		}
		mario->forward_velocity = speed;
		mario->jump_count = 0;

		/* Jump: the A button with a cool-off, like the real library's
		   "frames since A" check. */
		if (inputs->buttonA && !mario->jump_held)
		{
			mario->velocity[1] = JUMP_VELOCITY;
			mario->action = 0x03000880;   /* ACT_JUMP */
			mario->jump_count = 1;
		}
	}
	else
	{
		/* Airborne: a little air control, and gravity. */
		mario->velocity[0] += wanted_x * 0.35f * magnitude;
		mario->velocity[2] += wanted_z * 0.35f * magnitude;
		{
			float speed = sqrtf(mario->velocity[0] * mario->velocity[0]
				+ mario->velocity[2] * mario->velocity[2]);
			if (speed > MAX_WALK_SPEED * 1.4f)
			{
				mario->velocity[0] *= (MAX_WALK_SPEED * 1.4f) / speed;
				mario->velocity[2] *= (MAX_WALK_SPEED * 1.4f) / speed;
			}
		}
		if (inputs->buttonA && !mario->jump_held && mario->jump_count == 1)
		{
			mario->velocity[1] = JUMP_VELOCITY * 0.9f;   /* double jump */
			mario->jump_count = 2;
			mario->action = 0x03000881;   /* ACT_DOUBLE_JUMP */
		}
		else if (inputs->buttonA && !mario->jump_held && mario->jump_count == 2)
		{
			mario->velocity[1] = JUMP_VELOCITY * 1.1f;   /* triple jump */
			mario->jump_count = 3;
			mario->action = 0x01000882;   /* ACT_TRIPLE_JUMP */
		}
		mario->action |= 0x00000800;   /* ACT_FLAG_AIR */
	}
	mario->jump_held = inputs->buttonA != 0;

	mario->velocity[1] -= GRAVITY;
	if (mario->velocity[1] < TERMINAL_VELOCITY)
		mario->velocity[1] = TERMINAL_VELOCITY;

	mario->position[0] += mario->velocity[0];
	mario->position[1] += mario->velocity[1];
	mario->position[2] += mario->velocity[2];

	resolve_walls(mario);

	/* Ground, then ceiling. */
	floor_height = find_floor_height(mario->position[0], mario->position[1], mario->position[2]);
	if (floor_height > -10000.0f && mario->position[1] <= floor_height + 78.0f
		&& mario->velocity[1] <= 0.0f && mario->position[1] < floor_height + fabsf(mario->velocity[1]) + 4.0f)
	{
		mario->position[1] = floor_height;
		mario->velocity[1] = 0.0f;
	}
	ceiling_height = find_ceiling_height(mario->position[0], mario->position[1], mario->position[2]);
	if (ceiling_height < 10000.0f && mario->position[1] + MARIO_HEIGHT > ceiling_height)
	{
		mario->position[1] = ceiling_height - MARIO_HEIGHT;
		if (mario->velocity[1] > 0.0f)
			mario->velocity[1] = 0.0f;
	}

	outState->position[0] = mario->position[0];
	outState->position[1] = mario->position[1];
	outState->position[2] = mario->position[2];
	outState->velocity[0] = mario->velocity[0];
	outState->velocity[1] = mario->velocity[1];
	outState->velocity[2] = mario->velocity[2];
	outState->faceAngle = mario->face_angle;
	outState->forwardVelocity = mario->forward_velocity;
	outState->health = mario->health;
	outState->action = mario->action;
	outState->animID = -1;
	outState->animFrame = 0;
	outState->flags = 0;
	outState->particleFlags = 0;
	outState->invincTimer = 0;

	if (outBuffers)
	{
		/* No model: the double has nothing to draw. */
		outBuffers->numTrianglesUsed = 0;
	}
}

SM64_LIB_FN void sm64_set_mario_action(int32_t marioId, uint32_t action)
{
	if (marioId >= 0 && marioId < MAX_MARIOS && g_marios[marioId].used)
		g_marios[marioId].action = action;
}

SM64_LIB_FN void sm64_set_mario_action_arg(int32_t marioId, uint32_t action, uint32_t actionArg)
{
	(void)actionArg;
	sm64_set_mario_action(marioId, action);
}

SM64_LIB_FN void sm64_set_mario_animation(int32_t marioId, int32_t animID)
{
	(void)marioId; (void)animID;
}

SM64_LIB_FN void sm64_set_mario_anim_frame(int32_t marioId, int16_t animFrame)
{
	(void)marioId; (void)animFrame;
}

SM64_LIB_FN void sm64_set_mario_state(int32_t marioId, uint32_t flags)
{
	(void)marioId; (void)flags;
}

SM64_LIB_FN void sm64_set_mario_position(int32_t marioId, float x, float y, float z)
{
	if (marioId < 0 || marioId >= MAX_MARIOS || !g_marios[marioId].used)
		return;
	g_marios[marioId].position[0] = x;
	g_marios[marioId].position[1] = y;
	g_marios[marioId].position[2] = z;
	g_marios[marioId].velocity[0] = 0.0f;
	g_marios[marioId].velocity[1] = 0.0f;
	g_marios[marioId].velocity[2] = 0.0f;
}

SM64_LIB_FN void sm64_set_mario_angle(int32_t marioId, float x, float y, float z)
{
	(void)x; (void)z;
	if (marioId < 0 || marioId >= MAX_MARIOS || !g_marios[marioId].used)
		return;
	g_marios[marioId].face_angle = y;
}

SM64_LIB_FN void sm64_set_mario_faceangle(int32_t marioId, float y)
{
	sm64_set_mario_angle(marioId, 0.0f, y, 0.0f);
}

SM64_LIB_FN void sm64_set_mario_velocity(int32_t marioId, float x, float y, float z)
{
	if (marioId < 0 || marioId >= MAX_MARIOS || !g_marios[marioId].used)
		return;
	g_marios[marioId].velocity[0] = x;
	g_marios[marioId].velocity[1] = y;
	g_marios[marioId].velocity[2] = z;
}

SM64_LIB_FN void sm64_set_mario_forward_velocity(int32_t marioId, float vel)
{
	if (marioId < 0 || marioId >= MAX_MARIOS || !g_marios[marioId].used)
		return;
	g_marios[marioId].forward_velocity = vel;
}

SM64_LIB_FN void sm64_set_mario_invincibility(int32_t marioId, int16_t timer)
{
	(void)marioId; (void)timer;
}

SM64_LIB_FN void sm64_set_mario_water_level(int32_t marioId, signed int level)
{
	(void)marioId; (void)level;
}

SM64_LIB_FN void sm64_set_mario_gas_level(int32_t marioId, signed int level)
{
	(void)marioId; (void)level;
}

SM64_LIB_FN void sm64_set_mario_health(int32_t marioId, uint16_t health)
{
	if (marioId >= 0 && marioId < MAX_MARIOS && g_marios[marioId].used)
		g_marios[marioId].health = (int16_t)health;
}

SM64_LIB_FN void sm64_mario_take_damage(int32_t marioId, uint32_t damage, uint32_t subtype,
	float x, float y, float z)
{
	(void)damage; (void)subtype; (void)x; (void)y; (void)z;
	if (marioId >= 0 && marioId < MAX_MARIOS && g_marios[marioId].used)
		g_marios[marioId].health -= 0x100;
}

SM64_LIB_FN void sm64_mario_heal(int32_t marioId, uint8_t healCounter)
{
	(void)healCounter;
	if (marioId >= 0 && marioId < MAX_MARIOS && g_marios[marioId].used)
		g_marios[marioId].health = 0x880;
}

SM64_LIB_FN void sm64_mario_kill(int32_t marioId)
{
	if (marioId >= 0 && marioId < MAX_MARIOS && g_marios[marioId].used)
		g_marios[marioId].health = 0;
}

SM64_LIB_FN void sm64_mario_interact_cap(int32_t marioId, uint32_t capFlag, uint16_t capTime,
	uint8_t playMusic)
{
	(void)marioId; (void)capFlag; (void)capTime; (void)playMusic;
}

SM64_LIB_FN void sm64_mario_extend_cap(int32_t marioId, uint16_t capTime)
{
	(void)marioId; (void)capTime;
}

/* The double has no moving platforms: it takes the surfaces but never moves
   them.  The module calls these only for the optional dynamic objects. */
SM64_LIB_FN uint32_t sm64_surface_object_create(const struct SM64SurfaceObject *surfaceObject)
{
	(void)surfaceObject;
	return 0;
}

SM64_LIB_FN void sm64_surface_object_move(uint32_t objectId, const struct SM64ObjectTransform *transform)
{
	(void)objectId; (void)transform;
}

SM64_LIB_FN void sm64_surface_object_delete(uint32_t objectId)
{
	(void)objectId;
}

SM64_LIB_FN bool sm64_mario_attack(int32_t marioId, float x, float y, float z, float hitboxHeight)
{
	(void)marioId; (void)x; (void)y; (void)z; (void)hitboxHeight;
	return false;
}

/* Surface queries: the module does not call them, but a host might. */
SM64_LIB_FN int32_t sm64_surface_find_wall_collision(float *xPtr, float *yPtr, float *zPtr,
	float offsetY, float radius)
{
	(void)xPtr; (void)yPtr; (void)zPtr; (void)offsetY; (void)radius;
	return 0;
}

SM64_LIB_FN int32_t sm64_surface_find_wall_collisions(struct SM64WallCollisionData *colData)
{
	(void)colData;
	return 0;
}

SM64_LIB_FN float sm64_surface_find_ceil(float posX, float posY, float posZ,
	struct SM64SurfaceCollisionData **pceil)
{
	float height = find_ceiling_height(posX, posY, posZ);

	if (pceil)
		*pceil = NULL;
	return height > 10000.0f ? 20000.0f : height;
}

SM64_LIB_FN float sm64_surface_find_floor_height_and_data(float xPos, float yPos, float zPos,
	struct SM64FloorCollisionData **floorGeo)
{
	float height = find_floor_height(xPos, yPos, zPos);

	if (floorGeo)
		*floorGeo = NULL;
	return height;
}

SM64_LIB_FN float sm64_surface_find_floor_height(float x, float y, float z)
{
	return find_floor_height(x, y, z);
}

SM64_LIB_FN float sm64_surface_find_floor(float xPos, float yPos, float zPos,
	struct SM64SurfaceCollisionData **pfloor)
{
	if (pfloor)
		*pfloor = NULL;
	return find_floor_height(xPos, yPos, zPos);
}

SM64_LIB_FN float sm64_surface_find_water_level(float x, float z)
{
	(void)x; (void)z;
	return -11000.0f;
}

SM64_LIB_FN float sm64_surface_find_poison_gas_level(float x, float z)
{
	(void)x; (void)z;
	return -11000.0f;
}

/* Audio: the module never starts libsm64's audio, and the double makes no
   sound either. */
SM64_LIB_FN void sm64_audio_init(const uint8_t *rom) { (void)rom; }
SM64_LIB_FN uint32_t sm64_audio_tick(uint32_t numQueuedSamples, uint32_t numDesiredSamples,
	int16_t *audio_buffer)
{
	(void)numQueuedSamples;
	if (audio_buffer && numDesiredSamples)
		memset(audio_buffer, 0, numDesiredSamples * sizeof(*audio_buffer) * 2);
	return numDesiredSamples > 0xFFFF ? 0xFFFF : numDesiredSamples;
}

SM64_LIB_FN void sm64_seq_player_play_sequence(uint8_t player, uint8_t seqId, uint16_t arg2)
{
	(void)player; (void)seqId; (void)arg2;
}

SM64_LIB_FN void sm64_play_music(uint8_t player, uint16_t seqArgs, uint16_t fadeTimer)
{
	(void)player; (void)seqArgs; (void)fadeTimer;
}

SM64_LIB_FN void sm64_stop_background_music(uint16_t seqId) { (void)seqId; }
SM64_LIB_FN void sm64_fadeout_background_music(uint16_t arg0, uint16_t fadeOut)
{
	(void)arg0; (void)fadeOut;
}
SM64_LIB_FN uint16_t sm64_get_current_background_music(void) { return 0; }
SM64_LIB_FN void sm64_play_sound(int32_t soundBits, float *pos)
{
	(void)soundBits; (void)pos;
}
SM64_LIB_FN void sm64_play_sound_global(int32_t soundBits) { (void)soundBits; }
SM64_LIB_FN void sm64_set_sound_volume(float vol) { (void)vol; }
