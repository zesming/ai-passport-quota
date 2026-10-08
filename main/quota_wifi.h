#pragma once

#include <stdbool.h>

/* Station driver shared by the portable controller and the network task. Both run on the single
 * network task, so these calls are never concurrent. */

/* Bring up the stack on first use, then start (or keep) the radio. Failure unwinds the setup. */
bool quota_wifi_start(void);
/* Stop the radio for sleep. Returns true once it is stopped, including when it already was. */
bool quota_wifi_stop(void);
bool quota_wifi_started(void);
