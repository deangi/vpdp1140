#include "disk.h"
#include "config.h"
#include "platform.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"
#include <Arduino.h>
#include <SD_MMC.h>

struct DriveSlot {
  File     file;
  bool     mounted  = false;
  bool     changed  = false;       // media-change flag (set on mount)
  bool     readonly = false;       // image could only be opened read-only
  char     path[64] = {0};
  uint32_t size = 0;
  uint32_t reads = 0;
  uint32_t writes = 0;
};

static DriveSlot g_drv[DRIVE_COUNT];

static bool slot_valid(int s) { return s >= 0 && s < DRIVE_COUNT; }
static const char* slot_name(int s) {
  static char name[4];
  if (s == DRIVE_RP0) return "RP0";
  if (s >= DRIVE_A && s <= DRIVE_D) {
    name[0] = 'A' + s;
    name[1] = 0;
    return name;
  }
  return "?";
}

bool disk_mount(int slot, const char* path) {
  SD_FTP_StorageGuard guard;
  if (!slot_valid(slot) || !path || !*path) return false;

  disk_dismount(slot);   // ensure clean slot

  // Prefer read-write; fall back to read-only if the image is write-protected.
  bool readonly = false;
  File f = SD_MMC.open(path, "r+");
  if (!f) {
    f = SD_MMC.open(path, "r");
    if (!f) {
      LOGE("disk_mount[%d]: cannot open %s", slot, path);
      return false;
    }
    readonly = true;
  }
  uint32_t sz = (uint32_t)f.size();

  // PDP-11 era disk images come in several sizes (RL01 = 5 MB,
  // RL02 = 10 MB, RK05 = 2.5 MB) and some carry small SimH-style
  // headers. Accept anything between 100 KB and 256 MB; the RL11/RK11/RH11
  // emulators are responsible for sanity-checking offsets against the
  // slot's actual size.
  const uint32_t MIN_IMAGE = 100u * 1024u;
  const uint32_t MAX_IMAGE = 256u * 1024u * 1024u;
  if (sz < MIN_IMAGE || sz > MAX_IMAGE) {
    LOGE("disk_mount[%d]: %s is %u bytes, out of range [%u..%u]",
         slot, path, (unsigned)sz, (unsigned)MIN_IMAGE, (unsigned)MAX_IMAGE);
    f.close();
    return false;
  }
  g_drv[slot].file     = f;
  g_drv[slot].mounted  = true;
  g_drv[slot].changed  = true;
  g_drv[slot].readonly = readonly;
  g_drv[slot].size     = sz;
  g_drv[slot].reads    = 0;
  g_drv[slot].writes   = 0;
  strncpy(g_drv[slot].path, path, sizeof(g_drv[slot].path) - 1);
  g_drv[slot].path[sizeof(g_drv[slot].path) - 1] = 0;

  LOG("disk_mount[%s]: %s (%u bytes)%s", slot_name(slot), path, (unsigned)sz,
      readonly ? " [read-only]" : "");
  return true;
}

void disk_dismount(int slot) {
  SD_FTP_StorageGuard guard;
  if (!slot_valid(slot)) return;
  if (g_drv[slot].mounted) {
    g_drv[slot].file.close();
    LOG("disk_dismount[%s]: %s", slot_name(slot), g_drv[slot].path);
  }
  g_drv[slot].mounted = false;
  g_drv[slot].size = 0;
  g_drv[slot].path[0] = 0;
}

bool disk_is_mounted(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted;
}

bool disk_is_readonly(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted && g_drv[slot].readonly;
}

const char* disk_path(int slot) {
  return slot_valid(slot) ? g_drv[slot].path : "";
}

uint32_t disk_size_bytes(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted ? g_drv[slot].size : 0;
}

int disk_read(int slot, uint32_t byte_offset, void* buf, uint32_t bytes) {
  SD_FTP_StorageGuard guard;
  if (!disk_is_mounted(slot)) return -1;
  DriveSlot& d = g_drv[slot];
  if (byte_offset > d.size || bytes > d.size - byte_offset) {
    LOGE("disk_read[%s]: out of range off=%u len=%u size=%u",
         slot_name(slot), (unsigned)byte_offset, (unsigned)bytes, (unsigned)d.size);
    return -1;
  }
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_read[%s]: seek to %u failed", slot_name(slot), (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.read((uint8_t*)buf, bytes);
  d.reads++;
  if (n != bytes) {
    LOGE("disk_read[%s]: short read %u/%u at off %u",
         slot_name(slot), (unsigned)n, (unsigned)bytes, (unsigned)byte_offset);
    return -1;
  }
  return (int)bytes;
}

int disk_write(int slot, uint32_t byte_offset, const void* buf, uint32_t bytes) {
  SD_FTP_StorageGuard guard;
  if (!disk_is_mounted(slot)) return -1;
  DriveSlot& d = g_drv[slot];
  if (d.readonly) return -1;
  if (byte_offset > d.size || bytes > d.size - byte_offset) {
    LOGE("disk_write[%s]: out of range off=%u len=%u size=%u",
         slot_name(slot), (unsigned)byte_offset, (unsigned)bytes, (unsigned)d.size);
    return -1;
  }
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_write[%s]: seek to %u failed", slot_name(slot), (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.write((const uint8_t*)buf, bytes);
  d.file.flush();          // write-through
  d.writes++;
  if (n != bytes) {
    LOGE("disk_write[%s]: short write %u/%u", slot_name(slot), (unsigned)n, (unsigned)bytes);
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
