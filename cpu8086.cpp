#include "cpu8086.h"
#include "config.h"
#include "platform.h"
#include "bios_blob.h"
#include "disk.h"
#include "console.h"
#include "telnet.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// 8086tiny's BIOS uses disk index 1 for the floppy and 0 for the hard disk
// (DISK_READ/WRITE opcodes index a disk[] table). Map those onto our four-drive
// model. B:/D: are not reachable through the stock BIOS - addressed in m8.
static int map_8086_slot(int t8086_slot) {
  switch (t8086_slot) {
    case 0:  return DRIVE_C;   // 8086tiny "HD"
    case 1:  return DRIVE_A;   // 8086tiny "FD"
    default: return -1;
  }
}

// INT 13h / INT 11h interception helpers, defined below the extern block.
static void v8088_int13();
static int  v8088_floppy_count();

// Bridge to the C core in 8086tiny.c.
extern "C" {
  // Globals exported by 8086tiny.c
  extern unsigned char*  mem;
  extern unsigned short  reg_ip;
  extern unsigned char*  regs8;     // AL,AH,CL,CH,DL,DH,BL,BH at 0..7; flags 40+
  extern unsigned short* regs16;    // AX,CX,DX,BX,SP,BP,SI,DI,ES,CS,SS,DS

  // Core entry points
  void           v8088_set_mem(unsigned char* m);
  void           v8088_init(unsigned int hd_size_sectors);
  void           v8088_reset(void);
  void           v8088_set_boot_drive(unsigned char dl);
  int            v8088_run_one(void);
  unsigned int   v8088_run(unsigned int max_cycles);
  unsigned short v8088_get_reg16(int idx);
  unsigned short v8088_get_ip(void);
  unsigned int   v8088_get_inst_count(void);
  int            v8088_is_halted(void);
  void           v8088_pulse_int8(void);

  // ---- host hooks consumed by 8086tiny.c ----
  // m3 stubs - we only need PUTCHAR_AL plumbing for sanity output; disk/keyboard/RTC
  // will be wired in m4 / m5 / m7.

  // Guest console output: the BIOS ANSI/PUTCHAR stream. Fans out to the TFT
  // terminal emulator and the raw USB serial port (which is itself an ANSI
  // terminal). Telnet is added in m7.
  void v8088_host_putchar(unsigned char c) {
    console_feed(c);      // TFT terminal emulator
    telnet_write(c);      // raw to the telnet client
    Serial.write(c);      // raw to USB serial
  }

  // Keyboard: hand the guest one queued byte. The 8086tiny BIOS expects the
  // scancode/ASCII in mem[0x4A6] and then raises INT 7.
  int v8088_host_keyboard_poll(unsigned char* out) {
    return console_key_pop(out);
  }

  unsigned int v8088_host_disk_sectors(int slot) {
    int s = map_8086_slot(slot);
    if (s < 0) return 0;
    return disk_size_bytes(s) / 512u;
  }

  // lba = sector number (from BP:SI), bytes = byte count (from AX).
  int v8088_host_disk_read(int slot, unsigned int lba, void* buf, unsigned int bytes) {
    int s = map_8086_slot(slot);
    if (s < 0 || !disk_is_mounted(s)) return -1;
    return disk_read(s, lba * 512u, buf, bytes);
  }

  int v8088_host_disk_write(int slot, unsigned int lba, const void* buf, unsigned int bytes) {
    int s = map_8086_slot(slot);
    if (s < 0 || !disk_is_mounted(s)) return -1;
    return disk_write(s, lba * 512u, buf, bytes);
  }

  void v8088_host_get_rtc(unsigned char* dest36) {
    // struct tm (Linux layout) is 36 bytes + 2 byte millitm written by core after.
    memset(dest36, 0, 36);
  }

  // Software-interrupt interception. We fully service INT 13h (disk services)
  // and INT 11h (equipment list) in C, so all four drives work regardless of
  // the stock BIOS - which only knows one floppy + one hard disk.
  int v8088_host_intercept(unsigned char int_num) {
    if (int_num == 0x13) { v8088_int13(); return 1; }
    if (int_num == 0x11) {                       // equipment list -> AX
      int nf = v8088_floppy_count();
      regs16[0] = (unsigned short)(0x0021 | ((nf - 1) << 6));
      regs8[40] = 0;                             // CF = 0
      return 1;
    }
    return 0;
  }
}

// ===================== INT 13h disk service =============================
// Register file indices (8086tiny memory-mapped layout).
enum { V_AL=0, V_AH=1, V_CL=2, V_CH=3, V_DL=4, V_DH=5, V_BL=6 };
enum { V_AX=0, V_CX=1, V_DX=2, V_BX=3, V_ES=8 };
#define V_CF 40

