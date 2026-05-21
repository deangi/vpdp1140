#include "disk.h"
#include "config.h"
#include "platform.h"
#include <Arduino.h>
#include <SD_MMC.h>

struct DriveSlot {
  File     file;
  bool     mounted = false;
  bool     is_hdd  = false;
  bool     changed = false;        // media-change flag (set on mount)
  char     path[64] = {0};
  uint32_t size = 0;
  uint32_t reads = 0;
  uint32_t writes = 0;
};

static DriveSlot g_drv[DRIVE_COUNT];

static bool slot_valid(int s) { return s >= 0 && s < DRIVE_COUNT; }

bool disk_mount(int slot, const char* path) {
  if (!slot_valid(slot) || !path || !*path) return false;

  disk_dismount(slot);   // ensure clean slot

  File f = SD_MMC.open(path, "r+");
  if (!f) {
    LOGE("disk_mount[%d]: cannot open %s", slot, path);
    return false;
  }
  uint32_t sz = (uint32_t)f.size();

  bool is_hdd = (slot == DRIVE_C || slot == DRIVE_D);
  uint32_t want = is_hdd ? HD_BYTES : FD_BYTES;
  if (sz != want) {
    LOGE("disk_mount[%d]: %s is %u bytes, expected %u (%s)",
         slot, path, (unsigned)sz, (unsigned)want,
         is_hdd ? "HDD" : "floppy");
    f.close();
    return false;
  }

  g_drv[slot].file    = f;
  g_drv[slot].mounted = true;
  g_drv[slot].is_hdd  = is_hdd;
  g_drv[slot].changed = true;        // signal DOS the media changed
  g_drv[slot].size    = sz;
  g_drv[slot].reads   = 0;
  g_drv[slot].writes  = 0;
  strncpy(g_drv[slot].path, path, sizeof(g_drv[slot].path) - 1);
  g_drv[slot].path[sizeof(g_drv[slot].path) - 1] = 0;

  LOG("disk_mount[%c]: %s (%u bytes)", 'A' + slot, path, (unsigned)sz);
  return true;
}

void disk_dismount(int slot) {
  if (!slot_valid(slot)) return;
  if (g_drv[slot].mounted) {
    g_drv[slot].file.close();
    LOG("disk_dismount[%c]: %s", 'A' + slot, g_drv[slot].path);
  }
  g_drv[slot].mounted = false;
  g_drv[slot].size = 0;
  g_drv[slot].path[0] = 0;
}

bool disk_is_mounted(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted;
}

const char* disk_path(int slot) {
  return slot_valid(slot) ? g_drv[slot].path : "";
}

uint32_t disk_size_bytes(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted ? g_drv[slot].size : 0;
}

int disk_read(int slot, uint32_t byte_offset, void* buf, uint32_t bytes) {
  if (!disk_is_mounted(slot)) return -1;
  DriveSlot& d = g_drv[slot];
  if (byte_offset + bytes > d.size) {
    LOGE("disk_read[%c]: out of range off=%u len=%u size=%u",
         'A' + slot, (unsigned)byte_offset, (unsigned)bytes, (unsigned)d.size);
    return -1;
  }
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_read[%c]: seek to %u failed", 'A' + slot, (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.read((uint8_t*)buf, bytes);
  d.reads++;
  if (n != bytes) {
    LOGE("disk_read[%c]: short read %u/%u at off %u",
         'A' + slot, (unsigned)n, (unsigned)bytes, (unsigned)byte_offset);
    return -1;
  }
  return (int)bytes;
}

int disk_write(int slot, uint32_t byte_offset, const void* buf, uint32_t bytes) {
  if (!disk_is_mounted(slot)) return -1;
  DriveSlot& d = g_drv[slot];
  if (byte_offset + bytes > d.size) {
    LOGE("disk_write[%c]: out of range off=%u len=%u size=%u",
         'A' + slot, (unsigned)byte_offset, (unsigned)bytes, (unsigned)d.size);
    return -1;
  }
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_write[%c]: seek to %u failed", 'A' + slot, (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.write((const uint8_t*)buf, bytes);
  d.file.flush();          // write-through
  d.writes++;
  if (n != bytes) {
    LOGE("disk_write[%c]: short write %u/%u", 'A' + slot, (unsigned)n, (unsigned)bytes);
    return -1;
  }
  return (int)bytes;
}

void disk_stats(int slot, uint32_t* reads, uint32_t* writes) {
  if (!slot_valid(slot)) { if (reads) *reads = 0; if (writes) *writes = 0; return; }
  if (reads)  *reads  = g_drv[slot].reads;
  if (writes) *writes = g_drv[slot].writes;
}

bool disk_take_change(int slot) {
  if (!slot_valid(slot)) return false;
  bool c = g_drv[slot].changed;
  g_drv[slot].changed = false;
  return c;
}

bool disk_create_floppy(const char* path) {
  File f = SD_MMC.open(path, FILE_WRITE);     // create / truncate
  if (!f) {
    LOGE("disk_create_floppy: cannot create %s", path);
    return false;
  }
  uint8_t sec[512];

  // ---- sector 0: boot sector with a valid 1.44 MB FAT12 BPB ----
  memset(sec, 0, sizeof(sec));
  sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;     // JMP over the BPB
  memcpy(sec + 3, "v8088   ", 8);                  // OEM name
  sec[11] = 0x00; sec[12] = 0x02;                  // 512 bytes/sector
  sec[13] = 1;                                     // 1 sector/cluster
  sec[14] = 1; sec[15] = 0;                        // 1 reserved sector
  sec[16] = 2;                                     // 2 FATs
  sec[17] = 0xE0; sec[18] = 0x00;                  // 224 root entries
  sec[19] = 0x40; sec[20] = 0x0B;                  // 2880 total sectors
  sec[21] = 0xF0;                                  // media descriptor
  sec[22] = 9; sec[23] = 0;                        // 9 sectors/FAT
  sec[24] = 18; sec[25] = 0;                       // 18 sectors/track
  sec[26] = 2; sec[27] = 0;                        // 2 heads
  sec[36] = 0x00;                                  // BIOS drive number
  sec[38] = 0x29;                                  // extended boot signature
  uint32_t serial = (uint32_t)millis();
  memcpy(sec + 39, &serial, 4);                    // volume serial number
  memcpy(sec + 43, "NO NAME    ", 11);             // volume label
  memcpy(sec + 54, "FAT12   ", 8);                 // filesystem type
  sec[510] = 0x55; sec[511] = 0xAA;                // boot signature
  f.write(sec, 512);

  // ---- two FATs, 9 sectors each; first 3 bytes = F0 FF FF ----
  for (int fat = 0; fat < 2; fat++) {
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xF0; sec[1] = 0xFF; sec[2] = 0xFF;
    f.write(sec, 512);
    memset(sec, 0, sizeof(sec));
    for (int i = 1; i < 9; i++) f.write(sec, 512);
  }

  // ---- root directory + data area: zeros (sectors 19..2879) ----
  memset(sec, 0, sizeof(sec));
  for (int i = 19; i < 2880; i++) f.write(sec, 512);

  f.close();
  LOG("disk_create_floppy: formatted %s (1.44 MB FAT12)", path);
  return true;
}
