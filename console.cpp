#include "console.h"
#include "config.h"
#include "platform.h"
#include "font4x8.h"
#include "fifo.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// ---- 16-colour CGA palette in RGB565 ----
static const uint16_t kPalette[16] = {
  0x0000, // 0 black
  0x0015, // 1 blue
  0x0540, // 2 green
  0x0555, // 3 cyan
  0xA800, // 4 red
  0xA815, // 5 magenta
  0xAAA0, // 6 brown
  0xAD55, // 7 light grey
  0x52AA, // 8 dark grey
  0x001F, // 9 bright blue
  0x07E0, // 10 bright green
  0x07FF, // 11 bright cyan
  0xF800, // 12 bright red
  0xF81F, // 13 bright magenta
  0xFFE0, // 14 yellow
  0xFFFF, // 15 white
};

// ANSI SGR fg/bg code (30-37) -> CGA colour index
static const uint8_t kAnsiToCga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

#define DEF_ATTR 0x07          // light grey on black

// ---- screen state ----
static uint8_t  cell_ch[CON_ROWS][CON_COLS];
static uint8_t  cell_at[CON_ROWS][CON_COLS];
static uint8_t  shad_ch[CON_ROWS][CON_COLS];
static uint8_t  shad_at[CON_ROWS][CON_COLS];
static bool     shad_valid = false;

static int  cur_r = 0, cur_c = 0;
static int  prev_cur_r = 0, prev_cur_c = 0;
static uint8_t cur_attr = DEF_ATTR;
static int  sr_top = 0, sr_bot = CON_ROWS - 1;
static int  saved_r = 0, saved_c = 0;

// ---- ANSI parser ----
static enum { ST_GROUND, ST_ESC, ST_CSI } ansi_st = ST_GROUND;
static int  csi_param[8];
static int  csi_nparam = 0;
static bool csi_has_digit = false;

// ---- 8 KB serial-input FIFO (host USB-Serial -> KL11 TKB) and 8 KB
//      TFT-output FIFO (KL11 TPB -> ANSI parser -> cell grid). Both live
//      in PSRAM via EXT_RAM_BSS_ATTR; the indices stay in DRAM. Producer
//      and consumer for both are on core 1 (loop()), so plain volatile
//      head/tail is sufficient. ----
#define VPDP_FIFO_BYTES 8192   // must be power of two
EXT_RAM_BSS_ATTR static uint8_t serial_in_storage[VPDP_FIFO_BYTES];
EXT_RAM_BSS_ATTR static uint8_t tft_out_storage[VPDP_FIFO_BYTES];
static Fifo g_serial_in;
static Fifo g_tft_out;

// ---- output activity ----
static uint32_t g_feed_count   = 0;
static uint32_t g_last_feed_ms = 0;

// -------------------------------------------------------------------------

static void clear_row(int r, uint8_t attr) {
  for (int c = 0; c < CON_COLS; c++) { cell_ch[r][c] = ' '; cell_at[r][c] = attr; }
}

void console_init() {
  for (int r = 0; r < CON_ROWS; r++) clear_row(r, DEF_ATTR);
  cur_r = cur_c = 0;
  prev_cur_r = prev_cur_c = 0;
  cur_attr = DEF_ATTR;
  sr_top = 0; sr_bot = CON_ROWS - 1;
  ansi_st = ST_GROUND;
  shad_valid = false;
  g_serial_in.init(serial_in_storage, VPDP_FIFO_BYTES);
  g_tft_out.init(tft_out_storage, VPDP_FIFO_BYTES);
}

void console_force_redraw() { shad_valid = false; }

void console_get_cursor(int* row, int* col) {
  if (row) *row = cur_r;
  if (col) *col = cur_c;
}

// ---- scrolling within the current scroll region ----
static void scroll_region_up() {
  for (int r = sr_top; r < sr_bot; r++) {
    memcpy(cell_ch[r], cell_ch[r + 1], CON_COLS);
    memcpy(cell_at[r], cell_at[r + 1], CON_COLS);
  }
  clear_row(sr_bot, cur_attr);
}

static void scroll_region_down() {
  for (int r = sr_bot; r > sr_top; r--) {
    memcpy(cell_ch[r], cell_ch[r - 1], CON_COLS);
    memcpy(cell_at[r], cell_at[r - 1], CON_COLS);
  }
  clear_row(sr_top, cur_attr);
}

static void cursor_down_scroll() {
  cur_r++;
  if (cur_r > sr_bot) { scroll_region_up(); cur_r = sr_bot; }
}

