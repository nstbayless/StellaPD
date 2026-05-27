// StellaPD main.c — Playdate entry point. Drives the C++ Stella core via the
// thin C interface exposed in stella_glue.cpp.
//
// Display: TIA frame is 160 wide × ~210 visible scanlines, with each pixel
// encoded as a Stella palette index (0..255). We dither the index's luma to
// the Playdate's 1-bit 400×240 LCD. 160×2 = 320 wide leaves a small left
// margin; verticaly we crop/letterbox to fit 240.
//
// Input: D-pad → joystick, A → fire, B → fire (alt), Menu = Select/Reset.

#include "pd_api.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "dtcm.h"

// --- C interface to the C++ core ------------------------------------------
extern int           stella_init(const uint8_t* image, uint32_t size);
extern void          stella_run_frame(void);
extern const uint8_t* stella_framebuffer(void);
extern uint32_t      stella_fb_width(void);
extern uint32_t      stella_fb_height(void);
extern uint32_t      stella_display_start_scanline(void);
extern uint32_t      stella_display_num_scanlines(void);
extern void          stella_set_input(uint8_t up, uint8_t down, uint8_t left, uint8_t right,
                                      uint8_t fire, uint8_t select_, uint8_t reset_);
extern void          stella_set_paddle(int pos1023, uint8_t fire);
extern void          stella_set_paddle_mode(int on);
extern int           stella_get_paddle_mode(void);
extern void          stella_set_dtcm_alloc(void* (*fn)(size_t));
extern void          stella_alloc_buffers(void);
extern const char*   stella_cart_name(void);

static PlaydateAPI* pd_;
static uint8_t      rom_buf[512 * 1024];
static uint32_t     rom_size = 0;
static float        paddle_pos = 512.0f;   // 0..1023, persists between ticks

// --- 4x4 Bayer dither table ----------------------------------------------
static const uint8_t kBayer4[16] = {
     0, 128,  32, 160,
   192,  64, 224,  96,
    48, 176,  16, 144,
   240, 112, 208,  80,
};

// Palette-index -> luma (low nibble * 17). Filled once at init.
static uint8_t lumaLUT[256];
static void init_luma_lut(void)
{
    for (int i = 0; i < 256; ++i) lumaLUT[i] = (uint8_t)((i & 0x0F) * 17);
}

// --- Render the Stella 160×N framebuffer onto the Playdate 400×240 LCD.
// Horizontal scale is exactly 2× and the 40-px left margin is byte-aligned
// (40/8 = 5), so 4 source pixels map onto exactly one output byte. We build
// whole bytes (no per-bit read-modify-write) directly into the framebuffer.
//
// For an output byte covering source pixels p0..p3, the 8 dest bits use Bayer
// thresholds [t0,t1, t2,t3, t0,t1, t2,t3] (derived from (x*2+s)&3). A bit is
// white(1) when luma >= threshold, else black(0).
#define DEST_BYTE_X  5      // (LCD_COLUMNS - 320)/2 / 8 = 40/8
static void render_frame(void)
{
    const uint8_t* fb = stella_framebuffer();
    uint32_t startLine = stella_display_start_scanline();
    uint32_t numLines  = stella_display_num_scanlines();
    if (numLines > (uint32_t)LCD_ROWS) numLines = LCD_ROWS;

    uint8_t* dst = pd_->graphics->getFrame();
    memset(dst, 0xFF, LCD_ROWSIZE * LCD_ROWS);   // white background

    // Center the rendered scanlines vertically (e.g. 210 lines -> 15 px top
    // and bottom margins) instead of pinning to the top edge.
    int yOff = ((int)LCD_ROWS - (int)numLines) / 2;
    if (yOff < 0) yOff = 0;

    for (uint32_t y = 0; y < numLines; ++y)
    {
        const uint8_t* row = fb + (startLine + y) * 160;
        const uint8_t* by  = kBayer4 + ((y & 3) << 2);
        const uint8_t t0 = by[0], t1 = by[1], t2 = by[2], t3 = by[3];
        uint8_t* o = dst + (yOff + (int)y) * LCD_ROWSIZE + DEST_BYTE_X;

        for (int b = 0; b < 40; ++b)
        {
            const uint8_t* p = row + (b << 2);
            uint8_t l0 = lumaLUT[p[0]], l1 = lumaLUT[p[1]];
            uint8_t l2 = lumaLUT[p[2]], l3 = lumaLUT[p[3]];
            uint8_t v = 0;
            if (l0 >= t0) v |= 0x80; if (l0 >= t1) v |= 0x40;
            if (l1 >= t2) v |= 0x20; if (l1 >= t3) v |= 0x10;
            if (l2 >= t0) v |= 0x08; if (l2 >= t1) v |= 0x04;
            if (l3 >= t2) v |= 0x02; if (l3 >= t3) v |= 0x01;
            o[b] = v;
        }
    }

    pd_->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}

