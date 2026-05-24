// rk11.cpp - m1 stub for the RK11/RK05 disk controller.
//
// Replaces sam11's rk11.cpp wholesale for m1. The full sam11 RK11 emulator
// reads/writes RK05 disk images via SdFat; we'll wire it to our own
// disk.cpp block-I/O layer in m3. Until then, this stub just satisfies
// the linker: dd11.cpp dispatches all DEV_RK_* register accesses here,
// so we provide the namespace functions with safe defaults (zeros on
// read, drops on write) and the `attached_drives[]` global.

#include "rk11.h"
#include "sam11_platform.h"
#include "sam11.h"

namespace rk11 {

bool attached_drives[NUM_RK_DRIVES] = { false, false, false, false };

void reset()
{
    for (int i = 0; i < NUM_RK_DRIVES; i++)
        attached_drives[i] = false;
}

void write16(uint32_t /*a*/, uint16_t /*v*/)
{
    // m1 stub: ignore RK11 register writes.
}

uint16_t read16(uint32_t /*a*/)
{
    // m1 stub: every RK11 register reads as zero. RKDS bit 7 (DRY) being
    // clear means "drive not ready" which is fine - guest programs that
    // probe for a drive will see "no drive attached" and skip on.
    return 0;
}

};  // namespace rk11
