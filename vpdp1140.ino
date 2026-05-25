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
// touch settings menu, config.ini, dual-core split) carries over from v8088
// unchanged; only the CPU core, I/O page dispatch, and disk/console wiring
// are PDP-11-specific.
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
#include "disk.h"
#include "console.h"
#include "telnet.h"
#include "touch.h"
#include "ui.h"

static TFT_eSPI tft;
static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);
static AppConfig cfg;
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

static void tft_banner() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  tft.printf("%s  v%s", APP_TITLE, APP_VERSION);
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
//   row 2 = /config.ini
//   row 3 = HDD C: image
//   row 4 = WiFi
//   row 5 = IP
enum {
  ROW_PSRAM = 0, ROW_SD, ROW_CFG, ROW_HDD, ROW_WIFI, ROW_IP, ROW_CPU
};

static void wifi_connect() {
  const char* ssid = cfg.wifi_ssid.c_str();
  const char* pass = cfg.wifi_password.c_str();
  const char* host = cfg.wifi_hostname.length() ? cfg.wifi_hostname.c_str() : WIFI_HOSTNAME;

  if (cfg.wifi_ssid.length() == 0) {
    LOGE("WiFi SSID is empty - set [wifi] ssid= in /config.ini");
    tft_status(ROW_WIFI, "WiFi:  ", "no SSID in config.ini", TFT_RED);
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
    bool existed = config_load(cfg);
    tft_status(ROW_CFG, "Cfg:   ",
               existed ? "loaded /config.ini" : "wrote default /config.ini",
               existed ? TFT_GREEN : TFT_YELLOW);
  }
  config_print(cfg);

  // Ensure the C: HDD image exists (32 MB zeroed) if the path is non-empty.
  tft_status(ROW_HDD, "HDD C: ", "checking...", TFT_YELLOW);
  if (!sd_ok) {
    tft_status(ROW_HDD, "HDD C: ", "skipped (no SD)", TFT_DARKGREY);
  } else if (cfg.disk_c.length() == 0) {
    tft_status(ROW_HDD, "HDD C: ", "(dismounted)", TFT_DARKGREY);
  } else {
    bool ok = ensure_disk_image(cfg.disk_c.c_str(), HD_BYTES, true, "HDC");
    tft_status(ROW_HDD, "HDD C: ",
               ok ? cfg.disk_c.c_str() : "FAILED",
               ok ? TFT_GREEN : TFT_RED);
  }
}

// Mount the four guest drives from /config.ini paths.
static void disks_mount() {
  if (!sd_ok) { LOGE("disks_mount: SD not available"); return; }
  const String* paths[DRIVE_COUNT] = {
    &cfg.disk_a, &cfg.disk_b, &cfg.disk_c, &cfg.disk_d
  };
  static const char* unit_names[DRIVE_COUNT] = { "DL0", "DL1", "DX0", "DX1" };
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

  // Drive indicators DL0 DL1 DX0 DX1 - green=mounted, yellow=active, dim=empty.
  static const char* const unit_labels[DRIVE_COUNT] = { "DL0", "DL1", "DX0", "DX1" };
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
  char line[40];
  snprintf(line, sizeof(line), "%s   %.2f MIPS",
           telnet_connected() ? "TELNET" : "      ", mips);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(line, 158, sy + 22, 1);
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
  tft_status(ROW_HDD,  "HDD C: ", "(pending)", TFT_DARKGREY);
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

  wifi_connect();

  // ---- boot: mount drives, start peripherals, hand off to the console loop ----
  if (cpu_ok) {
    disks_mount();
    telnet_begin(cfg.telnet_port, cfg.telnet_enabled);
    pinMode(BUTTON_PIN, INPUT_PULLUP);   // onboard button opens the menu
    touch_init();
    ui_init();
    const char* boot_name = (cfg.boot_drive == 'a') ? "DL0"
                          : (cfg.boot_drive == 'b') ? "DL1"
                          : (cfg.boot_drive == 'c') ? "DX0"
                          : (cfg.boot_drive == 'd') ? "DX1" : "?";
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
void loop() {
  if (!cpu_running) { delay(100); return; }

  static bool     boot_done = false;
  static bool     btn_prev  = true;
  static uint32_t last_tap  = 0;
  static uint32_t wifi_ms   = 0;

  // Touch: menu open -> taps drive the menu; closed -> a double-tap opens it.
  int tx, ty;
  if (touch_poll(&tx, &ty)) {
    if (ui_is_open()) {
      ui_tap_locked(tx, ty);
    } else {
      uint32_t now = millis();
      if (now - last_tap < 450) { ui_open_locked(); last_tap = 0; }
      else                        last_tap = now;
    }
  }

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

  // While the menu is open the PDP-11 is paused; just keep polling for taps.
  if (ui_is_open()) { delay(8); return; }

  // Running: feed the keyboard, run the PDP-11 in small slices, service
  // telnet between slices so the network console stays responsive.
  for (int slice = 0; slice < 5; slice++) {
    while (Serial.available())
      console_key_push((uint8_t)Serial.read());
    telnet_poll();               // accept + RX -> keyboard, flush TX
    cpu_run(8000);
  }
  telnet_poll();


  // Once-per-five-seconds snapshot of guest CPU state - useful while
  // bringing up disk/OS bootstrap. If PC stays put, the guest is stuck
  // in a tight loop; if PC moves through a small window, it's a finite
  // poll loop.
  static uint32_t s_state_ms = 0;
  uint32_t s_now = millis();
  if (s_now - s_state_ms >= 5000) {
    s_state_ms = s_now;
    LOG("state: PC=0%o R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o PS=0%o inst=%u",
        (unsigned)cpu_pc(),
        (unsigned)cpu_reg16(0), (unsigned)cpu_reg16(1),
        (unsigned)cpu_reg16(2), (unsigned)cpu_reg16(3),
        (unsigned)cpu_reg16(4), (unsigned)cpu_reg16(5),
        (unsigned)cpu_reg16(6),
        (unsigned)cpu_psw(),
        (unsigned)cpu_inst_count());
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
