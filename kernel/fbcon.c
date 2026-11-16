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

/* The framebuffer is mapped write-combining (VM_PROT_FB): reads are
 * uncached and stores coalesce, so READING it back stays expensive —
 * the original scroll() memmoved the fb onto itself (megabytes of
 * uncached reads per newline) while the display engine scanned the
 * same region, which showed up as tearing: text visibly "cut off"
 * mid-scroll, like a sync problem.
 *
 * This console therefore never reads the framebuffer. A shadow grid of
 * characters lives in cached kernel memory; every mutation goes to the
 * grid first and the framebuffer is only ever written. Scrolling moves
 * the grid and redraws the screen from it (write-only blit). */

struct fbcon_state {
    uint8_t *fb;
    uint8_t *grid;              /* shadow text grid, cols * rows chars */
    uint8_t *onscreen;          /* glyph painted per cell — diff source */
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
    int pending;                /* scroll redraw deferred to fbcon_flush() */
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
 * WC stores on x86_64 are ordered relative to each other by sfence,
 * device memory on aarch64 and riscv64 needs an explicit barrier once
 * per batch. */
static void fb_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile ("dsb st" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile ("fence ow, ow" ::: "memory");
#else
    __asm__ volatile ("sfence" ::: "memory");
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

/* Force-paint a cell ignoring what was on screen: used for the first
 * paint, where the framebuffer starts with unknown content. */
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

/* Paint one cell, writing only pixels whose bit actually differs from
 * what was painted there before. The fb mapping makes every store a
 * separate uncached transaction, so each avoided store is the win:
 * identical glyphs cost nothing, text over blank cells writes only the
 * set bits, and a scrolled redraw touches roughly half the bits of a
 * full repaint. */
static void draw_cell(char c_new, int gx, int gy, char c_old)
{
    const unsigned char *gn = (const unsigned char *)font8x8_basic[(unsigned char)c_new];
    const unsigned char *go = (const unsigned char *)font8x8_basic[(unsigned char)c_old];

    if (c_new == c_old)
        return;

    for (int y = 0; y < GLYPH_H; y++) {
        unsigned char nb = gn[y];
        unsigned char ob = go[y];

        if (nb == ob)
            continue;
        for (int x = 0; x < GLYPH_W; x++) {
            unsigned mask = 1u << x;

            if ((nb & mask) == (ob & mask))
                continue;
            put_pixel((uint64_t)(gx * GLYPH_W + x),
                      (uint64_t)(gy * GLYPH_H + y),
                      (nb & mask) ? st.fg : st.bg);
        }
    }
}

static void paint_cell(int r, int c)
{
    uint64_t cell = (uint64_t)r * st.cols + c;
    char glyph = (char)st.grid[cell];

    draw_cell(glyph, c, r, (char)st.onscreen[cell]);
    st.onscreen[cell] = (uint8_t)glyph;
}

/* Blit the shadow grid (or a part of it) to the framebuffer. Write-only
 * against the fb; all state comes from cached memory, and each cell is
 * diffed against what is already on screen. */
static void redraw_rows(int from_row, int to_row)
{
    for (int r = from_row; r < to_row; r++)
        for (int c = 0; c < st.cols; c++)
            paint_cell(r, c);
    fb_barrier();
}

/* Moving the grid is cheap (cached, a few hundred bytes per line); the
 * full-screen redraw is the expensive part (hundreds of thousands of
 * uncached transactions), so it is deferred to fbcon_flush() — the
 * screen catches up at task-switch cadence instead of once per line. */
static void scroll(void)
{
    memmove(st.grid, st.grid + st.cols, (uint64_t)(st.rows - 1) * st.cols);
    memset(st.grid + (uint64_t)(st.rows - 1) * st.cols, 0, st.cols);
    st.pending = 1;
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
    paint_cell(st.row, st.col);
    if (++st.col >= st.cols)
        newline();
}

void fbcon_write(const char *buf, unsigned long n)
{
    if (!st.enabled)
        return;

    for (unsigned long i = 0; i < n; i++) {
        switch (buf[i]) {
        case '\n':
            newline();
            break;
        case '\r':
            st.col = 0;
            break;
        case '\t':
            st.col = (st.col + 8) & ~7;
            if (st.col >= st.cols)
                newline();
            break;
        default:
            fbcon_putc(buf[i]);
            break;
        }
    }
}

void fbcon_flush(void)
{
    if (!st.enabled || !st.pending)
        return;

    st.pending = 0;
    redraw_rows(0, st.rows);
}

void fbcon_putchar(char c)
{
    fbcon_write(&c, 1);
    fbcon_flush();
}

void fbcon_init(void)
{
    if (!bootinfo.fb_present || bootinfo.fb.width < GLYPH_W ||
        bootinfo.fb.height < GLYPH_H)
        return;

    if (bootinfo.fb.memory_model != LIMINE_FRAMEBUFFER_RGB ||
        bootinfo.fb.bpp % 8 != 0)
        return;

    st.fb = vmm_map_fb(bootinfo.fb.phys_addr,
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
    st.onscreen = kmalloc((uint64_t)st.cols * st.rows);
    if (st.grid == NULL || st.onscreen == NULL) {
        /* No shadow grids: keep serial-only output rather than touch a
         * framebuffer we cannot scroll safely. */
        if (st.onscreen != NULL)
            kfree(st.onscreen);
        if (st.grid != NULL)
            kfree(st.grid);
        st.grid = st.onscreen = NULL;
        return;
    }
    memset(st.grid, 0, (uint64_t)st.cols * st.rows);
    memset(st.onscreen, 0, (uint64_t)st.cols * st.rows);

    st.enabled = 1;

    /* First paint: force every cell — the fb content is unknown. */
    for (int r = 0; r < st.rows; r++)
        for (int c = 0; c < st.cols; c++)
            draw_glyph((char)st.grid[(uint64_t)r * st.cols + c], c, r);
    fb_barrier();
    memcpy(st.onscreen, st.grid, (uint64_t)st.cols * st.rows);
}
