
// SPDX-License-Identifier: MIT

#pragma once



// Early protocol-dependent initialization; before the boot announcement log.
void bootp_early_init();
// Full protocol-dependent initialization; stage that detects devices.
void bootp_full_init();
// Reclaim all reclaimable memory.
void bootp_reclaim_mem();
