// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#if C_FPU

static void FPU_FLD_16(PhysPt addr) {
	dyn_dh_fpu.temp.m1 = (uint32_t)mem_readw(addr);
}

static void FPU_FST_16(PhysPt addr) {
	mem_writew(addr,(uint16_t)dyn_dh_fpu.temp.m1);
}

static void FPU_FLD_32(PhysPt addr) {
	dyn_dh_fpu.temp.m1 = mem_readd(addr);
}

static void FPU_FST_32(PhysPt addr) {
	mem_writed(addr,dyn_dh_fpu.temp.m1);
}

static void FPU_FLD_64(PhysPt addr) {
	dyn_dh_fpu.temp.m1 = mem_readd(addr);
	dyn_dh_fpu.temp.m2 = mem_readd(addr+4);
}

static void FPU_FST_64(PhysPt addr) {
	mem_writed(addr,dyn_dh_fpu.temp.m1);
	mem_writed(addr+4,dyn_dh_fpu.temp.m2);
}

static void FPU_FLD_80(PhysPt addr) {
	dyn_dh_fpu.temp.m1 = mem_readd(addr);
	dyn_dh_fpu.temp.m2 = mem_readd(addr+4);
	dyn_dh_fpu.temp.m3 = mem_readw(addr+8);
}

static void FPU_FST_80(PhysPt addr) {
	mem_writed(addr,dyn_dh_fpu.temp.m1);
	mem_writed(addr+4,dyn_dh_fpu.temp.m2);
	mem_writew(addr+8,dyn_dh_fpu.temp.m3);
}

static void FPU_FLDCW_DH(PhysPt addr){
	dyn_dh_fpu.cw = mem_readw(addr);
	dyn_dh_fpu.temp.m1 = (uint32_t)(dyn_dh_fpu.cw|0x3f);
}

static void FPU_FNSTCW_DH(PhysPt addr){
	mem_writew(addr,(uint16_t)(dyn_dh_fpu.cw&0xffff));
}

static void FPU_FNINIT_DH(void){
	dyn_dh_fpu.cw = 0x37f;
}

static void FPU_FSTENV_DH(PhysPt addr){
	if(!cpu.code.big) {
		mem_writew(addr+0,dyn_dh_fpu.cw);
		mem_writew(addr+2,(uint16_t)dyn_dh_fpu.temp.m2);
		mem_writew(addr+4,dyn_dh_fpu.temp.m3);
	} else { 
		mem_writed(addr+0,dyn_dh_fpu.temp.m1);
		mem_writew(addr+0,dyn_dh_fpu.cw);
		mem_writed(addr+4,dyn_dh_fpu.temp.m2);
		mem_writed(addr+8,dyn_dh_fpu.temp.m3);
	}
}

static void FPU_FLDENV_DH(PhysPt addr){
	if(!cpu.code.big) {
		dyn_dh_fpu.cw = (uint32_t)mem_readw(addr);
		dyn_dh_fpu.temp.m1 = dyn_dh_fpu.cw|0x3f;
		dyn_dh_fpu.temp.m2 = (uint32_t)mem_readw(addr+2);
		dyn_dh_fpu.temp.m3 = mem_readw(addr+4);
	} else { 
		dyn_dh_fpu.cw = (uint32_t)mem_readw(addr);
		dyn_dh_fpu.temp.m1 = mem_readd(addr)|0x3f;
		dyn_dh_fpu.temp.m2 = mem_readd(addr+4);
		dyn_dh_fpu.temp.m3 = mem_readw(addr+8);
		dyn_dh_fpu.temp.d1 = mem_readw(addr+10);
	}
}

