// 8086tiny: a tiny, highly functional, highly portable PC emulator/VM
// Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
//
// Revision 1.25 - MIT License
//
// ESP32-S3 port for v8088 (2026): main() replaced by v8088_init/v8088_reset/v8088_run_one
// so the decoder can be driven from an Arduino sketch. Host hooks for disk/console/RTC
// are declared extern and implemented in cpu8086.cpp. SDL/POSIX/termios stripped.
//
// PORTING NOTE: every `(char)` cast here is for SIGN extension of 8-bit immediates
// (relative jumps, displacements, IMUL/IDIV operands). On x86 `char` is signed, but
// on Xtensa/ESP32 `char` is UNSIGNED - so the casts were changed to `(signed char)`.
// Do not revert them or relative jumps will land 256 bytes off.

#include <stdint.h>
#include <string.h>

#define NO_GRAPHICS 1

// ------------------------------------------------------------------ host hooks
// Implemented in cpu8086.cpp.
extern void          v8088_host_putchar(unsigned char c);
extern int           v8088_host_keyboard_poll(unsigned char* out);
extern unsigned int  v8088_host_disk_sectors(int slot);
extern int           v8088_host_disk_read (int slot, unsigned int lba, void* buf, unsigned int sectors);
extern int           v8088_host_disk_write(int slot, unsigned int lba, const void* buf, unsigned int sectors);
extern void          v8088_host_get_rtc(unsigned char* dest36);
// Software-interrupt interception: returns non-zero if the host fully handled
// the INT (used for INT 13h disk services and INT 11h equipment list), in
// which case the normal interrupt dispatch is skipped.
extern int           v8088_host_intercept(unsigned char int_num);

// ------------------------------------------------------------------ constants
#define IO_PORT_COUNT 0x10000
#define RAM_SIZE 0x10FFF0
#define REGS_BASE 0xF0000
#define VIDEO_RAM_SIZE 0x10000

#define KEYBOARD_TIMER_UPDATE_DELAY 20000

// 16-bit register decodes
#define REG_AX 0
#define REG_CX 1
#define REG_DX 2
#define REG_BX 3
#define REG_SP 4
#define REG_BP 5
#define REG_SI 6
#define REG_DI 7

#define REG_ES 8
#define REG_CS 9
#define REG_SS 10
#define REG_DS 11

#define REG_ZERO 12
#define REG_SCRATCH 13

// 8-bit register decodes
#define REG_AL 0
#define REG_AH 1
#define REG_CL 2
#define REG_CH 3
#define REG_DL 4
#define REG_DH 5
#define REG_BL 6
#define REG_BH 7

// FLAGS register decodes
#define FLAG_CF 40
#define FLAG_PF 41
#define FLAG_AF 42
#define FLAG_ZF 43
#define FLAG_SF 44
#define FLAG_TF 45
#define FLAG_IF 46
#define FLAG_DF 47
#define FLAG_OF 48

// Lookup tables in the BIOS binary
#define TABLE_XLAT_OPCODE 8
#define TABLE_XLAT_SUBFUNCTION 9
#define TABLE_STD_FLAGS 10
#define TABLE_PARITY_FLAG 11
#define TABLE_BASE_INST_SIZE 12
#define TABLE_I_W_SIZE 13
#define TABLE_I_MOD_SIZE 14
#define TABLE_COND_JUMP_DECODE_A 15
#define TABLE_COND_JUMP_DECODE_B 16
#define TABLE_COND_JUMP_DECODE_C 17
#define TABLE_COND_JUMP_DECODE_D 18
#define TABLE_FLAGS_BITFIELDS 19

#define FLAGS_UPDATE_SZP 1
#define FLAGS_UPDATE_AO_ARITH 2
#define FLAGS_UPDATE_OC_LOGIC 4

