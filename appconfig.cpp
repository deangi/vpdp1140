#include "appconfig.h"
#include "config.h"
#include "platform.h"
#include "secrets.h"

#include <SD_MMC.h>
#include <string.h>    // strrchr, strncmp, strcasecmp for variant discovery

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

  cfg.diag_pcping_sec = 5;
  cfg.diag_serialdelay_ms = 20;
  cfg.v4b_quirks      = true;
  cfg.kwp_enabled     = false;

  cfg.disk_a        = DEFAULT_A_IMG;
  cfg.disk_b        = "";
  cfg.disk_c        = DEFAULT_C_IMG;
  cfg.disk_d        = "";
  cfg.disk_rk0      = "";
  cfg.boot_drive    = 'a';
  cfg.boot_kind     = AppConfig::BK_RL;
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
  } else if (section == "diag" || section == "emu") {
    // "emu" kept as an alias for back-compat with the first revision of
    // the parser; "diag" is the canonical section going forward.
    if      (key == "pcping")     cfg.diag_pcping_sec = val.toInt();
    else if (key == "serialdelay") cfg.diag_serialdelay_ms = val.toInt();
    else if (key == "v4b_quirks") cfg.v4b_quirks = (val.equalsIgnoreCase("true") ||
                                                   val == "1" ||
                                                   val.equalsIgnoreCase("yes") ||
                                                   val.equalsIgnoreCase("on"));
    else if (key == "kwp_enabled") cfg.kwp_enabled = (val.equalsIgnoreCase("true") ||
                                                     val == "1" ||
                                                     val.equalsIgnoreCase("yes") ||
                                                     val.equalsIgnoreCase("on"));
  } else if (section == "disks") {
    // Drive slot 0..3 maps to PDP-11 unit names dl0/dl1 (RL02) and
    // dx0/dx1 (RX02). Internally we still index by char 'a'..'d' to
    // keep the slot-array indexing in vpdp1140.ino simple. rk0 is a
    // separate logical key that, when boot=rk0, gets mounted at slot 0
    // in place of dl0 so the RK11 controller can find it.
    if      (key == "dl0")      cfg.disk_a = val;
    else if (key == "dl1")      cfg.disk_b = val;
    else if (key == "dx0")      cfg.disk_c = val;
    else if (key == "dx1")      cfg.disk_d = val;
    else if (key == "rk0")      cfg.disk_rk0 = val;
    else if (key == "boot") {
      String v = to_lower(val);
      cfg.boot_kind = AppConfig::BK_RL;
      if      (v == "dl0" || v == "0")  cfg.boot_drive = 'a';
      else if (v == "dl1" || v == "1")  cfg.boot_drive = 'b';
      else if (v == "dx0" || v == "2")  cfg.boot_drive = 'c';
      else if (v == "dx1" || v == "3")  cfg.boot_drive = 'd';
      // rk0 (DEC) / dk0 (Bell Labs Unix V6 device name) both mean the RK05.
      else if (v == "rk0" || v == "dk0") {
        cfg.boot_drive = 'a';
        cfg.boot_kind  = AppConfig::BK_RK;
      }
      else if (v.length() == 1 && v[0] >= 'a' && v[0] <= 'd')
        cfg.boot_drive = v[0];           // legacy single-char form
      else {
        LOGE("pdpconfig.ini: unknown boot value \"%s\" - using dl0", val.c_str());
        cfg.boot_drive = 'a';
      }
    }
  }
}

// Internal: parse one config file at `path` into cfg through `parse_line`.
// Returns true if the file was opened and parsed; false if it didn't exist.
static bool parse_config_file(AppConfig& cfg, const char* path) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  String section;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    parse_line(cfg, section, line);
  }
  f.close();
  return true;
}