static void FPU_FSAVE_DH(PhysPt addr){
	if (!cpu.code.big) {
		mem_writew(addr,dyn_dh_fpu.cw);
		addr+=2;
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x04]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x05]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x08]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x09]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x0c]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x0d]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x10]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x11]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x14]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x15]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x18]);
		mem_writeb(addr++,dyn_dh_fpu.temp_state[0x19]);
		for(Bitu i=28;i<108;i++) mem_writeb(addr++,dyn_dh_fpu.temp_state[i]);
	} else {
		mem_writew(addr,dyn_dh_fpu.cw);
		addr+=2;
		for(Bitu i=2;i<108;i++) mem_writeb(addr++,dyn_dh_fpu.temp_state[i]);
	}
}

static void FPU_FRSTOR_DH(PhysPt addr){
	if (!cpu.code.big) {
		dyn_dh_fpu.cw = (uint32_t)mem_readw(addr);
		dyn_dh_fpu.temp_state[0x00] = mem_readb(addr++)|0x3f;
		dyn_dh_fpu.temp_state[0x01] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x04] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x05] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x08] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x09] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x0c] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x0d] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x10] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x11] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x14] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x15] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x18] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0x19] = mem_readb(addr++);
		for(Bitu i=28;i<108;i++) dyn_dh_fpu.temp_state[i] = mem_readb(addr++);
	} else {
		dyn_dh_fpu.cw = (uint32_t)mem_readw(addr);
		for(Bitu i=0;i<108;i++) dyn_dh_fpu.temp_state[i] = mem_readb(addr++);
		dyn_dh_fpu.temp_state[0]|=0x3f;
	}
}

static void dh_fpu_mem(uint8_t inst, Bitu reg=decode.modrm.reg, void* mem=&dyn_dh_fpu.temp.m1) {
#if C_TARGETCPU == X86
	cache_addb(inst);
	cache_addb(0x05|(reg<<3));
	cache_addd((uint32_t)(mem));
#else // X86_64
	opcode(reg).setabsaddr(mem).Emit8(inst);
#endif
}

static void dh_fpu_esc0(){
	dyn_get_modrm(); 
	if (decode.modrm.val >= 0xc0) {
		cache_addb(0xd8);
		cache_addb(decode.modrm.val);
	} else { 
		dyn_fill_ea();
		gen_call_function((void*)&FPU_FLD_32,"%Drd",DREG(EA));
		dh_fpu_mem(0xd8);
	}
}

static void dh_fpu_esc1(){
	dyn_get_modrm();  
	if (decode.modrm.val >= 0xc0) {
		cache_addb(0xd9);
		cache_addb(decode.modrm.val);
	} else {
		Bitu group=(decode.modrm.val >> 3) & 7;
		Bitu sub=(decode.modrm.val & 7);
		dyn_fill_ea(); 
		switch(group){
		case 0x00: /* FLD float*/
			gen_call_function((void*)&FPU_FLD_32,"%Drd",DREG(EA));
			dh_fpu_mem(0xd9);
			break;
		case 0x01: /* UNKNOWN */
			FPU_LOG_WARN(1,true,group,sub);
			break;
		case 0x02: /* FST float*/
		case 0x03: /* FSTP float*/
			dh_fpu_mem(0xd9);
			gen_call_function((void*)&FPU_FST_32,"%Drd",DREG(EA));
			break;
		case 0x04: /* FLDENV */
			gen_call_function((void*)&FPU_FLDENV_DH,"%Drd",DREG(EA));
			dh_fpu_mem(0xd9);
			break;
		case 0x05: /* FLDCW */
			gen_call_function((void *)&FPU_FLDCW_DH,"%Drd",DREG(EA));
			dh_fpu_mem(0xd9);
			break;
		case 0x06: /* FSTENV */
			dh_fpu_mem(0xd9);
			gen_call_function((void*)&FPU_FSTENV_DH,"%Drd",DREG(EA));
			break;
		case 0x07:  /* FNSTCW*/
			gen_call_function((void*)&FPU_FNSTCW_DH,"%Drd",DREG(EA));
			break;
		default:
			FPU_LOG_WARN(1,true,group,sub);
			break;
		}
	}
}

