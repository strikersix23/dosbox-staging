// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "int10.h"

#include "mem.h"
#include "inout.h"

#define ACTL_MAX_REG   0x14

static inline void ResetACTL(void) {
	IO_Read(real_readw(BIOSMEM_SEG,BIOSMEM_CRTC_ADDRESS) + 6);
}

static inline void WriteTandyACTL(uint8_t creg,uint8_t val) {
	IO_Write(VGAREG_TDY_ADDRESS,creg);
	if (is_machine_tandy()) {
		IO_Write(VGAREG_TDY_DATA,val);
	} else {
		IO_Write(VGAREG_PCJR_DATA,val);
	}
}

void INT10_SetSinglePaletteRegister(uint8_t reg, uint8_t val)
{
	switch (machine) {
	case MachineType::Pcjr:
		reg&=0xf;
		IO_Read(VGAREG_TDY_RESET);
		WriteTandyACTL(reg+0x10,val);
		IO_Write(0x3da,0x0); // palette back on
		break;

	case MachineType::Tandy:
		// TODO waits for vertical retrace
		switch(vga.mode) {
		case M_TANDY2:
			if (reg >= 0x10) break;
			else if (reg==1) reg = 0x1f;
			else reg |= 0x10;
			WriteTandyACTL(reg+0x10,val);
			break;
		case M_TANDY4: {
			if (CurMode->mode!=0x0a) {
				// Palette values are kept constand by the BIOS.
				// The four colors are mapped to special palette values by hardware.
				// 3D8/3D9 registers influence this mapping. We need to figure out
				// which entry is used for the requested color.
				if (reg > 3) break;
				if (reg != 0) { // 0 is assumed to be at 0
					uint8_t color_select=real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAL);
					reg = reg*2+8; // Green Red Brown
					if (color_select& 0x20) reg++; // Cyan Magenta White
				}
				WriteTandyACTL(reg+0x10,val);
			}
			// 4-color high resolution mode 0x0a isn't handled specially
			else WriteTandyACTL(reg+0x10,val);
			break;
		}
		default:
			WriteTandyACTL(reg+0x10,val);
			break;
		}
		IO_Write(0x3da,0x0); // palette back on
		break;

	case MachineType::Ega:
		reg &= 0x1f;
		[[fallthrough]];
	case MachineType::Vga:
		if(reg <= ACTL_MAX_REG) {
			ResetACTL();
			IO_Write(VGAREG_ACTL_ADDRESS,reg);
			IO_Write(VGAREG_ACTL_WRITE_DATA,val);
		}
		IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette
		break;

	case MachineType::Hercules:
	case MachineType::CgaMono:
	case MachineType::CgaColor:
		break;

	default: assertm(false, "Invalid MachineType value");
	}
}

void INT10_SetOverscanBorderColor(uint8_t val)
{
	switch (machine) {
	case MachineType::Pcjr:
	case MachineType::Tandy:
		IO_Read(VGAREG_TDY_RESET);
		WriteTandyACTL(0x02,val);
		IO_Write(VGAREG_TDY_ADDRESS, 0); // enable the screen
		break;

	case MachineType::Ega:
	case MachineType::Vga:
		ResetACTL();
		IO_Write(VGAREG_ACTL_ADDRESS,0x11);
		IO_Write(VGAREG_ACTL_WRITE_DATA,val);
		IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette
		break;

	case MachineType::Hercules:
	case MachineType::CgaMono:
	case MachineType::CgaColor:
		break;

	default: assertm(false, "Invalid MachineType value");
	}
}

void INT10_SetAllPaletteRegisters(PhysPt data)
{
	switch (machine) {
	case MachineType::Pcjr:
	case MachineType::Tandy:
		IO_Read(VGAREG_TDY_RESET);
		// First the colors
		for(uint8_t i=0;i<0x10;i++) {
			WriteTandyACTL(i+0x10,mem_readb(data));
			data++;
		}
		// Then the border
		WriteTandyACTL(0x02,mem_readb(data));
		break;

	case MachineType::Ega:
	case MachineType::Vga:
		ResetACTL();
		// First the colors
		for(uint8_t i=0;i<0x10;i++) {
			IO_Write(VGAREG_ACTL_ADDRESS,i);
			IO_Write(VGAREG_ACTL_WRITE_DATA,mem_readb(data));
			data++;
		}
		// Then the border
		IO_Write(VGAREG_ACTL_ADDRESS,0x11);
		IO_Write(VGAREG_ACTL_WRITE_DATA,mem_readb(data));
		IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette
		break;

	case MachineType::Hercules:
	case MachineType::CgaMono:
	case MachineType::CgaColor:
		break;

	default: assertm(false, "Invalid MachineType value");
	}
}

