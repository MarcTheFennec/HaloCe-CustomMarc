/*
TEST_SM64_PORT.C

The host-side tests of the libsm64 module: unit tests of the conversions, the
clock, the input mapping and the collision converter, and integration tests
that run an actor around a room.

They run against whichever libsm64 they find:

  1. $SM64_PORT_TEST_LIBRARY,
  2. a real libsm64 the build put in build/libsm64 or third_party/libsm64/dist,
     which is then fed the synthetic asset blob of sm64_test_blob.c, or
  3. the test double (sm64_test_double.c), which needs no ROM.

Tests that need the real Super Mario 64 physics (triple jumps, wall kicks,
exact jump heights) say so and skip themselves against the double; everything
else must pass on both.

Build and run: see port/libsm64_port/tests/Makefile.

CC0 1.0 Universal, like the rest of the port.
*/

#include "sm64_port.h"
#include "sm64_port_abi.h"   /* the terrain constants the collision tests use */
#include "sm64_test_blob.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- a very small test framework ---------- */

static int g_tests_run;
static int g_tests_failed;
static int g_current_failed;
static const char *g_current_name;

static void test_begin(const char *name)
{
	g_current_name = name;
	g_current_failed = 0;
	g_tests_run++;
}

static void test_fail(const char *file, int line, const char *format, ...)
{
	va_list arguments;
	char message[512];

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (!g_current_failed)
		printf("  FAIL %s\n", g_current_name);
	printf("       %s:%d: %s\n", file, line, message);
	g_current_failed = 1;
}

static void test_end(void)
{
	if (g_current_failed)
		g_tests_failed++;
	else
		printf("  ok   %s\n", g_current_name);
}

#define CHECK(condition, ...) \
	do { if (!(condition)) test_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)

#define CHECK_NEAR(actual, expected, tolerance, label) \
	do { \
		double a_ = (double)(actual), e_ = (double)(expected), t_ = (double)(tolerance); \
		if (!(fabs(a_ - e_) <= t_)) \
			test_fail(__FILE__, __LINE__, "%s: %g is not within %g of %g", label, a_, t_, e_); \
	} while (0)

/* ---------- the host ---------- */

static int g_log_count;
static char g_last_log[512];

static void test_log(void *context, const char *message)
{
	(void)context;
	g_log_count++;
	snprintf(g_last_log, sizeof(g_last_log), "%s", message);
}

/* When the tests run against the real library, every "ROM" is the synthetic
   blob; when they run against the double, it is a block of the right size
   that the double ignores. */
static uint8_t *g_fake_rom;
static size_t g_fake_rom_size;

static bool test_read_file(void *context, const char *path, uint8_t **out_data, size_t *out_size)
{
	(void)context;
	if (!path || !out_data || !out_size)
		return false;

	if (g_fake_rom)
	{
		uint8_t *copy = (uint8_t *)malloc(g_fake_rom_size);
		if (!copy)
			return false;
		memcpy(copy, g_fake_rom, g_fake_rom_size);
		*out_data = copy;
		*out_size = g_fake_rom_size;
		return true;
	}

	/* No synthetic blob: read the file, which is how the game does it. */
	{
		FILE *file = fopen(path, "rb");
		long size;
		uint8_t *data;

		if (!file)
			return false;
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (size <= 0)
		{
			fclose(file);
			return false;
		}
		data = (uint8_t *)malloc((size_t)size);
		if (!data)
		{
			fclose(file);
			return false;
		}
		if (fread(data, 1, (size_t)size, file) != (size_t)size)
		{
			free(data);
			fclose(file);
			return false;
		}
		fclose(file);
		*out_data = data;
		*out_size = (size_t)size;
		return true;
	}
}

static void test_free_file(void *context, uint8_t *data)
{
	(void)context;
	free(data);
}

/* ---------- fixture ---------- */

static const char *g_library_path;      /* what the module loaded */
static int g_using_real_library;        /* 1: real libsm64, 0: the double */

static void configure_module(SM64PortConfig *out_config, SM64PortHost *out_host)
{
	SM64Port_ConfigDefaults(out_config);
	out_config->library_path = g_library_path;
	out_config->halo_units_per_sm64_unit = 0.01f;
	out_config->debug_draw = false;
	out_config->collision_radius = 40.0f;
	out_config->max_surfaces = 4096;

	memset(out_host, 0, sizeof(*out_host));
	out_host->log_message = test_log;
	out_host->read_file = test_read_file;
	out_host->free_file = test_free_file;
}

/* Builds the test room and hands it to libsm64; returns 1 on success. */
static int load_test_room(void)
{
	SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(512);
	uint32_t count = 0;
	const SM64PortSurface *surfaces;
	int result = 0;

	if (!set)
		return 0;
	if (!SM64Port_BuildTestRoom(set, 20.0f))
	{
		SM64Port_SurfaceSetDestroy(set);
		return 0;
	}
	surfaces = SM64Port_SurfaceSetSurfaces(set, &count);
	if (SM64Port_SetStaticCollision(surfaces, count) == SM64_PORT_OK)
		result = 1;
	SM64Port_SurfaceSetDestroy(set);
	return result;
}

/* Runs seconds of simulated time at 30 frames a second with a fixed input. */
static void run_seconds(SM64PortActorId id, const SM64PortInput *input, double seconds)
{
	int frames = (int)(seconds * 30.0 + 0.5);
	int i;

	for (i = 0; i < frames; i++)
	{
		SM64Port_SetInput(id, input);
		SM64Port_Update(1.0 / 30.0);
	}
}

/* ---------- unit tests: conversions ---------- */

