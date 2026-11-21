/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_ACCOUNT_IDENTITY_H
#define POKOPIA_HORIZON_ACCOUNT_IDENTITY_H

#include <stdint.h>

/* Default local user from the pinned Switch2Recomp account profile, in guest
 * memory order. Both account and preselected launch storage use these words. */
#define HZN_OFFLINE_UID_WORD0 UINT64_C(0x0000000000000001)
#define HZN_OFFLINE_UID_WORD1 UINT64_C(0x6e78727475736572)

#endif
