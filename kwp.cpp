// KW11-P Programmable Real-Time Clock - minimum viable emulation.
//
// CSR layout we honor (DEC convention, bits not listed are read-as-written):
//   bit 0    GO     write 1 to start; cleared at end of single-shot
//   bits 1-3 RATE   000=stop  001=100KHz  010=10KHz  011=line  others=stop
//   bit 4    MODE   0=single-shot, 1=repeat
//   bit 6    IE     interrupt enable
//   bit 7    DONE   set when CNTR underflows; cleared on GO write
//
// Counter cadence: kwp::tick() is called once per executed CPU instruction
// (~983 KHz on this hardware - see kw11.cpp / cpu_pdp11.cpp). RATE selects a
// divisor (instructions per CNTR decrement). On CNTR underflow we set DONE
// and, if IE is still set, raise INTRTC at BR5 - re-checking IE matches the
// rk11.cpp deferred-IRQ pattern (RT-11 sometimes clears IE between expiry
// and the host catching up to tick()).
//
// In repeat mode the counter reloads from CSB. In single-shot mode GO clears
// and the counter stops.

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

// Instructions-per-CNTR-tick for each RATE field value. The host clock runs
// at roughly 983 KHz (matches KW11-L's LKS_PER = 16384 at a 60 Hz target),
// so the divisor for "real KW11-P 100 KHz" is ~10 host instructions, etc.
static const uint32_t kRateDiv[8] = {
    0,       // 0: stop
    10,      // 1: 100 KHz  (host_rate / 100_000)
    98,      // 2: 10 KHz   (host_rate / 10_000)
    16384,   // 3: line freq (matches LKS_PER)
    0,       // 4: external - not modeled, treat as stop
    0, 0, 0  // 5..7: undefined, treat as stop
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

    uint8_t rate = (CSR >> 1) & 0007;
    uint32_t div = kRateDiv[rate];
    if (div == 0) return;  // stop or unsupported rate

    if (++s_subtick < div) return;
    s_subtick = 0;

    // CNTR decrements; underflow at value 0.
    if (CNTR == 0) {
        CSR |= 0200;  // set DONE (bit 7)

        // Re-check IE at fire time (rk11 pattern - OS may have cleared it).
        if (CSR & 0100) {
            procNS::interrupt(INTRTC, 5);
        }

        bool repeat = (CSR & 0020) != 0;  // bit 4
        if (repeat) {
            CNTR = CSB;
        } else {
            CSR &= ~(uint16_t)0001;  // clear GO, stop counting
        }
    } else {
        CNTR--;
    }
}

};  // namespace kwp