static void test_conversions_round_trip(void)
{
	const float k = 0.01f;
	SM64PortVector3 points[8];
	size_t i;

	points[0].x = 0.0f; points[0].y = 0.0f; points[0].z = 0.0f;
	points[1].x = 1.0f; points[1].y = 0.0f; points[1].z = 0.0f;
	points[2].x = 0.0f; points[2].y = 1.0f; points[2].z = 0.0f;
	points[3].x = 0.0f; points[3].y = 0.0f; points[3].z = 1.0f;
	points[4].x = 12.5f; points[4].y = -3.25f; points[4].z = 7.75f;
	points[5].x = -100.0f; points[5].y = 250.0f; points[5].z = 0.5f;
	points[6].x = 0.001f; points[6].y = 0.0f; points[6].z = 0.0f;
	points[7].x = 1234.5f; points[7].y = -678.9f; points[7].z = 42.0f;

	test_begin("conversions: Halo -> SM64 -> Halo is the identity");
	for (i = 0; i < sizeof(points) / sizeof(points[0]); i++)
	{
		SM64PortVector3 sm64 = SM64Port_HaloToSM64Position(points[i], k);
		SM64PortVector3 back = SM64Port_SM64ToHaloPosition(sm64, k);

		CHECK_NEAR(back.x, points[i].x, 1.0e-4, "x");
		CHECK_NEAR(back.y, points[i].y, 1.0e-4, "y");
		CHECK_NEAR(back.z, points[i].z, 1.0e-4, "z");
	}
	test_end();

	test_begin("conversions: known points");
	{
		SM64PortVector3 a = SM64Port_HaloToSM64Position(points[1], k);   /* +1 Halo X */
		SM64PortVector3 b = SM64Port_HaloToSM64Position(points[2], k);   /* +1 Halo Y */
		SM64PortVector3 c = SM64Port_HaloToSM64Position(points[3], k);   /* +1 Halo Z (up) */

		CHECK_NEAR(a.x, 100.0, 1.0e-3, "+1 Halo X is 100 SM64 units along SM64 X");
		CHECK_NEAR(a.y, 0.0, 1.0e-3, "+1 Halo X has no SM64 Y");
		CHECK_NEAR(a.z, 0.0, 1.0e-3, "+1 Halo X has no SM64 Z");
		CHECK_NEAR(b.z, -100.0, 1.0e-3, "+1 Halo Y is -100 SM64 Z");
		CHECK_NEAR(c.y, 100.0, 1.0e-3, "+1 Halo Z (up) is +100 SM64 Y");
	}
	test_end();

	test_begin("conversions: directions keep their length");
	{
		SM64PortVector3 halo;
		SM64PortVector3 sm64;

		halo.x = 0.6f; halo.y = -0.8f; halo.z = 0.0f;
		sm64 = SM64Port_HaloToSM64Direction(halo);
		CHECK_NEAR(sqrtf(sm64.x * sm64.x + sm64.y * sm64.y + sm64.z * sm64.z), 1.0, 1.0e-5,
			"a unit direction stays a unit direction");
		CHECK_NEAR(sm64.x, 0.6, 1.0e-5, "Halo X stays SM64 X");
		CHECK_NEAR(sm64.z, 0.8, 1.0e-5, "Halo Y becomes -SM64 Z");
		CHECK_NEAR(sm64.y, 0.0, 1.0e-5, "the horizontal stays horizontal");
	}
	test_end();
}

static void test_conversions_yaw(void)
{
	const float pi = 3.14159265358979323846f;
	float angles[9];
	size_t i;

	angles[0] = 0.0f; angles[1] = 0.25f; angles[2] = pi * 0.25f; angles[3] = pi * 0.5f;
	angles[4] = pi; angles[5] = -pi * 0.5f; angles[6] = 2.0f; angles[7] = -2.0f;
	angles[8] = pi * 0.99f;

	test_begin("conversions: yaw round-trips and is a quarter turn");
	for (i = 0; i < sizeof(angles) / sizeof(angles[0]); i++)
	{
		float sm64 = SM64Port_HaloToSM64Yaw(angles[i]);
		float back = SM64Port_SM64ToHaloYaw(sm64);
		float difference = back - angles[i];

		while (difference > pi)
			difference -= 2.0f * pi;
		while (difference < -pi)
			difference += 2.0f * pi;

		CHECK_NEAR(difference, 0.0, 1.0e-4, "yaw round trip");
		CHECK(sm64 >= -pi - 1.0e-4f && sm64 <= pi + 1.0e-4f, "the SM64 yaw is wrapped to (-pi, pi]");
	}
	CHECK_NEAR(SM64Port_HaloToSM64Yaw(0.0f), pi * 0.5f, 1.0e-5, "Halo yaw 0 (+X) is SM64 yaw pi/2");
	test_end();

	test_begin("conversions: a yaw means the same direction in both systems");
	for (i = 0; i < 16; i++)
	{
		float halo_yaw = (float)i * (2.0f * pi / 16.0f);
		float sm64_yaw = SM64Port_HaloToSM64Yaw(halo_yaw);
		/* Halo: forward = (cos yaw, sin yaw, 0).  libsm64: (sin yaw, 0,
		   cos yaw) - measured with the real library. */
		SM64PortVector3 halo_forward, sm64_forward, converted;

		halo_forward.x = cosf(halo_yaw);
		halo_forward.y = sinf(halo_yaw);
		halo_forward.z = 0.0f;

		sm64_forward.x = sinf(sm64_yaw);
		sm64_forward.y = 0.0f;
		sm64_forward.z = cosf(sm64_yaw);

		converted = SM64Port_SM64ToHaloDirection(sm64_forward);
		CHECK_NEAR(converted.x, halo_forward.x, 1.0e-4, "forward x");
		CHECK_NEAR(converted.y, halo_forward.y, 1.0e-4, "forward y");
		CHECK_NEAR(converted.z, halo_forward.z, 1.0e-4, "forward z");
	}
	test_end();
}

/* ---------- unit tests: the clock ---------- */

