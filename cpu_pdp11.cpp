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
#include "rl11.h"
#include "ky11.h"
#include "dd11.h"
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

// ---- ring buffer of last N instructions for post-HALT diagnosis ----
// Each cpu_run iteration captures (PC, opcode, R0..R7, PS) before the
// instruction executes. On panic() we dump the most recent entries so
// the user can identify which sam11 instruction tripped the HALT.
struct TraceEntry {
  uint16_t pc;
  uint16_t instr;
  uint16_t r[8];
  uint16_t ps;
};
#define TRACE_RING_SIZE 64
static TraceEntry s_trace_ring[TRACE_RING_SIZE];
static uint32_t   s_trace_idx = 0;  // next write slot
static bool       s_trace_enable = true;

// sam11 stubs (referenced only in PRINTSIMLINES / DEBUG paths, all
// compile-time false in our build, but the symbols still need to exist).
// Note: printstate() and disasm() are NOT defined here - they live in
// disasm.cpp which is part of the vendored sam11 set.
void trap(uint16_t /*num*/) { /* deferred to sam11's own trap path */ }

// panic() is called by sam11 on conditions it considers unrecoverable
// (HALT instruction, RESET in user mode, invalid current/previous
// user-mode bits, etc.). We log the CPU state, set a flag, and longjmp
// out so the host loop can recover and continue running (instead of
// deadlocking core 1).
volatile bool g_panicked = false;
void panic() {
  LOGE("PDP-11 panic: halted  PC=0%o  R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o  PS=0%o",
       (unsigned)kd11::curPC,
       (unsigned)kd11::R[0], (unsigned)kd11::R[1], (unsigned)kd11::R[2],
       (unsigned)kd11::R[3], (unsigned)kd11::R[4], (unsigned)kd11::R[5],
       (unsigned)kd11::R[6], (unsigned)kd11::PS);
  // Memory window around curPC to spot the trapping instruction.
  uint32_t pc = kd11::curPC;
  Serial.printf("[vpdp1140]   mem near PC:");
  for (int o = -4; o <= 4; o++) {
    uint32_t a = (pc + o*2) & 0xFFFF;
    Serial.printf(" %s%06o", o == 0 ? "[" : " ", (unsigned)dd11::read16(a));
    if (o == 0) Serial.printf("]");
  }
  Serial.printf("\r\n");

  // Dump the last TRACE_RING_SIZE instructions leading up to the HALT.
  // s_trace_idx points at the next write slot, so the oldest valid
  // entry is at (s_trace_idx - TRACE_RING_SIZE) mod TRACE_RING_SIZE.
  Serial.printf("[vpdp1140]   --- last %d instructions before HALT ---\r\n",
                TRACE_RING_SIZE);
  for (int n = TRACE_RING_SIZE; n > 0; n--) {
    uint32_t i = (s_trace_idx + TRACE_RING_SIZE - n) % TRACE_RING_SIZE;
    TraceEntry& e = s_trace_ring[i];
    if (e.pc == 0 && e.instr == 0) continue;  // empty slot before fill
    Serial.printf("  PC=%06o ins=%06o R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o SP=%06o PS=%06o\r\n",
                  (unsigned)e.pc, (unsigned)e.instr,
                  (unsigned)e.r[0], (unsigned)e.r[1], (unsigned)e.r[2],
                  (unsigned)e.r[3], (unsigned)e.r[4], (unsigned)e.r[5],
                  (unsigned)e.r[6], (unsigned)e.ps);
  }

  g_panicked = true;
  // Bail out of step() via the trap path so cpu_run can return.
  longjmp(trapbuf, 0);
}

// ---- our PSRAM-backed guest memory ----
static uint8_t*       s_mem = nullptr;
static uint32_t       s_inst_count = 0;
static volatile bool  s_halt_requested = false;
static bool           s_sam11_inited = false;
// 0 = RL (bootrom_rl0, for RL02 XXDP+/etc.), 1 = RK (bootrom_rk0, for RK05
// RT-11/Unix V6/etc.). vpdp1140.ino sets this from cfg.boot_kind before
// calling cpu_reset().
static int            s_boot_kind = 0;

