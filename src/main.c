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
extern void          stella_set_control_mode(int mode);    // 0=Auto, 1=Joystick, 2=Paddle
extern int           stella_get_control_mode(void);
extern void          stella_set_crank_docked(int docked);  // capture for Auto resolution
extern void          stella_set_color_mode(int color);
extern void          stella_set_left_difficulty(int diff_a);
extern void          stella_set_right_difficulty(int diff_a);
extern void          stella_set_tv_type(int pal);
extern int           stella_get_tv_type(void);
extern void          stella_set_dtcm_alloc(void* (*fn)(size_t));
extern void          stella_alloc_buffers(void);
extern const char*   stella_cart_name(void);

// Backdrop fill colour for the LCD margins (0 = black, 1 = white). Mirrors
// the libcrankemu pref `a26:backdrop`. Read by render_begin via backdrop_byte().
int g_backdrop_white = 0;
void stellapd_set_backdrop_white(int white) {
    int v = white ? 1 : 0;
    if (v != g_backdrop_white) {
        g_backdrop_white = v;
        // Force a full repaint so the new margin colour gets blitted to the LCD.
        extern void stella_force_full_repaint(void);
        stella_force_full_repaint();
    }
}
int stellapd_get_backdrop_white(void) { return g_backdrop_white; }
static inline uint8_t backdrop_byte(void) { return g_backdrop_white ? 0xFFu : 0x00u; }

// pd_ and the button-state hook are non-static so the libcrankemu adapter
// (src/ce_iface.c) can share them when StellaPD is loaded as a dynamic
// library (pdll) and driven through the ce_* interface.
PlaydateAPI* pd_ = NULL;
void (*stellapd_get_buttons_hook)(PDButtons*, PDButtons*, PDButtons*) = NULL;
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

// Force a full repaint + full dirty mark on the next frame (first frame, or
// whenever the display geometry changes, e.g. controller/console reload).
static int  g_force_full_repaint = 1;
static int  g_last_numLines = -1, g_last_yOff = -1;

void stella_force_full_repaint(void) { g_force_full_repaint = 1; }

// Dither one 160-px source row to one packed LCD row (bytes [DEST_BYTE_X..+40)),
// XOR-comparing against what's there. Returns nonzero if the row changed.
static inline int dither_row_to(uint8_t* o, const uint8_t* row, int bayerRow)
{
    const uint8_t* by = kBayer4 + ((bayerRow & 3) << 2);
    const uint8_t t0 = by[0], t1 = by[1], t2 = by[2], t3 = by[3];
    uint8_t diff = 0;
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
        diff |= (uint8_t)(o[b] ^ v);
        o[b] = v;
    }
    return diff != 0;
}

#ifdef STELLA_SCANLINE_DITHER
// --- Scanline-at-a-time path -----------------------------------------------
// The TIA calls stella_emit_scanline() for each completed visible line; we
// dither it straight to the LCD here. Frame setup/flush bracket the two
// stella_run_frame() calls (we only display the 2nd emulated Atari frame).
static uint8_t* s_fb       = 0;     // cached getFrame() for the current tick
static int      s_yOff     = 0;
static int      s_numLines = 0;
static int      s_emit     = 0;     // dither this emulated frame? (only the 2nd)
static int      s_full     = 0;     // forced full repaint this tick
static int      s_run_start = -1;   // contiguous dirty-row run accumulator
static unsigned g_marked_rows = 0;  // diagnostic: dirty rows over the log window

void stella_emit_scanline(const uint8_t* line, int row)
{
#ifdef PROF_SKIP_EMIT
    (void)line; (void)row;
    return;   // bisection: skip dither+row-mark entirely
#endif
    if (!s_emit || row < 0 || row >= s_numLines) return;
    int destRow = s_yOff + row;
    uint8_t* o = s_fb + destRow * LCD_ROWSIZE + DEST_BYTE_X;
#ifdef PROF_SKIP_DITHER
    (void)o; (void)line;
    int changed = 0;   // bisection: skip dither work; still mark for blit
#else
    int changed = dither_row_to(o, line, row);
#endif
    if (s_full || changed) {
        if (s_run_start < 0) s_run_start = destRow;
        g_marked_rows++;   // diagnostic: count rows actually transferred
    } else if (s_run_start >= 0) {
#ifndef SKIP_ROW_MARK
        pd_->graphics->markUpdatedRows(s_run_start, destRow - 1);
#endif
        s_run_start = -1;
    }
}