static void clamp_cursor() {
  if (cur_c < 0) cur_c = 0;
  if (cur_c >= CON_COLS) cur_c = CON_COLS - 1;
  if (cur_r < 0) cur_r = 0;
  if (cur_r >= CON_ROWS) cur_r = CON_ROWS - 1;
}

// ---- printable character ----
static void put_glyph(uint8_t ch) {
  if (cur_c >= CON_COLS) { cur_c = 0; cursor_down_scroll(); }
  cell_ch[cur_r][cur_c] = ch;
  cell_at[cur_r][cur_c] = cur_attr;
  cur_c++;
}

// ---- SGR (colour) ----
static void apply_sgr() {
  if (csi_nparam == 0) { cur_attr = DEF_ATTR; return; }
  for (int i = 0; i < csi_nparam; i++) {
    int p = csi_param[i];
    if (p == 0)                       cur_attr = DEF_ATTR;
    else if (p == 1)                  cur_attr |= 0x08;            // bright fg
    else if (p >= 30 && p <= 37)      cur_attr = (cur_attr & 0xF8) | kAnsiToCga[p - 30];
    else if (p >= 40 && p <= 47)      cur_attr = (cur_attr & 0x8F) | (kAnsiToCga[p - 40] << 4);
    else if (p >= 90 && p <= 97)      cur_attr = (cur_attr & 0xF0) | kAnsiToCga[p - 90] | 0x08;
  }
}

