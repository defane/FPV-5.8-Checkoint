/* C6 side of the UART link to its local C5 (link_proto): receive the local
 * gate's passes, stats (with its health) and detector config, play the
 * centralized coin sound on a pass, and forward the admin's calibrate/reboot
 * commands to it. */
#pragma once
#include "link_proto.h"
void c6_link_start(void);
void c6_link_send_command(lp_cmd_t cmd);   /* LP_MSG_COMMAND to the local C5 */
