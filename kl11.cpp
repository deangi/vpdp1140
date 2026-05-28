/*
Modified BSD License

Copyright (c) 2021 Chloe Lunn

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// sam11 software emulation of DEC PDP-11/40 KL11 Main TTY

#include "kl11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "sam11.h"
#include "termopts.h"
#include "platform.h"
#include "fifo.h"

#include <Arduino.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// vpdp1140 m2: KL11 is routed through the v8088-inherited host scaffolding
// so guest TTY output appears on the TFT 80x25 grid AND any Telnet client
// AND USB-Serial, and guest TTY input is fed from any of those sources.
// m11 (2026-05-28): each host channel sits behind an 8 KB FIFO so a
// bursty guest (RT-11 DIR, V6 ls -l, type) can hand off output without
// being throttled by the slowest sink, and the host RX path can absorb
// fast-typed input without backpressuring the producers.
#include "console.h"
#include "telnet.h"

#if USE_11_45
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kl11 {

uint16_t TKS;
uint16_t TKB;
uint16_t TPS;
uint16_t TPB;

// 8 KB KL11->USB-Serial FIFO. The TFT and Telnet sinks own their own
// FIFOs inside console.cpp / telnet.cpp; this one stays here because
// there's no "serial" module to put it in. Storage lives in PSRAM.
#define VPDP_KL11_FIFO_BYTES 8192
EXT_RAM_BSS_ATTR static uint8_t serial_out_storage[VPDP_KL11_FIFO_BYTES];
static Fifo g_serial_out;
static bool g_serial_out_inited = false;

// Diagnostic flag (set from config.ini [diag] tty_trace). When true, every
// byte that enters addchar() gets LOG'd along with a millisecond timestamp,
// so the host operator can see the order the KL11 saw a burst of input.
bool input_trace_enabled = false;

// Inter-character delay (ms) between successive TKB loads (set from
// [diag] serialdelay in config.ini). After each addchar we record the
// host's millis(); the next byte can't enter TKB until at least this
// many ms have elapsed. Closes the burst-induced klrint re-entry window
// at the KL11 layer in an OS-agnostic way (no PSW priority inspection).
// Default 0 = no delay; recommended 10-50 ms for interactive guests.
uint32_t serial_in_delay_ms = 0;
static uint32_t s_last_addchar_ms = 0;
static bool     s_fifo_drained    = true;   // true at boot -> first char is immediate

void reset()
{
    TKS = 0;
    TPS = 1 << 7;
    TKB = 0;
    TPB = 0;
    if (!g_serial_out_inited) {
        g_serial_out.init(serial_out_storage, VPDP_KL11_FIFO_BYTES);
        g_serial_out_inited = true;
    }
}

void drain_serial_out()
{
    // Same chunked drain as telnet_poll: contiguous runs from the ring,
    // up to whatever Serial.write() will take in one call. Serial.write
    // on USB-CDC is generally non-blocking up to the host driver's buffer.
    if (g_serial_silenced) { g_serial_out.clear(); return; }
    const uint8_t* p;
    size_t n;
    while ((n = g_serial_out.peek(&p)) > 0) {
        size_t w = Serial.write(p, n);
        if (w == 0) break;
        g_serial_out.consume(w);
        if (w < n) break;
    }
}

static void addchar(char c)
{
#if CR_TO_LF && !LF_TO_CR
    if (c == _CR)
        c = _LF;
#endif

#if !CR_TO_LF && LF_TO_CR
    if (c == _LF)
        c = _CR;
#endif

#if BS_TO_DEL && !DEL_TO_BS
    // If enabled, change backspaces into deletes
    if (c == _BS)
        c = _DEL;
#endif

#if !BS_TO_DEL && DEL_TO_BS
    // If enabled, change deletes into backspaces
    if (c == _DEL)
        c = _BS;
#endif

#if REMAP_WITH_TABLE
    // If enabled, use the keymap array to change the character
    TKB = ascii_chart[c & 0x7F] & 0x7F;
#else
    // If not then just pass the character forward as is
    TKB = c & 0x7F;
#endif

    TKS |= 0x80;
    if (input_trace_enabled) {
        uint8_t pc = (uint8_t)(TKB & 0x7f);
        LOG("kl11 in: 0x%02x %s%c%s  ms=%lu",
            (unsigned)pc,
            (pc >= 0x20 && pc < 0x7f) ? "'" : "",
            (pc >= 0x20 && pc < 0x7f) ? pc  : '.',
            (pc >= 0x20 && pc < 0x7f) ? "'" : "",
            (unsigned long)millis());
    }
    if (TKS & (1 << 6))
    {
        procNS::interrupt(INTTTYIN, 4);
    }
}

uint8_t count;

void poll()
{
    // Read: round-robin between the host's two input FIFOs (Serial via
    // console_key_pop, Telnet via telnet_in_pop). Alternation happens
    // only on a successful pop, so if just one source has data we drain
    // it without skipped polls. Cross-source order can't be preserved
    // (both clients type independently), but each source's own order is.
    //
    // Three-stage guard before loading TKB:
    //  (a) bit 7 (RDONE) clear -> the guest has already read the prior
    //      byte from TKB. Without this we'd overwrite TKB between reads.
    //  (b) no INTTTYIN (vec 060) still pending in sam11's itab -> the
    //      guest has actually taken the prior IRQ via handleinterrupt
    //      (which pops it from itab).
    //  (c) inter-character delay: at least serial_in_delay_ms ms have
    //      elapsed since the last addchar. Without (c), a fast host-side
    //      burst can queue the next INTTTYIN while klrint is still
    //      running on the prior byte; sam11's IRQ check fires at >=
    //      priority (not strictly >, as a real PDP-11 would), so the
    //      second INTTTYIN re-enters klrint. The nested klrint saves
    //      the in-progress R0 (which holds the first byte) to the
    //      stack and reads the freshly-pushed TKB; V6's ttyinput()
    //      then receives the bytes in LIFO order as the stack unwinds.
    //      A small ms gap matches what a real serial line would have
    //      enforced via baud-rate timing, and stays OS-agnostic
    //      (no PSW priority inspection). serial_in_delay_ms is set
    //      from [diag] serialdelay in config.ini.
    //
    // Wraparound: millis() wraps every ~49.7 days. The unsigned
    // subtraction (now - s_last_addchar_ms) wraps correctly for any
    // delay < 2^31 ms. The corner case where the *previous* addchar
    // happened within `delay` ms of the wrap boundary is closed by
    // s_fifo_drained: once the host FIFO drains empty (no chars
    // pending), the next arriving char bypasses the delay entirely,
    // so an idle wraparound never matters.
    bool inttytin_queued = false;
    for (uint8_t i = 0; i < ITABN; i++) {
        if (itab[i].vec == 0) break;
        if (itab[i].vec == INTTTYIN) { inttytin_queued = true; break; }
    }

    if (!(TKS & 0x80) && !inttytin_queued)
    {
        uint32_t now = millis();
        bool delay_ok = s_fifo_drained ||
                        (uint32_t)(now - s_last_addchar_ms) >= serial_in_delay_ms;
        if (delay_ok)
        {
            static bool prefer_telnet = false;
            uint8_t c;
            bool got;
            if (prefer_telnet)
                got = telnet_in_pop(&c) || console_key_pop(&c);
            else
                got = console_key_pop(&c) || telnet_in_pop(&c);
            if (got)
            {
                prefer_telnet = !prefer_telnet;
                if (c == '\n' || c == '\r')
                {
                    procNS::trapped |= VTRAP_ON_NL;
                }
                addchar(c & 0x7F);
                s_last_addchar_ms = now;
                s_fifo_drained    = false;
            }
            else
            {
                // No host bytes pending. Mark idle so the next arriving
                // char bypasses the delay (and the millis() wraparound
                // corner case becomes a non-issue).
                s_fifo_drained = true;
            }
        }
    }

    // Write: when the guest puts a char in TPB, count up 32 polls (mimics
    // a baud-rate-limited UART) then fan the byte into all three host
    // FIFOs. Each sink owns its own 8 KB ring and drains independently,
    // so a slow USB host or a wedged telnet client can't stall the TFT
    // (or each other), and the KL11 itself never blocks.
    if ((TPS & 0x80) == 0)
    {
        if (++count > 32)
        {
            uint8_t out = TPB & 0x7f;  // strip parity bit
            console_feed(out);         // -> TFT-out FIFO
            telnet_write(out);         // -> Telnet-out FIFO (with IAC escape)
            g_serial_out.push(out);    // -> USB-Serial-out FIFO
            TPS |= 0x80;
            if (TPS & (1 << 6))
            {
                procNS::interrupt(INTTTYOUT, 4);
            }
        }
    }
}

uint16_t read16(uint32_t a)
{
    switch (a)
    {
    case DEV_CONSOLE_TTY_IN_STATUS:
        return TKS;
    case DEV_CONSOLE_TTY_IN_DATA:
        if (TKS & 0x80)
        {
            // Clear only bit 7 (RX done); bit 0 is the reader-enable
            // flag which is set by software, not by reading TKB.
            TKS &= 0xff7f;
            if (input_trace_enabled) {
                uint8_t rc = (uint8_t)(TKB & 0x7f);
                LOG("kl11 rd: 0x%02x %s%c%s  PC=0%o  ms=%lu",
                    (unsigned)rc,
                    (rc >= 0x20 && rc < 0x7f) ? "'" : "",
                    (rc >= 0x20 && rc < 0x7f) ? rc  : '.',
                    (rc >= 0x20 && rc < 0x7f) ? "'" : "",
                    (unsigned)procNS::R[7],
                    (unsigned long)millis());
            }
            return TKB;
        }
        if (input_trace_enabled) {
            LOG("kl11 rd: (TKS bit7=0, returning 0)  PC=0%o  ms=%lu",
                (unsigned)procNS::R[7], (unsigned long)millis());
        }
        return 0;
    case DEV_CONSOLE_TTY_OUT_STATUS:
        return TPS;
    case DEV_CONSOLE_TTY_OUT_DATA:
        return 0;
    default:
        if (PRINTSIMLINES)
        {
            if (!g_serial_silenced) Serial.println(F("%% kl11: read16 from invalid address"));  // " + ostr(a, 6))
            //panic();
        }
        return 0;
    }
}

void write16(uint32_t a, uint16_t v)
{
    switch (a)
    {
    case DEV_CONSOLE_TTY_IN_STATUS:
        // Real DL11: enabling IE while ready=1 (a queued char already
        // present) fires the IRQ immediately. RT-11 SJ relies on this
        // edge to kick its input handler.
        if (v & (1 << 6))
        {
            bool was_off = (TKS & (1 << 6)) == 0;
            TKS |= 1 << 6;
            if (was_off && (TKS & 0x80))
            {
                procNS::interrupt(INTTTYIN, 4);
            }
        }
        else
        {
            TKS &= ~(1 << 6);
        }
        break;
    case DEV_CONSOLE_TTY_OUT_STATUS:
        // Real DL11: enabling IE while ready=1 (transmitter idle) fires
        // the IRQ immediately so the output ISR can pull the first char
        // from its buffer. Without this, RT-11 SJ's output buffer never
        // starts draining and the console stays silent.
        if (v & (1 << 6))
        {
            bool was_off = (TPS & (1 << 6)) == 0;
            TPS |= 1 << 6;
            if (was_off && (TPS & 0x80))
            {
                procNS::interrupt(INTTTYOUT, 4);
            }
        }
        else
        {
            TPS &= ~(1 << 6);
        }
        break;
    case DEV_CONSOLE_TTY_OUT_DATA:
        TPB = v & 0xff;
        TPS &= 0xff7f;
        count = 0;
        break;
    case DEV_CONSOLE_TTY_IN_DATA:
        break;
    default:
        if (PRINTSIMLINES)
        {
            if (!g_serial_silenced) Serial.println(F("%% kl11: write16 to invalid address"));  // " + ostr(a, 6))
            //panic();
        }
    }
}

};  // namespace kl11
