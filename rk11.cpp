// rk11.cpp - RK11 / RK05 disk controller (vpdp1140 m3 RK addition).
//
// Implements the read/write/seek/reset operations that sam11's RK0 boot
// ROM (bootrom_rk0) and RT-11 / V6 Unix use. Backed by our disk_read /
// disk_write block I/O against the SD-card image files; parallels the
// fresh RL11 implementation in rl11.cpp - no SdFat dependency.
//
// Register addresses (from pdp1140.h):
//   0777400  RKDS   Drive Status
//   0777402  RKER   Error Register
//   0777404  RKCS   Control / Status
//   0777406  RKWC   Word Count (2's complement, ticks toward 0)
//   0777410  RKBA   Bus Address  (low 16 bits, high 2 in RKCS bits 5:4)
//   0777412  RKDA   Disk Address (drive/cyl/surf/sec packed)
//   0777414  RKMR   Maintenance Register
//   0777416  RKDB   Data Buffer
//
// RKDA bit layout (DEC RK11 manual):
//   bits 15:13 = drive number (3 bits, 0..7)
//   bits 12:5  = cylinder    (8 bits, 0..202 for RK05)
//   bit  4     = surface
//   bits 3:0   = sector      (4 bits, 0..11)
//
// RKCS function codes (bits 3:1 + GO=bit 0):
//   0 = control reset, 1 = write,        2 = read,        3 = write check,
//   4 = seek,          5 = read check,   6 = drive reset, 7 = write lock
//
// Geometry:
//   sectors per track = 12
//   surfaces per cyl  = 2
//   bytes per sector  = 512
//   cylinders         = 203  ->  RK05 = 2,494,464 bytes
// (RT-11 v5 distribution image is 4800 sectors = 2,457,600 bytes;
//  ~200 cylinders worth, which fits comfortably below the 203 max.)

#include "rk11.h"
#include "pdp1140.h"
#include "sam11_platform.h"
#include "sam11.h"
#include "dd11.h"
#include "kd11.h"   // procNS::interrupt for RK11 done-IRQ
#include "disk.h"

#include <Arduino.h>

#define procNS kd11

