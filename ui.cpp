#include "ui.h"
#include "config.h"
#include "platform.h"
#include "disk.h"
#include "telnet.h"
#include "appconfig.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <string.h>

#ifndef TFT_BL
#define TFT_BL 45
#endif

// ---- colours ----
#define COL_BG       0x0009
#define COL_TITLE    0x001F
#define COL_ITEM     0x4208
#define COL_ITEM_HI  0x0320     // mounted-drive green
#define COL_ACCENT   0x07E0
#define COL_DANGER   0xC000
#define COL_TEXT     TFT_WHITE
#define COL_DIM      0x9CD3

// ---- layout (big finger targets) ----
#define MENU_VISIBLE 4
#define ITEM_Y0      26
#define ITEM_H       44          // button drawn 42 px tall + 2 px gap
#define NAV_Y        204
#define NAV_H        34

#define HIT_NONE -1
#define HIT_BACK -2
#define HIT_UP   -3
#define HIT_DOWN -4

enum Screen { SC_CLOSED, SC_MAIN, SC_DRIVES, SC_DRIVE, SC_PICKER, SC_INFO, SC_BRIGHT };

static Screen   g_screen = SC_CLOSED;
static bool     g_dirty  = false;
static bool     g_reboot = false;
static int      g_sel    = 0;          // drive index for SC_DRIVE / SC_PICKER
static int      g_scroll = 0;
static uint8_t  g_bright = 255;

#define MAX_FILES 16
static char g_files[MAX_FILES][44];
static int  g_file_count = 0;

#define MAX_ITEMS 20
static char g_title[40];
static char g_items[MAX_ITEMS][44];
static int  g_count = 0;

// -------------------------------------------------------------------------
void ui_init() {
  g_screen = SC_CLOSED;
  g_dirty = false;
  g_reboot = false;
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, g_bright);
}

bool ui_is_open()        { return g_screen != SC_CLOSED; }
bool ui_consume_reboot() { bool r = g_reboot; g_reboot = false; return r; }

// ---- scan SD root for mountable images ----
static void scan_files() {
  g_file_count = 0;
  fs::File root = SD_MMC.open("/");
  if (!root) return;
  for (fs::File f = root.openNextFile(); f && g_file_count < MAX_FILES;
       f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char* n = f.name();
      const char* slash = strrchr(n, '/');
      const char* base = slash ? slash + 1 : n;
      const char* dot = strrchr(base, '.');
      if (dot && (!strcasecmp(dot, ".dsk") || !strcasecmp(dot, ".hdd") ||
                  !strcasecmp(dot, ".img") || !strcasecmp(dot, ".ima"))) {
        strncpy(g_files[g_file_count], base, 43);
        g_files[g_file_count][43] = 0;
        g_file_count++;
      }
    }
    f.close();
  }
  root.close();
}

// Create a new blank image with a generated name; writes the path to `out`.
static void create_image(bool is_hdd, char* out, size_t outsz) {
  for (int i = 0; i < 100; i++) {
    snprintf(out, outsz, "/%s%d.%s",
             is_hdd ? "HDDISK" : "FLOPPY", i, is_hdd ? "hdd" : "dsk");
    if (!SD_MMC.exists(out)) break;
  }
  LOG("ui: creating %s", out);
  if (is_hdd) ensure_disk_image(out, HD_BYTES, true, "NEW");  // zeroed; FDISK it
  else        disk_create_floppy(out);                        // pre-formatted
}

