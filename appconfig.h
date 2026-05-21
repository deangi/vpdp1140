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

  // [disks]
  String disk_a;       // floppy A (1.44 MB)
  String disk_b;       // floppy B
  String disk_c;       // HDD C (32 MB)
  String disk_d;       // HDD D
  char   boot_drive = 'a';
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
