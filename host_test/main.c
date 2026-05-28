// Host-side headless test harness for the Stella core.
//
// Compiles the same emucore + stella_glue as the Playdate build, but in a
// plain native binary that you can run from the shell. Drives the core for
// a configurable number of frames and dumps two images:
//
//   <prefix>.pgm  -- 8-bit Stella palette-index luminance (160 x DisplayLines)
//   <prefix>.pbm  -- 1-bit dithered render, sized like the Playdate frame
//                    (400 x 240, 50 B per row), so we can preview the same
//                    pipeline main.c uses on-device.
//
// Usage: host_test rom.a26 [frames] [out_prefix]
//   default frames = 300 (5 s @ 60 NES-frame ticks)
//   default prefix = basename(rom) without extension

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Core C API (same as main.c uses).
extern int           stella_init(const uint8_t* image, uint32_t size);
extern void          stella_run_frame(void);
extern const uint8_t* stella_framebuffer(void);
extern uint32_t      stella_fb_width(void);
extern uint32_t      stella_fb_height(void);
extern uint32_t      stella_display_start_scanline(void);
extern uint32_t      stella_display_num_scanlines(void);
extern void          stella_set_input(uint8_t up, uint8_t down, uint8_t left, uint8_t right,
                                      uint8_t fire, uint8_t select_, uint8_t reset_);
extern const char*   stella_cart_name(void);
#ifdef STELLA_DUAL_CPU
extern void          stella_set_cpu_core(int core);
extern void          stella_frame_signature(uint32_t* fb_crc, uint32_t* cpu_crc, uint32_t* cycles);
extern uint32_t      stella_regs(uint8_t out[7]);
// Divergence report populated by the instruction-level comparison driver.
extern int    g_cmp_diverged;
extern uint16_t g_cmp_pc;
extern uint8_t  g_cmp_op;
extern char   g_cmp_field[8];
extern uint32_t g_cmp_stella, g_cmp_smol;
#endif

#define LCD_W 400
#define LCD_H 240
#define LCD_ROW (LCD_W / 8)        // 50 bytes packed MSB

#ifdef STELLA_SCANLINE_DITHER
// In scanline mode the TIA calls this per completed visible line instead of
// filling a full frame buffer. Collect the lines so the capture path can still
// dump a PGM/PBM (holds the most recent frame's visible rows).
static uint8_t g_host_frame[160 * 300];
static int     g_host_rows = 0;
void stella_emit_scanline(const uint8_t* line, int row)
{
    if (row < 0 || row >= 300) return;
    memcpy(&g_host_frame[row * 160], line, 160);
    if (row + 1 > g_host_rows) g_host_rows = row + 1;
}
const uint8_t* host_collected_frame(int* rows) { *rows = g_host_rows; return g_host_frame; }
#endif

static const uint8_t kBayer4[16] = {
     0, 128,  32, 160,
   192,  64, 224,  96,
    48, 176,  16, 144,
   240, 112, 208,  80,
};

static inline uint8_t luma_of(uint8_t pi)
{
    uint8_t l = pi & 0x0F;
    return (uint8_t)(l * 17);
}

static void write_pgm(const char* path, const uint8_t* fb, int w, int h)
{
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    // Convert palette indices to luma.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t l = luma_of(fb[y * w + x]);
            fwrite(&l, 1, 1, f);
        }
    fclose(f);
}

// Same dither + 2x horizontal scale as main.c render_frame, but writes to a
// 50 B/row bitmap rather than the Playdate LCD framebuffer.
static void render_lcd_dither_off(const uint8_t* fb, uint8_t* lcd, uint32_t startLine)
{
    memset(lcd, 0xFF, LCD_ROW * LCD_H);   // white = bit 1
    uint32_t numLines  = stella_display_num_scanlines();
    if (numLines > LCD_H) numLines = LCD_H;
    const int xOff = (LCD_W - 160 * 2) / 2;
    for (uint32_t y = 0; y < numLines; ++y) {
        const uint8_t* row = fb + (startLine + y) * 160;
        const uint8_t* by  = kBayer4 + ((y & 3) << 2);
        for (uint32_t x = 0; x < 160; ++x) {
            uint8_t l = luma_of(row[x]);
            for (int s = 0; s < 2; ++s) {
                int dx = xOff + (int)x * 2 + s;
                uint8_t t = by[(x * 2 + s) & 3];
                int byteIdx = y * LCD_ROW + (dx >> 3);
                uint8_t mask = (uint8_t)(0x80 >> (dx & 7));
                if (l < t) lcd[byteIdx] &= (uint8_t)~mask;
            }
        }
    }
}