// Called before the two stella_run_frame() calls each tick.
static void render_begin(void)
{
    uint32_t numLines = stella_display_num_scanlines();
    if (numLines > (uint32_t)LCD_ROWS) numLines = LCD_ROWS;
    int yOff = ((int)LCD_ROWS - (int)numLines) / 2;
    if (yOff < 0) yOff = 0;

    if ((int)numLines != g_last_numLines || yOff != g_last_yOff)
        g_force_full_repaint = 1;
    g_last_numLines = (int)numLines;
    g_last_yOff = yOff;

    s_numLines = (int)numLines;
    s_yOff = yOff;
    s_fb = pd_->graphics->getFrame();
    s_run_start = -1;
    s_full = g_force_full_repaint;
    if (s_full) {
        memset(s_fb, backdrop_byte(), LCD_ROWSIZE * LCD_ROWS);   // paint static margins
        g_force_full_repaint = 0;
    }
}

// Called after the displayed (2nd) frame to flush trailing dirty rows.
static void render_end(void)
{
    if (s_run_start >= 0) {
#ifndef SKIP_ROW_MARK
        pd_->graphics->markUpdatedRows(s_run_start, s_yOff + s_numLines - 1);
#endif
        s_run_start = -1;
    }
    if (s_full) {
#ifndef SKIP_ROW_MARK
        pd_->graphics->markUpdatedRows(0, LCD_ROWS - 1);   // margins, once
#endif
    }
}

#else  // ---- full-buffer path (TIA fills sFrameBufferA, we dither it here) --

// Render the Stella 160xN framebuffer to the LCD with CrankBoy-style row
// diffing: only rows that actually changed since last frame are marked dirty.
static void render_frame(void)
{
    const uint8_t* fb = stella_framebuffer();
    uint32_t startLine = stella_display_start_scanline();
    uint32_t numLines  = stella_display_num_scanlines();
    if (numLines > (uint32_t)LCD_ROWS) numLines = LCD_ROWS;

    uint8_t* dst = pd_->graphics->getFrame();
    int yOff = ((int)LCD_ROWS - (int)numLines) / 2;
    if (yOff < 0) yOff = 0;

    if ((int)numLines != g_last_numLines || yOff != g_last_yOff)
        g_force_full_repaint = 1;
    g_last_numLines = (int)numLines;
    g_last_yOff = yOff;

    int full = g_force_full_repaint;
    if (full) {
        memset(dst, backdrop_byte(), LCD_ROWSIZE * LCD_ROWS);
        g_force_full_repaint = 0;
    }

    int run_start = -1;
    for (uint32_t y = 0; y < numLines; ++y)
    {
        const uint8_t* row = fb + (startLine + y) * 160;
        int destRow = yOff + (int)y;
        uint8_t* o = dst + destRow * LCD_ROWSIZE + DEST_BYTE_X;
        int changed = dither_row_to(o, row, (int)y);
        if (full || changed) {
            if (run_start < 0) run_start = destRow;
        } else if (run_start >= 0) {
#ifndef SKIP_ROW_MARK
            pd_->graphics->markUpdatedRows(run_start, destRow - 1);
#endif
            run_start = -1;
        }
    }
    if (run_start >= 0) {
#ifndef SKIP_ROW_MARK
        pd_->graphics->markUpdatedRows(run_start, yOff + (int)numLines - 1);
#endif
    }
    if (full) {
#ifndef SKIP_ROW_MARK
        pd_->graphics->markUpdatedRows(0, LCD_ROWS - 1);
#endif
    }
}
#endif  // STELLA_SCANLINE_DITHER

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
// Console-switch pulse state. Selecting "Select" or "Reset" from the single
// "Input" OS menu item holds that switch down for kInputHoldFrames frames,
// then auto-releases (and resets the menu to "None") so the user can pulse
// it again. ~12 frames @ 30 fps is ~0.4s of held-down time, plenty for the
// emulated cart's M6532 poll to see the bit transition.
#define kInputHoldFrames 12
static int sSelectHold = 0;  // frames remaining where Select is "pressed"
static int sResetHold  = 0;  // frames remaining where Reset is "pressed"