// ---- build the item list / title for the current screen ----
static void rebuild() {
  g_count = 0;
  switch (g_screen) {
    case SC_MAIN:
      strcpy(g_title, "PDP-11/40 Settings");
      strcpy(g_items[g_count++], "Drives");
      strcpy(g_items[g_count++], "System Info");
      strcpy(g_items[g_count++], "Brightness");
      strcpy(g_items[g_count++], "Reboot PDP-11");
      break;
    case SC_DRIVES:
      strcpy(g_title, "Drives");
      {
        static const char* const ui_unit_names[4] = { "DL0", "DL1", "DX0", "DX1" };
        for (int s = 0; s < 4; s++) {
          if (disk_is_mounted(s))
            snprintf(g_items[g_count++], 44, "%s %s%s", ui_unit_names[s],
                     disk_path(s),
                     disk_is_readonly(s) ? " [RO]" : "");
          else
            snprintf(g_items[g_count++], 44, "%s (empty)", ui_unit_names[s]);
        }
      }
      break;
    case SC_DRIVE: {
      static const char* const drv_unit_names[4] = { "DL0", "DL1", "DX0", "DX1" };
      snprintf(g_title, sizeof(g_title), "Drive %s", drv_unit_names[g_sel]);
      strcpy(g_items[g_count++],
             disk_is_mounted(g_sel) ? "Change Image" : "Mount Image");
      if (disk_is_mounted(g_sel))
        strcpy(g_items[g_count++], "Dismount");
      break;
    }
    case SC_PICKER: {
      static const char* const pick_unit_names[4] = { "DL0", "DL1", "DX0", "DX1" };
      snprintf(g_title, sizeof(g_title), "Mount into %s", pick_unit_names[g_sel]);
      // Item 0 always offers creating a new blank image of the right type.
      strcpy(g_items[g_count++],
             (g_sel >= DRIVE_C) ? "[+] Create New 32MB HDD"
                                : "[+] Create New Floppy");
      for (int i = 0; i < g_file_count; i++)
        strncpy(g_items[g_count++], g_files[i], 43);
      break;
    }
    case SC_BRIGHT:
      snprintf(g_title, sizeof(g_title), "Brightness  %d%%",
               (g_bright * 100) / 255);
      strcpy(g_items[g_count++], "-  Dimmer");
      strcpy(g_items[g_count++], "+  Brighter");
      break;
    case SC_INFO:
      strcpy(g_title, "System Info");
      break;
    default: break;
  }
  if (g_scroll > g_count - MENU_VISIBLE) g_scroll = g_count - MENU_VISIBLE;
  if (g_scroll < 0) g_scroll = 0;
}

static void go(Screen s) { g_screen = s; g_scroll = 0; rebuild(); g_dirty = true; }

void ui_open() { go(SC_MAIN); }

// -------------------------------------------------------------------------
static int list_hit(int x, int y) {
  if (y >= ITEM_Y0 && y < ITEM_Y0 + MENU_VISIBLE * ITEM_H && x >= 6 && x < 314) {
    return (y - ITEM_Y0) / ITEM_H;            // 0..3
  }
  if (y >= NAV_Y) {
    if (g_count > MENU_VISIBLE) {
      if (x < 156) return HIT_BACK;
      if (x < 236) return HIT_UP;
      return HIT_DOWN;
    }
    return HIT_BACK;
  }
  return HIT_NONE;
}

static void do_back() {
  switch (g_screen) {
    case SC_MAIN:   g_screen = SC_CLOSED; break;
    case SC_DRIVES: go(SC_MAIN);   break;
    case SC_DRIVE:  go(SC_DRIVES); break;
    case SC_PICKER: go(SC_DRIVES); break;
    case SC_INFO:   go(SC_MAIN);   break;
    case SC_BRIGHT: go(SC_MAIN);   break;
    default: break;
  }
}

static void activate(int idx) {        // idx = absolute item index
  if (idx < 0 || idx >= g_count) return;
  switch (g_screen) {
    case SC_MAIN:
      if      (idx == 0) go(SC_DRIVES);
      else if (idx == 1) go(SC_INFO);
      else if (idx == 2) go(SC_BRIGHT);
      else if (idx == 3) { g_reboot = true; g_screen = SC_CLOSED; }
      break;
    case SC_DRIVES:
      g_sel = idx;                       // 0..3 -> A..D
      if (disk_is_mounted(g_sel)) go(SC_DRIVE);
      else { scan_files(); go(SC_PICKER); }
      break;
    case SC_DRIVE:
      if (idx == 0) { scan_files(); go(SC_PICKER); }
      else          { disk_dismount(g_sel); go(SC_DRIVES); }
      break;
    case SC_PICKER: {
      char path[48];
      bool ok;
      if (idx == 0) {                    // create a new image, then mount it
        create_image(g_sel >= DRIVE_C, path, sizeof(path));
        ok = disk_mount(g_sel, path);
        LOG("ui: created+mounted %c: %s -> %s", 'A' + g_sel, path, ok ? "ok" : "FAIL");
      } else {                           // mount an existing image
        snprintf(path, sizeof(path), "/%s", g_files[idx - 1]);
        ok = disk_mount(g_sel, path);
        LOG("ui: mount %c: %s -> %s", 'A' + g_sel, path, ok ? "ok" : "FAIL");
      }
      go(SC_DRIVES);
      break;
    }
    case SC_BRIGHT:
      if (idx == 0) g_bright = (g_bright > 40) ? g_bright - 40 : 8;
      else          g_bright = (g_bright < 215) ? g_bright + 40 : 255;
      ledcWrite(TFT_BL, g_bright);
      rebuild(); g_dirty = true;
      break;
    default: break;
  }
}

