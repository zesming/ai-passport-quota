#pragma once

/* Firmware keeps these symbols file-local. Host tests build with -DQUOTA_HOST_TEST and link the
 * whole source file, so the same symbols become visible to the harness. */
#ifdef QUOTA_HOST_TEST
#define QUOTA_TESTABLE
#else
#define QUOTA_TESTABLE static
#endif
