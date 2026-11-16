#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "font.h"
#include "kheap.h"
#include "string.h"
#include "vmm.h"

#define GLYPH_W 8
#define GLYPH_H 8

/* The framebuffer is mapped uncached/device on every architecture, so
 * READING it back is extremely slow — the original scroll() memmoved
 * the fb onto itself (megabytes of uncached reads per newline) while
 * the display engine scanned the same region, which showed up as
 * tearing: text visibly "cut off" mid-scroll.
 *
 * This console therefore never reads the framebuffer. A shadow grid of
 * characters lives in cached kernel memory; every mutation goes to the
 * grid first and the framebuffer is only ever written. Scrolling moves
 * the grid and redraws the screen from it (write-only blit). */

struct fbcon_state {
    uint8_t *fb;
    uint8_t *grid;              /* shadow text grid, cols * rows chars */
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

/* Memory barriers so the display engine observes our writes in order:
 * x86_64 UC stores are already strongly ordered; device memory on
 * aarch64 and riscv64 needs an explicit barrier once per batch. */
static void fb_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile ("dsb st" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile ("fence ow, ow" ::: "memory");
#else
    __asm__ volatile ("" : : : "memory");
#endif
}

static inline void fb_write(uint8_t *p, uint64_t pixel)
{
    switch (st.bytes_pp) {
    case 4:
        *(volatile uint32_t *)p = (uint32_t)pixel;
        break;
    case 2:
        *(volatile uint16_t *)p = (uint16_t)pixel;
        break;
    case 1:
        *(volatile uint8_t *)p = (uint8_t)pixel;
        break;
    default:                    /* 3 bytes-per-pixel and exotic depths */
        for (uint32_t i = 0; i < st.bytes_pp; i++)
            p[i] = (uint8_t)(pixel >> (8 * i));
        break;
    }
}

static void put_pixel(uint64_t x, uint64_t y, uint64_t pixel)
{
    fb_write(st.fb + y * st.pitch + x * st.bytes_pp, pixel);
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

/* Blit the shadow grid (or a part of it) to the framebuffer. Write-only
 * against the fb; all state comes from cached memory. */
static void redraw_rows(int from_row, int to_row)
{
    for (int r = from_row; r < to_row; r++) {
        uint8_t *gline = st.grid + (uint64_t)r * st.cols;
        for (int c = 0; c < st.cols; c++)
            draw_glyph((char)gline[c], c, r);
    }
    fb_barrier();
}

static void scroll(void)
{
    memmove(st.grid, st.grid + st.cols, (uint64_t)(st.rows - 1) * st.cols);
    memset(st.grid + (uint64_t)(st.rows - 1) * st.cols, 0, st.cols);
    redraw_rows(0, st.rows);
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
    st.grid[(uint64_t)st.row * st.cols + st.col] = u;
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

    st.grid = kmalloc((uint64_t)st.cols * st.rows);
    if (st.grid == NULL) {
        /* No shadow grid: keep serial-only output rather than touch a
         * framebuffer we cannot scroll safely. */
        return;
    }
    memset(st.grid, 0, (uint64_t)st.cols * st.rows);

    st.enabled = 1;
    redraw_rows(0, st.rows);
}