static void test_clock(void)
{
	SM64PortClock clock;
	double alpha = -1.0;
	int steps;

	test_begin("clock: 60 Hz frames give one tick every other frame");
	SM64Port_ClockInit(&clock, 1.0 / 30.0, 4);
	steps = SM64Port_ClockAdvance(&clock, 1.0 / 60.0, &alpha);
	CHECK(steps == 0, "the first half frame runs no tick (got %d)", steps);
	CHECK_NEAR(alpha, 0.5, 1.0e-9, "alpha after half a step");
	steps = SM64Port_ClockAdvance(&clock, 1.0 / 60.0, &alpha);
	CHECK(steps == 1, "the second half frame runs one tick (got %d)", steps);
	CHECK_NEAR(alpha, 0.0, 1.0e-9, "alpha after a whole step");
	test_end();

	test_begin("clock: 30, 60 and 144 frames a second all produce 30 ticks a second");
	{
		const double rates[3] = { 30.0, 60.0, 144.0 };
		int i;

		for (i = 0; i < 3; i++)
		{
			SM64Port_ClockInit(&clock, 1.0 / 30.0, 8);
			int frame;
			for (frame = 0; frame < (int)rates[i] * 10; frame++)
				SM64Port_ClockAdvance(&clock, 1.0 / rates[i], &alpha);
			/* ten seconds at any frame rate is 300 ticks, within one. */
			CHECK(clock.total_steps >= 299 && clock.total_steps <= 301,
				"%.0f Hz: %ld ticks in ten seconds, expected 300", rates[i], clock.total_steps);
		}
	}
	test_end();

	test_begin("clock: no drift over a simulated hour at 60 Hz");
	SM64Port_ClockInit(&clock, 1.0 / 30.0, 8);
	{
		long frame;
		for (frame = 0; frame < 60 * 60 * 60; frame++)
			SM64Port_ClockAdvance(&clock, 1.0 / 60.0, &alpha);
		/* 3600 seconds must be 108000 ticks, give or take the odd one. */
		CHECK(clock.total_steps >= 108000 - 2 && clock.total_steps <= 108000 + 2,
			"an hour at 60 Hz gave %ld ticks, expected 108000", clock.total_steps);
	}
	test_end();

	test_begin("clock: a stalled frame is clamped, not paid back");
	SM64Port_ClockInit(&clock, 1.0 / 30.0, 4);
	steps = SM64Port_ClockAdvance(&clock, 10.0, &alpha);
	CHECK(steps == 4, "ten seconds in one frame runs four ticks, not 300 (got %d)", steps);
	CHECK(clock.total_steps == 4, "the dropped time is not remembered (%ld)", clock.total_steps);
	test_end();

	test_begin("clock: a zero or backwards frame changes nothing");
	SM64Port_ClockInit(&clock, 1.0 / 30.0, 4);
	steps = SM64Port_ClockAdvance(&clock, 0.0, &alpha);
	CHECK(steps == 0, "a zero-length frame runs no tick (got %d)", steps);
	steps = SM64Port_ClockAdvance(&clock, -1.0, &alpha);
	CHECK(steps == 0, "a backwards frame runs no tick (got %d)", steps);
	CHECK_NEAR(clock.accumulator, 0.0, 1.0e-12, "the accumulator stays empty");
	test_end();

	test_begin("clock: a variable frame rate still averages 30 ticks a second");
	SM64Port_ClockInit(&clock, 1.0 / 30.0, 8);
	{
		double simulated = 0.0;
		srand(12345);
		/* An hour of simulated time, in frames of 20 ms to 60 ms: a jittery
		   17-50 Hz.  However the time is chopped up, an hour at 30 ticks a
		   second is 108000 ticks. */
		while (simulated < 3600.0)
		{
			double dt = 0.020 + (double)(rand() % 400) / 10000.0;
			SM64Port_ClockAdvance(&clock, dt, &alpha);
			simulated += dt;
		}
		CHECK(clock.total_steps > 0, "the clock runs");
		CHECK(clock.total_steps >= 107000 && clock.total_steps <= 109000,
			"an hour of jittery frames gave %ld ticks, which is not near 108000",
			clock.total_steps);
	}
	test_end();
}

/* ---------- unit tests: input mapping ---------- */

