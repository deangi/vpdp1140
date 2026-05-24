#include "appconfig.h"
#include "config.h"
#include "platform.h"
#include "secrets.h"

#include <SD_MMC.h>

// -------- helpers --------

static String trim(const String& s) {
  int a = 0, b = (int)s.length();
  while (a < b && isspace((uint8_t)s[a])) a++;
  while (b > a && isspace((uint8_t)s[b - 1])) b--;
  return s.substring(a, b);
}

static String to_lower(const String& s) {
  String t = s;
  t.toLowerCase();
  return t;
}

// -------- SD --------

bool sd_mount() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  if (!SD_MMC.begin("/sdcard", false /*1bit*/, false /*format*/, 20000 /*freq*/)) {
    LOGE("SD_MMC.begin() failed");
    return false;
  }
  uint8_t type = SD_MMC.cardType();
  if (type == CARD_NONE) {
    LOGE("No SD card detected");
    return false;
  }
  const char* tname = (type == CARD_MMC)  ? "MMC"
                    : (type == CARD_SD)   ? "SDSC"
                    : (type == CARD_SDHC) ? "SDHC"
                                          : "?";
  uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  LOG("SD mounted: type=%s size=%llu MB", tname, (unsigned long long)mb);
  return true;
}

// -------- defaults --------

void config_apply_compiled_defaults(AppConfig& cfg) {
  cfg.title         = APP_TITLE;
  cfg.version       = APP_VERSION;
  cfg.build         = APP_BUILD_DATE;

  cfg.wifi_ssid     = WIFI_SSID;
  cfg.wifi_password = WIFI_PASS;
  cfg.wifi_hostname = WIFI_HOSTNAME;

  cfg.telnet_enabled = true;
  cfg.telnet_port    = TELNET_PORT;

  cfg.disk_a        = DEFAULT_A_IMG;
  cfg.disk_b        = "";
  cfg.disk_c        = DEFAULT_C_IMG;
  cfg.disk_d        = "";
  cfg.boot_drive    = 'a';
}

// -------- parser --------

static void parse_line(AppConfig& cfg, String& section, const String& raw) {
  String t = trim(raw);
  if (t.length() == 0) return;
  if (t.startsWith(";") || t.startsWith("#")) return;
  if (t.startsWith("[") && t.endsWith("]")) {
    section = to_lower(t.substring(1, t.length() - 1));
    return;
  }
  int eq = t.indexOf('=');
  if (eq < 0) return;

  String key = to_lower(trim(t.substring(0, eq)));
  String val = trim(t.substring(eq + 1));
  // Strip trailing inline comments
  int hashOrSemi = -1;
  for (int i = 0; i < (int)val.length(); i++) {
    if (val[i] == ';' || val[i] == '#') { hashOrSemi = i; break; }
  }
  if (hashOrSemi >= 0) val = trim(val.substring(0, hashOrSemi));

  if (section == "system") {
    if      (key == "title")    cfg.title   = val;
    else if (key == "version")  cfg.version = val;
    else if (key == "build")    cfg.build   = val;
  } else if (section == "wifi") {
    if      (key == "ssid")     cfg.wifi_ssid     = val;
    else if (key == "password") cfg.wifi_password = val;
    else if (key == "hostname") cfg.wifi_hostname = val;
  } else if (section == "telnet") {
    if      (key == "enabled")  cfg.telnet_enabled = (val.equalsIgnoreCase("true") ||
                                                     val == "1" ||
                                                     val.equalsIgnoreCase("yes") ||
                                                     val.equalsIgnoreCase("on"));
    else if (key == "port")     cfg.telnet_port = val.toInt();
  } else if (section == "disks") {
    if      (key == "a")        cfg.disk_a = val;
    else if (key == "b")        cfg.disk_b = val;
    else if (key == "c")        cfg.disk_c = val;
    else if (key == "d")        cfg.disk_d = val;
    else if (key == "boot")     cfg.boot_drive = val.length() ? (char)tolower((uint8_t)val[0]) : 'a';
  }
}

bool config_load(AppConfig& cfg) {
  config_apply_compiled_defaults(cfg);

  File f = SD_MMC.open(CFG_PATH, FILE_READ);
  if (!f) {
    LOG("%s not found, writing defaults", CFG_PATH);
    config_write_defaults(cfg);
    return false;
  }

  // Override defaults with file contents. Blank values in the file are
  // left as empty string; we patch back to compiled defaults below.
  AppConfig parsed = cfg;
  parsed.wifi_ssid = "";
  parsed.wifi_password = "";
  parsed.wifi_hostname = "";
  parsed.disk_a = "";
  parsed.disk_b = "";
  parsed.disk_c = "";
  parsed.disk_d = "";

  String section;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    parse_line(parsed, section, line);
  }
  f.close();

  // Fall back to secrets.h for any blank WiFi fields
  if (parsed.wifi_ssid.length() == 0)     parsed.wifi_ssid     = WIFI_SSID;
  if (parsed.wifi_password.length() == 0) parsed.wifi_password = WIFI_PASS;
  if (parsed.wifi_hostname.length() == 0) parsed.wifi_hostname = WIFI_HOSTNAME;

  cfg = parsed;
  return true;
}

