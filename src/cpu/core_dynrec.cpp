// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#if (C_DYNREC)

#include <cassert>
// simde needs std::isnan
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <type_traits>

#if defined (WIN32)
// clang-format off
// 'windows.h' must be included first, otherwise we'll get compilation errors
#include <windows.h>
#include <winbase.h>
// clang-format on
#endif

#if defined(HAVE_MPROTECT) || defined(HAVE_MMAP)
#include <sys/mman.h>

#include <climits>

#endif // HAVE_MPROTECT

#include "callback.h"
#include "cpu.h"
#include "debug.h"
#include "inout.h"
#include "lazyflags.h"
#include "mem.h"
#include "mmx.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "tracy.h"

#define CACHE_MAXSIZE	(4096*2)
#define CACHE_TOTAL		(1024*1024*8)
#define CACHE_PAGES		(512)
#define CACHE_BLOCKS	(128*1024)
#define CACHE_ALIGN		(16)
#define DYN_HASH_SHIFT	(4)
#define DYN_PAGE_HASH	(4096>>DYN_HASH_SHIFT)
#define DYN_LINKS		(16)


//#define DYN_LOG 1 //Turn Logging on.


#if C_FPU
#define CPU_FPU 1                                               //Enable FPU escape instructions
#endif


// the emulated x86 registers
#define DRC_REG_EAX 0
#define DRC_REG_ECX 1
#define DRC_REG_EDX 2
#define DRC_REG_EBX 3
#define DRC_REG_ESP 4
#define DRC_REG_EBP 5
#define DRC_REG_ESI 6
#define DRC_REG_EDI 7

// the emulated x86 segment registers
#define DRC_SEG_ES 0
#define DRC_SEG_CS 1
#define DRC_SEG_SS 2
#define DRC_SEG_DS 3
#define DRC_SEG_FS 4
#define DRC_SEG_GS 5


// access to a general register
#define DRCD_REG_VAL(reg) (&cpu_regs.regs[reg].dword)
// access to a segment register
#define DRCD_SEG_VAL(seg) (&Segs.val[seg])
// access to the physical value of a segment register/selector
#define DRCD_SEG_PHYS(seg) (&Segs.phys[seg])

// access to an 8bit general register
#define DRCD_REG_BYTE(reg,idx) (&cpu_regs.regs[reg].byte[idx?BH_INDEX:BL_INDEX])
// access to  16/32bit general registers
#define DRCD_REG_WORD(reg,dwrd) ((dwrd)?((void*)(&cpu_regs.regs[reg].dword[DW_INDEX])):((void*)(&cpu_regs.regs[reg].word[W_INDEX])))


enum BlockReturn {
	BR_Normal=0,
	BR_Cycles,
	BR_Link1,BR_Link2,
	BR_Opcode,
#if (C_DEBUG)
	BR_OpcodeFull,
#endif
	BR_Iret,
	BR_Callback,
	BR_SMCBlock
};

// identificator to signal self-modification of the currently executed block
#define SMC_CURRENT_BLOCK	0xffff


static void IllegalOptionDynrec(const char* msg) {
	E_Exit("DynrecCore: illegal option in %s",msg);
}

struct core_dynrec_t {
	BlockReturn (*runcode)(const uint8_t*);		// points to code that can start a block
	Bitu callback;				// the occurred callback
	Bitu readdata;				// spare space used when reading from memory
	uint32_t protected_regs[8];	// space to save/restore register values
};

static core_dynrec_t core_dynrec;

// core_dynrec is often being used this way:
//
//   function_expecting_int16_ptr((uint16_t*)(&core_dynrec.readdata));
//
// These uses are ok and safe as long as core_dynrec.readdata is correctly
// aligned.
//
static_assert(std::is_standard_layout<core_dynrec_t>::value,
              "core_dynrec_t must be a standard layout type, otherwise "
              "offsetof calculation is undefined behaviour.");