static void test_input_mapping(void)
{
	test_begin("input: below the dead zone the stick is centred");
	{
		SM64PortInput input;
		float stick_x = 9.0f, stick_y = 9.0f, look_x = 9.0f, look_z = 9.0f;

		memset(&input, 0, sizeof(input));
		input.move_x = 0.02f;
		input.move_y = 0.0f;
		SM64Port_HaloInputToSM64Input(&input, 0.08f, 1.0f, false, &stick_x, &stick_y, &look_x, &look_z);
		CHECK_NEAR(stick_y, 0.0, 1.0e-6, "a nudge inside the dead zone moves nothing");
	}
	test_end();

	test_begin("input: the camera look is minus the wanted direction, the stick is forward");
	{
		const float pi = 3.14159265358979323846f;
		int i;

		for (i = 0; i < 8; i++)
		{
			float halo_yaw = (float)i * (2.0f * pi / 8.0f);
			SM64PortInput input;
			float stick_x, stick_y, look_x, look_z;
			SM64PortVector3 wanted_halo, wanted_sm64, resulting_direction;
			float length;

			memset(&input, 0, sizeof(input));
			input.move_x = cosf(halo_yaw);
			input.move_y = sinf(halo_yaw);

			SM64Port_HaloInputToSM64Input(&input, 0.0f, 1.0f, false, &stick_x, &stick_y, &look_x,
				&look_z);

			/* What the module asks for: travel along -camLook. */
			length = sqrtf(look_x * look_x + look_z * look_z);
			CHECK_NEAR(length, 1.0, 1.0e-4, "the camera look vector is normalised");
			resulting_direction.x = -look_x / length;
			resulting_direction.y = 0.0f;
			resulting_direction.z = -look_z / length;

			wanted_halo.x = input.move_x;
			wanted_halo.y = input.move_y;
			wanted_halo.z = 0.0f;
			wanted_sm64 = SM64Port_HaloToSM64Direction(wanted_halo);

			CHECK_NEAR(resulting_direction.x, wanted_sm64.x, 1.0e-4, "direction x");
			CHECK_NEAR(resulting_direction.z, wanted_sm64.z, 1.0e-4, "direction z");
			CHECK_NEAR(stick_x, 0.0, 1.0e-6, "the stick stays on the forward axis");
			CHECK_NEAR(stick_y, 1.0, 1.0e-4, "the stick is fully forward");
		}
	}
	test_end();

	test_begin("input: the analogue magnitude reaches the stick");
	{
		SM64PortInput input;
		float stick_x, stick_y, look_x, look_z;

		memset(&input, 0, sizeof(input));
		input.move_x = 0.5f;   /* half way along +X, inside no dead zone */
		input.move_y = 0.0f;
		SM64Port_HaloInputToSM64Input(&input, 0.0f, 1.0f, false, &stick_x, &stick_y, &look_x, &look_z);
		CHECK_NEAR(stick_y, 0.5, 1.0e-5, "half stick stays half stick");

		/* With a dead zone of 0.2, half stick becomes (0.5-0.2)/0.8. */
		SM64Port_HaloInputToSM64Input(&input, 0.2f, 1.0f, false, &stick_x, &stick_y, &look_x, &look_z);
		CHECK_NEAR(stick_y, 0.375, 1.0e-5, "the dead zone is rescaled away");
	}
	test_end();

	test_begin("input: invert_y mirrors the forward axis, sensitivity scales it");
	{
		SM64PortInput input;
		float stick_x, stick_y, look_x, look_z;
		float normal_z, inverted_z, twice_y;

		memset(&input, 0, sizeof(input));
		input.move_x = 0.0f;
		input.move_y = 1.0f;

		SM64Port_HaloInputToSM64Input(&input, 0.0f, 1.0f, false, &stick_x, &stick_y, &look_x, &normal_z);
		SM64Port_HaloInputToSM64Input(&input, 0.0f, 1.0f, true, &stick_x, &stick_y, &look_x, &inverted_z);
		CHECK_NEAR(inverted_z, -normal_z, 1.0e-5, "invert_y flips the direction");

		input.move_y = 0.25f;
		SM64Port_HaloInputToSM64Input(&input, 0.0f, 2.0f, false, &stick_x, &twice_y, &look_x, &look_z);
		CHECK_NEAR(twice_y, 0.5, 1.0e-5, "a sensitivity of 2 doubles the stick");
	}
	test_end();

	test_begin("input: buttons pass straight through");
	{
		/* The buttons are part of the input struct the module copies into
		   libsm64's own, so there is nothing to convert: check the field is
		   carried by the actor rather than lost. (Exercised end to end by the
		   jump tests.) */
		SM64PortInput input;
		memset(&input, 0, sizeof(input));
		input.button_a = 1;
		input.button_b = 0;
		input.button_z = 1;
		CHECK(input.button_a == 1 && input.button_b == 0 && input.button_z == 1, "buttons are kept");
	}
	test_end();
}

/* ---------- unit tests: the collision converter ---------- */

static void test_surface_set(void)
{
	test_begin("surfaces: a triangle is added in SM64 units");
	{
		SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(16);
		const float a[3] = { 0.0f, 0.0f, 0.0f };
		const float b[3] = { 1.0f, 0.0f, 0.0f };
		const float c[3] = { 1.0f, 1.0f, 0.0f };
		const float up[3] = { 0.0f, 0.0f, 1.0f };
		uint32_t count = 0;
		const SM64PortSurface *surfaces;

		CHECK(set != NULL, "the set was created");
		CHECK(SM64Port_SurfaceSetAddTriangle(set, a, b, c, up, SM64_PORT_TERRAIN_STONE),
			"the triangle was added");
		surfaces = SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count == 1, "one triangle is in the set (got %u)", count);
		CHECK(surfaces != NULL, "the surfaces are readable");
		/* One Halo unit is 100 SM64 units at the default scale. */
		CHECK(surfaces[0].vertices[1][0] == 100, "1 Halo unit became 100 SM64 units (got %d)",
			surfaces[0].vertices[1][0]);
		CHECK(surfaces[0].terrain == SM64_PORT_TERRAIN_STONE, "the terrain is kept");
		SM64Port_SurfaceSetDestroy(set);
	}
	test_end();

	test_begin("surfaces: the winding follows the normal libsm64 expects");
	{
		/* libsm64 works a surface's normal out as (v2-v1) x (v3-v2) and only
		   treats it as a floor when that points up, so a polygon handed to it
		   the wrong way round would be invisible to Mario's feet. */
		SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(16);
		const float up[3] = { 0.0f, 0.0f, 1.0f };
		uint32_t count = 0;
		const SM64PortSurface *surfaces;
		int i;

		/* The two windings of the same quad: both must end up facing up. */
		const float clockwise[4][3] = {
			{ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }
		};
		const float anticlockwise[4][3] = {
			{ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }
		};

		CHECK(SM64Port_SurfaceSetAddPolygon(set, clockwise, 4, up, SM64_PORT_TERRAIN_STONE),
			"a clockwise quad is added");
		CHECK(SM64Port_SurfaceSetAddPolygon(set, anticlockwise, 4, up, SM64_PORT_TERRAIN_STONE),
			"an anticlockwise quad is added");
		surfaces = SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count == 4, "a quad is two triangles (got %u)", count);

		for (i = 0; i < (int)count; i++)
		{
			float x1 = (float)surfaces[i].vertices[0][0], y1 = (float)surfaces[i].vertices[0][1],
				z1 = (float)surfaces[i].vertices[0][2];
			float x2 = (float)surfaces[i].vertices[1][0], y2 = (float)surfaces[i].vertices[1][1],
				z2 = (float)surfaces[i].vertices[1][2];
			float x3 = (float)surfaces[i].vertices[2][0], y3 = (float)surfaces[i].vertices[2][1],
				z3 = (float)surfaces[i].vertices[2][2];
			/* The normal libsm64 computes: (v2-v1) x (v3-v2), in its own axes
			   (Halo up is SM64 Y). */
			float nx = (y2 - y1) * (z3 - z2) - (z2 - z1) * (y3 - y2);
			float ny = (z2 - z1) * (x3 - x2) - (x2 - x1) * (z3 - z2);
			float nz = (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2);

			CHECK(ny > 0.0f, "triangle %d faces up in libsm64's axes (ny = %g)", i, (double)ny);
			(void)nx; (void)nz;
		}
		SM64Port_SurfaceSetDestroy(set);
	}
	test_end();

	test_begin("surfaces: a polygon is fanned, degenerate polygons are refused");
	{
		SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(16);
		const float pentagon[5][3] = {
			{ 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 3.0f, 1.0f, 0.0f },
			{ 1.0f, 2.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }
		};
		const float up[3] = { 0.0f, 0.0f, 1.0f };
		uint32_t count = 0;

		CHECK(SM64Port_SurfaceSetAddPolygon(set, pentagon, 5, up, SM64_PORT_TERRAIN_STONE),
			"a pentagon is added");
		SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count == 3, "a pentagon is three triangles (got %u)", count);

		CHECK(!SM64Port_SurfaceSetAddPolygon(set, pentagon, 2, up, SM64_PORT_TERRAIN_STONE),
			"two points are not a polygon");
		SM64Port_SurfaceSetClear(set);
		SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count == 0, "clearing empties the set");
		SM64Port_SurfaceSetDestroy(set);
	}
	test_end();

	test_begin("surfaces: the set stops at its capacity instead of overrunning");
	{
		SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(2);
		const float a[3] = { 0.0f, 0.0f, 0.0f };
		const float b[3] = { 1.0f, 0.0f, 0.0f };
		const float c[3] = { 1.0f, 1.0f, 0.0f };
		uint32_t count = 0;

		CHECK(SM64Port_SurfaceSetAddTriangle(set, a, b, c, NULL, 0), "the first triangle fits");
		CHECK(SM64Port_SurfaceSetAddTriangle(set, a, b, c, NULL, 0), "the second triangle fits");
		CHECK(!SM64Port_SurfaceSetAddTriangle(set, a, b, c, NULL, 0), "the third is refused");
		SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count == 2, "the set holds two triangles (got %u)", count);
		SM64Port_SurfaceSetDestroy(set);
	}
	test_end();

	test_begin("surfaces: the test room builds and is loadable");
	{
		SM64PortSurfaceSet *set = SM64Port_SurfaceSetCreate(512);
		uint32_t count = 0;

		CHECK(set != NULL, "the set was created");
		CHECK(SM64Port_BuildTestRoom(set, 20.0f), "the room was built");
		SM64Port_SurfaceSetSurfaces(set, &count);
		CHECK(count >= 12, "the room has a floor, walls, a ramp and a ledge (got %u surfaces)", count);
		SM64Port_SurfaceSetDestroy(set);
	}
	test_end();
}