bool ui_handle_tap(int x, int y) {
  if (g_screen == SC_CLOSED) return false;
  int h = list_hit(x, y);
  if      (h == HIT_BACK) do_back();
  else if (h == HIT_UP)   { if (g_scroll > 0) { g_scroll--; g_dirty = true; } }
  else if (h == HIT_DOWN) { if (g_scroll + MENU_VISIBLE < g_count) { g_scroll++; g_dirty = true; } }
  else if (h >= 0)        activate(g_scroll + h);
  return true;
}

// -------------------------------------------------------------------------
static TFT_eSPI* T = nullptr;

static void draw_button(int x, int y, int w, int hh, const char* label,
                         uint16_t bg, uint16_t fg) {
  T->fillRoundRect(x, y, w, hh, 4, bg);
  T->drawRoundRect(x, y, w, hh, 4, COL_DIM);
  T->setTextColor(fg, bg);
  T->setTextDatum(ML_DATUM);
  T->drawString(label, x + 8, y + hh / 2, 2);
  T->setTextDatum(TL_DATUM);
}

static void draw_nav() {
  if (g_count > MENU_VISIBLE) {
    const char* bk = (g_screen == SC_MAIN) ? "Close" : "Back";
    draw_button(6,   NAV_Y, 144, NAV_H, bk,  COL_ITEM, COL_TEXT);
    draw_button(158, NAV_Y, 74,  NAV_H, " Up",   COL_ITEM, COL_TEXT);
    draw_button(240, NAV_Y, 74,  NAV_H, " Down", COL_ITEM, COL_TEXT);
  } else {
    const char* bk = (g_screen == SC_MAIN) ? "Close Menu" : "Back";
    draw_button(6, NAV_Y, 308, NAV_H, bk, COL_ITEM, COL_TEXT);
  }
}

static void draw_list() {
  T->fillScreen(COL_BG);
  T->fillRect(0, 0, 320, 24, COL_TITLE);
  T->setTextColor(COL_TEXT, COL_TITLE);
  T->setTextDatum(ML_DATUM);
  T->drawString(g_title, 6, 12, 2);
  T->setTextDatum(TL_DATUM);

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = g_scroll + i;
    if (idx >= g_count) break;
    int y = ITEM_Y0 + i * ITEM_H;
    uint16_t bg = COL_ITEM;
    if (g_screen == SC_DRIVES && idx < 4 && disk_is_mounted(idx)) bg = COL_ITEM_HI;
    if (g_screen == SC_MAIN && idx == 3) bg = COL_DANGER;
    if (g_screen == SC_DRIVE && idx == 1) bg = COL_DANGER;
    if (g_screen == SC_PICKER && idx == 0) bg = COL_ITEM_HI;   // create-new
    draw_button(6, y, 308, 42, g_items[idx], bg, COL_TEXT);
  }
  draw_nav();
}

static void draw_info() {
  T->fillScreen(COL_BG);
  T->fillRect(0, 0, 320, 24, COL_TITLE);
  T->setTextColor(COL_TEXT, COL_TITLE);
  T->setTextDatum(ML_DATUM);
  T->drawString(g_title, 6, 12, 2);
  T->setTextDatum(TL_DATUM);
  T->setTextColor(COL_TEXT, COL_BG);

  char line[64];
  int y = 34;
  snprintf(line, sizeof(line), "%s  version %s", APP_TITLE, APP_VERSION);
  T->drawString(line, 10, y, 2); y += 22;
  snprintf(line, sizeof(line), "build %s", APP_BUILD_DATE);
  T->drawString(line, 10, y, 2); y += 26;

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "WiFi: %s", WiFi.SSID().c_str());
    T->setTextColor(COL_ACCENT, COL_BG); T->drawString(line, 10, y, 2); y += 22;
    snprintf(line, sizeof(line), "IP:   %s", WiFi.localIP().toString().c_str());
    T->drawString(line, 10, y, 2); y += 26;
  } else {
    T->setTextColor(COL_DANGER, COL_BG);
    T->drawString("WiFi: disconnected", 10, y, 2); y += 26;
  }

  T->setTextColor(COL_TEXT, COL_BG);
  if (!telnet_enabled())
    snprintf(line, sizeof(line), "Telnet: disabled");
  else if (telnet_connected())
    snprintf(line, sizeof(line), "Telnet %u: client %s",
             telnet_port(), telnet_client_ip());
  else
    snprintf(line, sizeof(line), "Telnet %u: no client", telnet_port());
  T->drawString(line, 10, y, 2);

  draw_nav();
}

void ui_draw(TFT_eSPI& tft) {
  if (g_screen == SC_CLOSED || !g_dirty) return;
  T = &tft;
  if (g_screen == SC_INFO) draw_info();
  else                     draw_list();
  g_dirty = false;
}