void INT10_ToggleBlinkingBit(uint8_t state)
{
	if (is_machine_vga_or_better()) {
		uint8_t value;
	//	state&=0x01;
		if ((state > 1) && (svga_type == SvgaType::S3)) {
			return;
		}
		ResetACTL();
		
		IO_Write(VGAREG_ACTL_ADDRESS,0x10);
		value=IO_Read(VGAREG_ACTL_READ_DATA);
		if (state<=1) {
			value&=0xf7;
			value|=state<<3;
		}

		ResetACTL();
		IO_Write(VGAREG_ACTL_ADDRESS,0x10);
		IO_Write(VGAREG_ACTL_WRITE_DATA,value);
		IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette

		if (state<=1) {
			uint8_t msrval=real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_MSR)&0xdf;
			if (state) msrval|=0x20;
			real_writeb(BIOSMEM_SEG,BIOSMEM_CURRENT_MSR,msrval);
		}
	} else { // EGA
		// Usually it reads this from the mode list in ROM
		if (CurMode->type!=M_TEXT) return;

		uint8_t value = (CurMode->cwidth==9)? 0x4:0x0;
		if (state) value |= 0x8;
		
		ResetACTL();
		IO_Write(VGAREG_ACTL_ADDRESS,0x10);
		IO_Write(VGAREG_ACTL_WRITE_DATA,value);
		IO_Write(VGAREG_ACTL_ADDRESS,0x20);

		uint8_t msrval=real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_MSR)& ~0x20;
		if (state) msrval|=0x20;
		real_writeb(BIOSMEM_SEG,BIOSMEM_CURRENT_MSR,msrval);
	}
}

void INT10_GetSinglePaletteRegister(uint8_t reg,uint8_t * val) {
	if(reg<=ACTL_MAX_REG) {
		ResetACTL();
		IO_Write(VGAREG_ACTL_ADDRESS,reg+32);
		*val=IO_Read(VGAREG_ACTL_READ_DATA);
		IO_Write(VGAREG_ACTL_WRITE_DATA,*val);
	}
}

void INT10_GetOverscanBorderColor(uint8_t * val) {
	ResetACTL();
	IO_Write(VGAREG_ACTL_ADDRESS,0x11+32);
	*val=IO_Read(VGAREG_ACTL_READ_DATA);
	IO_Write(VGAREG_ACTL_WRITE_DATA,*val);
}

void INT10_GetAllPaletteRegisters(PhysPt data) {
	ResetACTL();
	// First the colors
	for(uint8_t i=0;i<0x10;i++) {
		IO_Write(VGAREG_ACTL_ADDRESS,i);
		mem_writeb(data,IO_Read(VGAREG_ACTL_READ_DATA));
		ResetACTL();
		data++;
	}
	// Then the border
	IO_Write(VGAREG_ACTL_ADDRESS,0x11+32);
	mem_writeb(data,IO_Read(VGAREG_ACTL_READ_DATA));
	ResetACTL();
}

void INT10_SetSingleDACRegister(uint8_t index,uint8_t red,uint8_t green,uint8_t blue) {
	IO_Write(VGAREG_DAC_WRITE_ADDRESS,(uint8_t)index);
	if ((real_readb(BIOSMEM_SEG, BiosDataArea::VgaFlagsRecOffset) & 0x06) == 0) {
		IO_Write(VGAREG_DAC_DATA,red);
		IO_Write(VGAREG_DAC_DATA,green);
		IO_Write(VGAREG_DAC_DATA,blue);
	} else {
		/* calculate clamped intensity, taken from VGABIOS */
		uint32_t i=(( 77*red + 151*green + 28*blue ) + 0x80) >> 8;
		uint8_t ic=(i>0x3f) ? 0x3f : ((uint8_t)(i & 0xff));
		IO_Write(VGAREG_DAC_DATA,ic);
		IO_Write(VGAREG_DAC_DATA,ic);
		IO_Write(VGAREG_DAC_DATA,ic);
	}
}

void INT10_GetSingleDACRegister(uint8_t index,uint8_t * red,uint8_t * green,uint8_t * blue) {
	IO_Write(VGAREG_DAC_READ_ADDRESS,index);
	*red=IO_Read(VGAREG_DAC_DATA);
	*green=IO_Read(VGAREG_DAC_DATA);
	*blue=IO_Read(VGAREG_DAC_DATA);
}

