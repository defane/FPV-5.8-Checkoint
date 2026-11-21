/* C6 side of the UART link to its local C5 (link_proto): receive the local
 * gate's passes, stats (with its health) and detector config, play the
 * centralized coin sound on a pass, and forward the admin's calibrate/reboot
 * commands to it. */
#pragma once
#include "link_proto.h"
void c6_link_start(void);
void c6_link_send_command(lp_cmd_t cmd);   /* LP_MSG_COMMAND to the local C5 */
/* Gate 0 takes the fleet's re-arm drop (c6_store): at the next CONFIG_STATE, asked for now, a
 * CONFIG_SET of the C5's echoed settings with the fleet's drop, if its own differs. Also done
 * at boot and when the C5 says HELLO (it rebooted). */
void c6_link_sync_rearm(void);
