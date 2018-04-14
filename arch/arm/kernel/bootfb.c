#include <linux/init.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/font.h>
#include <linux/delay.h>
#include <asm/early_ioremap.h>
#include <asm/bootfb.h>

static const unsigned long base = CONFIG_DEBUG_BOOTFB_PHYS;
void *bootfb_addr = (void *)CONFIG_DEBUG_BOOTFB_VIRT;
int bootfb_skip_for_pageinit;

static __ref void *map_bootfb(unsigned offs, unsigned len)
{
	if (bootfb_skip_for_pageinit)
		return NULL;

	if (bootfb_addr)
		return bootfb_addr + offs;
	else
		return early_ioremap(base + offs, len);
}

static __ref void unmap_bootfb(void *addr, unsigned long len)
{
	if (!bootfb_addr)
		early_iounmap(addr, len);
}

void __ref make_square(unsigned long x, unsigned long y, unsigned long c)
{
	const int row = 1280;
	unsigned offs = y * 16 * row + x * 16;

	for (y = 0; y < 16; ++y) {
		unsigned short *p = map_bootfb(offs*2, 16*2);
		if (p) {
			for (x = 0; x < 16; ++x)
				p[x] = c;
			unmap_bootfb(p, 16*2);
		}
		offs += row;
	}
}

static void bootfb_paint_char(unsigned offs, unsigned char c, unsigned color)
{
	unsigned char *src, cline;
	unsigned char *dst;
	int x, y, z;

	src = (unsigned char *)font_vga_8x8.data + c * 8;

	for (y = 0; y < 8; ++y) {
		cline = src[y];
		dst = map_bootfb(offs, 8 * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE);
		if (dst) {
			for (x = 0; x < 8; ++x) {
				for (z = 0; z < CONFIG_DEBUG_BOOTFB_PIXEL_SIZE; ++z)
					dst[x * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE + z] = (cline & 0x80) ? color >> (8 * z) : 0;
				cline <<= 1;
			}
			unmap_bootfb(dst, 16);
		}
		offs += CONFIG_DEBUG_BOOTFB_STRIDE;
	}
}

static void bootfb_clear_line(unsigned offs, unsigned len, unsigned color)
{
	unsigned char *dst;
	int x, y, z;

	for (y = 0; y < 8; ++y, offs += CONFIG_DEBUG_BOOTFB_STRIDE) {
		dst = map_bootfb(offs, len);
		if (!dst)
			continue;
		for (x = 0; x < len; x += CONFIG_DEBUG_BOOTFB_PIXEL_SIZE) {
			for (z = 0; z < CONFIG_DEBUG_BOOTFB_PIXEL_SIZE; ++z)
				dst[x + z] = color >> (8 * z);
		}
		unmap_bootfb(dst, len);
	}
}

struct bootfd_tmp
{
	unsigned short *ctl;
	unsigned short y, xoff;
	unsigned long cur_offset;
};

static bool bootfb_start(struct bootfd_tmp *info)
{
	unsigned short *p = info->ctl = map_bootfb(BOOTFB_SIZE, 4);
	if (!p)
		return false;

	info->y = p[0];
	info->xoff = p[1];
	if (info->y >= FB_CHARS_HEIGHT || info->xoff >= LINE_END)
		info->y = info->xoff = 0;

	info->cur_offset = info->y * 8 * CONFIG_DEBUG_BOOTFB_STRIDE + info->xoff;

	return true;
}

static void bootfb_stop(struct bootfd_tmp *info)
{
	unsigned short *p = info->ctl;
	unsigned short y = info->y, xoff = info->xoff;

	if (xoff >= LINE_END) {
		if (++y >= FB_CHARS_HEIGHT) {
			y = 0;
			if (CONFIG_DEBUG_BOOTFB_PAGE_DELAY_MS)
				mdelay(CONFIG_DEBUG_BOOTFB_PAGE_DELAY_MS);
		}
		p[0] = y;
		xoff = 0;
	}

	p[1] = xoff;
	unmap_bootfb(p, 4);

	info->cur_offset = y * 8 * CONFIG_DEBUG_BOOTFB_STRIDE + xoff;
	bootfb_paint_char(info->cur_offset, 254, 0xF000F000);
}

static void bootfb_write_char(unsigned char c, unsigned color)
{
	struct bootfd_tmp bootfb;

	if (!bootfb_start(&bootfb))
		return;

	bootfb_paint_char(bootfb.cur_offset, c, color);
	bootfb.xoff += 8 * CONFIG_DEBUG_BOOTFB_PIXEL_SIZE;

	bootfb_stop(&bootfb);
}

static void bootfb_clear_after_eol(unsigned color)
{
	struct bootfd_tmp bootfb;

	if (!bootfb_start(&bootfb))
		return;

	bootfb_clear_line(bootfb.cur_offset, LINE_END - bootfb.xoff, color);
	bootfb.xoff = LINE_END;

	bootfb_stop(&bootfb);
}

void early_bootfb_write_char(char c)
{
	bootfb_write_char(c, ~0);
}

void early_bootfb_eol(void)
{
	bootfb_write_char('#', 0x6A);
	bootfb_clear_after_eol(0);
}
