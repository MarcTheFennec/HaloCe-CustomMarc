/*
TEST_ABI.C

The module declares libsm64's structures again in its own header
(include/sm64_port_abi.h) and loads the library at run time, so nothing checks
at link time that the two agree.  This translation unit includes both headers
and makes the compiler compare them: if libsm64 changes the layout of a
structure it exports, this file stops compiling instead of the module handing
the library misread memory.

It is compiled by tests/Makefile, which needs a copy of libsm64's public
header (tests/libsm64.h, from third_party/libsm64/src/libsm64.h).  When the
submodule is checked out, that copy is compared against it too, so a libsm64
update cannot go unnoticed.

CC0 1.0 Universal, like the rest of the port.
*/

#include "sm64_port_abi.h"
#include "libsm64.h"

#include <stddef.h>

/* ---------- struct SM64Surface / SM64PortAbiSurface ---------- */

typedef char abi_surface_size[sizeof(struct SM64PortAbiSurface) == sizeof(struct SM64Surface) ? 1 : -1];
typedef char abi_surface_type[offsetof(struct SM64PortAbiSurface, type) == offsetof(struct SM64Surface, type) ? 1 : -1];
typedef char abi_surface_force[offsetof(struct SM64PortAbiSurface, force) == offsetof(struct SM64Surface, force) ? 1 : -1];
typedef char abi_surface_terrain[offsetof(struct SM64PortAbiSurface, terrain) == offsetof(struct SM64Surface, terrain) ? 1 : -1];
typedef char abi_surface_vertices[offsetof(struct SM64PortAbiSurface, vertices) == offsetof(struct SM64Surface, vertices) ? 1 : -1];

/* ---------- struct SM64MarioInputs / SM64PortAbiMarioInputs ---------- */

typedef char abi_inputs_size[sizeof(struct SM64PortAbiMarioInputs) == sizeof(struct SM64MarioInputs) ? 1 : -1];
typedef char abi_inputs_cam_look_x[offsetof(struct SM64PortAbiMarioInputs, camLookX) == offsetof(struct SM64MarioInputs, camLookX) ? 1 : -1];
typedef char abi_inputs_cam_look_z[offsetof(struct SM64PortAbiMarioInputs, camLookZ) == offsetof(struct SM64MarioInputs, camLookZ) ? 1 : -1];
typedef char abi_inputs_stick_x[offsetof(struct SM64PortAbiMarioInputs, stickX) == offsetof(struct SM64MarioInputs, stickX) ? 1 : -1];
typedef char abi_inputs_stick_y[offsetof(struct SM64PortAbiMarioInputs, stickY) == offsetof(struct SM64MarioInputs, stickY) ? 1 : -1];
typedef char abi_inputs_button_a[offsetof(struct SM64PortAbiMarioInputs, buttonA) == offsetof(struct SM64MarioInputs, buttonA) ? 1 : -1];
typedef char abi_inputs_button_b[offsetof(struct SM64PortAbiMarioInputs, buttonB) == offsetof(struct SM64MarioInputs, buttonB) ? 1 : -1];
typedef char abi_inputs_button_z[offsetof(struct SM64PortAbiMarioInputs, buttonZ) == offsetof(struct SM64MarioInputs, buttonZ) ? 1 : -1];

/* ---------- struct SM64MarioState / SM64PortAbiMarioState ---------- */

typedef char abi_state_size[sizeof(struct SM64PortAbiMarioState) == sizeof(struct SM64MarioState) ? 1 : -1];
typedef char abi_state_position[offsetof(struct SM64PortAbiMarioState, position) == offsetof(struct SM64MarioState, position) ? 1 : -1];
typedef char abi_state_velocity[offsetof(struct SM64PortAbiMarioState, velocity) == offsetof(struct SM64MarioState, velocity) ? 1 : -1];
typedef char abi_state_face_angle[offsetof(struct SM64PortAbiMarioState, faceAngle) == offsetof(struct SM64MarioState, faceAngle) ? 1 : -1];
typedef char abi_state_forward_velocity[offsetof(struct SM64PortAbiMarioState, forwardVelocity) == offsetof(struct SM64MarioState, forwardVelocity) ? 1 : -1];
typedef char abi_state_health[offsetof(struct SM64PortAbiMarioState, health) == offsetof(struct SM64MarioState, health) ? 1 : -1];
typedef char abi_state_action[offsetof(struct SM64PortAbiMarioState, action) == offsetof(struct SM64MarioState, action) ? 1 : -1];
typedef char abi_state_anim_id[offsetof(struct SM64PortAbiMarioState, animID) == offsetof(struct SM64MarioState, animID) ? 1 : -1];
typedef char abi_state_anim_frame[offsetof(struct SM64PortAbiMarioState, animFrame) == offsetof(struct SM64MarioState, animFrame) ? 1 : -1];
typedef char abi_state_flags[offsetof(struct SM64PortAbiMarioState, flags) == offsetof(struct SM64MarioState, flags) ? 1 : -1];
typedef char abi_state_particle_flags[offsetof(struct SM64PortAbiMarioState, particleFlags) == offsetof(struct SM64MarioState, particleFlags) ? 1 : -1];