static int v8088_drive_slot(uint8_t dl) {
  switch (dl) {
    case 0x00: return DRIVE_A;
    case 0x01: return DRIVE_B;
    case 0x80: return DRIVE_C;
    case 0x81: return DRIVE_D;
    default:   return -1;
  }
}

static int v8088_floppy_count() {
  return 1 + (disk_is_mounted(DRIVE_B) ? 1 : 0);
}
static int v8088_hdd_count() {
  return (disk_is_mounted(DRIVE_C) ? 1 : 0) + (disk_is_mounted(DRIVE_D) ? 1 : 0);
}

// Geometry: floppy 80x2x18, hard disk 1024x1x63 (matches what the stock BIOS
// presented for a 32 MB image, so a C: formatted earlier stays valid).
static uint32_t v8088_chs_to_lba(int slot, uint16_t cyl, uint8_t head, uint8_t sec) {
  bool hdd = (slot >= DRIVE_C);
  uint32_t heads = hdd ? 1 : 2;
  uint32_t spt   = hdd ? 63 : 18;
  return ((uint32_t)cyl * heads + head) * spt + (sec ? sec - 1u : 0u);
}

static void v8088_int13() {
  uint8_t*  r8  = regs8;
  uint16_t* r16 = regs16;

  uint8_t  ah = r8[V_AH], al = r8[V_AL];
  uint8_t  ch = r8[V_CH], cl = r8[V_CL];
  uint8_t  dh = r8[V_DH], dl = r8[V_DL];
  uint16_t bx = r16[V_BX], es = r16[V_ES];

  // Keep the BIOS data area consistent with the current drive set.
  int nf = v8088_floppy_count();
  int nh = v8088_hdd_count();
  mem[0x410] = (uint8_t)(0x21 | ((nf - 1) << 6));   // equipment word low byte
  mem[0x475] = (uint8_t)nh;                          // number of hard disks

  int slot = v8088_drive_slot(dl);
  uint16_t cyl = ch | ((uint16_t)(cl & 0xC0) << 2);
  uint8_t  sec = cl & 0x3F;

  switch (ah) {
    case 0x00:  // reset
    case 0x0C:  // seek
    case 0x10:  // test drive ready
    case 0x11:  // recalibrate
      r8[V_AH] = 0; r8[V_CF] = 0;
      break;

    case 0x01:  // get last status
      r8[V_AL] = 0; r8[V_CF] = 0;
      break;

    case 0x02: { // read sectors
      if (slot < 0 || !disk_is_mounted(slot)) { r8[V_AH] = 0x80; r8[V_CF] = 1; break; }
      // Media swapped since last access? Tell DOS to re-read the FAT.
      if (disk_take_change(slot)) { r8[V_AH] = 0x06; r8[V_CF] = 1; break; }
      uint32_t lba   = v8088_chs_to_lba(slot, cyl, dh, sec);
      uint32_t phys  = (uint32_t)es * 16u + bx;
      uint32_t bytes = (uint32_t)al * 512u;
      int n = disk_read(slot, lba * 512u, mem + phys, bytes);
      if (n == (int)bytes) { r8[V_AL] = al; r8[V_AH] = 0;    r8[V_CF] = 0; }
      else                 { r8[V_AL] = 0;  r8[V_AH] = 0x04; r8[V_CF] = 1; }
      break;
    }

    case 0x03: { // write sectors
      if (slot < 0 || !disk_is_mounted(slot)) { r8[V_AH] = 0x80; r8[V_CF] = 1; break; }
      if (disk_take_change(slot)) { r8[V_AH] = 0x06; r8[V_CF] = 1; break; }
      uint32_t lba   = v8088_chs_to_lba(slot, cyl, dh, sec);
      uint32_t phys  = (uint32_t)es * 16u + bx;
      uint32_t bytes = (uint32_t)al * 512u;
      int n = disk_write(slot, lba * 512u, mem + phys, bytes);
      if (n == (int)bytes) { r8[V_AL] = al; r8[V_AH] = 0;    r8[V_CF] = 0; }
      else                 { r8[V_AL] = 0;  r8[V_AH] = 0x03; r8[V_CF] = 1; }
      break;
    }

    case 0x04:  // verify sectors
    case 0x05:  // format track (no-op - images are pre-zeroed)
      r8[V_AH] = 0; r8[V_CF] = 0;
      break;

    case 0x08: { // get drive parameters
      if (slot < 0) { r8[V_AH] = 0x01; r8[V_CF] = 1; break; }
      bool     hdd   = (slot >= DRIVE_C);
      uint16_t cyls  = hdd ? 1024 : 80;
      uint8_t  heads = hdd ? 1    : 2;
      uint8_t  spt   = hdd ? 63   : 18;
      uint16_t maxc  = cyls - 1;
      r8[V_CH] = maxc & 0xFF;                           // max cyl low 8
      r8[V_CL] = ((maxc >> 2) & 0xC0) | (spt & 0x3F);   // cyl hi 2 + sectors
      r8[V_DH] = heads - 1;                             // max head
      r8[V_DL] = hdd ? nh : nf;                         // drive count
      if (!hdd) r8[V_BL] = 0x04;                        // 1.44 MB drive type
      r8[V_AH] = 0; r8[V_CF] = 0;
      break;
    }

    case 0x15: { // get disk type
      if (slot < 0 || !disk_is_mounted(slot)) { r8[V_AH] = 0; r8[V_CF] = 0; break; }
      if (slot >= DRIVE_C) {
        r8[V_AH] = 0x03;                                // fixed disk
        uint32_t secs = disk_size_bytes(slot) / 512u;
        r16[V_CX] = (uint16_t)(secs >> 16);             // CX:DX = sector count
        r16[V_DX] = (uint16_t)(secs & 0xFFFF);
      } else {
        r8[V_AH] = 0x02;                                // floppy with changeline
      }
      r8[V_CF] = 0;
      break;
    }

    case 0x16:  // detect disk change line
      if (disk_take_change(slot)) { r8[V_AH] = 0x06; r8[V_CF] = 1; }
      else                        { r8[V_AH] = 0x00; r8[V_CF] = 0; }
      break;

    case 0x17:  // set disk type
    case 0x18:  // set media type for format
      r8[V_AH] = 0; r8[V_CF] = 0;
      break;

    default:    // unsupported function
      r8[V_AH] = 0x01; r8[V_CF] = 1;
      break;
  }
}