bool config_load_wifi(AppConfig& cfg) {
  // Compiled defaults are already in cfg (caller did
  // config_apply_compiled_defaults). Clear wifi-only fields so a present
  // file overrides them, then fall back to secrets.h for any field left
  // blank.
  cfg.wifi_ssid     = "";
  cfg.wifi_password = "";
  cfg.wifi_hostname = "";

  bool existed = parse_config_file(cfg, WIFI_CFG_PATH);
  if (!existed) {
    LOG("%s not found, writing defaults", WIFI_CFG_PATH);
    // Restore compiled defaults so the writer emits a useful template.
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASS;
    cfg.wifi_hostname = WIFI_HOSTNAME;
    config_write_default_wifi(cfg);
    return false;
  }

  if (cfg.wifi_ssid.length() == 0)     cfg.wifi_ssid     = WIFI_SSID;
  if (cfg.wifi_password.length() == 0) cfg.wifi_password = WIFI_PASS;
  if (cfg.wifi_hostname.length() == 0) cfg.wifi_hostname = WIFI_HOSTNAME;
  return true;
}

bool config_load_pdp(AppConfig& cfg) {
  // Disk paths: clear so a present file overrides; blank lines in the
  // file leave the field empty (= dismounted), which is intended.
  cfg.disk_a = "";
  cfg.disk_b = "";
  cfg.disk_c = "";
  cfg.disk_d = "";
  cfg.disk_rk0 = "";

  bool existed = parse_config_file(cfg, PDP_CFG_PATH);
  if (!existed) {
    LOG("%s not found, writing defaults", PDP_CFG_PATH);
    cfg.disk_a = DEFAULT_A_IMG;
    cfg.disk_c = DEFAULT_C_IMG;
    config_write_default_pdp(cfg);
    return false;
  }
  return true;
}

bool config_write_default_wifi(const AppConfig& cfg) {
  // SD_MMC's FILE_WRITE truncates, which is what we want for a clean rewrite.
  File f = SD_MMC.open(WIFI_CFG_PATH, FILE_WRITE);
  if (!f) {
    LOGE("Could not open %s for write", WIFI_CFG_PATH);
    return false;
  }
  f.println("; vpdp1140 WiFi configuration");
  f.println("; Copy this to wificonfig-NAME.ini to create a named variant");
  f.println("; (then pick it from the Settings -> WiFi Config menu).");
  f.println();
  f.println("[wifi]");
  f.println("; Leave ssid/password blank to use the values compiled into secrets.h.");
  f.println("ssid     = ");
  f.println("password = ");
  f.printf("hostname = %s\r\n", cfg.wifi_hostname.c_str());
  f.close();
  LOG("Wrote default %s", WIFI_CFG_PATH);
  return true;
}