// ------------------------------------------------------------------ macros
#define DECODE_RM_REG scratch2_uint = 4 * !i_mod, \
					  op_to_addr = rm_addr = i_mod < 3 ? SEGREG(seg_override_en ? seg_override : bios_table_lookup[scratch2_uint + 3][i_rm], bios_table_lookup[scratch2_uint][i_rm], regs16[bios_table_lookup[scratch2_uint + 1][i_rm]] + bios_table_lookup[scratch2_uint + 2][i_rm] * i_data1+) : GET_REG_ADDR(i_rm), \
					  op_from_addr = GET_REG_ADDR(i_reg), \
					  i_d && (scratch_uint = op_from_addr, op_from_addr = rm_addr, op_to_addr = scratch_uint)

#define GET_REG_ADDR(reg_id) (REGS_BASE + (i_w ? 2 * reg_id : 2 * reg_id + reg_id / 4 & 7))

#define TOP_BIT 8*(i_w + 1)

#define OPCODE ;break; case
#define OPCODE_CHAIN ; case

#define MUL_MACRO(op_data_type,out_regs) (set_opcode(0x10), \
										  out_regs[i_w + 1] = (op_result = CAST(op_data_type)mem[rm_addr] * (op_data_type)*out_regs) >> 16, \
										  regs16[REG_AX] = op_result, \
										  set_OF(set_CF(op_result - (op_data_type)op_result)))
#define DIV_MACRO(out_data_type,in_data_type,out_regs) (scratch_int = CAST(out_data_type)mem[rm_addr]) && !(scratch2_uint = (in_data_type)(scratch_uint = (out_regs[i_w+1] << 16) + regs16[REG_AX]) / scratch_int, scratch2_uint - (out_data_type)scratch2_uint) ? out_regs[i_w+1] = scratch_uint - scratch_int * (*out_regs = scratch2_uint) : pc_interrupt(0)
#define DAA_DAS(op1,op2,mask,min) set_AF((((scratch2_uint = regs8[REG_AL]) & 0x0F) > 9) || regs8[FLAG_AF]) && (op_result = regs8[REG_AL] op1 6, set_CF(regs8[FLAG_CF] || (regs8[REG_AL] op2 scratch2_uint))), \
								  set_CF((((mask & 1 ? scratch2_uint : regs8[REG_AL]) & mask) > min) || regs8[FLAG_CF]) && (op_result = regs8[REG_AL] op1 0x60)
#define ADC_SBB_MACRO(a) OP(a##= regs8[FLAG_CF] +), \
						 set_CF(regs8[FLAG_CF] && (op_result == op_dest) || (a op_result < a(int)op_dest)), \
						 set_AF_OF_arith()

#define R_M_OP(dest,op,src) (i_w ? op_dest = CAST(unsigned short)dest, op_result = CAST(unsigned short)dest op (op_source = CAST(unsigned short)src) \
								 : (op_dest = dest, op_result = dest op (op_source = CAST(unsigned char)src)))
#define MEM_OP(dest,op,src) R_M_OP(mem[dest],op,mem[src])
#define OP(op) MEM_OP(op_to_addr,op,op_from_addr)

#define INDEX_INC(reg_id) (regs16[reg_id] -= (2 * regs8[FLAG_DF] - 1)*(i_w + 1))

#define R_M_PUSH(a) (i_w = 1, R_M_OP(mem[SEGREG(REG_SS, REG_SP, --)], =, a))
#define R_M_POP(a) (i_w = 1, regs16[REG_SP] += 2, R_M_OP(a, =, mem[SEGREG(REG_SS, REG_SP, -2+)]))

#define SEGREG(reg_seg,reg_ofs,op) 16 * regs16[reg_seg] + (unsigned short)(op regs16[reg_ofs])

#define SIGN_OF(a) (1 & (i_w ? CAST(short)a : a) >> (TOP_BIT - 1))

#define CAST(a) *(a*)&

// Keyboard driver - host poll. mem[0x4A6] = scancode, then raise int 7.
#define KEYBOARD_DRIVER (v8088_host_keyboard_poll(mem + 0x4A6) && (pc_interrupt(7), 0))
#define SDL_KEYBOARD_DRIVER KEYBOARD_DRIVER