namespace rk11 {

static const uint32_t SECS_PER_TRACK = 12;
static const uint32_t SURFACES       = 2;
static const uint32_t BYTES_PER_SEC  = 512;
static const uint32_t MAX_CYLINDER   = 202;

bool attached_drives[NUM_RK_DRIVES] = { false, false, false, false };

// Programmable registers (all 16-bit on the bus).
static uint16_t RKDS = 0;
static uint16_t RKER = 0;
static uint16_t RKCS = 0;
static uint16_t RKWC = 0;
static uint16_t RKBA = 0;
static uint16_t RKDA = 0;

// Deferred "operation complete" IRQ countdown. We can't fire the IRQ
// synchronously from execute() - that delivers it BEFORE the guest's
// WAIT executes (RT-11 issues `MOV cmd,RKCS; WAIT` and our cpu_run drains
// the IRQ between those two instructions). Real disk hardware takes
// milliseconds, so the WAIT runs first. We mimic that by counting down
// a few hundred host instructions in rk11::tick() before raising INTRK.
static int      irq_pending  = 0;       // > 0 = ticks remaining
static uint16_t irq_cs_snap  = 0;       // RKCS snapshot when op finished

void reset()
{
    // RKDS: bit 11 = RK05 ident, bit 7 = DRDY (drive ready), bit 6 = SOK.
    // We claim "drive present and good" so the boot ROM finds unit 0.
    RKDS = (1 << 11) | (1 << 7) | (1 << 6);
    RKER = 0;
    RKCS = 1 << 7;   // CRDY (controller ready)
    RKWC = 0;
    RKBA = 0;
    RKDA = 0;
    for (int i = 0; i < NUM_RK_DRIVES; i++)
        attached_drives[i] = disk_is_mounted(i);
}

static uint32_t da_to_offset(uint16_t da)
{
    uint32_t cyl  = (da >> 5) & 0xFF;
    uint32_t surf = (da >> 4) & 1;
    uint32_t sec  = da & 0xF;
    return ((cyl * SURFACES + surf) * SECS_PER_TRACK + sec) * BYTES_PER_SEC;
}

// 18-bit bus address: low 16 in RKBA, high 2 in RKCS bits 5:4.
static uint32_t bus_addr()
{
    return ((uint32_t)RKBA) | (((uint32_t)(RKCS & 0x0030)) << 12);
}

static void set_bus_addr(uint32_t ba)
{
    RKBA = ba & 0xFFFF;
    RKCS = (RKCS & ~0x0030) | (uint16_t)((ba >> 12) & 0x0030);
}

static const char* func_name(int f)
{
    switch (f) {
    case 0: return "CRESET";
    case 1: return "WRITE";
    case 2: return "READ";
    case 3: return "WRCHK";
    case 4: return "SEEK";
    case 5: return "RDCHK";
    case 6: return "DRESET";
    case 7: return "WRLOCK";
    }
    return "?";
}

// Execute the function currently encoded in RKCS bits 3:1. Synchronous -
// the whole transfer happens before we return and re-set CRDY. Real
// hardware would be interruptible; for our purposes instant is fine.
static void execute()
{
    int func = (RKCS >> 1) & 7;
    int drv  = (RKDA >> 13) & 7;

    RKER = 0;

    // Log the first ~40 RK commands of each boot so the bootrom + early
    // disk activity is visible, then go quiet.
    static int dbg_left = 40;
    if (dbg_left > 0) {
        Serial.printf("[vpdp1140] RK11 %s drv=%d  DA=%06o WC=%06o BA=%06o CS=%06o\r\n",
                      func_name(func), drv, RKDA, RKWC, RKBA, RKCS);
        dbg_left--;
        if (dbg_left == 0)
            Serial.printf("[vpdp1140] RK11 (silencing further command logs)\r\n");
    }

    switch (func) {
    case 0:  // CONTROL RESET
        reset();
        return;

    case 6:  // DRIVE RESET
        RKER = 0;
        break;

    case 4:  // SEEK - we have no head motion to track, just acknowledge
        break;

    case 3:  // WRITE CHECK  - claim match
    case 5:  // READ  CHECK  - claim match
    case 7:  // WRITE LOCK   - ignore
        break;

    case 1:  // WRITE
    case 2: {// READ
        if (drv >= (int)NUM_RK_DRIVES || !attached_drives[drv]) {
            Serial.printf("[vpdp1140] RK11 %s drv=%d -> drive not attached\r\n",
                          func_name(func), drv);
            RKER |= RKNXD;
            RKCS |= 0x8000;       // composite error
            break;
        }
        uint32_t cyl = (RKDA >> 5) & 0xFF;
        if (cyl > MAX_CYLINDER) {
            RKER |= RKNXC;
            RKCS |= 0x8000;
            break;
        }
        uint32_t sec = RKDA & 0xF;
        if (sec >= SECS_PER_TRACK) {
            RKER |= RKNXS;
            RKCS |= 0x8000;
            break;
        }

        bool writing = (func == 1);
        uint32_t off = da_to_offset(RKDA);
        uint32_t ba  = bus_addr();
        uint32_t words_left = (uint32_t)((~RKWC + 1) & 0xFFFF);
        if (words_left == 0) break;

        uint8_t scratch[512];
        bool failed = false;
        while (words_left) {
            uint32_t chunk_words = words_left > 256 ? 256 : words_left;
            uint32_t chunk_bytes = chunk_words * 2;

            if (writing) {
                for (uint32_t i = 0; i < chunk_words; i++) {
                    uint16_t v = dd11::read16(ba + i * 2);
                    scratch[i * 2]     = (uint8_t)(v & 0xFF);
                    scratch[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
                }
                if (disk_write(drv, off, scratch, chunk_bytes) < 0) {
                    RKER |= RKOVR;
                    RKCS |= 0x8000;
                    failed = true;
                    break;
                }
            } else {
                if (disk_read(drv, off, scratch, chunk_bytes) < 0) {
                    RKER |= RKOVR;
                    RKCS |= 0x8000;
                    failed = true;
                    break;
                }
                for (uint32_t i = 0; i < chunk_words; i++) {
                    uint16_t v = (uint16_t)scratch[i * 2]
                               | ((uint16_t)scratch[i * 2 + 1] << 8);
                    dd11::write16(ba + i * 2, v);
                }
            }

            ba         += chunk_bytes;
            off        += chunk_bytes;
            words_left -= chunk_words;
            RKWC       = (uint16_t)(RKWC + chunk_words);
        }
        set_bus_addr(ba);

        // Advance RKDA's sector/surface/cyl by the number of sectors
        // moved so OS sector-by-sector iteration sees a coherent
        // position. Only meaningful when the transfer actually
        // succeeded; on error we leave RKDA pointing at the failing
        // sector for the OS to inspect.
        if (!failed) {
            uint32_t total_secs = (off - da_to_offset(RKDA)) / BYTES_PER_SEC;
            uint32_t ncyl  = (RKDA >> 5) & 0xFF;
            uint32_t nsurf = (RKDA >> 4) & 1;
            uint32_t nsec  = RKDA & 0xF;
            nsec += total_secs;
            while (nsec >= SECS_PER_TRACK) {
                nsec -= SECS_PER_TRACK;
                nsurf ^= 1;
                if (nsurf == 0) ncyl++;
            }
            RKDA = (uint16_t)(((RKDA & 0xE000))
                              | ((ncyl & 0xFF) << 5)
                              | ((nsurf & 1)    << 4)
                              | (nsec & 0xF));
        }
        break;
    }
    }

    // Op complete: controller ready (RKCS bit 7) and drive ready
    // (RKDS bit 7). The RK0 bootrom polls TSTB (R1) (= RKCS low byte
    // sign bit) waiting for CRDY before proceeding.
    RKCS |= (1 << 7);
    RKDS |= (1 << 7);

    // Fire the RK11 done-interrupt if Interrupt Enable (RKCS bit 6) is
    // set, but DEFERRED: schedule it to land in ~256 host CPU steps via
    // rk11::tick(). Synchronous firing from inside write16() raises the
    // IRQ before the guest's WAIT instruction has executed, and our
    // cpu_run dispatcher drains the IRQ immediately, leaving the WAIT
    // hanging forever (the symptom: RT-11 monitor loads but never reaches
    // its sign-on print). Real RK05 hardware takes ms to complete; we
    // approximate with a few hundred instruction-counts.
    if (RKCS & (1 << 6)) {
        irq_pending = 256;
        irq_cs_snap = RKCS;
    }
}

void tick()
{
    if (irq_pending > 0) {
        if (--irq_pending == 0) {
            // Only raise INTRK if IE is still set when the timer expires;
            // RT-11 might have cleared it between issue and now.
            if (RKCS & (1 << 6)) {
                procNS::interrupt(INTRK, 5);
            }
        }
    }
}

uint16_t read16(uint32_t a)
{
    switch (a) {
    case DEV_RK_DS:  return RKDS;
    case DEV_RK_ER:  return RKER;
    case DEV_RK_CS:  return RKCS;
    case DEV_RK_WC:  return RKWC;
    case DEV_RK_BA:  return RKBA;
    case DEV_RK_DA:  return RKDA;
    case DEV_RK_DB:  return 0;
    case DEV_RK_MR:  return 0;
    }
    return 0;
}

void write16(uint32_t a, uint16_t v)
{
    switch (a) {
    case DEV_RK_DS:  break;   // read-only
    case DEV_RK_ER:  break;   // read-only

    case DEV_RK_CS:
        // Preserve high error bits (15:12); software writes the low 12.
        // Bit 7 (CRDY) is set by hardware on op completion, so writing
        // it clears the "ready" state until we re-assert it inside
        // execute(). The GO bit (bit 0) triggers execution.
        RKCS = (RKCS & 0xF000) | (v & 0x0FFF);
        RKCS &= ~(1 << 7);
        if (v & 1) {
            execute();
        } else {
            // No GO bit: instantly re-ready (the boot ROM's later
            // CLRB (R1) just disarms the function code; we shouldn't
            // hang on the subsequent CRDY poll).
            RKCS |= (1 << 7);
        }
        break;

    case DEV_RK_WC:  RKWC = v; break;
    case DEV_RK_BA:  RKBA = v; break;
    case DEV_RK_DA:  RKDA = v; break;
    case DEV_RK_DB:  break;
    case DEV_RK_MR:  break;
    }
}

};  // namespace rk11