// --- Screenshot ----------------------------------------------------------
// After kScreenshotFrame ticks, write the LCD framebuffer to "frame.pbm" in
// the Playdate's writable data dir (P4 packed binary, 400x240, 50 B/row).
// On Linux that lands in ~/.Playdate/Data/<bundleID>/.
#define kScreenshotFrame 150

static void save_screenshot(void)
{
    uint8_t* fb = pd_->graphics->getFrame();
    SDFile* f = pd_->file->open("frame.pbm", kFileWrite);
    if (!f) { pd_->system->logToConsole("screenshot: open failed: %s", pd_->file->geterr()); return; }
    const char* hdr = "P4\n400 240\n";
    pd_->file->write(f, (void*)hdr, (uint32_t)strlen(hdr));
    // PBM bit = 1 means BLACK; Playdate framebuffer bit = 0 means black.
    // Invert each row's 50 valid bytes (skip the 2 padding bytes per row).
    for (int y = 0; y < LCD_ROWS; ++y) {
        uint8_t inverted[50];
        const uint8_t* src = fb + y * LCD_ROWSIZE;
        for (int x = 0; x < 50; ++x) inverted[x] = (uint8_t)(~src[x]);
        pd_->file->write(f, inverted, 50);
    }
    pd_->file->close(f);
    pd_->system->logToConsole("screenshot written to frame.pbm");
}

// --- Input ---------------------------------------------------------------
static int sSelectPress = 0;
static int sResetPress  = 0;

static void poll_input(void)
{
    PDButtons cur;
    pd_->system->getButtonState(&cur, NULL, NULL);

    uint8_t fire  = (cur & (kButtonA | kButtonB)) ? 1 : 0;

    if (stella_get_paddle_mode()) {
        // Crank drives the paddle. If docked, fall back to Left/Right d-pad
        // nudging so the game is still playable without cranking.
        if (pd_->system->isCrankDocked()) {
            if (cur & kButtonLeft)  paddle_pos -= 24.0f;
            if (cur & kButtonRight) paddle_pos += 24.0f;
        } else {
            // 0..360 deg -> 0..1023. One full turn sweeps the whole range.
            paddle_pos = pd_->system->getCrankAngle() * (1023.0f / 360.0f);
        }
        if (paddle_pos < 0.0f)    paddle_pos = 0.0f;
        if (paddle_pos > 1023.0f) paddle_pos = 1023.0f;
        stella_set_paddle((int)paddle_pos, fire);
        // Still allow select/reset.
        stella_set_input(0, 0, 0, 0, 0, sSelectPress ? 1 : 0, sResetPress ? 1 : 0);
    } else {
        uint8_t up    = (cur & kButtonUp)    ? 1 : 0;
        uint8_t down  = (cur & kButtonDown)  ? 1 : 0;
        uint8_t left  = (cur & kButtonLeft)  ? 1 : 0;
        uint8_t right = (cur & kButtonRight) ? 1 : 0;
        stella_set_input(up, down, left, right, fire,
                         sSelectPress ? 1 : 0,
                         sResetPress  ? 1 : 0);
    }
    // Single-shot select/reset are nudged by the system menu callbacks.
    sSelectPress = 0;
    sResetPress  = 0;
}