bool config_write_default_pdp(const AppConfig& cfg) {
  File f = SD_MMC.open(PDP_CFG_PATH, FILE_WRITE);
  if (!f) {
    LOGE("Could not open %s for write", PDP_CFG_PATH);
    return false;
  }
  f.println("; vpdp1140 PDP-11 configuration");
  f.println("; Copy this to pdpconfig-NAME.ini to create a named variant");
  f.println("; (then pick it from the Settings -> PDP Config menu).");
  f.println();
  f.println("[system]");
  f.printf("title   = %s\r\n", cfg.title.c_str());
  f.printf("version = %s\r\n", cfg.version.c_str());
  f.printf("build   = %s\r\n", cfg.build.c_str());
  f.println();
  f.println("[telnet]");
  f.printf("enabled = %s\r\n", cfg.telnet_enabled ? "true" : "false");
  f.printf("port    = %d\r\n", cfg.telnet_port);
  f.println();
  f.println("[diag]");
  f.println("; pcping      = seconds between host's periodic PC/register dump");
  f.println(";               to USB-Serial. 0 disables it (so do large values).");
  f.println("; v4b_quirks  = absorb KE11-A (0o772100..0o772176) and TT1");
  f.println(";               (0o776500..0o776516) probes silently. Default");
  f.println(";               true makes RSTS/E V4B + RT-11 + V6 + XXDP boot;");
  f.println(";               set false to attempt RSTS/E V7 (V4B will not");
  f.println(";               boot in that mode).");
  f.println("; kwp_enabled = activate the KW11-P programmable real-time clock");
  f.println(";               at 0o772540. Default false (stub mode) because");
  f.println(";               RSTS V4B sees a working KW11-P and programs it");
  f.println(";               for interrupts that break its terminal echo");
  f.println(";               (upper case shows as lower case). Set true only");
  f.println(";               for RSTS V7 hardware-test bring-up.");
  f.println("; serialdelay = minimum ms between successive characters loaded");
  f.println(";               into the KL11 TKB. Prevents back-to-back addchars");
  f.println(";               while the guest is still inside klrint on the");
  f.println(";               prior byte (which would re-enter klrint on sam11");
  f.println(";               and reverse the order). 0 disables; 10-50 ms");
  f.println(";               typical for V6 / RT-11 / RSTS under a line-");
  f.println(";               buffered host (Arduino IDE Serial Monitor).");
  f.printf("pcping      = %d\r\n", cfg.diag_pcping_sec);
  f.printf("serialdelay = %d\r\n", cfg.diag_serialdelay_ms);
  f.printf("v4b_quirks  = %s\r\n", cfg.v4b_quirks ? "true" : "false");
  f.printf("kwp_enabled = %s\r\n", cfg.kwp_enabled ? "true" : "false");
  f.println();
  f.println("[disks]");
  f.println("; dl0, dl1 = RL02 10 MB removable disk packs.");
  f.println("; dx0, dx1 = RX02 512 KB double-density floppies.");
  f.println("; rk0      = RK05 2.5 MB disk pack (e.g. RT-11).");
  f.println("; When boot=rk0 (or dk0, the Unix V6 name) the rk0 image takes");
  f.println("; slot 0 in place of dl0 so the RK11 controller sees it as drive 0.");
  f.println("; Leave a slot blank to dismount it at boot.");
  f.printf("dl0  = %s\r\n", cfg.disk_a.c_str());
  f.printf("dl1  = %s\r\n", cfg.disk_b.c_str());
  f.printf("dx0  = %s\r\n", cfg.disk_c.c_str());
  f.printf("dx1  = %s\r\n", cfg.disk_d.c_str());
  f.printf("rk0  = %s\r\n", cfg.disk_rk0.c_str());
  // Friendly boot value: dl0/dl1/dx0/dx1/rk0
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else boot_name = (cfg.boot_drive == 'a') ? "dl0"
                 : (cfg.boot_drive == 'b') ? "dl1"
                 : (cfg.boot_drive == 'c') ? "dx0"
                 : (cfg.boot_drive == 'd') ? "dx1" : "dl0";
  f.printf("boot = %s\r\n", boot_name);
  f.close();
  LOG("Wrote default %s", PDP_CFG_PATH);
  return true;
}

bool config_copy_file(const char* src, const char* dst) {
  File s = SD_MMC.open(src, FILE_READ);
  if (!s) { LOGE("config_copy_file: can't open %s for read", src); return false; }
  uint32_t srcSize = (uint32_t)s.size();

  // Defensively remove the destination before opening for write. Some
  // Arduino-ESP32 SD_MMC builds don't truncate on FILE_WRITE when the
  // file already exists, leaving stale trailing bytes from a longer
  // previous version - and those stale bytes would then be parsed at
  // the next boot, silently keeping old settings around.
  if (SD_MMC.exists(dst)) {
    bool removed = SD_MMC.remove(dst);
    LOG("config_copy_file: removed pre-existing %s (%s)",
        dst, removed ? "ok" : "FAILED");
    if (!removed) { s.close(); return false; }
  }

  File d = SD_MMC.open(dst, FILE_WRITE);
  if (!d) {
    LOGE("config_copy_file: can't open %s for write", dst);
    s.close();
    return false;
  }

  uint8_t buf[512];
  size_t total = 0;
  while (s.available()) {
    int n = s.read(buf, sizeof(buf));
    if (n <= 0) break;
    int w = d.write(buf, n);
    if (w != n) {
      LOGE("config_copy_file: short write (%d/%d) at %u into %s",
           w, n, (unsigned)total, dst);
      s.close(); d.close();
      return false;
    }
    total += n;
  }
  s.close();
  d.close();

  // Verify by reopening - tells us whether the bytes actually landed
  // on the SD card and matches what we expect from the source.
  File v = SD_MMC.open(dst, FILE_READ);
  uint32_t verifySize = v ? (uint32_t)v.size() : 0;
  if (v) v.close();
  LOG("config_copy_file: %s -> %s  src=%u  written=%u  on-disk=%u",
      src, dst, (unsigned)srcSize, (unsigned)total, (unsigned)verifySize);
  if (verifySize != srcSize) {
    LOGE("config_copy_file: size mismatch (src %u vs on-disk %u) - copy likely failed",
         (unsigned)srcSize, (unsigned)verifySize);
    return false;
  }
  return true;
}