// ------------------------------------------------------------------ globals
unsigned char *mem = 0;             // 1.07 MB PSRAM block, set by v8088_set_mem()
unsigned char io_ports[IO_PORT_COUNT];
unsigned char *opcode_stream, *regs8;
unsigned char i_rm, i_w, i_reg, i_mod, i_mod_size, i_d, i_reg4bit, raw_opcode_id, xlat_opcode_id, extra, rep_mode, seg_override_en, rep_override_en, trap_flag, int8_asap, scratch_uchar, io_hi_lo, *vid_mem_base, spkr_en;
unsigned char bios_table_lookup[20][256];
unsigned short *regs16, reg_ip, seg_override, file_index, wave_counter;
unsigned int op_source, op_dest, rm_addr, op_to_addr, op_from_addr, i_data0, i_data1, i_data2, scratch_uint, scratch2_uint, inst_counter, set_flags_type, GRAPHICS_X, GRAPHICS_Y, pixel_colors[16], vmem_ctr;
int op_result, disk[3], scratch_int;
unsigned int v8088_halted = 0;

// ------------------------------------------------------------------ helpers
char set_CF(int new_CF) { return regs8[FLAG_CF] = !!new_CF; }
char set_AF(int new_AF) { return regs8[FLAG_AF] = !!new_AF; }
char set_OF(int new_OF) { return regs8[FLAG_OF] = !!new_OF; }

char set_AF_OF_arith(void)
{
	set_AF((op_source ^= op_dest ^ op_result) & 0x10);
	if (op_result == op_dest) return set_OF(0);
	return set_OF(1 & (regs8[FLAG_CF] ^ op_source >> (TOP_BIT - 1)));
}

void make_flags(void)
{
	scratch_uint = 0xF002;
	for (int i = 9; i--;)
		scratch_uint += regs8[FLAG_CF + i] << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i];
}

void set_flags(int new_flags)
{
	for (int i = 9; i--;)
		regs8[FLAG_CF + i] = !!(1 << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i] & new_flags);
}

void set_opcode(unsigned char opcode)
{
	xlat_opcode_id  = bios_table_lookup[TABLE_XLAT_OPCODE][raw_opcode_id = opcode];
	extra           = bios_table_lookup[TABLE_XLAT_SUBFUNCTION][opcode];
	i_mod_size      = bios_table_lookup[TABLE_I_MOD_SIZE][opcode];
	set_flags_type  = bios_table_lookup[TABLE_STD_FLAGS][opcode];
}

char pc_interrupt(unsigned char interrupt_num)
{
	set_opcode(0xCD);
	make_flags();
	R_M_PUSH(scratch_uint);
	R_M_PUSH(regs16[REG_CS]);
	R_M_PUSH(reg_ip);
	MEM_OP(REGS_BASE + 2 * REG_CS, =, 4 * interrupt_num + 2);
	R_M_OP(reg_ip, =, mem[4 * interrupt_num]);
	return regs8[FLAG_TF] = regs8[FLAG_IF] = 0;
}

int AAA_AAS(char which_operation)
{
	return (regs16[REG_AX] += 262 * which_operation*set_AF(set_CF(((regs8[REG_AL] & 0x0F) > 9) || regs8[FLAG_AF])), regs8[REG_AL] &= 0x0F);
}

// ------------------------------------------------------------------ init / reset
// Caller in cpu8086.cpp:
//   1. allocates mem in PSRAM and points us at it
//   2. copies the BIOS blob into mem + 0xF0100
//   3. calls v8088_init() to set up regs pointers and decode tables
//   4. optionally calls v8088_reset() to set CS=F000 IP=0x100 (BIOS entry)

void v8088_set_mem(unsigned char* m) { mem = m; }

