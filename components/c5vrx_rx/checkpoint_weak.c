/* Weak no-op fallbacks for the checkpoint extension seam (checkpoint.h).
 *
 * console.c and the boot path call checkpoint_*; with no checkpoint component
 * linked, c5vrx_rx still builds and runs as a receiver with no pass detection
 * and no on-screen menu (pruned, see CMakeLists.txt).
 * The checkpoint component provides strong definitions of the
 * same symbols, which the linker selects over these weak ones; main.c's direct
 * call to checkpoint_start() forces that component into the link. */
#include "checkpoint.h"

__attribute__((weak)) void checkpoint_start(void) {}
__attribute__((weak)) void checkpoint_print(void) {}
__attribute__((weak)) void checkpoint_help(void) {}
__attribute__((weak)) bool checkpoint_key(char key) { (void)key; return false; }