static void dh_fpu_esc2(){
	dyn_get_modrm();  
	if (decode.modrm.val >= 0xc0) { 
		cache_addb(0xda);
		cache_addb(decode.modrm.val);
	} else {
		dyn_fill_ea(); 
		gen_call_function((void*)&FPU_FLD_32,"%Drd",DREG(EA)); 
		dh_fpu_mem(0xda);
	}
}

static void dh_fpu_esc3(){
	dyn_get_modrm();
	const unsigned group = (decode.modrm.val >> 3) & 7;
	const unsigned sub = (decode.modrm.val & 7);
	if (decode.modrm.val >= 0xc0) { 
		switch (group) {
		case 0x04:
			switch (sub) {
			case 0x00:				//FNENI
			case 0x01:				//FNDIS
				LOG(LOG_FPU,LOG_ERROR)("8087 only fpu code used esc 3: group 4: subfunction: %u", sub);
				break;
			case 0x02:				//FNCLEX FCLEX
				cache_addb(0xdb);
				cache_addb(decode.modrm.val);
				break;
			case 0x03:				//FNINIT FINIT
				gen_call_function((void*)&FPU_FNINIT_DH,""); 
				cache_addb(0xdb);
				cache_addb(decode.modrm.val);
				break;
			case 0x04:				//FNSETPM
			case 0x05:				//FRSTPM
				// LOG(LOG_FPU,LOG_ERROR)("80267 protected mode (un)set. Nothing done");
				break;
			default:
				E_Exit("ESC 3:ILLEGAL OPCODE group %u subfunction %u", group, sub);
			}
			break;
		default:
			FPU_LOG_WARN(3, false, group, sub);
			break;
		}
	} else {
		dyn_fill_ea(); 
		switch (group) {
		case 0x00:	/* FILD */
			gen_call_function((void*)&FPU_FLD_32,"%Drd",DREG(EA));
			dh_fpu_mem(0xdb);
			break;
		case 0x01:	/* FISTTP */
			FPU_LOG_WARN(3, true, 1, sub);
			break;
		case 0x02:	/* FIST */
		case 0x03:	/* FISTP */
			dh_fpu_mem(0xdb);
			gen_call_function((void*)&FPU_FST_32,"%Drd",DREG(EA));
			break;
		case 0x05:	/* FLD 80 Bits Real */
			gen_call_function((void*)&FPU_FLD_80,"%Drd",DREG(EA));
			dh_fpu_mem(0xdb);
			break;
		case 0x07:	/* FSTP 80 Bits Real */
			dh_fpu_mem(0xdb);
			gen_call_function((void*)&FPU_FST_80,"%Drd",DREG(EA));
			break;
		default:
			FPU_LOG_WARN(3, true, group, sub);
			break;
		}
	}
}

static void dh_fpu_esc4(){
	dyn_get_modrm();  
	if (decode.modrm.val >= 0xc0) { 
		cache_addb(0xdc);
		cache_addb(decode.modrm.val);
	} else { 
		dyn_fill_ea(); 
		gen_call_function((void*)&FPU_FLD_64,"%Drd",DREG(EA)); 
		dh_fpu_mem(0xdc);
	}
}

