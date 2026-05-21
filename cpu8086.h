#pragma once
#include <stdint.h>
#include <stddef.h>

// Public surface of the 8088 CPU core.
//
// Memory model: a single 0x10FFF0-byte block in PSRAM owned by the core.
// Segment:offset access is mem[16*seg + off]. Registers live at mem[0xF0000..]
// per 8086tiny convention.

#define V8088_RAM_SIZE   0x10FFF0u
#define V8088_REGS_BASE  0xF0000u

// Index constants for cpu_reg16().
enum {
  V8088_AX = 0, V8088_CX, V8088_DX, V8088_BX,
  V8088_SP,     V8088_BP, V8088_SI, V8088_DI,
  V8088_ES,     V8088_CS, V8088_SS, V8088_DS,
};

// Lifecycle ---------------------------------------------------------------

// Allocate guest RAM in PSRAM, copy the BIOS blob, populate decode tables.
// Safe to call multiple times - returns true if already initialized.
bool cpu_init();

// 8086 reset: CS=0xF000, IP=0x0100 (BIOS entry per 8086tiny convention).
void cpu_reset();

// Full cold boot: wipe conventional RAM + video, reload a pristine BIOS image
// (so the BIOS re-runs its cold-boot disk init), re-build decode tables.
// Use this for the menu's "Reboot 8088"; follow with cpu_set_hd_sectors() etc.
void cpu_cold_boot();

// Execute up to `max_cycles` instructions. Returns count actually run; stops
// early if the core halts (CS:IP wraps to 0:0).
uint32_t cpu_run(uint32_t max_cycles);

// Convenience: stop the CPU loop. (Sets a flag the next cpu_run() reads.)
void cpu_request_halt();

// Memory + register accessors --------------------------------------------

uint8_t* cpu_mem();                 // base of the 1 MB block (PSRAM)
uint32_t cpu_mem_size();            // == V8088_RAM_SIZE

uint16_t cpu_reg16(int idx);        // 0..11 per V8088_* enum
uint16_t cpu_ip();
uint32_t cpu_inst_count();

// Manually set CS:IP - used by m3 test rig to start a custom program.
void cpu_set_cs_ip(uint16_t cs, uint16_t ip);

// Set the boot drive byte (DL on entry to BIOS). 0x00=A, 0x80=C.
void cpu_set_boot_drive(uint8_t dl);

// Tell the BIOS the hard-disk size, in 512-byte sectors, via CX:AX. The BIOS
// reads this once at cold boot to compute HD geometry and expose INT 13h for
// DL=0x80. Pass 0 for "no hard disk". Call after cpu_reset(), before cpu_run().
void cpu_set_hd_sectors(uint32_t sectors);