static void write_pbm(const char* path, const uint8_t* lcd)
{
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P4\n%d %d\n", LCD_W, LCD_H);
    // PBM bit 1 = black; lcd bit 1 = white. Invert.
    uint8_t inverted[LCD_ROW];
    for (int y = 0; y < LCD_H; ++y) {
        for (int x = 0; x < LCD_ROW; ++x) inverted[x] = (uint8_t)~lcd[y * LCD_ROW + x];
        fwrite(inverted, 1, LCD_ROW, f);
    }
    fclose(f);
}

static int load_file(const char* path, uint8_t* buf, int max)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    int n = (int)fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static void basename_no_ext(const char* path, char* out, int outsz)
{
    const char* slash = strrchr(path, '/');
    const char* base  = slash ? slash + 1 : path;
    const char* dot   = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, base, len);
    out[len] = 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.a26 [frames] [out_prefix]\n", argv[0]);
        return 2;
    }
    const char* rom_path = argv[1];

    // --- compare mode: run the same ROM under both CPU cores and report the
    //     first frame whose state signature diverges. -----------------------
#ifdef STELLA_DUAL_CPU
    int compare = (argc > 2 && strcmp(argv[2], "--compare") == 0);
    if (compare) {
        int n = (argc > 3) ? atoi(argv[3]) : 600;
        static uint8_t rombuf[512 * 1024];
        int sz = load_file(rom_path, rombuf, sizeof(rombuf));
        if (sz <= 0) return 1;
        fprintf(stderr, "instruction-level compare: %s (%d bytes), up to %d frames\n",
                rom_path, sz, n);

        // Instruction-level co-execution: Stella runs canonically while the
        // smol core predicts each instruction; the driver flags the first
        // disagreement (see M6502Low::execute_compare).
        if (!stella_init(rombuf, (uint32_t)sz)) { fprintf(stderr, "init failed\n"); return 1; }
        stella_set_cpu_core(2);          // compare mode
        stella_set_input(0,0,0,0,0,0,0);

        int diverged_frame = -1;
        for (int i = 0; i < n; ++i) {
            stella_run_frame();
            if (g_cmp_diverged) { diverged_frame = i; break; }
        }

        if (!g_cmp_diverged) {
            fprintf(stderr, "RESULT: cores agree on every compared instruction "
                    "over %d frames (device reads skipped).\n", n);
            return 0;
        }
        fprintf(stderr, "RESULT: first divergence at frame %d\n", diverged_frame);
        fprintf(stderr, "  PC=%04x  opcode=%02x  field=%s\n",
                g_cmp_pc, g_cmp_op, g_cmp_field);
        fprintf(stderr, "  stella=%u (0x%x)   smol=%u (0x%x)\n",
                g_cmp_stella, g_cmp_stella, g_cmp_smol, g_cmp_smol);
        return 1;
    }
#endif // STELLA_DUAL_CPU

    int frames = (argc > 2) ? atoi(argv[2]) : 300;
    char prefix_buf[256];
    const char* prefix;
    if (argc > 3) {
        prefix = argv[3];
    } else {
        basename_no_ext(rom_path, prefix_buf, sizeof(prefix_buf));
        prefix = prefix_buf;
    }

    static uint8_t rom[512 * 1024];
    int size = load_file(rom_path, rom, sizeof(rom));
    if (size <= 0) return 1;
    fprintf(stderr, "loaded %d bytes from %s\n", size, rom_path);

    if (!stella_init(rom, (uint32_t)size)) {
        fprintf(stderr, "stella_init failed\n");
        return 1;
    }
    fprintf(stderr, "cart detected: %s\n", stella_cart_name());
    fprintf(stderr, "display window: start=%u, num=%u\n",
            stella_display_start_scanline(), stella_display_num_scanlines());

    // No input pressed for the basic capture.
    stella_set_input(0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < frames; ++i) stella_run_frame();

    const uint8_t* fb;
    uint32_t startLine;
    int w = 160;
    int h;
#ifdef STELLA_SCANLINE_DITHER
    // Scanline mode: the full buffer is never filled; use the per-line frame
    // collected by our stella_emit_scanline (rows are already visible-relative).
    extern const uint8_t* host_collected_frame(int* rows);
    int rows = 0;
    fb = host_collected_frame(&rows);
    startLine = 0;
    h = rows;
#else
    fb = stella_framebuffer();
    startLine = stella_display_start_scanline();
    h = (int)stella_fb_height();
#endif

    char path[512];
    snprintf(path, sizeof(path), "%s.pgm", prefix);
    write_pgm(path, fb + startLine * 160, w, h);
    fprintf(stderr, "wrote %s (%dx%d, 8-bit)\n", path, w, h);

    static uint8_t lcd[LCD_ROW * LCD_H];
    render_lcd_dither_off(fb, lcd, startLine);
    snprintf(path, sizeof(path), "%s.pbm", prefix);
    write_pbm(path, lcd);
    fprintf(stderr, "wrote %s (%dx%d, 1-bit dithered)\n", path, LCD_W, LCD_H);

    return 0;
}