static void poll_input(void)
{
    PDButtons cur, pressed, released;
    if (stellapd_get_buttons_hook)
        stellapd_get_buttons_hook(&cur, &pressed, &released);
    else
        pd_->system->getButtonState(&cur, &pressed, &released);

    uint8_t fire  = (cur & (kButtonA | kButtonB)) ? 1 : 0;

    uint8_t sel_now = (sSelectHold > 0) ? 1 : 0;
    uint8_t rst_now = (sResetHold  > 0) ? 1 : 0;

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
        // Still allow select/reset pulses.
        stella_set_input(0, 0, 0, 0, 0, sel_now, rst_now);
    } else {
        uint8_t up    = (cur & kButtonUp)    ? 1 : 0;
        uint8_t down  = (cur & kButtonDown)  ? 1 : 0;
        uint8_t left  = (cur & kButtonLeft)  ? 1 : 0;
        uint8_t right = (cur & kButtonRight) ? 1 : 0;
        stella_set_input(up, down, left, right, fire, sel_now, rst_now);
    }

    // Tick down the held-switch counters. When a hold finishes, reset the
    // menu UI to "None" so the user can re-trigger the same switch.
    if (sSelectHold > 0) --sSelectHold;
    if (sResetHold  > 0) --sResetHold;
    extern void stellapd_input_menu_idle_if_done(void);
    stellapd_input_menu_idle_if_done();
}

