# vpdp1140 — a DEC PDP-11/40 emulator for the ESP32-S3

> **Cloned from [v8088](../v8088) on 2026-05-23 and being retargeted to
> emulate a DEC PDP-11/40 with 248 KiB of guest memory, RK05 / RL02 disks,
> and KL11 console.**
>
> The ESP32-S3 host scaffolding — TFT console, telnet, USB serial, SD-backed
> disk images, capacitive-touch settings menu, `/config.ini`, dual-core split,
> WS2812 status LED — is inherited from v8088 unchanged. Only the CPU core,
> I/O-page dispatch, and disk/console wiring are PDP-11-specific.
>
> **Scope note (2026-05-23):** the original plan was an 11/70 with 1 MB / 22-bit
> MMU. After inspecting the vendored CPU core (sam11), we discovered it is
> 11/40 / 18-bit-MMU only, capped at 248 KiB. The 22-bit MMU does not exist
> in that codebase and would have to be written. We reset the target to
> "what sam11 actually delivers" — a PDP-11/40 with 248 KiB. The folder was
> renamed `vpdp1170` → `vpdp1140`. Sam11's tested config (V6 Unix on RK05) and
> XXDP+ diagnostics both fit comfortably in 248 KiB.
>
> The staging plan and milestones live at
> `~/.claude/plans/enumerated-skipping-glade.md`. **Current state: m0 — scaffolding
> only, no PDP-11 instructions execute yet (m1 in progress).**

