// KW11-P Programmable Real-Time Clock emulation for vpdp1140.
//
// Three live registers at 0o772540..0o772544 (CSR, CSB, CNTR). Counts down
// at a programmable rate; on underflow sets DONE and (if IE) interrupts at
// vector INTRTC=0o104, BR5. In repeat mode the counter reloads from CSB.
//
// Added to give RSTS V7 / RT-11 / RSX a scheduler-quantum tick source.
// Previously stubbed in dd11.cpp; that stub block is now removed for this
// address range.

#pragma once
#include "pdp1140.h"

namespace kwp {

extern uint16_t CSR;
extern uint16_t CSB;
extern uint16_t CNTR;

// Runtime gate: when false the module is a pure absorber - all reads
// in the 0o772540..0o772556 window return 0, writes are silently
// discarded, tick() is a no-op. Required for RSTS V4B (a working
// KW11-P breaks its terminal driver's case handling). Set by
// vpdp1140.ino from cfg.kwp_enabled after config_load_pdp.
extern bool enabled;

void     reset();
void     tick();
uint16_t read16(uint32_t a);
void     write16(uint32_t a, uint16_t v);

};  // namespace kwp
