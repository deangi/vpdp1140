#pragma once
#include <Arduino.h>

struct AppConfig {
  // [system]
  String title;
  String version;
  String build;

  // [wifi]
  String wifi_ssid;
  String wifi_password;
  String wifi_hostname;

  // [telnet]
  bool   telnet_enabled = true;
  int    telnet_port    = 23;

  // [diag]
  // Interval (seconds) for the host's periodic "state: PC=... R0=..." dump
  // to USB-Serial. 0 disables it; large values (e.g. 9999) effectively do
  // the same. Useful to set short for live debugging or to silence when
  // capturing other output. Default 5.
  int    diag_pcping_sec = 5;

  // [compat]
  // V4B (RSTS/E V4B) requires its probe-by-write of two non-emulated
  // device ranges to be silently absorbed in dd11 (KE11-A EAE at
  // 0o772100..0o772176 and the second DL11 / TT1 at 0o776500..0o776516).
  // Without absorbs, V4B's bus-error handler unconditionally HALTs.
  // RSTS V7 is the opposite case: with the TT1 absorb V7's INIT.SYS
  // sees a phantom DL11, allocates a floating vector for it, and later
  // critical devices (RK, RL) collide on the next vector and disable.
  // Default true (V4B + V6 + XXDP + RT-11 all work). Set to false to
  // attempt RSTS V7; V4B will not boot in that mode.
  bool   v4b_quirks = true;

  // [disks]
  // slot 0..3 holds whatever the host has mounted at DL0/DL1/DX0/DX1 -
  // RL controller sees them as RL02/RX02 packs.
  String disk_a;
  String disk_b;
  String disk_c;
  String disk_d;
  // Optional RK05 image. When boot_kind == BK_RK we mount this at slot 0
  // (overriding disk_a) so the RK11 controller can find it as RK drive 0.
  String disk_rk0;
  // Boot drive: which of the four physical slots is the boot disk (encoded
  // as 'a'..'d' for slot 0..3). boot_kind tells cpu_reset() which boot ROM
  // to install + which controller is being used (RL11 vs RK11).
  char   boot_drive = 'a';
  enum BootKind { BK_RL = 0, BK_RK = 1 };
  uint8_t boot_kind = BK_RL;
};

// SD card
bool sd_mount();

// Config file IO
bool config_load(AppConfig& cfg);                            // returns true if /config.ini existed and parsed
bool config_write_defaults(const AppConfig& cfg);            // writes a fresh /config.ini
void config_apply_compiled_defaults(AppConfig& cfg);         // fills cfg with secrets.h + config.h defaults

// Disk image management
bool ensure_disk_image(const char* path, uint32_t bytes,
                       bool create_if_missing,
                       const char* label);                   // returns true if usable image present

// Logging helper
void config_print(const AppConfig& cfg);
