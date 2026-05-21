# v8088 — an 8088 / MS-DOS emulator for the ESP32-S3

v8088 turns a **Freenove ESP32-S3 2.8" Display** board into a tiny IBM-PC-compatible
machine that boots **MS-DOS 3.31** from disk images on an SD card. The DOS console
appears on the onboard TFT, and simultaneously over Telnet and USB serial.

It ports Adrian Cable's [8086tiny](https://github.com/adriancable/8086tiny) CPU core
to the ESP32-S3, with 1 MB of guest RAM in PSRAM.

## Features

- 8088 CPU emulation booting real MS-DOS 3.31 (~0.6 MIPS — close to a real 4.77 MHz PC)
- TFT console: 80×25 ANSI terminal, 4×8 font
- Telnet server **and** USB serial as additional live consoles
- Four drives — A:/B: 1.44 MB floppies, C:/D: 32 MB hard disks — via INT 13h interception
- Capacitive-touch settings menu: mount / dismount / create disk images, reboot, brightness
- `/config.ini` on the SD card for WiFi, Telnet and disk configuration
- RGB status LED: red = starting, blue = booting, green = ready

## Hardware

- **Freenove ESP32-S3 WROVER 2.8" Display** board (FNK0104B): ILI9341 TFT,
  FT6336U capacitive touch, micro-SD slot, 8 MB Octal PSRAM, 16 MB flash.

## Building

Arduino IDE with the ESP32 board package. Required libraries:

- **TFT_eSPI** — with the `FNK0104B` setup enabled in `User_Setup_Select.h`
  (the same setup used by the Freenove tutorials)
- **FT6336U** — the Freenove-bundled touch library
- **Freenove_WS2812_Lib_for_ESP32**

### Tools menu settings (important)

| Setting            | Value                              |
|--------------------|------------------------------------|
| Board              | **ESP32S3 Dev Module** (not "Octal") |
| USB CDC On Boot    | **Enabled**                        |
| PSRAM              | **OPI PSRAM**                      |
| Flash Size         | 16MB (128Mb)                       |
| Partition Scheme   | Huge App (3MB)                     |

Selecting an "...Octal" board variant, or PSRAM set to anything but OPI, will
bootloop the board.

## SD card layout

```
/config.ini          settings (auto-created with defaults if missing)
/MSDOS331boot.dsk     a bootable 1.44 MB MS-DOS 3.31 floppy image
/hd_c_32mb.hdd        32 MB hard-disk image (auto-created, then FDISK + FORMAT)
/<name>.dsk /.hdd     additional floppy / hard-disk images
```

A bootable MS-DOS 3.31 floppy image can be obtained from winworldpc.com.

## config.ini

```ini
[system]
title   = v8088
version = 1.0

[wifi]
ssid     = YourNetwork
password = YourPassword
hostname = v8088

[telnet]
enabled = true
port    = 23

[disks]
a    = /MSDOS331boot.dsk    ; A: floppy
b    =                     ; B: floppy (empty)
c    = /hd_c_32mb.hdd       ; C: hard disk
d    =                     ; D: hard disk (empty)
boot = a
```

## Using it

- The DOS console shows on the TFT and is reachable at `telnet <board-ip> 23`
  and on the USB serial port (115200 baud).
- **Settings menu:** double-tap the screen, or press the onboard button.
  From there you can mount/dismount/create disk images, reboot the 8088,
  adjust brightness, and view WiFi / Telnet status.
- New hard-disk images are blank — run `FDISK` then `FORMAT C: /S` from DOS.
  New floppy images are created pre-formatted (FAT12) and ready to use.

## Credits

- CPU core & BIOS: [8086tiny](https://github.com/adriancable/8086tiny) by
  Adrian Cable (MIT License)
- 8×8 font: public-domain IBM VGA font (via dhepper/font8x8)
