// cpu_pdp11.cpp - PDP-11/40 CPU core wrapper around sam11.
//
// Cloned from v8088/cpu8086.cpp on 2026-05-23. m0 was the no-op stub;
// m1 (this revision) wires up sam11's kd11 (11/40 CPU), kt11 (MMU),
// ms11 (RAM - replaced with our PSRAM-backed version), dd11 (UNIBUS
// dispatcher), and the stubbed device modules (kl11, rk11, kw11,
// lp11, ky11). We do NOT include sam11.cpp because it defines its
// own setup()/loop()/panic() that would collide with vpdp1140.ino;
// instead this file mirrors sam11.cpp's loop0() pattern inside
// cpu_run().
//
// PDP-11/23 was the original ask; the project went 1170 -> 1140 on
// 2026-05-23 because sam11's MMU is hard 18-bit / 248 KiB.
// https://pdp-11.org.ru/en/files.pl - images of various products

#include "cpu_pdp11.h"
#include "platform.h"
#include "sam11.h"
#include "sam11_platform.h"
#include "kd11.h"
#include "kt11.h"
#include "ms11.h"
#include "kl11.h"
#include "kw11.h"
#include "rk11.h"
#include "ky11.h"
#include "bootrom.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <setjmp.h>

// ---- globals normally defined in sam11.cpp ----
// kd11.cpp already defines `pdp11::intr itab[ITABN]`.
jmp_buf trapbuf;

// User-mode label tables (kd11/kt11/etc. reference these in debug paths).
const char* users_str[4]  = { "kernel", "illegal", "illegal", "user" };
const char  users_char[4] = { 'K', 'X', 'X', 'U' };

// sam11 stubs (referenced only in PRINTSIMLINES / DEBUG paths, all
// compile-time false in our build, but the symbols still need to exist).
// Note: printstate() and disasm() are NOT defined here - they live in
// disasm.cpp which is part of the vendored sam11 set.
void trap(uint16_t /*num*/) { /* deferred to sam11's own trap path */ }
void panic() {
  LOGE("PDP-11 panic: halted");
  // sam11 calls panic() on unrecoverable conditions. For now we just
  // log and spin; later we can surface this on the TFT.
  for (;;) delay(1000);
}

// ---- our PSRAM-backed guest memory ----
static uint8_t*       s_mem = nullptr;
static uint32_t       s_inst_count = 0;
static volatile bool  s_halt_requested = false;
static bool           s_sam11_inited = false;

bool cpu_init() {
  if (s_mem) return true;
  s_mem = (uint8_t*)heap_caps_aligned_alloc(
      32, VPDP_RAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_mem) {
    LOGE("cpu_init: PSRAM alloc failed (%u bytes)", (unsigned)VPDP_RAM_SIZE);
    return false;
  }
  memset(s_mem, 0, VPDP_RAM_SIZE);
  s_inst_count     = 0;
  s_halt_requested = false;
  ms11::begin();
  // First call to cpu_reset() will pull in the bootrom and reset the CPU.
  s_sam11_inited = false;
  LOG("cpu_init: %u bytes guest RAM allocated in PSRAM (sam11 KD11/KT11 wired)",
      (unsigned)VPDP_RAM_SIZE);
  return true;
}

void cpu_reset() {
  if (!s_mem) return;
  // kd11::reset() does the heavy lifting: zeros the GPRs/PS/MMU, calls
  // kw11::reset() + ms11::clear() + kl11::reset() + rk11::reset(), writes
  // sam11's bootrom_rk0 boot block to BOOT_START in RAM, sets R7 to
  // BOOT_START. We just add ky11::reset() (the front panel) since sam11.cpp
  // does that outside kd11::reset().
  ky11::reset();
  kd11::reset();
  s_sam11_inited = true;
  s_halt_requested = false;
}

void cpu_cold_boot() {
  if (s_mem) memset(s_mem, 0, VPDP_RAM_SIZE);
  cpu_reset();
}