void INT10_SetDACBlock(uint16_t index,uint16_t count,PhysPt data) {
 	IO_Write(VGAREG_DAC_WRITE_ADDRESS,(uint8_t)index);
	if ((real_readb(BIOSMEM_SEG, BiosDataArea::VgaFlagsRecOffset) & 0x06) == 0) {
		for (;count>0;count--) {
			IO_Write(VGAREG_DAC_DATA,mem_readb(data++));
			IO_Write(VGAREG_DAC_DATA,mem_readb(data++));
			IO_Write(VGAREG_DAC_DATA,mem_readb(data++));
		}
	} else {
		for (;count>0;count--) {
			uint8_t red=mem_readb(data++);
			uint8_t green=mem_readb(data++);
			uint8_t blue=mem_readb(data++);

			/* calculate clamped intensity, taken from VGABIOS */
			uint32_t i=(( 77*red + 151*green + 28*blue ) + 0x80) >> 8;
			uint8_t ic=(i>0x3f) ? 0x3f : ((uint8_t)(i & 0xff));
			IO_Write(VGAREG_DAC_DATA,ic);
			IO_Write(VGAREG_DAC_DATA,ic);
			IO_Write(VGAREG_DAC_DATA,ic);
		}
	}
}

void INT10_GetDACBlock(uint16_t index,uint16_t count,PhysPt data) {
 	IO_Write(VGAREG_DAC_READ_ADDRESS,(uint8_t)index);
	for (;count>0;count--) {
		mem_writeb(data++,IO_Read(VGAREG_DAC_DATA));
		mem_writeb(data++,IO_Read(VGAREG_DAC_DATA));
		mem_writeb(data++,IO_Read(VGAREG_DAC_DATA));
	}
}

void INT10_SelectDACPage(uint8_t function,uint8_t mode) {
	ResetACTL();
	IO_Write(VGAREG_ACTL_ADDRESS,0x10);
	uint8_t old10=IO_Read(VGAREG_ACTL_READ_DATA);
	if (!function) {		//Select paging mode
		if (mode) old10|=0x80;
		else old10&=0x7f;
		//IO_Write(VGAREG_ACTL_ADDRESS,0x10);
		IO_Write(VGAREG_ACTL_WRITE_DATA,old10);
	} else {				//Select page
		IO_Write(VGAREG_ACTL_WRITE_DATA,old10);
		if (!(old10 & 0x80)) mode<<=2;
		mode&=0xf;
		IO_Write(VGAREG_ACTL_ADDRESS,0x14);
		IO_Write(VGAREG_ACTL_WRITE_DATA,mode);
	}
	IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette
}

void INT10_GetDACPage(uint8_t* mode,uint8_t* page) {
	ResetACTL();
	IO_Write(VGAREG_ACTL_ADDRESS,0x10);
	uint8_t reg10=IO_Read(VGAREG_ACTL_READ_DATA);
	IO_Write(VGAREG_ACTL_WRITE_DATA,reg10);
	*mode=(reg10&0x80)?0x01:0x00;
	IO_Write(VGAREG_ACTL_ADDRESS,0x14);
	*page=IO_Read(VGAREG_ACTL_READ_DATA);
	IO_Write(VGAREG_ACTL_WRITE_DATA,*page);
	if(*mode) {
		*page&=0xf;
	} else {
		*page&=0xc;
		*page>>=2;
	}
	IO_Write(VGAREG_ACTL_ADDRESS,32);		//Enable output and protect palette
}

void INT10_SetPelMask(uint8_t mask) {
	IO_Write(VGAREG_PEL_MASK,mask);
}	

void INT10_GetPelMask(uint8_t & mask) {
	mask=IO_Read(VGAREG_PEL_MASK);
}