int config_list_variants(const char* prefix, char names[][44], int max) {
  // Scan SD root for files matching "<prefix>NAME.ini" (case-insensitive
  // on the ".ini" suffix; the prefix is matched as written). Stores the
  // middle NAME portion in names[i]. Skips a file that's exactly the
  // active filename (prefix-without-trailing-dash + ".ini") to keep
  // wificonfig.ini / pdpconfig.ini out of the picker.
  if (max <= 0) return 0;
  int count = 0;

  fs::File root = SD_MMC.open("/");
  if (!root) return 0;

  size_t plen = strlen(prefix);
  for (fs::File f = root.openNextFile(); f && count < max;
       f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char* fullname = f.name();
      const char* slash = strrchr(fullname, '/');
      const char* base  = slash ? slash + 1 : fullname;
      size_t blen = strlen(base);

      // prefix match
      if (strncmp(base, prefix, plen) == 0 &&
          blen > plen + 4 /* at least 1 char + ".ini" */ &&
          strcasecmp(base + blen - 4, ".ini") == 0) {
        size_t midlen = blen - plen - 4;
        if (midlen > 0 && midlen < 43) {
          memcpy(names[count], base + plen, midlen);
          names[count][midlen] = 0;
          count++;
        }
      }
    }
    f.close();
  }
  root.close();
  return count;
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
  LOG("---- /wificonfig.ini + /pdpconfig.ini effective values ----");
  LOG("[system]  title=\"%s\"  version=\"%s\"  build=\"%s\"",
      cfg.title.c_str(), cfg.version.c_str(), cfg.build.c_str());
  LOG("[wifi]    ssid=\"%s\"  hostname=\"%s\"  (password=%d chars)",
      cfg.wifi_ssid.c_str(), cfg.wifi_hostname.c_str(),
      (int)cfg.wifi_password.length());
  LOG("[telnet]  enabled=%s  port=%d",
      cfg.telnet_enabled ? "true" : "false", cfg.telnet_port);
  LOG("[diag]    pcping=%d sec%s  serialdelay=%d ms  v4b_quirks=%s  kwp_enabled=%s",
      cfg.diag_pcping_sec, cfg.diag_pcping_sec <= 0 ? " (disabled)" : "",
      cfg.diag_serialdelay_ms,
      cfg.v4b_quirks  ? "true" : "false",
      cfg.kwp_enabled ? "true (V7 mode)" : "false (V4B-safe)");
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else boot_name = (cfg.boot_drive == 'a') ? "dl0"
                 : (cfg.boot_drive == 'b') ? "dl1"
                 : (cfg.boot_drive == 'c') ? "dx0"
                 : (cfg.boot_drive == 'd') ? "dx1" : "?";
  LOG("[disks]   dl0=\"%s\"  dl1=\"%s\"",
      cfg.disk_a.c_str(), cfg.disk_b.c_str());
  LOG("          dx0=\"%s\"  dx1=\"%s\"",
      cfg.disk_c.c_str(), cfg.disk_d.c_str());
  LOG("          rk0=\"%s\"  boot=%s",
      cfg.disk_rk0.c_str(), boot_name);
  LOG("--------------------------------------");
}