static_assert(offsetof(core_dynrec_t, readdata) % sizeof(uint16_t) == 0,
              "core_dynrec.readdata must be word aligned");
static_assert(offsetof(core_dynrec_t, readdata) % sizeof(uint32_t) == 0,
              "core_dynrec.readdata must be double-word aligned");

#include "dyn_cache.h"

#define X86			0x01
#define X86_64		0x02
#define MIPSEL		0x03
#define ARMV4LE		0x04
#define ARMV7LE		0x05
#define POWERPC		0x06
#define ARMV8LE		0x07
#define PPC64LE		0x08

#if C_TARGETCPU == X86_64
#include "core_dynrec/risc_x64.h"
#elif C_TARGETCPU == X86
#include "core_dynrec/risc_x86.h"
#elif C_TARGETCPU == MIPSEL
#include "core_dynrec/risc_mipsel32.h"
#elif (C_TARGETCPU == ARMV4LE) || (C_TARGETCPU == ARMV7LE)
#include "core_dynrec/risc_armv4le.h"
#elif C_TARGETCPU == POWERPC
#include "core_dynrec/risc_ppc.h"
#elif C_TARGETCPU == ARMV8LE
#include "core_dynrec/risc_armv8le.h"
#elif C_TARGETCPU == PPC64LE
#include "core_dynrec/risc_ppc64le.h"
#endif

#include "simde/x86/mmx.h"

#if !defined(WORDS_BIGENDIAN)
#define gen_add_LE gen_add
#define gen_mov_LE_word_to_reg gen_mov_word_to_reg
#endif

#include "core_dynrec/decoder.h"

CacheBlock *LinkBlocks(BlockReturn ret)
{
	// the last instruction was a control flow modifying instruction
	const auto temp_ip = SegPhys(cs) + reg_eip;

	const auto read_handler = get_tlb_readhandler(temp_ip);
	if (!read_handler) {
		return nullptr;
	}

	const auto cp_handler = reinterpret_cast<CodePageHandler*>(read_handler);
	if (!cp_handler) {
		return nullptr;
	}

	const bool cp_has_code = cp_handler->flags &
	                         (cpu.code.big ? PFLAG_HASCODE32 : PFLAG_HASCODE16);
	if (!cp_has_code) {
		return nullptr;
	}

	// see if the target is an already translated block
	const auto cache_block = cp_handler->FindCacheBlock(temp_ip & 4095);
	if (!cache_block) {
		return nullptr;
	}

	// found it, link the current block to
	cache.block.running->LinkTo(ret == BR_Link2, cache_block);
	return cache_block;
}

/*
	The core tries to find the block that should be executed next.
	If such a block is found, it is run, otherwise the instruction
	stream starting at ip_point is translated (see decoder.h) and
	makes up a new code block that will be run.
	When control is returned to CPU_Core_Dynrec_Run (which might
	be right after the block is run, or somewhen long after that
	due to the direct cacheblock linking) the returncode decides
	the next action. This might be continuing the translation and
	execution process, or returning from the core etc.
*/

