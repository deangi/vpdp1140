//-------------------------------------------------------------------------------
// vpdp1140 - DEC PDP-11/40 emulator on Freenove ESP32-S3 2.8" Display
//
// Cloned from v8088 (Intel 8088 / MS-DOS 3.31 emulator) on 2026-05-23 and
// retargeted to emulate a Digital Equipment Corporation PDP-11/40 with
// 248 KiB of guest memory (the 18-bit MMU max), RK05 / RL02 disks, KL11
// console. Originally planned as an 11/70 with 1 MB / 22-bit MMU; reset
// to 11/40 + 248 KiB on 2026-05-23 after discovering the vendored CPU
// core (sam11) is 11/40-only and the 22-bit MMU would have to be written.
//
// CPU core is vendored from Chloe Lunn's sam11 (BSD-3), which descends
// from Julius Schmidt's JS pdp11 -> Dave Cheney's avr11. Target guest OS
// is V6 Unix (sam11's tested config) and/or XXDP+ diagnostics + RT-11.
//
// The ESP32-S3 host scaffolding (TFT console, telnet, USB serial, SD images,
// touch settings menu, wificonfig.ini + pdpconfig.ini, dual-core split)
// carries over from v8088 unchanged; only the CPU core, I/O page dispatch,
// and disk/console wiring are PDP-11-specific.
//
// Requires the TFT_eSPI library to have FNK0104B selected in
// User_Setup_Select.h (this is the same setup used by all Freenove
// tutorials for this board).
// Board: Freenove ESP32-S3 with 2.8" TFT and capacitive touch screen
//  (WROVER2 w/PSRAM, 4Mb flash)
// Board: ESP32S3 Dev Module
// Partitioning: Huge App (3mb app, 1mb spiffs)
// Need to go to tools : USB CDC on boot - enable, enable OPI PSRAM, set flash to 16MB
// V1.0 23-May-2026, Dean Gienger, Claude
// Set up to boot from a RL02 disk (10mb) - eventually support 2 RL02 disks (DL0 and DL1)
// and 2 floppy's DX0/DX1 (RX02 model 512kb)
//
// RL02K disks are single-platter cartridges with 512 tracks per side, 40 sectors per track, and 
// a sector size of 256 bytes, for a total capacity of 10Mb (10,485,760 bytes). They are used in 
// RL02 disk drives in conjunction with an RL11 Disk Controller.  These are front loading disk
// cartidges.  Round, about 15" diameter, about 3-4" thick.
//
// Sample disks: https://www.pcjs.org/software/dec/pdp11/disks/rl02k/xxdp/

//------------------------------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include "Freenove_WS2812_Lib_for_ESP32.h"

#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "appconfig.h"
#include "cpu_pdp11.h"
#include "kl11.h"        // kl11::drain_serial_out() in loop()
#include "dd11.h"  // dd11::v4b_quirks_enabled gate
#include "kwp.h"   // kwp::enabled gate
#include "disk.h"
#include "console.h"
#include "telnet.h"
#include "touch.h"
#include "ui.h"

static TFT_eSPI tft;
static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);
AppConfig cfg;             // non-static so ui.cpp (System Info screen,
                           // title display) can read it via the extern in
                           // appconfig.h. Only vpdp1140.ino writes it.
static bool sd_ok = false;
static bool cpu_running = false;   // true once the PDP-11 is booting in loop()

// The PDP-11 runs on core 1 (loop); all TFT rendering runs on core 0
// (render_task). The settings menu is the only shared mutable UI state -
// this mutex guards it. The 80x25 console grid is updated lock-free; a
// torn read just produces one self-correcting frame.
static SemaphoreHandle_t g_ui_mutex = nullptr;

enum BootState { BOOT_RUNNING, BOOT_OK, BOOT_FAIL };
static BootState boot_state = BOOT_RUNNING;

static void led(uint8_t r, uint8_t g, uint8_t b) {
  strip.setLedColorData(0, r, g, b);
  strip.show();
}

