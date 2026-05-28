# vpdp1140 — a DEC PDP-11/40 emulator for the load 1.bESP32-S3

A **Freenove ESP32-S3 2.8" Display** board turned into a tiny DEC
PDP-11/40 that boots **V6 Unix** from an SD-card disk image. The console
appears on the onboard TFT, on Telnet, and on USB-Serial — all three live
simultaneously.

```
            +------------------------------+
            |  RT-11SJ V05.07              |
            |                              |
            |  .DIR                        |
            |  RT11 .SYS    79 12-Jun-77  ... |
            |  ...                         |
            +------------------------------+
                 ESP32-S3 / ILI9341 TFT
                 + Telnet + USB-Serial
```

The CPU core is vendored from
[**sam11**](https://gitlab.com/ChloeLunn/sam11) by Chloe Lunn
(BSD-3-Clause), descended from Julius Schmidt's JavaScript PDP-11
emulator via Dave Cheney's `avr11`. The host scaffolding (TFT console,
Telnet server, dual-core split, SD-backed block I/O, `/wificonfig.ini` + `/pdpconfig.ini`,
capacitive-touch settings menu, WS2812 status LED) is inherited
unchanged from the [v8088](../v8088) Intel 8088 emulator on the same
board — that's the whole point of the fork: swap the guest CPU, keep
everything else.

## Status

| Guest OS              | Disk image          | Result                            |
|-----------------------|---------------------|-----------------------------------|
| **V6 Unix**           | `unixv6.dsk` (RK05) | ✅ Boots to `@`, then `#` shell    |
| **XXDP+ diagnostics** | `xxdp25.dsk` (RL02) | ✅ Boots to XXDP-SM `.` monitor    |
| RT-11 SJ V5           | `rt11v5.dsk` (RK05) | ⚠️ Loads then stalls in init      |
| RSTS/E V7             | `rsts_full_rl.dsk`  | ⚠️ Loads several sectors then HALTs |

V6 Unix and XXDP+ both work end-to-end (keyboard input, disk read/write,
console output on all three channels). RT-11 V5 and RSTS/E hit code
paths that touch features sam11 doesn't fully emulate (KW11-P
programmable clock at `0o172540`, FP11 floating point, 22-bit MMU). This
is the same status sam11's upstream README documents: V6 is the
validated path; "some other OSes boot, but crash out for various
reasons."

## Hardware

- **Freenove ESP32-S3 WROVER 2.8" Display** board (FNK0104B): ILI9341
  TFT, FT6336U capacitive touch, micro-SD slot, 8 MB Octal PSRAM,
  16 MB flash.

## Emulated configuration

| Component       | What we emulate                                                   |
|-----------------|-------------------------------------------------------------------|
| CPU             | KD11 (PDP-11/40), full ISA + EIS, no FP11 / FIS                   |
| Memory          | 248 KiB physical, 18-bit address bus, allocated from PSRAM        |
| MMU             | KT11 (kernel + user address spaces)                               |
| Console         | KL11 UART at `0o177560` (vector 060), bridged to TFT+Telnet+USB    |
| RK05 disk       | RK11 controller at `0o177400` (vector 220), up to 4 drives        |
| RL01/02 disk    | RL11 controller at `0o174400` (vector 160), up to 4 drives        |
| Line clock      | KW11-L at `0o177546` (vector 100), tickrate ~60 Hz                |
| Boot ROM        | DEC M9312-style RK0 / RL0 stubs (selected by `boot=` in config)   |

The status bar below the 80×25 console shows drive activity, WiFi IP,
Telnet state and MIPS in real time.

## Building

Arduino IDE with the ESP32 board package and these libraries (same set
as v8088 — nothing PDP-11-specific):

- **TFT_eSPI** — with the `FNK0104B` setup enabled in `User_Setup_Select.h`
- **FT6336U** — Freenove-bundled touch library
- **Freenove_WS2812_Lib_for_ESP32**

The sam11 sources we use are copied directly into the sketch root (see
the file list below) so the Arduino IDE picks them up automatically. No
SdFat library is needed — we route all sam11 disk I/O through our own
`disk.cpp` block layer.

### Tools-menu settings (important)