void cpu_set_boot_kind(int kind) { s_boot_kind = (kind == 1) ? 1 : 0; }


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
  // BOOT_START. We add ky11::reset() (the front panel) since sam11.cpp
  // does that outside kd11::reset(), and rl11::reset() because vpdp1140
  // boots from RL02 not RK05.
  ky11::reset();
  kd11::reset();
  rl11::reset();
  rk11::reset();

  // Install the chosen boot ROM at BOOT_START (02000 octal). kd11::reset()
  // installs bootrom_rk0 by default; we overwrite that region with either
  // bootrom_rl0 (RL02 packs - XXDP+, RSTS, V6 on RL) or bootrom_rk0
  // (RK05 packs - RT-11, V6 on RK). The boot ROM does the actual disk-
  // sector-0 load and jump-to-zero; we just stamp it into RAM.
  if (s_boot_kind == 1) {
    const uint32_t rk_words = sizeof(bootrom_rk0) / sizeof(uint16_t);
    LOG("cpu_reset: loading RK0 boot ROM (%u words) at PC = 0%o",
        (unsigned)rk_words, (unsigned)BOOT_START);
    for (uint32_t i = 0; i < rk_words; i++) {
      dd11::write16(BOOT_START + (i * 2), bootrom_rk0[i]);
    }
  } else {
    const uint32_t rl_words = sizeof(bootrom_rl0) / sizeof(uint16_t);
    LOG("cpu_reset: loading RL0 boot ROM (%u words) at PC = 0%o",
        (unsigned)rl_words, (unsigned)BOOT_START);
    for (uint32_t i = 0; i < rl_words; i++) {
      dd11::write16(BOOT_START + (i * 2), bootrom_rl0[i]);
    }
  }

  // ----- Banner program at 01000 -----
  // A tiny native PDP-11 program that prints "vpdp1140: booting PDP-11..."
  // to the KL11 console then jumps into the RL0 boot ROM at 02000. Lets
  // us verify the KL11 -> TFT/Telnet/Serial pipe independent of any
  // disk-boot path. Code + message live below the bootrom, well below
  // any stack the bootrom will set up (SP = 02000).
  //
  //   01000  MOV   #MSG, R0       ; 012700 + 01100
  //   01004  MOVB  (R0)+, R1
  //   01006  BEQ   +6              ; if char==0 -> JMP @#02000
  //   01010  TSTB  @#TPS           ; 105737 + 0177564  (TWO words!)
  //   01014  BPL   -3              ; back to TSTB (skip BOTH its words)
  //   01016  MOVB  R1, @#TPB       ; 110137 + 0177566
  //   01022  BR    -8              ; next char
  //   01024  JMP   @#02000         ; 000137 + 002000
  //   01100  "vpdp1140: booting PDP-11...\r\n\0"
  static const uint16_t banner_prog[] = {
    0012700, 0001100,   // MOV  #01100, R0
    0112001,            // MOVB (R0)+, R1
    0001406,            // BEQ  +6 -> JMP at 01024
    0105737, 0177564,   // TSTB @#TPS  (2-word instr)
    0100375,            // BPL  -3 -> back to 01010 TSTB
    0110137, 0177566,   // MOVB R1, @#TPB  (2-word instr)
    0000770,            // BR   -8 -> back to 01004 MOVB(R0)+
    0000137, 0002000,   // JMP  @#02000
  };
  const uint32_t banner_words = sizeof(banner_prog) / sizeof(uint16_t);
  for (uint32_t i = 0; i < banner_words; i++) {
    dd11::write16(0001000 + (i * 2), banner_prog[i]);
  }
  const char* msg = (s_boot_kind == 1)
                  ? "vpdp1140: booting PDP-11/40 from RK0...\r\n"
                  : "vpdp1140: booting PDP-11/40 from DL0...\r\n";
  uint8_t* bytes = (uint8_t*)s_mem;
  uint32_t mi = 0;
  while (msg[mi]) { bytes[0001100 + mi] = (uint8_t)msg[mi]; mi++; }
  bytes[0001100 + mi] = 0;
  LOG("cpu_reset: banner installed at 01000, msg at 01100 (%u chars)", (unsigned)mi);

  // Start the CPU at the banner program (01000); it prints + JMP @#02000.
  kd11::R[7]  = 0001000;
  kd11::curPC = 0001000;
  LOG("cpu_reset: ready, R7 = 0%o (banner -> bootrom @02000)",
      (unsigned)kd11::R[7]);

  g_panicked = false;

  // Clear the instruction-trace ring so the post-HALT dump shows only
  // entries from this run, not stale ones from the previous boot.
  for (int i = 0; i < TRACE_RING_SIZE; i++) {
    s_trace_ring[i].pc = 0;
    s_trace_ring[i].instr = 0;
  }
  s_trace_idx = 0;

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
  if (!s_sam11_inited || g_panicked) { yield(); return 0; }

  uint32_t executed = 0;
  // Catch panics (longjmp from panic()) BEFORE we'd otherwise route them
  // through kd11::trapat() with a bogus odd vector and recurse forever.
  uint16_t vec = setjmp(trapbuf);
  if (g_panicked) {
    return executed;
  }
  if (vec) {
    kd11::trapat(vec);
    return executed;  // re-enter cpu_run() to install a fresh trapbuf
  }

  while (executed < max_cycles && !s_halt_requested && !g_panicked) {
    // Pending interrupt? handleinterrupt() loads the new PC/PSW from the
    // vector and returns - it may longjmp out for nested-trap cases, in
    // which case the setjmp at the top of this function will catch it.
    // Do NOT return here, or we never get to actually run an instruction
    // when the line clock or other periodic IRQ is pending most of the
    // time (MIPS would read 0).
    if (itab[0].vec && (itab[0].pri >= ((kd11::PS >> 5) & 7))) {
      kd11::handleinterrupt();
    }
    // Record this instruction in the trace ring BEFORE step() runs,
    // so on panic() we can see exactly what was about to execute.
    if (s_trace_enable) {
      TraceEntry& e = s_trace_ring[s_trace_idx];
      e.pc    = kd11::R[7];
      e.instr = dd11::read16(kt11::decode_instr(kd11::R[7], false, kd11::curuser));
      for (int i = 0; i < 8; i++) e.r[i] = (uint16_t)kd11::R[i];
      e.ps    = kd11::PS;
      s_trace_idx = (s_trace_idx + 1) % TRACE_RING_SIZE;
    }
    kd11::step();
    kw11::tick();
    rk11::tick();   // drives the deferred RK-done IRQ countdown
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