// Re-draw just the title row (top 22 px). Called once at boot before the
// config is loaded (shows APP_TITLE) and again after config_load_pdp so
// [system] title = ... from pdpconfig.ini takes effect on the boot screen.
static void tft_banner_title() {
  tft.fillRect(0, 0, TFT_W, 22, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  tft.printf("%s  v%s", title, APP_VERSION);
}

static void tft_banner() {
  tft.fillScreen(TFT_BLACK);
  tft_banner_title();
  tft.setCursor(4, 22);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("build %s", APP_BUILD_DATE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void tft_status(int row, const char* label, const char* value, uint16_t color) {
  int y = 50 + row * 18;
  tft.fillRect(0, y, TFT_W, 18, TFT_BLACK);
  tft.setCursor(4, y);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.print(label);
  tft.setTextColor(color, TFT_BLACK);
  tft.print(value);
}

// Row map for the boot status display:
//   row 0 = PSRAM
//   row 1 = SD card
//   row 2 = /wificonfig.ini + /pdpconfig.ini
//   row 3 = boot drive image
//   row 4 = WiFi
//   row 5 = IP
enum {
  ROW_PSRAM = 0, ROW_SD, ROW_CFG, ROW_BOOT, ROW_WIFI, ROW_IP, ROW_CPU
};

// Boot drive unit label (e.g. "DL0", "DL1", "DX0", "DX1", "RK0") and the
// configured image path for the slot named by cfg.boot_drive ('a'..'d').
static const char* boot_unit_label() {
  int slot = (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
               ? (cfg.boot_drive - 'a') : 0;
  static const char* rl_names[4] = { "DL0", "DL1", "DX0", "DX1" };
  if (slot == 0 && cfg.boot_kind == AppConfig::BK_RK) return "RK0";
  return rl_names[slot];
}
static const String& boot_image_path() {
  int slot = (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
               ? (cfg.boot_drive - 'a') : 0;
  if (slot == 0 && cfg.boot_kind == AppConfig::BK_RK
      && cfg.disk_rk0.length()) return cfg.disk_rk0;
  const String* paths[4] = { &cfg.disk_a, &cfg.disk_b, &cfg.disk_c, &cfg.disk_d };
  return *paths[slot];
}

static void wifi_connect() {
  const char* ssid = cfg.wifi_ssid.c_str();
  const char* pass = cfg.wifi_password.c_str();
  const char* host = cfg.wifi_hostname.length() ? cfg.wifi_hostname.c_str() : WIFI_HOSTNAME;

  if (cfg.wifi_ssid.length() == 0) {
    LOGE("WiFi SSID is empty - set [wifi] ssid= in /wificonfig.ini");
    tft_status(ROW_WIFI, "WiFi:  ", "no SSID in wificonfig.ini", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);

  LOG("WiFi connecting to \"%s\" (hostname=%s) ...", ssid, host);
  tft_status(ROW_WIFI, "WiFi:  ", "connecting...", TFT_YELLOW);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    LOG("WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
    tft_status(ROW_WIFI, "WiFi:  ", ssid, TFT_GREEN);
    tft_status(ROW_IP,   "IP:    ", WiFi.localIP().toString().c_str(), TFT_GREEN);
    boot_state = BOOT_OK;
  } else {
    LOGE("WiFi connect timed out");
    tft_status(ROW_WIFI, "WiFi:  ", "FAILED", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
  }
}

static void sd_and_config_init() {
  tft_status(ROW_SD,  "SD:    ", "mounting...", TFT_YELLOW);
  if (sd_mount()) {
    char info[32];
    uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    snprintf(info, sizeof(info), "OK  %llu MB", (unsigned long long)mb);
    tft_status(ROW_SD, "SD:    ", info, TFT_GREEN);
    sd_ok = true;
  } else {
    tft_status(ROW_SD, "SD:    ", "FAILED",     TFT_RED);
    sd_ok = false;
  }

  tft_status(ROW_CFG, "Cfg:   ", "(reading)", TFT_YELLOW);
  if (!sd_ok) {
    config_apply_compiled_defaults(cfg);
    tft_status(ROW_CFG, "Cfg:   ", "defaults (no SD)", TFT_YELLOW);
  } else {
    config_apply_compiled_defaults(cfg);
    bool wifi_existed = config_load_wifi(cfg);
    bool pdp_existed  = config_load_pdp(cfg);
    const char* msg =
        (wifi_existed && pdp_existed) ? "loaded wifi+pdp"
      : (wifi_existed)                ? "wrote default pdpconfig"
      : (pdp_existed)                 ? "wrote default wificonfig"
                                      : "wrote defaults (both)";
    uint16_t col = (wifi_existed && pdp_existed) ? TFT_GREEN : TFT_YELLOW;
    tft_status(ROW_CFG, "Cfg:   ", msg, col);
  }
  config_print(cfg);

  // Push the V4B-quirks flag down to dd11 so its probe-absorb ranges
  // honor what pdpconfig.ini said. Must happen before cpu_reset() / any
  // guest memory access.
  dd11::v4b_quirks_enabled = cfg.v4b_quirks;
  kwp::enabled             = cfg.kwp_enabled;
  kl11::serial_in_delay_ms = (uint32_t)(cfg.diag_serialdelay_ms < 0 ? 0
                                      : cfg.diag_serialdelay_ms);

  // Show the boot drive's image path (e.g. "Boot DL0:" / "Boot RK0:").
  char boot_label[16];
  snprintf(boot_label, sizeof(boot_label), "Boot %s:", boot_unit_label());
  tft_status(ROW_BOOT, boot_label, "checking...", TFT_YELLOW);
  const String& bpath = boot_image_path();
  if (!sd_ok) {
    tft_status(ROW_BOOT, boot_label, "skipped (no SD)", TFT_DARKGREY);
  } else if (bpath.length() == 0) {
    tft_status(ROW_BOOT, boot_label, "(no image)", TFT_RED);
  } else {
    tft_status(ROW_BOOT, boot_label,
               SD_MMC.exists(bpath) ? bpath.c_str() : "MISSING",
               SD_MMC.exists(bpath) ? TFT_GREEN : TFT_RED);
  }
}

// Mount the four guest drives from /pdpconfig.ini paths.
// When boot=rk0 we substitute disk_rk0 into slot 0 (so the RK11 controller
// sees the RK05 image as drive 0) and the corresponding unit name flips
// from "DL0" to "RK0".
static void disks_mount() {
  if (!sd_ok) { LOGE("disks_mount: SD not available"); return; }
  const bool rk_boot = (cfg.boot_kind == AppConfig::BK_RK);
  const String* paths[DRIVE_COUNT] = {
    rk_boot && cfg.disk_rk0.length() ? &cfg.disk_rk0 : &cfg.disk_a,
    &cfg.disk_b, &cfg.disk_c, &cfg.disk_d
  };
  const char* unit_names[DRIVE_COUNT] = {
    rk_boot ? "RK0" : "DL0", "DL1", "DX0", "DX1"
  };
  for (int s = 0; s < DRIVE_COUNT; s++) {
    if (paths[s]->length() == 0) continue;
    bool ok = disk_mount(s, paths[s]->c_str());
    LOG("disks_mount %s: \"%s\" -> %s",
        unit_names[s], paths[s]->c_str(), ok ? "mounted" : "FAILED");
  }
}

// Status bar drawn in the 40 px strip below the 80x25 console: drive activity
// indicators, IP address, telnet state and emulation speed.
static void draw_status_bar() {
  static uint32_t prev_io[DRIVE_COUNT] = {0, 0, 0, 0};
  static uint32_t prev_inst = 0;
  static uint32_t prev_ms   = 0;
  const int sy = CON_ROWS * CELL_H;          // 200

  tft.drawFastHLine(0, sy, TFT_W, TFT_DARKGREY);

  // Drive indicators DL0/DL1/DX0/DX1 (or RK0/DL1/DX0/DX1 when booting RK05) -
  // green=mounted, yellow=active, dim=empty.
  const char* unit_labels[DRIVE_COUNT] = {
    (cfg.boot_kind == AppConfig::BK_RK) ? "RK0" : "DL0",
    "DL1", "DX0", "DX1"
  };
  for (int s = 0; s < DRIVE_COUNT; s++) {
    uint32_t r = 0, w = 0;
    disk_stats(s, &r, &w);
    bool active = (r + w) != prev_io[s];
    prev_io[s] = r + w;
    uint16_t col = !disk_is_mounted(s) ? 0x2945
                 : active             ? TFT_YELLOW
                                      : TFT_GREEN;
    int bx = 6 + s * 36;
    tft.fillRoundRect(bx, sy + 5, 32, 16, 2, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(unit_labels[s], bx + 16, sy + 13, 1);
  }
  tft.setTextDatum(TL_DATUM);

  // Emulation speed over the last interval.
  uint32_t now  = millis();
  uint32_t inst = cpu_inst_count();
  float mips = 0.0f;
  if (prev_ms && now > prev_ms && inst >= prev_inst)
    mips = (float)(inst - prev_inst) / (float)(now - prev_ms) / 1000.0f;
  prev_inst = inst;
  prev_ms   = now;

  tft.fillRect(156, sy + 1, TFT_W - 156, TFT_H - sy - 1, TFT_BLACK);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_WHITE : TFT_RED, TFT_BLACK);
  tft.drawString(WiFi.status() == WL_CONNECTED
                   ? WiFi.localIP().toString().c_str() : "WiFi down",
                 158, sy + 6, 1);
  // TELNET indicator stays anchored to the left of the right column;
  // MIPS gets right-aligned to the screen edge via TR_DATUM so it's
  // always at the rightmost column regardless of how many digits.
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(telnet_connected() ? "TELNET" : "      ",
                 158, sy + 22, 1);
  char mips_str[16];
  snprintf(mips_str, sizeof(mips_str), "%.2f MIPS", mips);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(mips_str, TFT_W - 4, sy + 22, 1);
  tft.setTextDatum(TL_DATUM);   // restore for the title row below

  // [system] title from pdpconfig.ini, drawn below the drive indicators
  // (left half, the empty strip under the DL0/DL1/DX0/DX1 boxes). Falls
  // back to APP_TITLE if the user left the field blank.
  tft.fillRect(0, sy + 22, 156, TFT_H - sy - 22, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  tft.drawString(title, 6, sy + 24, 2);
}

// Boot (or reboot) the PDP-11 with the currently-mounted drives. cold=true
// re-stamps the bootstrap ROM into high memory and re-zeros guest RAM (used
// by the "Reboot PDP-11" menu item). PDP-11 has no BIOS - boot is just "PC :=
// bootstrap entry"; the ROM is responsible for loading the disk's first
// block and jumping into it.
static void start_cpu(bool cold) {
  if (cold) cpu_cold_boot();
  else      cpu_reset();

  // A fresh boot re-reads every disk; clear stale media-change flags so the
  // boot-block reads don't come back as "disk changed".
  for (int s = 0; s < DRIVE_COUNT; s++) disk_take_change(s);

  // m0 stub: PC defaults to 0 from cpu_reset(). m3+ will stamp a bootstrap
  // ROM into high memory and cpu_set_pc() to its entry point here.

  console_init();
  console_force_redraw();   // render_task repaints the whole console + status bar
}

// ---- mutex-guarded UI calls (menu state is shared core1 <-> core0) ----
static void ui_open_locked() {
  xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
  ui_open();
  xSemaphoreGive(g_ui_mutex);
}
static void ui_tap_locked(int x, int y) {
  xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
  ui_handle_tap(x, y);
  xSemaphoreGive(g_ui_mutex);
}

// ---- core 0: all TFT rendering ----
static void render_task(void* arg) {
  (void)arg;
  bool     was_open  = false;
  uint32_t status_ms = 0;
  for (;;) {
    bool open = ui_is_open();
    if (was_open && !open) {
      // Menu just closed: clear the strip below the console, full repaint.
      tft.fillRect(0, CON_ROWS * CELL_H, TFT_W, TFT_H - CON_ROWS * CELL_H,
                   TFT_BLACK);
      console_force_redraw();
      status_ms = 0;
    }
    was_open = open;

    if (open) {
      xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
      ui_draw(tft);
      xSemaphoreGive(g_ui_mutex);
    } else {
      console_render(tft);
      uint32_t now = millis();
      if (now - status_ms >= 500) { status_ms = now; draw_status_bar(); }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  int i;
  delay(200);
  Serial.begin(115200);
  // ESP32-S3 native USB CDC re-enumerates after the post-flash reset; the host
  // serial monitor needs a moment to reconnect. 
  for (i = 0; i < 3; i++)
  {
    delay(1000);
    Serial.println(i);
  }  
  Serial.println();
  LOG("%s v%s build %s", APP_TITLE, APP_VERSION, APP_BUILD_DATE);

  strip.begin();
  strip.setBrightness(20);
  led(32, 0, 0);  // red while booting

  tft.init();
  tft.setRotation(1);     // landscape 320x240
  tft_banner();

  // PSRAM line first
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(ESP.getPsramSize() / 1024));
    tft_status(ROW_PSRAM, "PSRAM: ", buf,
               ESP.getPsramSize() ? TFT_GREEN : TFT_RED);
  }
  tft_status(ROW_SD,   "SD:    ", "(pending)", TFT_DARKGREY);
  tft_status(ROW_CFG,  "Cfg:   ", "(pending)", TFT_DARKGREY);
  {
    char boot_label[16];
    snprintf(boot_label, sizeof(boot_label), "Boot %s:", boot_unit_label());
    tft_status(ROW_BOOT, boot_label, "(pending)", TFT_DARKGREY);
  }
  tft_status(ROW_WIFI, "WiFi:  ", "(pending)", TFT_DARKGREY);
  tft_status(ROW_IP,   "IP:    ", "(none)",    TFT_DARKGREY);
  tft_status(ROW_CPU,  "CPU:   ", "(pending)", TFT_DARKGREY);

  // Allocate the PDP-11's 256 KiB guest RAM before WiFi/SD take their share.
  tft_status(ROW_CPU, "CPU:   ", "init...", TFT_YELLOW);
  bool cpu_ok = cpu_init();

  // m1 acceptance test: prove sam11 actually executes PDP-11 instructions
  // in our PSRAM. Writes MOV/MOV/ADD/BR at 02000 and asserts R0/R1 state.
  // start_cpu() below re-runs kd11::reset() which clobbers the test program
  // with the RK0 bootrom, so this is a one-shot diagnostic.
  if (cpu_ok) {
    bool selftest_ok = cpu_selftest();
    tft_status(ROW_CPU, "CPU:   ",
               selftest_ok ? "selftest PASS" : "selftest FAIL",
               selftest_ok ? TFT_GREEN : TFT_RED);
  }

  sd_and_config_init();
  tft_banner_title();        // refresh banner with cfg.title from pdpconfig.ini

  wifi_connect();

  // ---- boot: mount drives, start peripherals, hand off to the console loop ----
  if (cpu_ok) {
    disks_mount();
    telnet_begin(cfg.telnet_port, cfg.telnet_enabled);
    pinMode(BUTTON_PIN, INPUT_PULLUP);   // onboard button opens the menu
    touch_init();
    ui_init();
    const char* boot_name;
    if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "RK0";
    else boot_name = (cfg.boot_drive == 'a') ? "DL0"
                   : (cfg.boot_drive == 'b') ? "DL1"
                   : (cfg.boot_drive == 'c') ? "DX0"
                   : (cfg.boot_drive == 'd') ? "DX1" : "?";
    cpu_set_boot_kind(cfg.boot_kind == AppConfig::BK_RK ? 1 : 0);
    LOG("--- booting PDP-11 from %s, console -> TFT ---", boot_name);
    start_cpu(false);
    cpu_running = true;
    led(0, 0, 32);          // blue = PDP-11 booting

    // Spin up the core-0 rendering task. The PDP-11 then runs in loop() on core 1.
    g_ui_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(render_task, "render", 10240, NULL, 1, NULL, 0);
  } else {
    tft_status(ROW_CPU, "CPU:   ", "alloc FAILED", TFT_RED);
    led(32, 0, 0);
  }
}

// loop() runs on core 1 and IS the PDP-11: CPU emulation plus telnet, touch
// and the settings-menu logic. It never touches the TFT - render_task (core
// 0) owns the display.
// Touch handling lives at file scope so cpu_run's per-slice work and the
// menu-open early-return path can share the same state. The 750 ms
// window is wider than the original 450 ms because users were missing
// the second tap of a fast double-tap when the timer rolled over.
static uint32_t g_last_tap_ms = 0;
#define UI_DOUBLE_TAP_MS 750

// Poll the touchscreen once. When the menu is open, route the tap into
// the menu; when closed, accumulate it as a double-tap candidate that
// opens the menu when two taps land within UI_DOUBLE_TAP_MS of each
// other. Called from loop() once per slice so a fast double-tap can't
// fall between two touch_poll calls (cpu_run runs ~40 ms total, and
// the FT6336U doesn't buffer events for us).
static void poll_touch_once() {
  int tx, ty;
  if (!touch_poll(&tx, &ty)) return;
  if (ui_is_open()) {
    ui_tap_locked(tx, ty);
    return;
  }
  uint32_t now = millis();
  if ((uint32_t)(now - g_last_tap_ms) < UI_DOUBLE_TAP_MS) {
    ui_open_locked();
    g_last_tap_ms = 0;
  } else {
    g_last_tap_ms = now;
  }
}

void loop() {
  if (!cpu_running) { delay(100); return; }

  static bool     boot_done = false;
  static bool     btn_prev  = true;
  static uint32_t wifi_ms   = 0;

  poll_touch_once();

  // Onboard button (GPIO0, active low): press opens the menu.
  bool btn_now = digitalRead(BUTTON_PIN);
  if (btn_prev && !btn_now && !ui_is_open()) ui_open_locked();
  btn_prev = btn_now;

  // "Reboot PDP-11" from the menu (the menu has already closed itself).
  if (ui_consume_reboot()) {
    LOG("ui: reboot PDP-11");
    start_cpu(true);
    boot_done = false;
    led(0, 0, 32);
  }

  // "Reset ESP32" from the menu. NO serial activity on this path - if the
  // host isn't reading USB-CDC (Arduino IDE Serial Monitor closed, no PC
  // attached, etc.), Serial.write / Serial.printf / our kl11 drain all
  // block on the USB-CDC TX semaphore (default ~5 s timeout, sometimes
  // hangs indefinitely). The user already saw the on-screen confirmation,
  // so we just reset immediately and let any in-flight serial bytes drop.
  if (ui_consume_esp_restart()) {
    ESP.restart();   // does not return
  }

  // While the menu is open the PDP-11 is paused; just keep polling for taps.
  if (ui_is_open()) { delay(8); return; }

  // Running: feed the keyboard, run the PDP-11 in small slices, service
  // telnet between slices so the network console stays responsive, and
  // drain the KL11->host output FIFOs (USB-Serial + TFT) so the 8 KB
  // rings stay near empty during steady-state output. Telnet's TX FIFO
  // is drained inside telnet_poll().
  for (int slice = 0; slice < 5; slice++) {
    // Per-slice touch poll: cpu_run(8000) takes ~8 ms, so polling here
    // catches the second tap of a fast double-tap that would otherwise
    // fall between two loop iterations (~40 ms gap). Cheap - touch_poll
    // is a single I2C transaction to the FT6336U.
    poll_touch_once();
    while (Serial.available())
      console_key_push((uint8_t)Serial.read());   // -> Serial-in FIFO
    telnet_poll();               // accept + RX -> Telnet-in FIFO, flush TX FIFO
    cpu_run(8000);
    console_drain_tft();         // TFT-out FIFO -> ANSI parser -> cell grid
    kl11::drain_serial_out();    // Serial-out FIFO -> Serial.write
  }
  telnet_poll();
  console_drain_tft();
  kl11::drain_serial_out();


  // Periodic snapshot of guest CPU state - useful while bringing up
  // disk/OS bootstrap. If PC stays put, the guest is stuck in a tight
  // loop; if PC moves through a small window, it's a finite poll loop.
  // Rate is [diag] pcping in pdpconfig.ini (seconds). 0 disables it.
  static uint32_t s_state_ms = 0;
  uint32_t s_now = millis();
  if (cfg.diag_pcping_sec > 0 &&
      s_now - s_state_ms >= (uint32_t)cfg.diag_pcping_sec * 1000U) {
    s_state_ms = s_now;
    LOG("state: PC=0%o R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o PS=0%o inst=%u",
        (unsigned)cpu_pc(),
        (unsigned)cpu_reg16(0), (unsigned)cpu_reg16(1),
        (unsigned)cpu_reg16(2), (unsigned)cpu_reg16(3),
        (unsigned)cpu_reg16(4), (unsigned)cpu_reg16(5),
        (unsigned)cpu_reg16(6),
        (unsigned)cpu_psw(),
        (unsigned)cpu_inst_count());
    // cpu_dump_trace() is available if you need it for stuck-in-loop
    // diagnosis - the cpu_pdp11.h function dumps the last N entries of
    // the trace ring. We leave it off by default so the serial console
    // stays usable for the guest OS.
  }

  // WiFi health check (~0.1 Hz).
  uint32_t now = millis();
  if (now - wifi_ms >= 10000) {
    wifi_ms = now;
    if (WiFi.status() != WL_CONNECTED) {
      LOGE("WiFi link down - reconnecting");
      WiFi.reconnect();
    }
  }

  // Status LED: blue while booting, green once the PDP-11 has gone quiet at a prompt.
  if (!boot_done && console_feed_count() > 0 &&
      millis() - console_last_feed_ms() > 800) {
    boot_done = true;
    led(0, 32, 0);
  }
}