// -------------------------------------------------------------------------
static bool g_initialized = false;
static volatile bool g_halt_requested = false;

bool cpu_init() {
  if (g_initialized) return true;

  // Allocate one big PSRAM block. 32-byte aligned helps the cache controller.
  uint8_t* buf = (uint8_t*)heap_caps_aligned_alloc(32, V8088_RAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    LOGE("cpu_init: PSRAM alloc of %u bytes failed (free=%u)",
         (unsigned)V8088_RAM_SIZE, (unsigned)ESP.getFreePsram());
    return false;
  }
  memset(buf, 0, V8088_RAM_SIZE);

  // Copy BIOS blob from PROGMEM to mem + 0xF0100 (per 8086tiny convention).
  memcpy_P(buf + 0xF0100, bios_blob, bios_blob_len);

  v8088_set_mem(buf);

  // hd size = 0 sectors (no disk in m3). cpu8086.cpp will revisit in m8.
  v8088_init(0);

  LOG("cpu_init: %u bytes guest RAM in PSRAM at %p, BIOS %u bytes at 0xF0100",
      (unsigned)V8088_RAM_SIZE, buf, (unsigned)bios_blob_len);
  LOG("cpu_init: PSRAM free now = %u bytes", (unsigned)ESP.getFreePsram());

  g_initialized = true;
  return true;
}

void cpu_reset() {
  if (!g_initialized) return;
  g_halt_requested = false;
  v8088_reset();
}

void cpu_cold_boot() {
  if (!g_initialized) return;
  g_halt_requested = false;
  memset(mem, 0, 0xC0000);                           // conventional RAM + video
  memcpy_P(mem + 0xF0100, bios_blob, bios_blob_len);  // pristine BIOS
  v8088_init(0);                                      // rebuild decode tables
}

uint32_t cpu_run(uint32_t max_cycles) {
  if (!g_initialized) return 0;
  uint32_t executed = 0;
  // Run in chunks of 4096 to allow halt requests and FreeRTOS yields.
  while (executed < max_cycles && !g_halt_requested) {
    uint32_t chunk = max_cycles - executed;
    if (chunk > 4096) chunk = 4096;
    uint32_t n = v8088_run(chunk);
    executed += n;
    if (n < chunk) break;   // halted
    yield();
  }
  return executed;
}

void cpu_request_halt() { g_halt_requested = true; }

uint8_t* cpu_mem()       { return mem; }
uint32_t cpu_mem_size()  { return V8088_RAM_SIZE; }
uint16_t cpu_reg16(int idx) { return v8088_get_reg16(idx); }
uint16_t cpu_ip()        { return v8088_get_ip(); }
uint32_t cpu_inst_count(){ return v8088_get_inst_count(); }

void cpu_set_cs_ip(uint16_t cs, uint16_t ip) {
  uint16_t* regs16 = (uint16_t*)(mem + V8088_REGS_BASE);
  regs16[V8088_CS] = cs;
  reg_ip = ip;
}

void cpu_set_boot_drive(uint8_t dl) {
  v8088_set_boot_drive(dl);
}

void cpu_set_hd_sectors(uint32_t sectors) {
  uint16_t* regs16 = (uint16_t*)(mem + V8088_REGS_BASE);
  regs16[V8088_AX] = (uint16_t)(sectors & 0xFFFF);          // low word
  regs16[V8088_CX] = (uint16_t)((sectors >> 16) & 0xFFFF);  // high word
}