// --- Frame tick ----------------------------------------------------------
static int update_cb(void* userdata)
{
    (void)userdata;
    static int frame_count = 0;
    static unsigned int last_ms = 0;

    poll_input();
    // Atari is 60 Hz; the Playdate display runs at 30 Hz. Emulate two Atari
    // frames per tick so the game runs at correct speed, and only render the
    // second (we'd throw the first frame's pixels away anyway).
    unsigned int t0 = pd_->system->getCurrentTimeMilliseconds();
    stella_run_frame();
    stella_run_frame();
    unsigned int t1 = pd_->system->getCurrentTimeMilliseconds();
    render_frame();
    unsigned int t2 = pd_->system->getCurrentTimeMilliseconds();
    static unsigned int emu_ms = 0, ren_ms = 0;
    emu_ms += (t1 - t0);
    ren_ms += (t2 - t1);
    pd_->system->drawFPS(0, 0);

    if (++frame_count % 120 == 0) {
        unsigned int now = pd_->system->getCurrentTimeMilliseconds();
        if (last_ms != 0) {
            unsigned int dt = now - last_ms;
            // 120 frames over dt ms -> fps*100 to avoid float in the log
            unsigned int fps100 = dt ? (120u * 100000u) / dt : 0;
            pd_->system->logToConsole("FPS: %u.%02u (%u ms) emu=%u ms ren=%u ms /120f",
                                      fps100 / 100, fps100 % 100, dt, emu_ms, ren_ms);
        }
        last_ms = now;
        emu_ms = 0;
        ren_ms = 0;
    }
    if (frame_count == kScreenshotFrame) save_screenshot();
    return 1;
}

// --- ROM load: try rom.a26 then rom.bin from the .pdx ---------------------
static int load_rom(PlaydateAPI* pd)
{
    static const char* candidates[] = { "rom.a26", "rom.bin", "Combat.bin", NULL };
    for (int i = 0; candidates[i]; ++i)
    {
        FileStat st;
        if (pd->file->stat(candidates[i], &st) != 0) continue;
        SDFile* f = pd->file->open(candidates[i], kFileRead | kFileReadData);
        if (!f) continue;
        int to_read = (int)st.size;
        if (to_read > (int)sizeof(rom_buf)) to_read = (int)sizeof(rom_buf);
        pd->file->read(f, rom_buf, to_read);
        pd->file->close(f);
        rom_size = (uint32_t)to_read;
        stella_init(rom_buf, rom_size);
        return 1;
    }
    pd->system->error("No ROM found (looked for rom.a26 / rom.bin / Combat.bin)");
    return 0;
}

static void on_menu_reset(void* ud) { (void)ud; sResetPress = 1; }
static void on_menu_select(void* ud) { (void)ud; sSelectPress = 1; }

// Controller options menu: "Joystick" / "Paddle". Switching rebuilds the
// console (controller objects are chosen at construction time).
static PDMenuItem* sCtrlMenu = NULL;
static void on_menu_controller(void* ud)
{
    (void)ud;
    int idx = pd_->system->getMenuItemValue(sCtrlMenu);  // 0=Joystick, 1=Paddle
    stella_set_paddle_mode(idx);
    if (rom_size) stella_init(rom_buf, rom_size);         // reload with new controller
    paddle_pos = 512.0f;
}

// On the device the Playdate startup (setup.c) never walks .init_array, so
// C++ global constructors don't run and every global object's vtable pointer
// stays zero -- the first virtual call then faults. Walk the init array
// ourselves before touching any C++ globals. (The simulator/host runtime
// already does this, so only do it on the device build.)
#if defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
static void run_static_ctors(void)
{
    for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
        (*p)();
}
#else
static void run_static_ctors(void) {}
#endif

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    (void)arg;
    if (event == kEventInit)
    {
        pd_ = pd;
        run_static_ctors();          // must precede any C++ global use
        init_luma_lut();
        // NOTE: the stack-region DTCM pool (dtcm.c) is left dormant for now --
        // it can only safely hold a few KB and our hot cart buffer is ~16-20 KB,
        // so stella_alloc_buffers() uses static RAM. The infra stays for a
        // future ITCM/DTCM pass on smaller hot state.
        stella_alloc_buffers();
        if (!load_rom(pd)) return 0;
        pd->display->setRefreshRate(30);   // half-rate to match TIA workload
        pd->system->addMenuItem("Reset",  on_menu_reset,  NULL);
        pd->system->addMenuItem("Select", on_menu_select, NULL);
        static const char* ctrl_opts[] = { "Joystick", "Paddle" };
        sCtrlMenu = pd->system->addOptionsMenuItem("Control", ctrl_opts, 2,
                                                   on_menu_controller, NULL);
        pd->system->setUpdateCallback(update_cb, pd);
    }
    return 0;
}