| Setting            | Value                                |
|--------------------|--------------------------------------|
| Board              | **ESP32S3 Dev Module** (not "Octal") |
| USB CDC On Boot    | **Enabled**                          |
| PSRAM              | **OPI PSRAM**                        |
| Flash Size         | 16MB (128Mb)                         |
| Partition Scheme   | Huge App (3MB)                       |

Selecting an "…Octal" board variant, or PSRAM set to anything but OPI,
will bootloop the board.

## SD card layout

```
/wificonfig.ini      WiFi credentials (auto-created if missing)
/pdpconfig.ini       PDP-11 settings (auto-created if missing)
/wificonfig-*.ini    optional WiFi variants picked from the settings menu
/pdpconfig-*.ini     optional PDP variants picked from the settings menu
/unixv6.dsk          V6 Unix RK05 image  (2.5 MB)  ← validated boot
/xxdp25.dsk          XXDP+ diagnostics  (RL02, 10 MB)
/rt11v5.dsk          RT-11 SJ V5  (RK05, optional)
/rsts_full_rl.dsk    RSTS/E V7 boot pack  (RL01, optional)
/rsts_swap_rl.dsk    RSTS/E V7 swap pack  (RL01, optional)
```

Sample images for V6 / XXDP+ / RT-11 / BSD 2.9 / Caldera V5/V6 ship in
sam11's [`OS Images/`](https://gitlab.com/ChloeLunn/sam11/-/tree/master/OS%20Images)
directory.

## Config files

WiFi credentials live in `/wificonfig.ini`; everything else in
`/pdpconfig.ini`. Either file can be missing on first boot — the
firmware writes a default. Drop named copies onto the SD card
(`wificonfig-home.ini`, `pdpconfig-rt11.ini`, ...) and pick one from
the **WiFi Config** / **PDP Config** menu items; selection copies the
chosen variant over the active filename and you get a confirmation
screen offering to reset the ESP32 to apply it.

`/wificonfig.ini`:
```ini
[wifi]
ssid     = YourNetwork        ; blank uses secrets.h defaults
password = YourPassword
hostname = vpdp1140
```

`/pdpconfig.ini`:
```ini
[telnet]
enabled = true
port    = 23

[diag]
pcping      = 5               ; sec between PC dumps; 0 disables
serialdelay = 20              ; ms gate between bursty input chars
v4b_quirks  = true            ; RSTS V4B / RT-11 / V6 / XXDP
kwp_enabled = false           ; true for RSTS V7 bring-up

[disks]
; dl0, dl1 = RL01/RL02 packs (RL11 controller)
; dx0, dx1 = RX02 floppies   (not yet wired)
; rk0      = RK05 pack       (RK11 controller)
; When boot=rk0 the rk0 image takes slot 0 in place of dl0.
dl0  = /xxdp25.dsk
dl1  =
dx0  =
dx1  =
rk0  = /unixv6.dsk
boot = rk0                    ; or dl0, dl1, dx0, dx1
```

## Using it

- The TFT console comes up at boot; the same byte stream is available
  via `telnet <board-ip> 23` and on USB serial (115200 baud).
- **Settings menu:** tap the screen or press the onboard button. From
  there you can mount/dismount/create disk images, reboot the PDP-11,
  adjust brightness, and view WiFi / Telnet status.

### Booting V6 Unix

1. Set `boot = rk0` and `rk0 = /unixv6.dsk` in `/pdpconfig.ini`.
2. Power the board. After the WiFi line you should see:
   ```
   vpdp1140: booting PDP-11/40 from RK0...
   @
   ```
3. Type `unix` and Enter — the V6 kernel loads and drops you at `#`.
4. Try `ls /`, `date`, `cat /etc/passwd`, even `cc hello.c`. The
   PDP-11 C compiler is on the disk.

### Booting XXDP+

1. Set `boot = dl0` and `dl0 = /xxdp25.dsk`.
2. After reset you'll see the XXDP-SM monitor prompt `.`. Try
   `R FKAAC0` for the basic instruction-set diagnostic.

## What sam11 looks like in this build