// Step the CPU up to max_cycles times. Mirrors sam11.cpp's loop0() pattern:
// pending-interrupt check, kd11::step(), kw11::tick(), kl11::poll(). Traps
// are caught by the setjmp/longjmp pair (kd11/dd11 longjmp to trapbuf with
// the trap vector, and we route it back through kd11::trapat()).
uint32_t cpu_run(uint32_t max_cycles) {
  if (!s_sam11_inited) { yield(); return 0; }
  uint32_t executed = 0;

  // setjmp returns 0 the first time and the trap vector when a longjmp
  // arrives from inside the step path. We MUST set up trapbuf each loop
  // iteration the same way sam11 does.
  uint16_t vec = setjmp(trapbuf);
  if (vec) {
    kd11::trapat(vec);
  }

  while (executed < max_cycles && !s_halt_requested) {
    // Pending interrupt?
    if (itab[0].vec && (itab[0].pri >= ((kd11::PS >> 5) & 7))) {
      kd11::handleinterrupt();
      // After handleinterrupt() we re-setjmp by continuing the outer loop
      // (handleinterrupt may longjmp). Easiest: return now and the caller
      // will re-enter cpu_run() which re-installs the trapbuf.
      return executed;
    }
    kd11::step();
    kw11::tick();
    kl11::poll();
    executed++;
    s_inst_count++;
  }
  return executed;
}

void cpu_request_halt() { s_halt_requested = true; }

uint8_t* cpu_mem()       { return s_mem; }
uint32_t cpu_mem_size()  { return VPDP_RAM_SIZE; }

uint16_t cpu_reg16(int idx) {
  if (idx < 0 || idx > 7) return 0;
  return (uint16_t)kd11::R[idx];
}
uint16_t cpu_pc()         { return kd11::curPC; }
uint16_t cpu_psw()        { return kd11::PS; }
uint32_t cpu_inst_count() { return s_inst_count; }

void cpu_set_pc(uint16_t pc) { kd11::R[7] = pc; kd11::curPC = pc; }

// ---- m1 self-test --------------------------------------------------------
//
// Writes the following PDP-11 program at octal address 02000:
//   02000: 012700  MOV #5, R0      \ two-word: opcode + immediate
//   02002: 000005
//   02004: 012701  MOV #7, R1      \ two-word: opcode + immediate
//   02006: 000007
//   02010: 060001  ADD R0, R1      ; R1 := R0 + R1 = 5 + 7 = 12 (014 octal)
//   02012: 000777  BR .-2          ; spin in place
// After ~20 cycles we expect R0==5, R1==014. The BR-to-self lets us verify
// without relying on HALT (sam11's HALT handler triggers special behavior).

bool cpu_selftest() {
  if (!s_mem) {
    LOGE("cpu_selftest: PSRAM not allocated");
    return false;
  }
  // Initialize sam11 state. kd11::reset() also installs the RK0 bootrom at
  // BOOT_START=02000; we overwrite that region with our test program.
  kd11::reset();
  ky11::reset();

  uint16_t* w = (uint16_t*)s_mem;
  uint32_t addr = 0002000;
  w[addr >> 1] = 0012700; addr += 2;
  w[addr >> 1] = 0000005; addr += 2;
  w[addr >> 1] = 0012701; addr += 2;
  w[addr >> 1] = 0000007; addr += 2;
  w[addr >> 1] = 0060001; addr += 2;
  w[addr >> 1] = 0000777; // BR .-2 (spin)

  kd11::R[7]   = 0002000;
  kd11::curPC  = 0002000;
  kd11::PS     = 0;
  s_sam11_inited = true;

  uint32_t pre_inst = s_inst_count;
  uint32_t executed = cpu_run(20);
  uint32_t r0 = (uint32_t)kd11::R[0];
  uint32_t r1 = (uint32_t)kd11::R[1];
  uint32_t r7 = (uint32_t)kd11::R[7];

  bool pass = (r0 == 5) && (r1 == 014); // 014 octal = 12 decimal

  LOG("cpu_selftest: R0=%o R1=%o R7=%o ran=%u total=%u -> %s",
      (unsigned)r0, (unsigned)r1, (unsigned)r7,
      (unsigned)executed, (unsigned)(s_inst_count - pre_inst),
      pass ? "PASS" : "FAIL");

  // Reset s_inst_count so the MIPS readout in the status bar starts at 0
  // when the real boot begins; the test instructions otherwise show up as
  // a one-time spike in the rate.
  s_inst_count = 0;
  return pass;
}
