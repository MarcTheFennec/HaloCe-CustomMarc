/*
SM64_PORT_ABI.H

The parts of libsm64's interface the module calls, declared again so that the
module compiles without libsm64's headers (the library is loaded at run time,
not linked).  Every declaration must match ``src/libsm64.h`` of
third_party/libsm64 field for field: the host tests include both headers and
static-assert that the sizes and offsets agree
(``port/libsm64_port/tests/test_abi.c``), so a mismatch breaks the build of
the tests rather than corrupting memory at run time.

Because the library is loaded with dlopen/LoadLibrary, the structures only
have to be *laid out* the same way: no compiler flag of the game's ABI
(-malign-double, -freg-struct-return, ...) changes the layout of a struct
made of floats, 32-bit integers and pointers, which these are.

libsm64 is CC0 1.0 Universal; this mirror of its interface is too.
*/

#ifndef SM64_PORT_ABI_H
#define SM64_PORT_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The values of libsm64's enum SM64_*. */
enum
{
	SM64_PORT_GEO_MAX_TRIANGLES = 1024,
	SM64_PORT_TEXTURE_WIDTH = 64 * 11,
	SM64_PORT_TEXTURE_HEIGHT = 64
};

struct SM64PortAbiSurface
{
	int16_t type;
	int16_t force;
	uint16_t terrain;
	int32_t vertices[3][3];
};

struct SM64PortAbiMarioInputs
{
	float camLookX, camLookZ;
	float stickX, stickY;
	uint8_t buttonA, buttonB, buttonZ;
};

struct SM64PortAbiObjectTransform
{
	float position[3];
	float eulerRotation[3];
};

struct SM64PortAbiSurfaceObject
{
	struct SM64PortAbiObjectTransform transform;
	uint32_t surfaceCount;
	struct SM64PortAbiSurface *surfaces;
};

struct SM64PortAbiMarioState
{
	float position[3];
	float velocity[3];
	float faceAngle;
	float forwardVelocity;
	int16_t health;
	uint32_t action;
	int32_t animID;
	int16_t animFrame;
	uint32_t flags;
	uint32_t particleFlags;
	int16_t invincTimer;
};

struct SM64PortAbiMarioGeometryBuffers
{
	float *position;
	float *normal;
	float *color;
	float *uv;
	uint16_t numTrianglesUsed;
};

/* Mario's action groups, from libsm64's sm64.h: an actor is off the ground
   when its action is in the airborne group or carries the air flag. */
#define SM64_PORT_ACT_GROUP_MASK 0x000001C0u
#define SM64_PORT_ACT_GROUP_AIRBORNE 0x00000080u
#define SM64_PORT_ACT_FLAG_AIR 0x00000800u

/* Terrain types of libsm64's surface_terrains.h, for the collision
   converter: what a Halo material becomes. */
#define SM64_PORT_TERRAIN_GRASS 0x0000
#define SM64_PORT_TERRAIN_STONE 0x0001
#define SM64_PORT_TERRAIN_SNOW 0x0003
#define SM64_PORT_TERRAIN_SAND 0x0004
#define SM64_PORT_TERRAIN_SPOOKY 0x0005
#define SM64_PORT_TERRAIN_WATER 0x000A
#define SM64_PORT_TERRAIN_SLIDE 0x000D
#define SM64_PORT_TERRAIN_DEFAULT 0x0016

#ifdef __cplusplus
}
#endif

#endif /* SM64_PORT_ABI_H */