Bits CPU_Core_Dynrec_Run() noexcept
{
	ZoneScoped;
	for (;;) {
		// Determine the linear address of CS:EIP
		PhysPt ip_point=SegPhys(cs)+reg_eip;
#if C_HEAVY_DEBUG
		if (DEBUG_HeavyIsBreakpoint())
			return debugCallback;
#endif

		CodePageHandler *chandler = nullptr;
		// see if the current page is present and contains code
		if (MakeCodePage(ip_point, chandler)) {
			// page not present, throw the exception
			CPU_Exception(cpu.exception.which,cpu.exception.error);
			continue;
		}

		// page doesn't contain code or is special
		if (!chandler) {
			return CPU_Core_Normal_Run();
		}

		// find correct Dynamic Block to run
		CacheBlock *block = chandler->FindCacheBlock(ip_point & 4095);
		if (!block) {
			// no block found, thus translate the instruction stream
			// unless the instruction is known to be modified
			if (!chandler->invalidation_map || (chandler->invalidation_map[ip_point&4095]<4)) {
				// translate up to 32 instructions
				block=CreateCacheBlock(chandler,ip_point,32);
			} else {
				// let the normal core handle this instruction to avoid zero-sized blocks
				Bitu old_cycles=CPU_Cycles;
				CPU_Cycles=1;
				Bits nc_retcode=CPU_Core_Normal_Run();
				if (!nc_retcode) {
					CPU_Cycles=old_cycles-1;
					continue;
				}
				CPU_CycleLeft+=old_cycles;
				return nc_retcode;
			}
		}

run_block:
		cache.block.running=nullptr;
		// now we're ready to run the dynamic code block
//		BlockReturn ret=((BlockReturn (*)(void))(block->cache.start))();
		BlockReturn ret=core_dynrec.runcode(block->cache.start);

		switch (ret) {
		case BR_Iret:
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			if (!GETFLAG(TF)) {
				if (GETFLAG(IF) && PIC_IRQCheck) return CBRET_NONE;
				break;
			}
			// trapflag is set, switch to the trap-aware decoder
			cpudecoder=CPU_Core_Dynrec_Trap_Run;
			return CBRET_NONE;

		case BR_Normal:
			// the block was exited due to a non-predictable control flow
			// modifying instruction (like ret) or some nontrivial cpu state
			// changing instruction (for example switch to/from pmode),
			// or the maximum number of instructions to translate was reached
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			break;

		case BR_Cycles:
			// cycles went negative, return from the core to handle
			// external events, schedule the pic...
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			return CBRET_NONE;

		case BR_Callback:
			// the callback code is executed in dosbox.conf, return the callback number
			FillFlags();
			return core_dynrec.callback;

		case BR_SMCBlock:
//			LOG_MSG("selfmodification of running block at %x:%x",SegValue(cs),reg_eip);
			cpu.exception.which=0;
			// let the normal core handle the block-modifying
			// instruction
			[[fallthrough]];
		case BR_Opcode:
			// some instruction has been encountered that could not be translated
			// (thus it is not part of the code block), the normal core will
			// handle this instruction
			CPU_CycleLeft+=CPU_Cycles;
			CPU_Cycles=1;
			return CPU_Core_Normal_Run();

#if (C_DEBUG)
		case BR_OpcodeFull:
			CPU_CycleLeft+=CPU_Cycles;
			CPU_Cycles=1;
			return CPU_Core_Full_Run();
#endif

		case BR_Link1:
		case BR_Link2:
			block=LinkBlocks(ret);
			if (block) goto run_block;
			break;

		default:
			E_Exit("Invalid return code %d", ret);
		}
	}
	return CBRET_NONE;
}

Bits CPU_Core_Dynrec_Trap_Run() noexcept
{
	Bits oldCycles = CPU_Cycles;
	CPU_Cycles = 1;
	cpu.trap_skip = false;

	// let the normal core execute the next (only one!) instruction
	Bits ret=CPU_Core_Normal_Run();

	// trap to int1 unless the last instruction deferred this
	// (allows hardware interrupts to be served without interaction)
	if (!cpu.trap_skip) CPU_DebugException(DBINT_STEP,reg_eip);

	CPU_Cycles = oldCycles-1;
	// continue (either the trapflag was clear anyways, or the int1 cleared it)
	cpudecoder = &CPU_Core_Dynrec_Run;

	return ret;
}

void CPU_Core_Dynrec_Init(void) {
}

void CPU_Core_Dynrec_Cache_Init(bool enable_cache) {
	// Initialize code cache and dynamic blocks
	cache_init(enable_cache);
}

void CPU_Core_Dynrec_Cache_Close(void) {
	cache_close();
}

#endif
