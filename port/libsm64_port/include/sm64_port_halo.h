/*
SM64_PORT_HALO.H

The game side of the libsm64 module: what source/main/main.c,
source/main/console.c and the platform layer call.  Everything here is
compiled only when the build is configured with libsm64 enabled
(configure.py --enable-libsm64), and every function is safe to call when the
module could not start: they do nothing and say so once.

The portable half of the module lives in include/sm64_port.h; this header is
the small surface the game uses.

CC0 1.0 Universal, like the rest of the port.
*/

#ifndef SM64_PORT_HALO_H
#define SM64_PORT_HALO_H

#include "cseries.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the module's settings and starts it.  Called once, from main_loop()
   before its first frame; calling it more than once does nothing. */
void sm64_port_halo_initialize(void);

/* Stops the module and frees everything it owns.  Called when the game
   leaves a map, so that no actor survives into the next one. */
void sm64_port_halo_dispose(void);

/* One frame: gathers the player's input, runs libsm64's fixed steps, writes
   the result back onto the player's unit when "sm64.possess_player" is set,
   and queues the debug drawing.  delta_time_seconds is the same value the
   game hands to game_time_update(). */
void sm64_port_halo_update(real delta_time_seconds);

/* Offers a line typed into the console (source/main/console.c) to the
   module.  Returns TRUE when the module recognised it as one of its own
   "sm64_..." commands, whether or not it succeeded. */
boolean sm64_port_halo_console_command(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* SM64_PORT_HALO_H */
