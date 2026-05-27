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

#include <Arduino.h>

// vpdp1140 m2: KL11 is routed through the v8088-inherited host scaffolding
// so guest TTY output appears on the TFT 80x25 grid AND any Telnet client
// AND USB-Serial, and guest TTY input is fed from any of those sources.
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

void reset()
{
    TKS = 0;
    TPS = 1 << 7;
    TKB = 0;
    TPB = 0;
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
    if (TKS & (1 << 6))
    {
        procNS::interrupt(INTTTYIN, 4);
    }
}

uint8_t count;

void poll()
{
    // Read: drain one byte from the host key queue if available.
    // vpdp1140.ino's loop() pushes USB-Serial bytes into this queue;
    // telnet.cpp pushes bytes received from a connected client. So this
    // single source fans in Serial + Telnet without duplicating work.
    // (The original kl11.cpp's inline ESC -> 0x7F translation is gone for
    // now; we can revive it via termopts.h if XXDP/RT-11 needs delete-
    // key handling.)
    // Only pull a new char if the guest has consumed the previous one
    // (TKS bit 7 cleared by reading TKB). Otherwise queued bytes would
    // overwrite each other in TKB between guest reads, and the guest
    // would see only the last byte of any burst (CR or LF instead of
    // the actual character typed).
    if (!(TKS & 0x80))
    {
        uint8_t c;
        if (console_key_pop(&c))
        {
            if (c == '\n' || c == '\r')
            {
                procNS::trapped |= VTRAP_ON_NL;
            }
            addchar(c & 0x7F);
        }
    }

    // Write: when the guest puts a char in TPB, count up 32 polls (mimics
    // a baud-rate-limited UART) then fan out to all three host channels.
    if ((TPS & 0x80) == 0)
    {
        if (++count > 32)
        {
            uint8_t out = TPB & 0x7f;  // strip parity bit
            console_feed(out);          // TFT 80x25 grid
            telnet_write(out);          // any connected telnet client
            if (!g_serial_silenced) Serial.write(out);  // USB-Serial monitor
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
            return TKB;
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