// ---- erase ----
static void erase_in_display(int mode) {
  if (mode == 2 || mode == 3) {
    for (int r = 0; r < CON_ROWS; r++) clear_row(r, cur_attr);
  } else if (mode == 0) {                       // cursor -> end
    for (int c = cur_c; c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
    for (int r = cur_r + 1; r < CON_ROWS; r++) clear_row(r, cur_attr);
  } else if (mode == 1) {                       // start -> cursor
    for (int r = 0; r < cur_r; r++) clear_row(r, cur_attr);
    for (int c = 0; c <= cur_c && c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  }
}

static void erase_in_line(int mode) {
  if (mode == 0)      for (int c = cur_c; c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  else if (mode == 1) for (int c = 0; c <= cur_c && c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  else                clear_row(cur_r, cur_attr);
}

// ---- execute a completed CSI sequence ----
static void exec_csi(uint8_t final) {
  int p0 = csi_nparam > 0 ? csi_param[0] : 0;
  int p1 = csi_nparam > 1 ? csi_param[1] : 0;
  switch (final) {
    case 'H': case 'f':                          // cursor position (1-based)
      cur_r = (csi_nparam > 0 ? p0 : 1) - 1;
      cur_c = (csi_nparam > 1 ? p1 : 1) - 1;
      clamp_cursor();
      break;
    case 'A': cur_r -= (p0 ? p0 : 1); clamp_cursor(); break;
    case 'B': cur_r += (p0 ? p0 : 1); clamp_cursor(); break;
    case 'C': cur_c += (p0 ? p0 : 1); clamp_cursor(); break;
    case 'D': cur_c -= (p0 ? p0 : 1); clamp_cursor(); break;
    case 'J': erase_in_display(p0); break;
    case 'K': erase_in_line(p0); break;
    case 'm': apply_sgr(); break;
    case 'r':                                    // scroll region
      sr_top = (csi_nparam > 0 ? p0 : 1) - 1;
      sr_bot = (csi_nparam > 1 ? p1 : CON_ROWS) - 1;
      if (sr_top < 0) sr_top = 0;
      if (sr_bot >= CON_ROWS) sr_bot = CON_ROWS - 1;
      if (sr_top > sr_bot) { sr_top = 0; sr_bot = CON_ROWS - 1; }
      break;
    case 's': saved_r = cur_r; saved_c = cur_c; break;
    case 'u': cur_r = saved_r; cur_c = saved_c; clamp_cursor(); break;
    case 'M':                                    // scroll up (1 line)
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_up();
      break;
    case 'S':                                    // scroll up N
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_up();
      break;
    case 'T':                                    // scroll down N
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_down();
      break;
    case 'L':                                    // insert lines at cursor
      for (int i = 0; i < (p0 ? p0 : 1); i++) {
        for (int r = sr_bot; r > cur_r; r--) {
          memcpy(cell_ch[r], cell_ch[r - 1], CON_COLS);
          memcpy(cell_at[r], cell_at[r - 1], CON_COLS);
        }
        clear_row(cur_r, cur_attr);
      }
      break;
    default: break;
  }
}

// -------------------------------------------------------------------------
// Internal ANSI-parser entrypoint. Runs on core 1 from console_drain_tft().
// Updates cell_ch/cell_at, which render_task on core 0 reads without a
// lock (single-byte cells tolerate the race).
static void feed_ansi(uint8_t c) {
  g_feed_count++;
  g_last_feed_ms = millis();
  switch (ansi_st) {
    case ST_GROUND:
      switch (c) {
        case 0x1B: ansi_st = ST_ESC; break;
        case 0x07: break;                                  // BEL - ignore
        case 0x08: if (cur_c > 0) cur_c--; break;           // BS
        case 0x09:                                          // TAB
          cur_c = (cur_c + 8) & ~7;
          if (cur_c >= CON_COLS) cur_c = CON_COLS - 1;
          break;
        case 0x0A: cur_c = 0; cursor_down_scroll(); break;  // LF (BIOS: CR+LF)
        case 0x0D: cur_c = 0; break;                        // CR
        default:
          if (c >= 0x20) put_glyph(c);
          break;
      }
      break;

    case ST_ESC:
      if (c == '[') {
        ansi_st = ST_CSI;
        csi_nparam = 0; csi_param[0] = 0; csi_has_digit = false;
      } else {
        ansi_st = ST_GROUND;       // other ESC sequences not supported
      }
      break;

    case ST_CSI:
      if (c >= '0' && c <= '9') {
        if (csi_nparam == 0) csi_nparam = 1;
        csi_param[csi_nparam - 1] = csi_param[csi_nparam - 1] * 10 + (c - '0');
        csi_has_digit = true;
      } else if (c == ';') {
        if (csi_nparam == 0) csi_nparam = 1;
        if (csi_nparam < 8) { csi_param[csi_nparam] = 0; csi_nparam++; }
        csi_has_digit = false;
      } else if (c == '?' || c == '>' || c == '=') {
        // private-mode introducer - ignore, keep parsing
      } else if (c >= 0x40 && c <= 0x7E) {
        exec_csi(c);
        ansi_st = ST_GROUND;
      } else {
        ansi_st = ST_GROUND;       // malformed - bail
      }
      break;
  }
}

// ---- TFT-out FIFO: KL11 push, main-loop drain ----
// Public entrypoint kl11::poll() calls per output byte. Just buffers.
void console_feed(uint8_t c) {
  g_tft_out.push(c);          // drop-newest if full (sink falls behind)
}

// Drain pending TFT bytes through the ANSI parser. Called from loop() on
// core 1 once per slice so the cell grid updates in roughly real time.
void console_drain_tft() {
  uint8_t b;
  while (g_tft_out.pop(&b)) feed_ansi(b);
}

// ---- keyboard (host USB-Serial -> KL11 input FIFO) ----
void console_key_push(uint8_t c) { g_serial_in.push(c); }
int  console_key_pop(uint8_t* out) { return g_serial_in.pop(out) ? 1 : 0; }

uint32_t console_feed_count()   { return g_feed_count; }
uint32_t console_last_feed_ms() { return g_last_feed_ms; }

// ---- TFT rendering ----
// Text area is anchored at the top-left: 80*4 = 320 wide, 25*8 = 200 tall.
static void draw_cell(TFT_eSPI& tft, int r, int c, bool cursor) {
  uint8_t ch  = cell_ch[r][c];
  uint8_t at  = cell_at[r][c];
  uint16_t fg = kPalette[at & 0x0F];
  uint16_t bg = kPalette[(at >> 4) & 0x0F];

  uint16_t buf[4 * 8];
  for (int y = 0; y < 8; y++) {
    uint8_t bits = pgm_read_byte(&font4x8[ch][y]);
    for (int x = 0; x < 4; x++)
      buf[y * 4 + x] = (bits & (1 << x)) ? fg : bg;
  }
  if (cursor) {                          // underline on the bottom two rows
    for (int x = 0; x < 4; x++) { buf[7 * 4 + x] = fg; buf[6 * 4 + x] = fg; }
  }
  tft.pushImage(c * CELL_W, r * CELL_H, CELL_W, CELL_H, buf);
}

void console_render(TFT_eSPI& tft) {
  bool full = !shad_valid;
  for (int r = 0; r < CON_ROWS; r++) {
    for (int c = 0; c < CON_COLS; c++) {
      bool is_cur  = (r == cur_r && c == cur_c);
      bool was_cur = (r == prev_cur_r && c == prev_cur_c);
      bool changed = full ||
                     cell_ch[r][c] != shad_ch[r][c] ||
                     cell_at[r][c] != shad_at[r][c] ||
                     is_cur != was_cur;
      if (changed) {
        draw_cell(tft, r, c, is_cur);
        shad_ch[r][c] = cell_ch[r][c];
        shad_at[r][c] = cell_at[r][c];
      }
    }
  }
  prev_cur_r = cur_r;
  prev_cur_c = cur_c;
  shad_valid = true;
}
