#pragma once
#include <stdint.h>

// Four guest drives. A:/B: are 1.44 MB floppies, C:/D: are 32 MB HDDs.
enum {
  DRIVE_A = 0,   // floppy
  DRIVE_B = 1,   // floppy
  DRIVE_C = 2,   // hard disk
  DRIVE_D = 3,   // hard disk
  DRIVE_COUNT = 4
};

// Mount an image file (path on the SD card) into a drive slot.
// Validates the size against the expected floppy/HDD geometry.
// Returns true on success.
bool disk_mount(int slot, const char* path);

// Close the image and free the slot.
void disk_dismount(int slot);

bool        disk_is_mounted(int slot);
const char* disk_path(int slot);          // mounted path, or ""
uint32_t    disk_size_bytes(int slot);    // 0 if not mounted

// Byte-level transfer (matches the 8086tiny DISK_READ/WRITE semantics:
// the BIOS passes a byte offset and a byte count). Returns bytes
// transferred, or -1 on error.
int disk_read (int slot, uint32_t byte_offset, void* buf, uint32_t bytes);
int disk_write(int slot, uint32_t byte_offset, const void* buf, uint32_t bytes);

// Diagnostics counter (reads/writes since boot).
void disk_stats(int slot, uint32_t* reads, uint32_t* writes);

// Media-change flag: set true whenever an image is (re)mounted. Returns the
// current value and clears it. INT 13h uses this to tell DOS a floppy was
// swapped, so DOS re-reads the FAT without needing a reboot.
bool disk_take_change(int slot);

// Create a blank, pre-formatted 1.44 MB FAT12 floppy image on the SD card so
// DOS can use it immediately (a zeroed image has a garbage BPB).
bool disk_create_floppy(const char* path);
