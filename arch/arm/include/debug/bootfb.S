/* arch/arm/include/debug/bootfb.S
 *
 * Debugging macro include header
 *
 *  Copyright (C) 2017 Michał Mirosław
 *  Moved from linux/arch/arm/kernel/debug.S by Ben Dooks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <asm/bootfb.h>

	.extern font_vga_8x8


	.macro	addruart, rp, rv, tmp
	ldr	\rp, =CONFIG_DEBUG_BOOTFB_PHYS
	ldr	\rv, =CONFIG_DEBUG_BOOTFB_VIRT
	.endm


	.macro	pushbyte,rd,rx,tmpx,tmpy
// load Y and calculate line offset
	ldr	\tmpx, =BOOTFB_SIZE
	ldrh	\tmpy, [\rx, \tmpx]
	ldr	\tmpx, =CONFIG_DEBUG_BOOTFB_STRIDE
	mul	\tmpy, \tmpy, \tmpx
// load and add X offset
	ldr	\tmpx, =BOOTFB_SIZE
	add	\tmpx, \tmpx, #2
	ldrh	\tmpx, [\rx, \tmpx]
	add	\tmpy, \tmpy, \tmpx

	paint	\rx, \tmpy, \rd, \tmpx

// recover X offset (\rd is reused for Y)
	ldr	\tmpx, =BOOTFB_SIZE
	ldrh	\rd, [\rx, \tmpx]
	ldr	\tmpx, =CONFIG_DEBUG_BOOTFB_STRIDE
	mul	\tmpx, \tmpx, \rd
	rsb	\tmpx, \tmpx, \tmpy

	teq	\tmpx, #LINE_END
	bne	1001f
// wrap to next line and save new Y
	add	\rd, #FONT_HEIGHT
	cmp	\rd, #(FB_CHARS_HEIGHT * FONT_HEIGHT)
	movhs	\rd, #0
	ldr	\tmpx, =BOOTFB_SIZE
	strh	\rd, [\rx, \tmpx]
	mov	\tmpx, #0
// save new X offset
1001:
	ldr	\tmpy, =BOOTFB_SIZE
	strh	\tmpx, [\rx, \tmpy]
	.endm


	.macro	paint,rx,roffs,rd,tmp
// print char or clear on LF, ignore CR
	teq	\rd, #10
	beq	1002f
	teq	\rd, #13
	beq	1004f
	wrchar	\rx, \roffs, \rd, \tmp
	b	1003f
1002:	clreol	\rx, \roffs, \rd, \tmp
1003:
// restore \rx
	ldr	\rd, =(CONFIG_DEBUG_BOOTFB_STRIDE * FONT_HEIGHT)
	sub	\rx, \rx, \roffs
	sub	\rx, \rx, \rd
1004:
	.endm


	.macro	wrchar,rx,roffs,rd,tmp
// get address of first line of glyph
	ldr	\tmp, =font_vga_8x8
	ldr	\tmp, [\tmp, #FONT_STRUCT_DATA_OFFSET]
	add	\rd, \tmp, \rd, lsl #FONT_CHAR_SHIFT

	add	\rx, \rx, \roffs
	.rept	FONT_HEIGHT

// paint one row
	ldrb	\tmp, [\rd], #1
	lsl	\tmp, \tmp, #24
	.rept	FONT_WIDTH
	asr	\tmp, \tmp, #24
	ror	\tmp, \tmp, #7
	.if CONFIG_DEBUG_BOOTFB_PIXEL_SIZE % 1
	strb	\tmp, [\rx], #1
	.elseif CONFIG_DEBUG_BOOTFB_PIXEL_SIZE == 2
	strh	\tmp, [\rx], #2
	.elseif CONFIG_DEBUG_BOOTFB_PIXEL_SIZE == 3
	strb	\tmp, [\rx], #1
	strb	\tmp, [\rx], #1
	strb	\tmp, [\rx], #1
	.elseif CONFIG_DEBUG_BOOTFB_PIXEL_SIZE == 4
	str	\tmp, [\rx], #4
	.else
	* CONFIG BUG *
	.endif
	.endr

	ldr	\tmp, =(CONFIG_DEBUG_BOOTFB_STRIDE - FONT_WIDTH * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE)
	add	\rx, \rx, \tmp
	.endr

	add	\roffs, \roffs, #(FONT_WIDTH * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE)
	add	\rx, \rx, #(FONT_WIDTH * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE)
	.endm

	.macro	clreol,rx,roffs,rd,tmp
// calc line end offset (\tmp has current X offset)
	rsb	\rd, \tmp, #LINE_END
// prepare loop
	add	\rx, \rx, \roffs
	add	\roffs, \roffs, \rd
// fill pixels to the end of line (\rd is mostly zero)
	.rept	FONT_HEIGHT
	mov	\tmp, \rd
1010:
	str	\tmp, [\rx], #4
	sub	\tmp, \tmp, #4
	bhi	1010b
	add	\rx, \rx, #CONFIG_DEBUG_BOOTFB_STRIDE
	sub	\rx, \rx, \rd
	.endr
	.endm

	.macro	senduart, rd, rx, tmp1, tmp2
//	pushbyte \rd, \rx, \tmp1, \tmp2
	.endm

	.macro	waituart, rd, rx
	.endm

	.macro	busyuart, rd, rx
	.endm