/* ---------- struct SM64MarioGeometryBuffers ---------- */

typedef char abi_buffers_size[sizeof(struct SM64PortAbiMarioGeometryBuffers) == sizeof(struct SM64MarioGeometryBuffers) ? 1 : -1];
typedef char abi_buffers_position[offsetof(struct SM64PortAbiMarioGeometryBuffers, position) == offsetof(struct SM64MarioGeometryBuffers, position) ? 1 : -1];
typedef char abi_buffers_normal[offsetof(struct SM64PortAbiMarioGeometryBuffers, normal) == offsetof(struct SM64MarioGeometryBuffers, normal) ? 1 : -1];
typedef char abi_buffers_color[offsetof(struct SM64PortAbiMarioGeometryBuffers, color) == offsetof(struct SM64MarioGeometryBuffers, color) ? 1 : -1];
typedef char abi_buffers_uv[offsetof(struct SM64PortAbiMarioGeometryBuffers, uv) == offsetof(struct SM64MarioGeometryBuffers, uv) ? 1 : -1];
typedef char abi_buffers_used[offsetof(struct SM64PortAbiMarioGeometryBuffers, numTrianglesUsed) == offsetof(struct SM64MarioGeometryBuffers, numTrianglesUsed) ? 1 : -1];

/* ---------- struct SM64ObjectTransform and SM64SurfaceObject ---------- */

typedef char abi_transform_size[sizeof(struct SM64PortAbiObjectTransform) == sizeof(struct SM64ObjectTransform) ? 1 : -1];
typedef char abi_transform_position[offsetof(struct SM64PortAbiObjectTransform, position) == offsetof(struct SM64ObjectTransform, position) ? 1 : -1];
typedef char abi_transform_rotation[offsetof(struct SM64PortAbiObjectTransform, eulerRotation) == offsetof(struct SM64ObjectTransform, eulerRotation) ? 1 : -1];
typedef char abi_object_size[sizeof(struct SM64PortAbiSurfaceObject) == sizeof(struct SM64SurfaceObject) ? 1 : -1];
typedef char abi_object_transform[offsetof(struct SM64PortAbiSurfaceObject, transform) == offsetof(struct SM64SurfaceObject, transform) ? 1 : -1];
typedef char abi_object_count[offsetof(struct SM64PortAbiSurfaceObject, surfaceCount) == offsetof(struct SM64SurfaceObject, surfaceCount) ? 1 : -1];
typedef char abi_object_surfaces[offsetof(struct SM64PortAbiSurfaceObject, surfaces) == offsetof(struct SM64SurfaceObject, surfaces) ? 1 : -1];

/* ---------- the constants the module uses ---------- */

typedef char abi_max_triangles[(int)SM64_PORT_GEO_MAX_TRIANGLES == (int)SM64_GEO_MAX_TRIANGLES ? 1 : -1];
typedef char abi_texture_width[(int)SM64_PORT_TEXTURE_WIDTH == (int)SM64_TEXTURE_WIDTH ? 1 : -1];
typedef char abi_texture_height[(int)SM64_PORT_TEXTURE_HEIGHT == (int)SM64_TEXTURE_HEIGHT ? 1 : -1];

/* A copy of libsm64's header is kept here so that these tests build without
   the submodule.  "make check-header" compares the copy against the
   submodule's, so a libsm64 update that changed the interface is noticed. */
/* The module's public surface type has to match libsm64's too, since callers
   build triangles with it. */
#include "sm64_port.h"
typedef char abi_public_surface_size[sizeof(SM64PortSurface) == sizeof(struct SM64Surface) ? 1 : -1];
typedef char abi_public_surface_vertices[offsetof(SM64PortSurface, vertices) == offsetof(struct SM64Surface, vertices) ? 1 : -1];

int main(void)
{
	/* If this runs, every assertion above held at compile time. */
	return 0;
}