vpdp1140 turns a **Freenove ESP32-S3 2.8" Display** board into a tiny DEC
PDP-11/40 that boots a DEC operating system from disk images on an SD card.
First-target guest is **V6 Unix on RK05** (sam11's tested config); secondary
targets are **XXDP+ diagnostics** and **RT-11 SJ** on RL02. The console
appears on the onboard TFT, and simultaneously over Telnet and USB serial.

The CPU core is vendored from **[sam11](https://gitlab.com/ChloeLunn/sam11)**
by Chloe Lunn (BSD-3-Clause), descended from Julius Schmidt's JavaScript PDP-11
emulator via Dave Cheney's `avr11`. sam11 emulates a PDP-11/40 (KD11 CPU,
KT11 MMU, MS11 RAM, DD11 backplane, KL11 console, RK11 disk, KW11 line clock)
with full EIS. The 11/45 / 11/70 mode (KB11) is partial / WIP in sam11.

## Target features (post-m10)

- KD11 PDP-11/40 CPU with full ISA + EIS, KT11 18-bit MMU
- 248 KiB of guest RAM in PSRAM (sam11 cap)
- KL11 console UART at `17777560` octal (vector 060)
- RK11 controller at `17777400` octal with up to 4× RK05 drives (2.5 MB each)
- RL11 controller at `17774400` octal for RL01/RL02 disks (10 MB; sam11 WIP)
- KW11-L line clock at `17777546` octal (vector 100)
- TFT console: 80×25 ANSI / VT-100-ish terminal, 4×8 font
- Telnet server **and** USB serial as additional live consoles
- Capacitive-touch settings menu: mount / dismount / create disk images,
  reboot, brightness
- `/config.ini` on the SD card for WiFi, Telnet and disk configuration
- RGB status LED: red = starting, blue = booting, green = ready

## Hardware

- **Freenove ESP32-S3 WROVER 2.8" Display** board (FNK0104B): ILI9341 TFT,
  FT6336U capacitive touch, micro-SD slot, 8 MB Octal PSRAM, 16 MB flash.

## Building

Arduino IDE with the ESP32 board package. Required libraries (same as v8088):

- **TFT_eSPI** — with the `FNK0104B` setup enabled in `User_Setup_Select.h`
- **FT6336U** — Freenove-bundled touch library
- **Freenove_WS2812_Lib_for_ESP32**
- **SdFat** — required by sam11 for its disk image access (added in m1)

### Tools menu settings (important)

| Setting            | Value                                |
|--------------------|--------------------------------------|
| Board              | **ESP32S3 Dev Module** (not "Octal") |
| USB CDC On Boot    | **Enabled**                          |
| PSRAM              | **OPI PSRAM**                        |
| Flash Size         | 16MB (128Mb)                         |
| Partition Scheme   | Huge App (3MB)                       |

Selecting an "...Octal" board variant, or PSRAM set to anything but OPI, will
bootloop the board.

## SD card layout (target)

```
/config.ini          settings (auto-created with defaults if missing)
/unixv6.dsk          a bootable V6 Unix RK05 image  (RK0, 2.5 MB)
/xxdp.dsk            DEC XXDP+ diagnostics RL02 image
/rt11.dsk            RT-11 SJ RL02 image (optional)
```

sam11's `OS Images/` directory ships sample images: `unixv6.dsk`,
`bsd2.9.dsk`, `xxdp.dsk`, `rt11v1.dsk`..`rt11v5.dsk`, and several Caldera
V5/V6 builds.

## config.ini

The exact key names migrate from v8088's `a/b/c/d` to PDP-11-native names
(`rk0`..`rk3` for RK05 drives, `dl0`..`dl1` for RL02) in milestone m6.
Until then, the v8088 schema is honoured for back-compat.

```ini
[system]
title   = vpdp1140
version = 0.0-m0

[wifi]
ssid     = YourNetwork
password = YourPassword
hostname = vpdp1140

[telnet]
enabled = true
port    = 23

[disks]
a    = /unixv6.dsk         ; will be renamed to rk0 in m6
b    =
c    =
d    =
boot = a                   ; will be renamed to boot=rk0 in m6
```

## Using it

- The console appears on the TFT and is reachable at `telnet <board-ip> 23`
  and on the USB serial port (115200 baud).
- **Settings menu:** double-tap the screen, or press the onboard button.
  From there you can mount/dismount/create disk images, reboot the PDP-11,
  adjust brightness, and view WiFi / Telnet status.

## Roadmap

The full milestone list is in
`C:\Users\deang\.claude\plans\enumerated-skipping-glade.md`. In short:

- **m0** Clone + rename + scaffolding stub
- **m1** Vendor sam11 (kd11 / kt11 / ms11 / dd11 + cpu/) into the build,
  replace platform.h with an ESP32-S3 version, stub kl11/rk11/lp11/ky11,
  route ms11 at our PSRAM; in-setup self-test executes a NOP/MOV/branch loop
- **m2** Wire KL11 console to our existing TFT/Telnet/Serial byte streams
- **m3** Wire RK11 disk to our existing `disk.cpp` block I/O
- **m4** Boot V6 Unix on RK05 to the `#` prompt
- **m5** Boot XXDP+ diagnostics (RL02)
- **m6** Settings menu retarget (rk0..rk3 / dl0..dl1 / config.ini keys / strings)
- **m7** KW11-L line clock + interrupts (mostly already in sam11)
- **m8** Second DL11 serial port tunneling SD-card file I/O (user-requested)
- **m9** Optional RT-11 boot, optional RL11 enable
- **m10** README polish + GitHub push to `deangi/vpdp1140`

## Credits

- CPU core: **sam11** by Chloe Lunn — BSD-3-Clause —
  https://gitlab.com/ChloeLunn/sam11
- sam11 descends from Julius Schmidt's JavaScript PDP-11 emulator and
  Dave Cheney's [avr11](https://dave.cheney.net/2014/01/23/avr11-simulating-minicomputers-on-microcontrollers).
- Host scaffolding: forked from [v8088](../v8088), which itself ports
  Adrian Cable's [8086tiny](https://github.com/adriancable/8086tiny).
- 4×8 font: public-domain IBM VGA font (via dhepper/font8x8).
- Sample diagnostic disk images: pcjs.org —
  https://www.pcjs.org/software/dec/pdp11/disks/rl02k/xxdp/