static void dh_fpu_esc5(){
	dyn_get_modrm();  
	if (decode.modrm.val >= 0xc0) { 
		cache_addb(0xdd);
		cache_addb(decode.modrm.val);
	} else {
		dyn_fill_ea(); 
		Bitu group=(decode.modrm.val >> 3) & 7;
		Bitu sub=(decode.modrm.val & 7);
		switch(group){
		case 0x00:  /* FLD double real*/
			gen_call_function((void*)&FPU_FLD_64,"%Drd",DREG(EA));
			dh_fpu_mem(0xdd);
			break;
		case 0x01:  /* FISTTP longint*/
			FPU_LOG_WARN(5,true,1,sub);
			break;
		case 0x02:   /* FST double real*/
		case 0x03:	/* FSTP double real*/
			dh_fpu_mem(0xdd);
			gen_call_function((void*)&FPU_FST_64,"%Drd",DREG(EA));
			break;
		case 0x04:	/* FRSTOR */
			gen_call_function((void*)&FPU_FRSTOR_DH,"%Drd",DREG(EA));
			dh_fpu_mem(0xdd, decode.modrm.reg, &(dyn_dh_fpu.temp_state[0]));
			break;
		case 0x06:	/* FSAVE */
			dh_fpu_mem(0xdd, decode.modrm.reg, &(dyn_dh_fpu.temp_state[0]));
			gen_call_function((void*)&FPU_FSAVE_DH,"%Drd",DREG(EA));
			cache_addw(0xE3DB);
			break;
		case 0x07:   /* FNSTSW */
			dh_fpu_mem(0xdd);
			gen_call_function((void*)&FPU_FST_16,"%Drd",DREG(EA));
			break;
		default:
			FPU_LOG_WARN(5,true,group,sub);
			break;
		}
	}
}

static void dh_fpu_esc6(){
	dyn_get_modrm();  
	if (decode.modrm.val >= 0xc0) { 
		cache_addb(0xde);
		cache_addb(decode.modrm.val);
	} else {
		dyn_fill_ea(); 
		gen_call_function((void*)&FPU_FLD_16,"%Drd",DREG(EA)); 
		dh_fpu_mem(0xde);
	}
}

static void dh_fpu_esc7(){
	dyn_get_modrm();
	Bitu group=(decode.modrm.val >> 3) & 7;
	Bitu sub=(decode.modrm.val & 7);
	if (decode.modrm.val >= 0xc0) { 
		switch (group){
		case 0x00: /* FFREEP STi*/
		case 0x01: /* FXCH STi*/
		case 0x02:  /* FSTP STi*/
		case 0x03:  /* FSTP STi*/
			cache_addb(0xdf);
			cache_addb(decode.modrm.val);
			break;
		case 0x04:
			switch(sub){
				case 0x00:     /* FNSTSW AX*/
					dh_fpu_mem(0xdd, 7);
					gen_load_host(&(dyn_dh_fpu.temp.m1),DREG(TMPB),4);
					gen_dop_word(DOP_MOV,false,DREG(EAX),DREG(TMPB));
					gen_releasereg(DREG(TMPB));
					break;
				default:
					FPU_LOG_WARN(7,false,4,sub);
					break;
			}
			break;
		default:
			FPU_LOG_WARN(7,false,group,sub);
			break;
		}
	} else {
		dyn_fill_ea(); 
		switch(group){
		case 0x00:  /* FILD int16_t */
			gen_call_function((void*)&FPU_FLD_16,"%Drd",DREG(EA));
			dh_fpu_mem(0xdf);
			break;
		case 0x01:
			FPU_LOG_WARN(7,true,1,sub);
			break;
		case 0x02:   /* FIST int16_t */
		case 0x03:	/* FISTP int16_t */
			dh_fpu_mem(0xdf);
			gen_call_function((void*)&FPU_FST_16,"%Drd",DREG(EA));
			break;
		case 0x04:   /* FBLD packed BCD */
			gen_call_function((void*)&FPU_FLD_80,"%Drd",DREG(EA));
			dh_fpu_mem(0xdf);
			break;
		case 0x05:  /* FILD int64_t */
			gen_call_function((void*)&FPU_FLD_64,"%Drd",DREG(EA));
			dh_fpu_mem(0xdf);
			break;
		case 0x06:	/* FBSTP packed BCD */
			dh_fpu_mem(0xdf);
			gen_call_function((void*)&FPU_FST_80,"%Drd",DREG(EA));
			break;
		case 0x07:  /* FISTP int64_t */
			dh_fpu_mem(0xdf);
			gen_call_function((void*)&FPU_FST_64,"%Drd",DREG(EA));
			break;
		default:
			FPU_LOG_WARN(7,true,group,sub);
			break;
		}
	}
}

#endif // C_FPU