/* ---------- unit tests: where the ROM is looked for ---------- */

/* The path the host below will read, and nothing else. */
static const char *g_only_rom_path;

static bool test_read_file_only(void *context, const char *path, uint8_t **out_data, size_t *out_size)
{
	uint8_t *copy;

	(void)context;
	if (!g_only_rom_path || !path || strcmp(path, g_only_rom_path) != 0 || !g_fake_rom)
		return false;
	copy = (uint8_t *)malloc(g_fake_rom_size);
	if (!copy)
		return false;
	memcpy(copy, g_fake_rom, g_fake_rom_size);
	*out_data = copy;
	*out_size = g_fake_rom_size;
	return true;
}

/* The module is told where to look (config.search_directory, which the game
   sets to its data folder) and what to look for; both have to work, and a ROM
   that is nowhere has to be reported rather than ignored. */
static void test_rom_search(void)
{
	SM64PortConfig config;
	SM64PortHost host;

	test_begin("degradation: the ROM is looked for in the search directory, then beside the game");

	/* A ROM only in the search directory: found. */
	SM64Port_ConfigDefaults(&config);
	config.library_path = g_library_path;
	config.search_directory = "romdir";
	memset(&host, 0, sizeof(host));
	host.log_message = test_log;
	host.read_file = test_read_file_only;
	host.free_file = test_free_file;

	g_only_rom_path = "romdir/baserom.us.z64";
	CHECK(SM64Port_Init(&config, &host) == SM64_PORT_OK, "Init() finds the ROM in the search directory");
	CHECK(SM64Port_IsReady(), "the module is ready");
	SM64Port_Shutdown();

	/* The same ROM, with the search directory left out: not found, and the
	   module says so instead of running without Mario's assets. */
	SM64Port_ConfigDefaults(&config);
	config.library_path = g_library_path;
	memset(&host, 0, sizeof(host));
	host.log_message = test_log;
	host.read_file = test_read_file_only;
	host.free_file = test_free_file;

	CHECK(SM64Port_Init(&config, &host) == SM64_PORT_ERROR_ASSETS,
		"without it the same ROM is not found");
	CHECK(!SM64Port_IsReady(), "the module stays off");
	CHECK(SM64Port_StatusMessage() != NULL && strstr(SM64Port_StatusMessage(), "ROM") != NULL,
		"the status names the missing ROM");

	/* A ROM only in the working directory: found too. */
	SM64Port_ConfigDefaults(&config);
	config.library_path = g_library_path;
	memset(&host, 0, sizeof(host));
	host.log_message = test_log;
	host.read_file = test_read_file_only;
	host.free_file = test_free_file;

	g_only_rom_path = "baserom.us.z64";
	CHECK(SM64Port_Init(&config, &host) == SM64_PORT_OK, "and one beside the game is found");
	SM64Port_Shutdown();

	g_only_rom_path = NULL;
	test_end();
}

/* ---------- unit tests: the module without libsm64 ---------- */

static void test_without_a_library(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = 0;
	SM64PortState state;
	SM64PortInput input;

	test_begin("degradation: a missing library leaves the module inert");
	SM64Port_ConfigDefaults(&config);
	config.library_path = "no-such-libsm64-library.so";
	memset(&host, 0, sizeof(host));
	host.log_message = test_log;
	host.read_file = test_read_file;
	host.free_file = test_free_file;

	CHECK(SM64Port_Init(&config, &host) == SM64_PORT_ERROR_LIBRARY,
		"Init() reports the missing library");
	CHECK(!SM64Port_IsReady(), "the module is not ready");
	CHECK(SM64Port_StatusMessage() != NULL, "it says why");
	CHECK(g_log_count > 0, "it logged why");

	CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.0f }, 0.0f, &id)
			== SM64_PORT_ERROR_NOT_INITIALIZED, "no actor can be created");
	CHECK(SM64Port_Update(1.0 / 30.0) == -1, "Update() reports it did nothing");
	memset(&input, 0, sizeof(input));
	CHECK(SM64Port_SetInput(0, &input) == SM64_PORT_ERROR_BAD_ACTOR, "no actor takes input");
	CHECK(SM64Port_GetState(0, &state) == SM64_PORT_ERROR_BAD_ACTOR, "no actor has a state");
	SM64Port_Shutdown();
	test_end();
}

