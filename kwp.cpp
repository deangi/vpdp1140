// KW11-P Programmable Real-Time Clock - minimum viable emulation.
//
// CSR layout that matches what RSTS V7 (and we expect RT-11 / RSX) actually
// pokes - not quite the DEC PDP-11/70 handbook layout, which lists RATE as a
// 3-bit field at bits 1-3. RSTS V7's KW11-P diagnostic writes CSR = 0o13 to
// start the counter (bits 0, 1, 3 set). For the test to succeed, bit 1 alone
// must encode "100 KHz" - i.e. RATE is a 2-bit field at bits 1-2, and bit 3 is
// a separate flag (repeat-vs-single or similar). 0o13 then decodes as
// GO=1 | RATE=01 (100 KHz) | bit3=1.
//
//   bit 0    GO     write 1 to start; hardware clears at end of single-shot
//   bits 1-2 RATE   00=stop  01=100KHz  10=10KHz  11=line freq
//   bit 3    MODE   software-chosen flag; we treat as "repeat" indicator
//   bit 6    IE     interrupt enable
//   bit 7    DONE   set when CNTR underflows (0 -> wrap); cleared on GO write
//
// COUNT DIRECTION: CNTR counts DOWN from CSB to 0; DONE fires when CNTR
// underflows past 0. RSTS V7's test loads CSB = 0o177777 and reads CNTR in a
// tight loop expecting it to visibly change - which only happens with a
// down-counter, because immediate-reload up-counter semantics keep CNTR at
// 0o177777 between every overflow tick.
//
// Counter cadence: kwp::tick() is called once per executed CPU instruction
// (~983 KHz host tick rate - see kw11.cpp / cpu_pdp11.cpp). RATE selects a
// divisor (instructions per CNTR step). On underflow we set DONE and, if IE
// is still set, raise INTRTC at BR5 - re-checking IE matches the rk11.cpp
// deferred-IRQ pattern (the OS may clear IE between expiry and the host
// catching up to tick()).
//
// In repeat mode (bit 3 set) the counter reloads from CSB on each underflow.
// Otherwise GO clears and the counter stops.

#include "kwp.h"

#include "kb11.h"
#include "kd11.h"
#include "sam11_platform.h"

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kwp {

uint16_t CSR  = 0;
uint16_t CSB  = 0;
uint16_t CNTR = 0;

// Instructions-per-CNTR-tick for each RATE field value (bits 1-2, 2-bit
// field). The host clock runs at roughly 983 KHz (matches KW11-L's
// LKS_PER = 16384 at a 60 Hz target), so the divisor for "real KW11-P
// 100 KHz" is ~10 host instructions, etc.
static const uint32_t kRateDiv[4] = {
    0,       // 0: stop
    10,      // 1: 100 KHz  (host_rate / 100_000)
    98,      // 2: 10 KHz   (host_rate / 10_000)
    16384,   // 3: line freq (matches LKS_PER)
};

static uint32_t s_subtick = 0;  // instructions counted toward next CNTR step

void reset()
{
    CSR = 0;
    CSB = 0;
    CNTR = 0;
    s_subtick = 0;
}

uint16_t read16(uint32_t a)
{
    switch (a) {
        case DEV_KWP_CSR:  return CSR;
        case DEV_KWP_CSB:  return CSB;
        case DEV_KWP_CNTR: return CNTR;
        case DEV_KWP:      return 0;
    }
    return 0;
}

void write16(uint32_t a, uint16_t v)
{
    switch (a) {
        case DEV_KWP_CSR: {
            // GO bit transitioning to 1 (re)starts the counter from CSB.
            bool go_now = (v & 0001) != 0;
            CSR = v;
            if (go_now) {
                CNTR = CSB;
                CSR &= ~(uint16_t)0200;  // clear DONE (bit 7)
                s_subtick = 0;
            }
            return;
        }
        case DEV_KWP_CSB:
            CSB = v;
            CNTR = v;  // DEC: writing preset also loads the live counter
            return;
        case DEV_KWP_CNTR:
            CNTR = v;
            return;
        case DEV_KWP:
            return;
    }
}

void tick()
{
    if ((CSR & 0001) == 0) return;  // GO clear

    uint8_t rate = (CSR >> 1) & 0003;  // RATE = bits 1-2 (2-bit field)
    uint32_t div = kRateDiv[rate];
    if (div == 0) return;  // stop

    if (++s_subtick < div) return;
    s_subtick = 0;

    // Down-counter: CNTR decrements; underflow at value 0 fires DONE.
    // RSTS V7's diagnostic test reads CNTR in a tight loop with CSB
    // preset to 0o177777 - the test passes when CNTR is observed to be
    // moving, which requires a long visible decrement run.
    if (CNTR == 0) {
        CSR |= 0200;  // set DONE (bit 7)

        // Re-check IE at fire time (rk11 pattern - OS may have cleared it).
        if (CSR & 0100) {
            procNS::interrupt(INTRTC, 5);
        }

        bool repeat = (CSR & 0010) != 0;  // bit 3
        if (repeat) {
            CNTR = CSB;  // reload preset for next interval
        } else {
            CSR &= ~(uint16_t)0001;  // clear GO, stop counting
        }
    } else {
        CNTR--;
    }
}

};  // namespace kwp