// --- Frame tick ----------------------------------------------------------
static int update_cb(void* userdata)
{
    (void)userdata;
    static int frame_count = 0;
    static unsigned int last_ms = 0;

    // Just two timestamps: very start and very end of update_cb. Sum across
    // 120 ticks. If sum_inside < dt (period across 120 ticks), the wall is
    // BETWEEN ticks (system pacing). If sum_inside ~= dt, the wall is INSIDE
    // (something we call blocks/runs that long). Decisive test.
    static unsigned int sum_inside = 0;
    unsigned int ta = pd_->system->getCurrentTimeMilliseconds();

    // Incremental bisection: TICK_STAGE controls what runs inside update_cb.
    //   0: nothing (empty tick) -- measured ~13000 Hz
    //   1: + 2x stella_run_frame (CPU + TIA, no display calls at all)
    //   2: + render_begin / render_end (markUpdatedRows on dirty rows)
    //   3: + drawFPS (full normal build)
#ifndef TICK_STAGE
#define TICK_STAGE 3
#endif

#if TICK_STAGE == 0
    static int frame_count_e = 0;
    static unsigned int last_ms_e = 0;
    if (++frame_count_e % 120 == 0) {
        unsigned int now = pd_->system->getCurrentTimeMilliseconds();
        if (last_ms_e) {
            unsigned int dt = now - last_ms_e;
            unsigned int fps100 = dt ? (120u * 100000u) / dt : 0;
            pd_->system->logToConsole("STAGE0 EMPTY FPS %u.%02u dt=%u",
                fps100/100, fps100%100, dt);
        }
        last_ms_e = now;
    }
    return 1;
#endif

    poll_input();
#ifdef STELLA_SCANLINE_DITHER
#if TICK_STAGE >= 2
    render_begin();
#endif
#if TICK_STAGE >= 1
    s_emit = 0; stella_run_frame();
#if TICK_STAGE >= 2
    s_emit = 1; stella_run_frame();
#else
    s_emit = 0; stella_run_frame();   // both frames non-emit if no display calls
#endif
#endif
#if TICK_STAGE >= 2
    render_end();
#endif
    unsigned int t1, t2;
    t1 = t2 = pd_->system->getCurrentTimeMilliseconds();
#else
    unsigned int t1, t2;
    stella_run_frame();
    stella_run_frame();
    t1 = pd_->system->getCurrentTimeMilliseconds();
    render_frame();
    t2 = pd_->system->getCurrentTimeMilliseconds();
#endif
    static unsigned int emu_ms = 0, ren_ms = 0;
    emu_ms += (t1 - ta);   // crude: time from start of update_cb to after run2
    ren_ms += (t2 - t1);
#if TICK_STAGE >= 3
    pd_->system->drawFPS(0, 0);
#endif

    unsigned int th = pd_->system->getCurrentTimeMilliseconds();
    sum_inside += (th - ta);

    if (++frame_count % 120 == 0) {
        unsigned int now = pd_->system->getCurrentTimeMilliseconds();
        if (last_ms != 0) {
            unsigned int dt = now - last_ms;
            // 120 frames over dt ms -> fps*100 to avoid float in the log
            unsigned int fps100 = dt ? (120u * 100000u) / dt : 0;
            unsigned rows_per_frame = 0;
#ifdef STELLA_SCANLINE_DITHER
            rows_per_frame = g_marked_rows / 120;
#endif
            pd_->system->logToConsole("FPS %u.%02u dt=%u inside=%u gap_between=%d (per120f, ms)",
                fps100 / 100, fps100 % 100, dt, sum_inside, (int)dt - (int)sum_inside);
            sum_inside = 0;
            emu_ms = ren_ms = 0;
        }
        last_ms = now;
        emu_ms = 0;
        ren_ms = 0;
#ifdef STELLA_SCANLINE_DITHER
        g_marked_rows = 0;
#endif
    }
    if (frame_count == kScreenshotFrame) save_screenshot();
    // Diagnostic: returning 0 tells the system not to refresh the display this
    // tick. If the wall is the display refresh, this should let update_cb run
    // back-to-back (much higher FPS counter). If it doesn't, the wall is
    // elsewhere.
#ifdef RETURN_ZERO
    return 0;
#else
    return 1;
#endif
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

// Single "Input" OS-menu item -- one slot per libcrankemu spec ("the
// emulator should only set up to 1 menu item"). Values: 0=None, 1=Select,
// 2=Reset. Selecting one of the latter holds that console switch down for
// kInputHoldFrames frames, then the poll_input tick auto-resets the menu
// back to "None" so the user can re-trigger.
static PDMenuItem* sInputMenu = NULL;
static const char* k_input_opts[] = { "None", "Select", "Reset" };

static void on_menu_input(void* ud)
{
    (void)ud;
    int idx = pd_->system->getMenuItemValue(sInputMenu);
    if (idx == 1)      sSelectHold = kInputHoldFrames;
    else if (idx == 2) sResetHold  = kInputHoldFrames;
    // idx == 0 (None) just leaves the holds alone; if a pulse is already
    // in flight, the user can't shorten it from here. That's fine.
}

// Called from poll_input each tick: once both holds have drained, snap the
// menu UI back to "None" so a fresh selection of Select/Reset re-fires.
void stellapd_input_menu_idle_if_done(void)
{
    if (!sInputMenu) return;
    if (sSelectHold == 0 && sResetHold == 0) {
        int cur = pd_->system->getMenuItemValue(sInputMenu);
        if (cur != 0) pd_->system->setMenuItemValue(sInputMenu, 0);
    }
}

// Controller options menu: "Auto" / "Joystick" / "Paddle". Switching
// rebuilds the console (controller objects are chosen at construction
// time). Auto resolves to Joystick or Paddle based on crank dock state at
// stella_init time.
static PDMenuItem* sCtrlMenu = NULL;
static void on_menu_controller(void* ud)
{
    (void)ud;
    int idx = pd_->system->getMenuItemValue(sCtrlMenu);   // 0=Auto, 1=Joystick, 2=Paddle
    stella_set_control_mode(idx);
    if (rom_size) {
        stella_set_crank_docked(pd_->system->isCrankDocked());
        stella_init(rom_buf, rom_size);                   // reload with new controller
    }
    paddle_pos = 512.0f;
    stella_force_full_repaint();                          // resync the row-diff baseline
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

// Single-tick entry point so the libcrankemu adapter (ce_update) can drive
// the same update body as the standalone setUpdateCallback path.
void stellapd_run_tick(void) { (void)update_cb(NULL); }

// Common event handling. When `dynamic` is non-zero we skip the
// standalone-only setup (rom loading, refresh rate, menu items, update
// callback) — under libcrankemu the frontend owns those.
int stellapd_event_handler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg, int dynamic)
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
        if (dynamic) return 0;
        // For the standalone launch path we own ROM load + the OS menu items.
        // Capture current crank dock state before the first stella_init so
        // Auto-mode resolution picks the right controller.
        stella_set_crank_docked(pd->system->isCrankDocked());
        if (!load_rom(pd)) return 0;
        pd->display->setRefreshRate(0);    // ASAP: system clocks update_cb faster
                                           // than 30Hz when dirty rows are few
                                           // (per SDK: refresh=0 -> indeterminate
                                           // higher rate). Matches CrankBoy.
        pd->system->setAutoLockDisabled(1); // keep the device awake during dev/testing
        // Per libcrankemu spec ("emulator should only set up to 1 menu item"),
        // we own exactly one OS menu slot in either launch mode. Use it for
        // Select/Reset pulses; the Control / TV Type / L Diff / R Diff /
        // Backdrop / NTSC-PAL knobs live as libcrankemu preferences instead.
        sInputMenu = pd->system->addOptionsMenuItem("Input", k_input_opts, 3,
                                                    on_menu_input, NULL);
        // Push initial switch defaults (B&W, B Novice, B Novice) into the
        // running Console so SWCHB starts with the intended bits even
        // before the user opens preferences. Matches the ce_iface defaults.
        stella_set_color_mode(0);
        stella_set_left_difficulty(0);
        stella_set_right_difficulty(0);
        pd->system->setUpdateCallback(update_cb, pd);
    }
    return 0;
}

// The exported `eventHandler` lives in src/ce_iface.c; that TU also owns the
// PDLL export table so a libcrankemu frontend can pdll-load us. In a
// standalone launch the pdll detection there is a no-op and the call falls
// through to stellapd_event_handler() with dynamic=0.