| File                          | Role                                                |
|-------------------------------|-----------------------------------------------------|
| `kd11.cpp` / `.h`             | PDP-11/40 CPU (KD11), step / trap / interrupt       |
| `kt11.cpp` / `.h`             | 11/40 MMU, kernel + user spaces                     |
| `ms11.cpp` / `.h`             | RAM controller — routed to our PSRAM block          |
| `dd11.cpp` / `.h`             | UNIBUS backplane, I/O page dispatch                 |
| `kl11.cpp` / `.h`             | KL11 console — rewired to TFT+Telnet+USB           |
| `rk11.cpp` / `.h`             | RK11 controller — rewired to `disk.cpp`             |
| `rl11.cpp` / `.h`             | RL11 controller (fresh implementation; not sam11's) |
| `kw11.cpp` / `.h`             | KW11-L line clock                                   |
| `cpu/cpu_*.cpp.h`             | Instruction implementations                         |
| `pdp1140.h`                   | Device addresses, trap vectors, build feature flags |
| `bootrom.h`                   | M9312-style RK0 / RL0 boot ROMs                     |
| `sam11_platform.h`            | Our ESP32-S3 platform shim (replaces sam11's)       |

The aggregated sam11 source originally vendored in `_upstream_sam11/`
(gitignored) was copied to the sketch root and edited for our needs.
The most material changes are:

- **14 instruction-correctness fixes** in `cpu/cpu_instr.cpp.h` (INC, ROR,
  SWAB, ADD, SUB, NEG, ADC, SBC, ASR, MUL, SXT, MARK, CCC, SBC) — found
  by running XXDP+ FKAAC0.
- **3 new instructions** added (SPL, MTPS, MFPS) — needed for XXDP's
  FKABD0 trap test.
- **PSRAM-backed `ms11`** — sam11's stock allocator wanted a 248 KiB
  array in DRAM, which is most of the ESP32's RAM.
- **Custom `rl11.cpp`** — sam11's stock RL11 is WIP and didn't drive
  XXDP+; rewrote from scratch using the same DEC RL11 manual.
- **Deferred RK11 done-IRQ** in `rk11.cpp` — sam11's stock fires the IRQ
  synchronously inside the controller-register write, which beats the
  guest's `MOV cmd,RKCS / WAIT` pattern and hangs RT-11. We delay by
  ~256 host steps so the WAIT runs first.

## Milestones

- ✅ **m0** Fork v8088, rename / strip down, sketch compiles
- ✅ **m1** Vendor sam11, in-sketch CPU self-test passes
- ✅ **m2** KL11 console on TFT + Telnet + USB-Serial
- ✅ **m3** RL11 → XXDP+ boots; 14 sam11 CPU bugs fixed; SPL/MTPS/MFPS added
- ✅ **m4** V6 Unix boots from RK05 to `#` prompt
- ⏸ **m5** — m4's V6 boot is what m5 was supposed to add (XXDP); already done in m3
- ⏳ **m6** Settings-menu retarget — drive labels DL0/DL1/DX0/DX1/RK0 already in place; mount/dismount-at-runtime UI still pending
- ⏳ **m7** KW11-L line clock — present, but tickrate could be calibrated
- ⏳ **m8** Second DL11 tunneling SD file I/O — designed in chat, not yet built
- ⏳ **m9** RT-11 / RSTS chase — sam11 known-broken; deep-dive if motivated
- ✅ **m10** README polish + GitHub push  ← **this commit**

## Credits

- CPU core: **sam11** by Chloe Lunn — BSD-3-Clause —
  https://gitlab.com/ChloeLunn/sam11
- sam11 descends from Julius Schmidt's JavaScript PDP-11 emulator and
  Dave Cheney's [avr11](https://dave.cheney.net/2014/01/23/avr11-simulating-minicomputers-on-microcontrollers).
- Host scaffolding: ESP32-S3 TFT/Telnet/SD/dual-core stack forked from
  [v8088](https://github.com/deangi/vMSDOS), which itself ports Adrian
  Cable's [8086tiny](https://github.com/adriancable/8086tiny).
- 4×8 console font: public-domain IBM VGA font (via dhepper/font8x8).
- Sample disk images: sam11's `OS Images/` and
  https://www.pcjs.org/software/dec/pdp11/disks/rl02k/xxdp/

## License

vpdp1140 itself is provided under the same license as the upstream
sam11 code it builds on: **BSD 3-Clause**. See `LICENSE` for the full
text. The vendored sam11 sources retain their original copyright notice
(Copyright 2021 Chloe Lunn).