bool config_write_defaults(const AppConfig& cfg) {
  // SD_MMC's FILE_WRITE truncates, which is what we want for a clean rewrite.
  File f = SD_MMC.open(CFG_PATH, FILE_WRITE);
  if (!f) {
    LOGE("Could not open %s for write", CFG_PATH);
    return false;
  }
  f.println("; vpdp1140 configuration file");
  f.println("; Edit values then power-cycle the board.");
  f.println("; Lines starting with ; or # are comments.");
  f.println();
  f.println("[system]");
  f.printf("title   = %s\r\n", cfg.title.c_str());
  f.printf("version = %s\r\n", cfg.version.c_str());
  f.printf("build   = %s\r\n", cfg.build.c_str());
  f.println();
  f.println("[wifi]");
  f.println("; Leave ssid/password blank to use the values compiled into secrets.h.");
  f.println("ssid     = ");
  f.println("password = ");
  f.printf("hostname = %s\r\n", cfg.wifi_hostname.c_str());
  f.println();
  f.println("[telnet]");
  f.printf("enabled = %s\r\n", cfg.telnet_enabled ? "true" : "false");
  f.printf("port    = %d\r\n", cfg.telnet_port);
  f.println();
  f.println("[disks]");
  f.println("; a, b = 1.44 MB floppy images.   c, d = 32 MB HDD images.");
  f.println("; Leave a slot blank to dismount it at boot.");
  f.printf("a    = %s\r\n", cfg.disk_a.c_str());
  f.printf("b    = %s\r\n", cfg.disk_b.c_str());
  f.printf("c    = %s\r\n", cfg.disk_c.c_str());
  f.printf("d    = %s\r\n", cfg.disk_d.c_str());
  f.printf("boot = %c\r\n", cfg.boot_drive);
  f.close();
  LOG("Wrote default %s", CFG_PATH);
  return true;
}

// -------- disk image creation --------

bool ensure_disk_image(const char* path, uint32_t bytes,
                       bool create_if_missing, const char* label) {
  if (!path || !*path) return false;
  if (SD_MMC.exists(path)) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
      LOGE("[%s] exists but cannot open %s", label, path);
      return false;
    }
    uint32_t sz = (uint32_t)f.size();
    f.close();
    if (sz == bytes) {
      LOG("[%s] %s OK (%u bytes)", label, path, (unsigned)sz);
      return true;
    }
    LOGE("[%s] %s wrong size: %u (expected %u) - leaving file alone",
         label, path, (unsigned)sz, (unsigned)bytes);
    return false;
  }
  if (!create_if_missing) {
    LOG("[%s] %s missing (not auto-created)", label, path);
    return false;
  }
  LOG("[%s] creating zeroed %s (%u bytes) - this can take a while ...",
      label, path, (unsigned)bytes);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    LOGE("[%s] could not create %s", label, path);
    return false;
  }
  static uint8_t buf[4096] = {0};
  uint32_t remaining = bytes;
  uint32_t t0 = millis();
  uint32_t lastReport = t0;
  while (remaining) {
    size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    size_t w = f.write(buf, n);
    if (w != n) {
      LOGE("[%s] write failed at offset %u", label, (unsigned)(bytes - remaining));
      f.close();
      return false;
    }
    remaining -= n;
    if (millis() - lastReport >= 2000) {
      uint32_t done = bytes - remaining;
      LOG("[%s]  ... %u / %u bytes (%u%%)",
          label, (unsigned)done, (unsigned)bytes, (unsigned)(done * 100ULL / bytes));
      lastReport = millis();
    }
  }
  f.close();
  LOG("[%s] created %s in %u ms", label, path, (unsigned)(millis() - t0));
  return true;
}

// -------- printer --------

void config_print(const AppConfig& cfg) {
  LOG("---- /config.ini effective values ----");
  LOG("[system]  title=\"%s\"  version=\"%s\"  build=\"%s\"",
      cfg.title.c_str(), cfg.version.c_str(), cfg.build.c_str());
  LOG("[wifi]    ssid=\"%s\"  hostname=\"%s\"  (password=%d chars)",
      cfg.wifi_ssid.c_str(), cfg.wifi_hostname.c_str(),
      (int)cfg.wifi_password.length());
  LOG("[telnet]  enabled=%s  port=%d",
      cfg.telnet_enabled ? "true" : "false", cfg.telnet_port);
  LOG("[disks]   a=\"%s\"  b=\"%s\"",
      cfg.disk_a.c_str(), cfg.disk_b.c_str());
  LOG("          c=\"%s\"  d=\"%s\"  boot=%c",
      cfg.disk_c.c_str(), cfg.disk_d.c_str(), cfg.boot_drive);
  LOG("--------------------------------------");
}
