#include <stdint.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "font.h"
#include "string.h"
#include "vmm.h"

#define GLYPH_W 8
#define GLYPH_H 8

struct fbcon_state {
    uint8_t *fb;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint32_t bytes_pp;
    uint8_t red_size, red_shift;
    uint8_t green_size, green_shift;
    uint8_t blue_size, blue_shift;
    uint64_t fg;
    uint64_t bg;
    int cols, rows;
    int col, row;
    int enabled;
};

static struct fbcon_state st;

static uint32_t scale_component(uint8_t v, uint8_t bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 8)
        return v;
    return (uint32_t)v >> (8 - bits);
}

static uint64_t pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint64_t pixel = 0;

    pixel |= (uint64_t)scale_component(r, st.red_size) << st.red_shift;
    pixel |= (uint64_t)scale_component(g, st.green_size) << st.green_shift;
    pixel |= (uint64_t)scale_component(b, st.blue_size) << st.blue_shift;
    return pixel;
}

static void put_pixel(uint64_t x, uint64_t y, uint64_t pixel)
{
    uint8_t *p = st.fb + y * st.pitch + x * st.bytes_pp;

    for (uint32_t i = 0; i < st.bytes_pp; i++)
        p[i] = (uint8_t)(pixel >> (8 * i));
}

static void draw_glyph(char c, int gx, int gy)
{
    const unsigned char *glyph = (const unsigned char *)font8x8_basic[(unsigned char)c];

    for (int y = 0; y < GLYPH_H; y++) {
        unsigned char rowbits = glyph[y];
        for (int x = 0; x < GLYPH_W; x++) {
            uint64_t pixel = (rowbits & (1u << x)) ? st.fg : st.bg;
            put_pixel((uint64_t)(gx * GLYPH_W + x),
                      (uint64_t)(gy * GLYPH_H + y), pixel);
        }
    }
}

static void clear_rows(int from_row, int to_row)
{
    for (int r = from_row; r < to_row; r++)
        for (int y = 0; y < GLYPH_H; y++) {
            uint64_t py = (uint64_t)r * GLYPH_H + (uint64_t)y;
            uint8_t *p = st.fb + py * st.pitch;
            for (uint64_t x = 0; x < st.width * st.bytes_pp; x++)
                p[x] = 0;
        }
}

static void scroll(void)
{

    memmove(st.fb,
            st.fb + (uint64_t)GLYPH_H * st.pitch,
            (st.height - (uint64_t)GLYPH_H) * st.pitch);
    clear_rows(st.rows - 1, st.rows);
}

static void newline(void)
{
    st.col = 0;
    if (++st.row >= st.rows) {
        st.row = st.rows - 1;
        scroll();
    }
}

static void fbcon_putc(char c)
{
    unsigned char u = (unsigned char)c;

    if (u > 127)
        u = '?';
    draw_glyph((char)u, st.col, st.row);
    if (++st.col >= st.cols)
        newline();
}

void fbcon_putchar(char c)
{
    if (!st.enabled)
        return;

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        st.col = 0;
        return;
    case '\t':
        st.col = (st.col + 8) & ~7;
        if (st.col >= st.cols)
            newline();
        return;
    default:
        fbcon_putc(c);
        return;
    }
}

void fbcon_init(void)
{
    if (!bootinfo.fb_present || bootinfo.fb.width < GLYPH_W ||
        bootinfo.fb.height < GLYPH_H)
        return;

    if (bootinfo.fb.memory_model != LIMINE_FRAMEBUFFER_RGB ||
        bootinfo.fb.bpp % 8 != 0)
        return;

    st.fb = vmm_map_device(bootinfo.fb.phys_addr,
                           bootinfo.fb.pitch * bootinfo.fb.height);
    st.width = bootinfo.fb.width;
    st.height = bootinfo.fb.height;
    st.pitch = bootinfo.fb.pitch;
    st.bytes_pp = bootinfo.fb.bpp / 8;
    st.red_size = bootinfo.fb.red_size;
    st.red_shift = bootinfo.fb.red_shift;
    st.green_size = bootinfo.fb.green_size;
    st.green_shift = bootinfo.fb.green_shift;
    st.blue_size = bootinfo.fb.blue_size;
    st.blue_shift = bootinfo.fb.blue_shift;
    st.fg = pack_rgb(0xFF, 0xFF, 0xFF);
    st.bg = pack_rgb(0x00, 0x00, 0x00);
    st.cols = (int)(st.width / GLYPH_W);
    st.rows = (int)(st.height / GLYPH_H);
    st.col = 0;
    st.row = 0;
    st.enabled = 1;

    clear_rows(0, st.rows);
}