void INT10_SetBackgroundBorder(uint8_t val)
{
	uint8_t color_select=real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAL);
	color_select=(color_select & 0xe0) | (val & 0x1f);
	real_writeb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAL,color_select);

	switch (machine) {
	case MachineType::CgaMono:
	case MachineType::CgaColor:
		// only write the color select register
		IO_Write(0x3d9, color_select);
		break;

	case MachineType::Tandy:
		// TODO handle val == 0x1x, wait for retrace
		switch(CurMode->mode) {
		default: // modes 0-5: write to color select and border
			INT10_SetOverscanBorderColor(val);
			IO_Write(0x3d9, color_select);
			break;
		case 0x06: // 2-color: only write the color select register
			IO_Write(0x3d9, color_select);
			break;
		case 0x07: // Tandy monochrome not implemented
			break;
		case 0x08:
		case 0x09: // 16-color: write to color select, border and pal. index 0
			INT10_SetOverscanBorderColor(val);
			INT10_SetSinglePaletteRegister(0, val);
			IO_Write(0x3d9, color_select);
			break;
		case 0x0a: // 4-color highres:
			// write zero to color select, write palette to indexes 1-3
			// TODO palette
			IO_Write(0x3d9, 0);
			break;
		}
		break;

	case MachineType::Pcjr:
		IO_Read(VGAREG_TDY_RESET); // reset the flipflop
		if (vga.mode!=M_TANDY_TEXT) {
			IO_Write(VGAREG_TDY_ADDRESS, 0x10);
			IO_Write(VGAREG_PCJR_DATA, color_select&0xf);
		}
		IO_Write(VGAREG_TDY_ADDRESS, 0x2); // border color
		IO_Write(VGAREG_PCJR_DATA, color_select&0xf);
		break;

	case MachineType::Ega:
	case MachineType::Vga:
		val = ((val << 1) & 0x10) | (val & 0x7);
		/* Always set the overscan color */
		INT10_SetSinglePaletteRegister( 0x11, val );
		/* Don't set any extra colors when in text mode */
		if (CurMode->mode <= 3)
			return;
		INT10_SetSinglePaletteRegister( 0, val );
		val = (color_select & 0x10) | 2 | ((color_select & 0x20) >> 5);
		INT10_SetSinglePaletteRegister( 1, val );
		val+=2;
		INT10_SetSinglePaletteRegister( 2, val );
		val+=2;
		INT10_SetSinglePaletteRegister( 3, val );
		break;

	case MachineType::Hercules:
		break;

	default: assertm(false, "Invalid MachineType value");
	}
}

void INT10_SetColorSelect(uint8_t val) {
	uint8_t temp=real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAL);
	temp=(temp & 0xdf) | ((val & 1) ? 0x20 : 0x0);
	real_writeb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAL,temp);
	if (is_machine_cga() || is_machine_tandy())
		IO_Write(0x3d9,temp);
	else if (is_machine_pcjr()) {
		IO_Read(VGAREG_TDY_RESET); // reset the flipflop
		switch (CurMode->mode) {
		case 4: // CGA 4-color mode
		case 5: // Also CGA 4-color mode
			for (uint8_t i = 0x11; i < 0x14; i++) {
				const uint8_t t4_table[] = {0,2,4,6, 0,3,5,0xf};
				IO_Write(VGAREG_TDY_ADDRESS, i);
				IO_Write(VGAREG_PCJR_DATA, t4_table[(i-0x10)+(val&1? 4:0)]);
			}
			break;
		case 6: // CGA 2-color mode
			IO_Write(VGAREG_TDY_ADDRESS, 0x11);
			IO_Write(VGAREG_PCJR_DATA, val & 1 ? 0xf : 0);
			break;
		default:
			// 16-color modes: always write the same palette
			for(uint8_t i = 0x11; i < 0x20; i++) {
				IO_Write(VGAREG_TDY_ADDRESS, i);
				IO_Write(VGAREG_PCJR_DATA, i-0x10);
			}
			break;
		}
		IO_Write(VGAREG_TDY_ADDRESS, 0); // enable palette
	}
	else if (is_machine_ega_or_better()) {
		if (CurMode->mode <= 3) //Maybe even skip the total function!
			return;
		val = (temp & 0x10) | 2 | val;
		INT10_SetSinglePaletteRegister( 1, val );
		val+=2;
		INT10_SetSinglePaletteRegister( 2, val );
		val+=2;
		INT10_SetSinglePaletteRegister( 3, val );
	}
}

void INT10_PerformGrayScaleSumming(uint16_t start_reg,uint16_t count) {
	if (count>0x100) count=0x100;
	for (Bitu ct=0; ct<count; ct++) {
		IO_Write(VGAREG_DAC_READ_ADDRESS,start_reg+ct);
		uint8_t red=IO_Read(VGAREG_DAC_DATA);
		uint8_t green=IO_Read(VGAREG_DAC_DATA);
		uint8_t blue=IO_Read(VGAREG_DAC_DATA);

		/* calculate clamped intensity, taken from VGABIOS */
		uint32_t i=(( 77*red + 151*green + 28*blue ) + 0x80) >> 8;
		uint8_t ic=(i>0x3f) ? 0x3f : ((uint8_t)(i & 0xff));
		INT10_SetSingleDACRegister(start_reg+ct,ic,ic,ic);
	}
}