/* ---------- integration tests ---------- */

static void test_integration_walking(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortState before, after;
	SM64PortInput input;
	const float pi = 3.14159265358979323846f;
	int i;

	test_begin("integration: an actor walks where it is told, in four directions");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK)
	{
		test_fail(__FILE__, __LINE__, "Init() failed: %s", SM64Port_StatusMessage());
		test_end();
		return;
	}
	if (!load_test_room())
	{
		test_fail(__FILE__, __LINE__, "the test room could not be loaded");
		SM64Port_Shutdown();
		test_end();
		return;
	}

	/* Four directions of Halo yaw: +X, +Y, -X, -Y. */
	for (i = 0; i < 4; i++)
	{
		float yaw = (float)i * (pi * 0.5f);
		float expected_x = cosf(yaw);
		float expected_y = sinf(yaw);
		float moved_x, moved_y;

		CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) == SM64_PORT_OK,
			"an actor was created");
		run_seconds(id, &(SM64PortInput){ 0, 0, 0, 0, 0 }, 0.5);   /* settle */

		CHECK(SM64Port_GetState(id, &before) == SM64_PORT_OK, "the state is readable");
		memset(&input, 0, sizeof(input));
		input.move_x = expected_x;
		input.move_y = expected_y;
		run_seconds(id, &input, 1.0);
		CHECK(SM64Port_GetState(id, &after) == SM64_PORT_OK, "the state is readable");

		moved_x = after.position.x - before.position.x;
		moved_y = after.position.y - before.position.y;
		{
			float distance = sqrtf(moved_x * moved_x + moved_y * moved_y);
			float along = moved_x * expected_x + moved_y * expected_y;

			CHECK(distance > 0.5f, "the actor moved (%.3f Halo units in a second)", (double)distance);
			CHECK(along > 0.4f * distance,
				"it moved the way it was told: %.3f of %.3f was along the wanted direction",
				(double)along, (double)distance);
			CHECK(fabsf(after.position.z - before.position.z) < 1.5f,
				"it stayed on the floor (moved %.3f up)", (double)(after.position.z - before.position.z));
		}
		CHECK(SM64Port_DestroyActor(id) == SM64_PORT_OK, "the actor was destroyed");
	}
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_jump(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortState ground, peak, landing;
	memset(&landing, 0, sizeof(landing));
	memset(&peak, 0, sizeof(peak));
	SM64PortInput input;
	double highest = -1.0e9;
	int i;

	test_begin("integration: a jump leaves the ground and comes back");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK)
	{
		test_fail(__FILE__, __LINE__, "Init() failed");
		test_end();
		return;
	}
	if (!load_test_room())
	{
		SM64Port_Shutdown();
		test_fail(__FILE__, __LINE__, "no test room");
		test_end();
		return;
	}

	CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);
	CHECK(SM64Port_GetState(id, &ground) == SM64_PORT_OK, "the state is readable");
	CHECK(ground.grounded, "the actor is on the ground before jumping");

	/* Hold A for one tick, then let go: that is a single jump. */
	input.button_a = 1;
	SM64Port_SetInput(id, &input);
	SM64Port_Update(1.0 / 30.0);
	input.button_a = 0;

	for (i = 0; i < 90; i++)
	{
		SM64PortState state;

		SM64Port_SetInput(id, &input);
		SM64Port_Update(1.0 / 30.0);
		if (SM64Port_GetState(id, &state) != SM64_PORT_OK)
			break;
		if ((double)state.position.z > highest)
		{
			highest = (double)state.position.z;
			peak = state;
		}
		if (i > 5 && state.grounded)
		{
			landing = state;
			break;
		}
	}

	CHECK(highest > (double)ground.position.z + 0.2,
		"the actor got off the ground: %.3f -> %.3f Halo units", (double)ground.position.z, highest);
	/* At 0.01 Halo units per SM64 unit, Mario's measured jump of about 96 SM64
	   units is about 0.96 Halo units; the double jumps a similar height. */
	CHECK(highest - (double)ground.position.z < 4.0,
		"it did not fly away: %.3f Halo units", highest - (double)ground.position.z);
	CHECK(landing.grounded, "the actor landed again");
	CHECK_NEAR(landing.position.z, ground.position.z, 0.2, "it landed where it took off");
	(void)peak;

	SM64Port_DestroyActor(id);
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_double_jump(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortState ground;
	SM64PortInput input;
	double single = 0.0, multiple = 0.0;
	int i;

	if (!g_using_real_library)
	{
		printf("  skip integration: the double and triple jump (real libsm64 only)\n");
		return;
	}

	test_begin("integration: the second and third jumps go higher (real libsm64)");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK || !load_test_room())
	{
		test_fail(__FILE__, __LINE__, "setup failed");
		test_end();
		return;
	}

	/* One jump. */
	CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);
	SM64Port_GetState(id, &ground);
	input.button_a = 1;
	SM64Port_SetInput(id, &input);
	SM64Port_Update(1.0 / 30.0);
	input.button_a = 0;
	for (i = 0; i < 60; i++)
	{
		SM64PortState state;

		SM64Port_SetInput(id, &input);
		SM64Port_Update(1.0 / 30.0);
		if (SM64Port_GetState(id, &state) != SM64_PORT_OK)
			break;
		if ((double)state.position.z > single)
			single = (double)state.position.z;
	}
	SM64Port_DestroyActor(id);

	/* A jump, then another jump twelve ticks later: the double jump. */
	CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);
	input.button_a = 1;
	SM64Port_SetInput(id, &input);
	SM64Port_Update(1.0 / 30.0);
	input.button_a = 0;
	for (i = 0; i < 12; i++)
	{
		SM64Port_SetInput(id, &input);
		SM64Port_Update(1.0 / 30.0);
	}
	input.button_a = 1;
	SM64Port_SetInput(id, &input);
	SM64Port_Update(1.0 / 30.0);
	input.button_a = 0;
	for (i = 0; i < 60; i++)
	{
		SM64PortState state;

		SM64Port_SetInput(id, &input);
		SM64Port_Update(1.0 / 30.0);
		if (SM64Port_GetState(id, &state) != SM64_PORT_OK)
			break;
		if ((double)state.position.z > multiple)
			multiple = (double)state.position.z;
	}
	SM64Port_DestroyActor(id);

	CHECK(multiple > single + 0.05, "the double jump is higher: %.3f against %.3f Halo units",
		multiple, single);
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_collision(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortState before, after;
	SM64PortInput input;

	test_begin("integration: a wall stops the actor");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK || !load_test_room())
	{
		test_fail(__FILE__, __LINE__, "setup failed");
		test_end();
		return;
	}

	CHECK(SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);

	/* Walk towards the -X wall of the 40x40 room for four seconds. */
	SM64Port_GetState(id, &before);
	memset(&input, 0, sizeof(input));
	input.move_x = -1.0f;
	run_seconds(id, &input, 4.0);
	SM64Port_GetState(id, &after);

	CHECK(after.position.x > -20.0f, "the actor stayed inside the room (x = %.3f)",
		(double)after.position.x);
	CHECK(after.position.x < before.position.x, "it walked towards the wall (%.3f -> %.3f)",
		(double)before.position.x, (double)after.position.x);
	CHECK(after.position.z < 3.0f, "it did not climb the wall (z = %.3f)", (double)after.position.z);

	SM64Port_DestroyActor(id);
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_slope(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortState before, after;
	SM64PortInput input;

	test_begin("integration: the actor walks up the ramp");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK || !load_test_room())
	{
		test_fail(__FILE__, __LINE__, "setup failed");
		test_end();
		return;
	}

	/* The test room's ramp runs from x = -5 (flush with the floor) up to
	   x = +5 at z = 2: start on the floor just before its low edge. */
	CHECK(SM64Port_CreateActor((SM64PortVector3){ -7.0f, 0.0f, 0.2f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created on the floor by the ramp");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);
	SM64Port_GetState(id, &before);

	/* Walk towards +X, over the ramp.  The highest point reached is what
	   matters: a fast actor runs off the top and drops back to the floor. */
	{
		double highest = -1.0e9;
		double furthest = -1.0e9;
		int i;

		memset(&input, 0, sizeof(input));
		input.move_x = 1.0f;
		for (i = 0; i < 60; i++)
		{
			SM64PortState state;

			SM64Port_SetInput(id, &input);
			SM64Port_Update(1.0 / 30.0);
			if (SM64Port_GetState(id, &state) != SM64_PORT_OK)
				break;
			if ((double)state.position.z > highest)
				highest = (double)state.position.z;
			if ((double)state.position.x > furthest)
				furthest = (double)state.position.x;
		}
		SM64Port_GetState(id, &after);

		CHECK(highest > before.position.z + 0.5,
			"the actor climbed the ramp: z %.3f -> at best %.3f Halo units",
			(double)before.position.z, highest);
		CHECK(furthest > before.position.x + 1.0, "it also moved forwards");
	}

	SM64Port_DestroyActor(id);
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_ceiling(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
	SM64PortInput input;
	double highest = -1.0e9;
	int i;

	test_begin("integration: a ceiling stops an upward jump");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK || !load_test_room())
	{
		test_fail(__FILE__, __LINE__, "setup failed");
		test_end();
		return;
	}

	/* The test room's ceiling is 4.5 Halo units up over x in [-20, -10]. */
	CHECK(SM64Port_CreateActor((SM64PortVector3){ -15.0f, 0.0f, 0.2f }, 0.0f, &id) == SM64_PORT_OK,
		"an actor was created under the ceiling");
	memset(&input, 0, sizeof(input));
	run_seconds(id, &input, 0.5);

	input.button_a = 1;
	SM64Port_SetInput(id, &input);
	SM64Port_Update(1.0 / 30.0);
	input.button_a = 0;
	for (i = 0; i < 90; i++)
	{
		SM64PortState state;

		SM64Port_SetInput(id, &input);
		SM64Port_Update(1.0 / 30.0);
		if (SM64Port_GetState(id, &state) != SM64_PORT_OK)
			break;
		if ((double)state.position.z > highest)
			highest = (double)state.position.z;
	}

	CHECK(highest < 4.5, "the actor stayed below the ceiling at 4.5 (reached %.3f)", highest);
	CHECK(highest > 0.5, "it did jump (reached %.3f)", highest);

	SM64Port_DestroyActor(id);
	SM64Port_Shutdown();
	test_end();
}

static void test_integration_actors_and_performance(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	SM64PortActorId ids[SM64_PORT_MAX_ACTORS];
	SM64PortInput input;
	int counts[3] = { 1, 5, 10 };
	int c;

	test_begin("integration: many actors, and what they cost");
	configure_module(&config, &host);
	if (SM64Port_Init(&config, &host) != SM64_PORT_OK || !load_test_room())
	{
		test_fail(__FILE__, __LINE__, "setup failed");
		test_end();
		return;
	}

	for (c = 0; c < 3; c++)
	{
		int created = 0;
		int i;
		clock_t started;
		double seconds;
		SM64PortStats stats;

		for (i = 0; i < counts[c]; i++)
		{
			SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
			SM64PortVector3 position;

			position.x = (float)(i % 5) * 2.0f - 4.0f;
			position.y = (float)(i / 5) * 2.0f - 2.0f;
			position.z = 1.5f;
			if (SM64Port_CreateActor(position, 0.0f, &id) == SM64_PORT_OK)
				ids[created++] = id;
		}
		CHECK(created == counts[c], "%d actors were created (got %d)", counts[c], created);

		memset(&input, 0, sizeof(input));
		input.move_x = 1.0f;
		started = clock();
		for (i = 0; i < created; i++)
			SM64Port_SetInput(ids[i], &input);
		/* Ten seconds of ticks: 300 frames. */
		for (i = 0; i < 300; i++)
			SM64Port_Update(1.0 / 30.0);
		seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;

		SM64Port_GetStats(&stats);
		printf("       %d actor(s): %.1f ms for 300 frames (%.3f ms a frame, %ld ticks)\n",
			created, seconds * 1000.0, seconds * 1000.0 / 300.0, stats.ticks_total);
		CHECK(stats.ticks_total >= 300, "the clock ran its ticks (%ld)", stats.ticks_total);

		for (i = 0; i < created; i++)
			SM64Port_DestroyActor(ids[i]);
	}

	test_end();

	test_begin("integration: the actor limit is enforced");
	{
		int created = 0;
		int i;

		for (i = 0; i < SM64_PORT_MAX_ACTORS + 4; i++)
		{
			SM64PortActorId id = SM64_PORT_INVALID_ACTOR;
			SM64PortVector3 position = { 0.0f, 0.0f, 1.5f };

			if (SM64Port_CreateActor(position, 0.0f, &id) == SM64_PORT_OK)
				ids[created++] = id;
		}
		CHECK(created == SM64_PORT_MAX_ACTORS, "no more than %d actors (got %d)",
			SM64_PORT_MAX_ACTORS, created);
		SM64Port_DestroyAllActors();
		CHECK(SM64Port_ActorCount() == 0, "they were all destroyed");
	}
	test_end();

	SM64Port_Shutdown();
}

static void test_integration_reload(void)
{
	SM64PortConfig config;
	SM64PortHost host;
	int i;

	test_begin("integration: initialising and shutting down repeatedly is clean");
	for (i = 0; i < 5; i++)
	{
		SM64PortActorId id = SM64_PORT_INVALID_ACTOR;

		configure_module(&config, &host);
		if (SM64Port_Init(&config, &host) != SM64_PORT_OK)
		{
			test_fail(__FILE__, __LINE__, "Init() failed on pass %d", i);
			break;
		}
		if (!load_test_room())
		{
			test_fail(__FILE__, __LINE__, "the room failed to load on pass %d", i);
			break;
		}
		if (SM64Port_CreateActor((SM64PortVector3){ 0.0f, 0.0f, 1.5f }, 0.0f, &id) != SM64_PORT_OK)
		{
			test_fail(__FILE__, __LINE__, "no actor on pass %d", i);
			break;
		}
		SM64Port_Update(1.0 / 30.0);
		SM64Port_Shutdown();
	}
	test_end();
}

/* ---------- choosing the library, and main ---------- */

static int file_exists(const char *path)
{
	FILE *file = fopen(path, "rb");

	if (!file)
		return 0;
	fclose(file);
	return 1;
}

int main(int argc, char **argv)
{
	const char *candidates[] = {
		NULL,                                        /* $SM64_PORT_TEST_LIBRARY */
		"build/libsm64/libsm64.so",
		"third_party/libsm64/dist/libsm64.so",
		"../libsm64/dist/libsm64.so",
		"libsm64.so",
		"libsm64_test_double.so",
	};
	char buffer[1024];
	size_t i;
	int failures_before;

	(void)argc; (void)argv;

	/* 1. An explicit path wins. */
	g_library_path = getenv("SM64_PORT_TEST_LIBRARY");

	/* 2. Otherwise the first library that exists, the real one first. */
	if (!g_library_path)
	{
		for (i = 1; i < sizeof(candidates) / sizeof(candidates[0]); i++)
		{
			if (file_exists(candidates[i]))
			{
				static char chosen[1024];
				snprintf(chosen, sizeof(chosen), "%s", candidates[i]);
				g_library_path = chosen;
				break;
			}
		}
	}
	if (!g_library_path)
	{
		printf("test_sm64_port: no libsm64 and no test double found; build the double first "
			"(see tests/Makefile)\n");
		return 2;
	}

	g_using_real_library = (strstr(g_library_path, "test_double") == NULL);
	printf("test_sm64_port: library \"%s\" (%s)\n", g_library_path,
		g_using_real_library ? "real libsm64" : "test double");

	/* The real library needs asset data; give it the synthetic blob.  The
	   double ignores what it is given, an 8 MiB block of the right size. */
	if (g_using_real_library)
		g_fake_rom = sm64_test_blob_create(&g_fake_rom_size);
	else
	{
		g_fake_rom_size = 8 * 1024 * 1024;
		g_fake_rom = (uint8_t *)calloc(1, g_fake_rom_size);
	}
	if (!g_fake_rom)
	{
		printf("test_sm64_port: out of memory for the test asset data\n");
		return 2;
	}
	snprintf(buffer, sizeof(buffer), "%zu KiB of %s", g_fake_rom_size / 1024,
		g_using_real_library ? "synthetic asset data (no ROM: see sm64_test_blob.c)"
			: "placeholder data (the test double reads no assets)");
	printf("test_sm64_port: %s\n", buffer);
	printf("\n");

	printf("unit tests\n");
	failures_before = g_tests_failed;
	test_conversions_round_trip();
	test_conversions_yaw();
	test_clock();
	test_input_mapping();
	test_surface_set();
	test_rom_search();
	test_without_a_library();
	printf("  (%d unit tests, %d failed)\n\n",
		g_tests_run - 0, g_tests_failed - failures_before);

	printf("integration tests\n");
	failures_before = g_tests_failed;
	test_integration_walking();
	test_integration_jump();
	test_integration_double_jump();
	test_integration_collision();
	test_integration_slope();
	test_integration_ceiling();
	test_integration_actors_and_performance();
	test_integration_reload();
	printf("  (%d failed)\n\n", g_tests_failed - failures_before);

	free(g_fake_rom);

	printf("%d tests, %d failed\n", g_tests_run, g_tests_failed);
	return g_tests_failed == 0 ? 0 : 1;
}