void v8088_init(unsigned int hd_size_sectors)
{
	// regs16/regs8 point to F000:0 where the memory-mapped registers live.
	regs16 = (unsigned short *)(regs8 = mem + REGS_BASE);
	regs16[REG_CS] = 0xF000;
	regs8[FLAG_TF] = 0;

	// CS:AX gets hard-disk size in sectors when BIOS asks.
	*(unsigned*)&regs16[REG_AX] = hd_size_sectors;

	// disk[0] = HD, [1] = FD, [2] = BIOS. Slot numbers we route to host hooks.
	disk[0] = 0;   // HD slot
	disk[1] = 1;   // FD slot
	disk[2] = 2;   // BIOS slot (unused after init; BIOS already copied into mem)

	// Boot device in DL. Default 0 = FD.
	regs8[REG_DL] = 0;

	// IP starts where the BIOS lives.
	reg_ip = 0x100;

	// Populate the 20 decode tables from inside the BIOS image.
	for (int i = 0; i < 20; i++)
		for (int j = 0; j < 256; j++)
			bios_table_lookup[i][j] = regs8[regs16[0x81 + i] + j];

	memset(io_ports, 0, sizeof(io_ports));
	inst_counter   = 0;
	seg_override_en = 0;
	rep_override_en = 0;
	trap_flag      = 0;
	int8_asap      = 0;
	v8088_halted   = 0;
}

void v8088_reset(void)
{
	regs16[REG_CS] = 0xF000;
	reg_ip = 0x100;
	regs8[FLAG_TF] = 0;
	seg_override_en = 0;
	rep_override_en = 0;
	int8_asap = 0;
	v8088_halted = 0;
}

void v8088_set_boot_drive(unsigned char dl) { regs8[REG_DL] = dl; }

// Direct register access for diagnostics.
unsigned short v8088_get_reg16(int idx) { return regs16[idx]; }
unsigned short v8088_get_ip(void)       { return reg_ip; }
unsigned int   v8088_get_inst_count(void) { return inst_counter; }
int            v8088_is_halted(void)    { return (int)v8088_halted; }

// ------------------------------------------------------------------ core step
// Decode and execute ONE instruction at CS:IP. Returns 0 on halt (CS:IP=0:0),
// 1 otherwise. The body is the inside of the original main() while loop with
// the SDL graphics path stripped and disk/console/RTC ports vectored to
// v8088_host_* hooks.
int v8088_run_one(void)
{
	opcode_stream = mem + 16 * regs16[REG_CS] + reg_ip;
	if (opcode_stream == mem) { v8088_halted = 1; return 0; }

	set_opcode(*opcode_stream);

	i_w = (i_reg4bit = raw_opcode_id & 7) & 1;
	i_d = i_reg4bit / 2 & 1;

	i_data0 = CAST(short)opcode_stream[1];
	i_data1 = CAST(short)opcode_stream[2];
	i_data2 = CAST(short)opcode_stream[3];

	if (seg_override_en) seg_override_en--;
	if (rep_override_en) rep_override_en--;

	if (i_mod_size)
	{
		i_mod = (i_data0 & 0xFF) >> 6;
		i_rm  = i_data0 & 7;
		i_reg = i_data0 / 8 & 7;

		if ((!i_mod && i_rm == 6) || (i_mod == 2))
			i_data2 = CAST(short)opcode_stream[4];
		else if (i_mod != 1)
			i_data2 = i_data1;
		else
			i_data1 = (signed char)i_data1;

		DECODE_RM_REG;
	}

	switch (xlat_opcode_id)
	{
		OPCODE_CHAIN 0: // Conditional jump
			scratch_uchar = raw_opcode_id / 2 & 7;
			reg_ip += (signed char)i_data0 * (i_w ^ (regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_A][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_B][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_C][scratch_uchar]] ^ regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_D][scratch_uchar]]))
		OPCODE 1: // MOV reg, imm
			i_w = !!(raw_opcode_id & 8);
			R_M_OP(mem[GET_REG_ADDR(i_reg4bit)], =, i_data0)
		OPCODE 3:
			R_M_PUSH(regs16[i_reg4bit])
		OPCODE 4:
			R_M_POP(regs16[i_reg4bit])
		OPCODE 2:
			i_w = 1;
			i_d = 0;
			i_reg = i_reg4bit;
			DECODE_RM_REG;
			i_reg = extra
		OPCODE_CHAIN 5: // INC|DEC|JMP|CALL|PUSH
			if (i_reg < 2)
				MEM_OP(op_from_addr, += 1 - 2 * i_reg +, REGS_BASE + 2 * REG_ZERO),
				op_source = 1,
				set_AF_OF_arith(),
				set_OF(op_dest + 1 - i_reg == 1 << (TOP_BIT - 1)),
				(xlat_opcode_id == 5) && (set_opcode(0x10), 0);
			else if (i_reg != 6)
				i_reg - 3 || R_M_PUSH(regs16[REG_CS]),
				i_reg & 2 && R_M_PUSH(reg_ip + 2 + i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6)),
				i_reg & 1 && (regs16[REG_CS] = CAST(short)mem[op_from_addr + 2]),
				R_M_OP(reg_ip, =, mem[op_from_addr]),
				set_opcode(0x9A);
			else
				R_M_PUSH(mem[rm_addr])
		OPCODE 6:
			op_to_addr = op_from_addr;
			switch (i_reg)
			{
				OPCODE_CHAIN 0:
					set_opcode(0x20);
					reg_ip += i_w + 1;
					R_M_OP(mem[op_to_addr], &, i_data2)
				OPCODE 2:
					OP(=~)
				OPCODE 3:
					OP(=-);
					op_dest = 0;
					set_opcode(0x28);
					set_CF(op_result > op_dest)
				OPCODE 4:
					i_w ? MUL_MACRO(unsigned short, regs16) : MUL_MACRO(unsigned char, regs8)
				OPCODE 5:
					i_w ? MUL_MACRO(short, regs16) : MUL_MACRO(signed char, regs8)
				OPCODE 6:
					i_w ? DIV_MACRO(unsigned short, unsigned, regs16) : DIV_MACRO(unsigned char, unsigned short, regs8)
				OPCODE 7:
					i_w ? DIV_MACRO(short, int, regs16) : DIV_MACRO(signed char, short, regs8);
			}
		OPCODE 7:
			rm_addr = REGS_BASE;
			i_data2 = i_data0;
			i_mod = 3;
			i_reg = extra;
			reg_ip--;
		OPCODE_CHAIN 8:
			op_to_addr = rm_addr;
			regs16[REG_SCRATCH] = (i_d |= !i_w) ? (signed char)i_data2 : i_data2;
			op_from_addr = REGS_BASE + 2 * REG_SCRATCH;
			reg_ip += !i_d + 1;
			set_opcode(0x08 * (extra = i_reg));
		OPCODE_CHAIN 9:
			switch (extra)
			{
				OPCODE_CHAIN 0:
					OP(+=),
					set_CF(op_result < op_dest)
				OPCODE 1:
					OP(|=)
				OPCODE 2:
					ADC_SBB_MACRO(+)
				OPCODE 3:
					ADC_SBB_MACRO(-)
				OPCODE 4:
					OP(&=)
				OPCODE 5:
					OP(-=),
					set_CF(op_result > op_dest)
				OPCODE 6:
					OP(^=)
				OPCODE 7:
					OP(-),
					set_CF(op_result > op_dest)
				OPCODE 8:
					OP(=);
			}
		OPCODE 10:
			if (!i_w)
				i_w = 1,
				i_reg += 8,
				DECODE_RM_REG,
				OP(=);
			else if (!i_d)
				seg_override_en = 1,
				seg_override = REG_ZERO,
				DECODE_RM_REG,
				R_M_OP(mem[op_from_addr], =, rm_addr);
			else
				R_M_POP(mem[rm_addr])
		OPCODE 11:
			i_mod = i_reg = 0;
			i_rm = 6;
			i_data1 = i_data0;
			DECODE_RM_REG;
			MEM_OP(op_from_addr, =, op_to_addr)
		OPCODE 12:
			scratch2_uint = SIGN_OF(mem[rm_addr]),
			scratch_uint = extra ?
				++reg_ip,
				(signed char)i_data1
			:
				i_d
					? 31 & regs8[REG_CL]
			:
				1;
			if (scratch_uint)
			{
				if (i_reg < 4)
					scratch_uint %= i_reg / 2 + TOP_BIT,
					R_M_OP(scratch2_uint, =, mem[rm_addr]);
				if (i_reg & 1)
					R_M_OP(mem[rm_addr], >>=, scratch_uint);
				else
					R_M_OP(mem[rm_addr], <<=, scratch_uint);
				if (i_reg > 3)
					set_opcode(0x10);
				if (i_reg > 4)
					set_CF(op_dest >> (scratch_uint - 1) & 1);
			}
			switch (i_reg)
			{
				OPCODE_CHAIN 0:
					R_M_OP(mem[rm_addr], += , scratch2_uint >> (TOP_BIT - scratch_uint));
					set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1))
				OPCODE 1:
					scratch2_uint &= (1 << scratch_uint) - 1,
					R_M_OP(mem[rm_addr], += , scratch2_uint << (TOP_BIT - scratch_uint));
					set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)))
				OPCODE 2:
					R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (scratch_uint - 1)) + , scratch2_uint >> (1 + TOP_BIT - scratch_uint));
					set_OF(SIGN_OF(op_result) ^ set_CF(scratch2_uint & 1 << (TOP_BIT - scratch_uint)))
				OPCODE 3:
					R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (TOP_BIT - scratch_uint)) + , scratch2_uint << (1 + TOP_BIT - scratch_uint));
					set_CF(scratch2_uint & 1 << (scratch_uint - 1));
					set_OF(SIGN_OF(op_result) ^ SIGN_OF(op_result * 2))
				OPCODE 4:
					set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))))
				OPCODE 5:
					set_OF(SIGN_OF(op_dest))
				OPCODE 7:
					scratch_uint < TOP_BIT || set_CF(scratch2_uint);
					set_OF(0);
					R_M_OP(mem[rm_addr], +=, scratch2_uint *= ~(((1 << TOP_BIT) - 1) >> scratch_uint));
			}
		OPCODE 13:
			scratch_uint = !!--regs16[REG_CX];
			switch(i_reg4bit)
			{
				OPCODE_CHAIN 0:
					scratch_uint &= !regs8[FLAG_ZF]
				OPCODE 1:
					scratch_uint &= regs8[FLAG_ZF]
				OPCODE 3:
					scratch_uint = !++regs16[REG_CX];
			}
			reg_ip += scratch_uint*(signed char)i_data0
		OPCODE 14:
			reg_ip += 3 - i_d;
			if (!i_w)
			{
				if (i_d)
					reg_ip = 0,
					regs16[REG_CS] = i_data2;
				else
					R_M_PUSH(reg_ip);
			}
			reg_ip += i_d && i_w ? (signed char)i_data0 : i_data0
		OPCODE 15:
			MEM_OP(op_from_addr, &, op_to_addr)
		OPCODE 16:
			i_w = 1;
			op_to_addr = REGS_BASE;
			op_from_addr = GET_REG_ADDR(i_reg4bit);
		OPCODE_CHAIN 24:
			if (op_to_addr != op_from_addr)
				OP(^=),
				MEM_OP(op_from_addr, ^=, op_to_addr),
				OP(^=)
		OPCODE 17:
			scratch2_uint = seg_override_en ? seg_override : REG_DS;
			for (scratch_uint = rep_override_en ? regs16[REG_CX] : 1; scratch_uint; scratch_uint--)
			{
				MEM_OP(extra < 2 ? SEGREG(REG_ES, REG_DI,) : REGS_BASE, =, extra & 1 ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,)),
				extra & 1 || INDEX_INC(REG_SI),
				extra & 2 || INDEX_INC(REG_DI);
			}
			if (rep_override_en) regs16[REG_CX] = 0
		OPCODE 18:
			scratch2_uint = seg_override_en ? seg_override : REG_DS;
			if ((scratch_uint = rep_override_en ? regs16[REG_CX] : 1))
			{
				for (; scratch_uint; rep_override_en || scratch_uint--)
				{
					MEM_OP(extra ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,), -, SEGREG(REG_ES, REG_DI,)),
					extra || INDEX_INC(REG_SI),
					INDEX_INC(REG_DI), rep_override_en && !(--regs16[REG_CX] && (!op_result == rep_mode)) && (scratch_uint = 0);
				}
				set_flags_type = FLAGS_UPDATE_SZP | FLAGS_UPDATE_AO_ARITH;
				set_CF(op_result > op_dest);
			}
		OPCODE 19:
			i_d = i_w;
			R_M_POP(reg_ip);
			if (extra) R_M_POP(regs16[REG_CS]);
			if (extra & 2) set_flags(R_M_POP(scratch_uint));
			else if (!i_d) regs16[REG_SP] += i_data0
		OPCODE 20:
			R_M_OP(mem[op_from_addr], =, i_data2)
		OPCODE 21: // IN AL/AX
			io_ports[0x20] = 0;
			io_ports[0x42] = --io_ports[0x40];
			io_ports[0x3DA] ^= 9;
			scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
			scratch_uint == 0x60 && (io_ports[0x64] = 0);
			scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (io_ports[0x3D5] = ((mem[0x49E]*80 + mem[0x49D] + CAST(short)mem[0x4AD]) & (io_ports[0x3D4] & 1 ? 0xFF : 0xFF00)) >> (io_ports[0x3D4] & 1 ? 0 : 8));
			R_M_OP(regs8[REG_AL], =, io_ports[scratch_uint]);
		OPCODE 22: // OUT
			scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
			R_M_OP(io_ports[scratch_uint], =, regs8[REG_AL]);
			scratch_uint == 0x61 && (io_hi_lo = 0, spkr_en |= regs8[REG_AL] & 3);
			(scratch_uint == 0x40 || scratch_uint == 0x42) && (io_ports[0x43] & 6) && (mem[0x469 + scratch_uint - (io_hi_lo ^= 1)] = regs8[REG_AL]);
			scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 6) && (mem[0x4AD + !(io_ports[0x3D4] & 1)] = regs8[REG_AL]);
			scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (scratch2_uint = ((mem[0x49E]*80 + mem[0x49D] + CAST(short)mem[0x4AD]) & (io_ports[0x3D4] & 1 ? 0xFF00 : 0xFF)) + (regs8[REG_AL] << (io_ports[0x3D4] & 1 ? 0 : 8)) - CAST(short)mem[0x4AD], mem[0x49D] = scratch2_uint % 80, mem[0x49E] = scratch2_uint / 80);
			scratch_uint == 0x3B5 && io_ports[0x3B4] == 1 && (GRAPHICS_X = regs8[REG_AL] * 16);
			scratch_uint == 0x3B5 && io_ports[0x3B4] == 6 && (GRAPHICS_Y = regs8[REG_AL] * 4);
		OPCODE 23:
			rep_override_en = 2;
			rep_mode = i_w;
			seg_override_en && seg_override_en++
		OPCODE 25:
			R_M_PUSH(regs16[extra])
		OPCODE 26:
			R_M_POP(regs16[extra])
		OPCODE 27:
			seg_override_en = 2;
			seg_override = extra;
			rep_override_en && rep_override_en++
		OPCODE 28:
			i_w = 0;
			extra ? DAA_DAS(-=, >=, 0xFF, 0x99) : DAA_DAS(+=, <, 0xF0, 0x90)
		OPCODE 29:
			op_result = AAA_AAS(extra - 1)
		OPCODE 30:
			regs8[REG_AH] = -SIGN_OF(regs8[REG_AL])
		OPCODE 31:
			regs16[REG_DX] = -SIGN_OF(regs16[REG_AX])
		OPCODE 32:
			R_M_PUSH(regs16[REG_CS]);
			R_M_PUSH(reg_ip + 5);
			regs16[REG_CS] = i_data2;
			reg_ip = i_data0
		OPCODE 33:
			make_flags();
			R_M_PUSH(scratch_uint)
		OPCODE 34:
			set_flags(R_M_POP(scratch_uint))
		OPCODE 35:
			make_flags();
			set_flags((scratch_uint & 0xFF00) + regs8[REG_AH])
		OPCODE 36:
			make_flags(),
			regs8[REG_AH] = scratch_uint
		OPCODE 37:
			i_w = i_d = 1;
			DECODE_RM_REG;
			OP(=);
			MEM_OP(REGS_BASE + extra, =, rm_addr + 2)
		OPCODE 38:
			++reg_ip;
			pc_interrupt(3)
		OPCODE 39:
			reg_ip += 2;
			if (!v8088_host_intercept((unsigned char)i_data0))
				pc_interrupt(i_data0)
		OPCODE 40:
			++reg_ip;
			regs8[FLAG_OF] && pc_interrupt(4)
		OPCODE 41:
			if (i_data0 &= 0xFF)
				regs8[REG_AH] = regs8[REG_AL] / i_data0,
				op_result = regs8[REG_AL] %= i_data0;
			else
				pc_interrupt(0)
		OPCODE 42:
			i_w = 0;
			regs16[REG_AX] = op_result = 0xFF & regs8[REG_AL] + i_data0 * regs8[REG_AH]
		OPCODE 43:
			regs8[REG_AL] = -regs8[FLAG_CF]
		OPCODE 44:
			regs8[REG_AL] = mem[SEGREG(seg_override_en ? seg_override : REG_DS, REG_BX, regs8[REG_AL] +)]
		OPCODE 45:
			regs8[FLAG_CF] ^= 1
		OPCODE 46:
			regs8[extra / 2] = extra & 1
		OPCODE 47:
			R_M_OP(regs8[REG_AL], &, i_data0)
		OPCODE 48: // Emulator-specific 0F xx
			switch ((signed char)i_data0)
			{
				OPCODE_CHAIN 0: // PUTCHAR_AL  -> host putchar
					v8088_host_putchar(regs8[REG_AL])
				OPCODE 1: // GET_RTC -> host fills 36-byte struct tm + millitm
					v8088_host_get_rtc(mem + SEGREG(REG_ES, REG_BX,))
				OPCODE 2: // DISK_READ
				OPCODE_CHAIN 3: // DISK_WRITE
				{
					int slot = regs8[REG_DL];
					unsigned int sectors = regs16[REG_AX];
					unsigned int lba = *(unsigned int*)&regs16[REG_BP];
					void* host_addr = mem + SEGREG(REG_ES, REG_BX,);
					int rc;
					if ((signed char)i_data0 == 3) rc = v8088_host_disk_write(slot, lba, host_addr, sectors);
					else                    rc = v8088_host_disk_read (slot, lba, host_addr, sectors);
					regs8[REG_AL] = (rc >= 0) ? (unsigned char)rc : 0;
				}
			}
	}

	// Advance IP by computed instruction length.
	reg_ip += (i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6))*i_mod_size + bios_table_lookup[TABLE_BASE_INST_SIZE][raw_opcode_id] + bios_table_lookup[TABLE_I_W_SIZE][raw_opcode_id]*(i_w + 1);

	if (set_flags_type & FLAGS_UPDATE_SZP)
	{
		regs8[FLAG_SF] = SIGN_OF(op_result);
		regs8[FLAG_ZF] = !op_result;
		regs8[FLAG_PF] = bios_table_lookup[TABLE_PARITY_FLAG][(unsigned char)op_result];
		if (set_flags_type & FLAGS_UPDATE_AO_ARITH) set_AF_OF_arith();
		if (set_flags_type & FLAGS_UPDATE_OC_LOGIC) set_CF(0), set_OF(0);
	}

	if (!(++inst_counter % KEYBOARD_TIMER_UPDATE_DELAY))
		int8_asap = 1;

	if (trap_flag) pc_interrupt(1);
	trap_flag = regs8[FLAG_TF];

	if (int8_asap && !seg_override_en && !rep_override_en && regs8[FLAG_IF] && !regs8[FLAG_TF])
		pc_interrupt(0xA), int8_asap = 0, KEYBOARD_DRIVER;

	return 1;
}

// Run up to max_cycles steps. Returns actual count executed.
unsigned int v8088_run(unsigned int max_cycles)
{
	unsigned int n = 0;
	while (n < max_cycles && v8088_run_one()) n++;
	return n;
}

// Tell core that an IRQ should fire on the next instruction boundary
// (used by v8088 PIT/keyboard host code in later milestones).
void v8088_pulse_int8(void) { int8_asap = 1; }
