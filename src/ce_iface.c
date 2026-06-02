// ce_iface.c — libcrankemu (https://github.com/CrankBoyHQ/libcrankemu)
// adapter for StellaPD. When the .pdex is loaded via pdll by a libcrankemu
// frontend, this TU owns the exported `eventHandler` and the `ce_*` core
// symbols; everything else still lives in main.c / stella_glue.cpp.
//
// The simulator build feeds this file to g++ (with `-x c`), and g++ has been
// observed to still produce C++ mangling for it — which broke pdll's dlopen
// lookup of `ce_*`/`eventHandler`/stella_force_full_repaint`. We force C
// linkage explicitly so the build mode doesn't matter.

#ifdef __cplusplus
extern "C" {
#endif

#include "pd_api.h"
#include "pdll.h"
#include "libcrankemu.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// --- imports from main.c / stella_glue.cpp ---------------------------------
extern PlaydateAPI* pd_;
extern void (*stellapd_get_buttons_hook)(PDButtons*, PDButtons*, PDButtons*);
extern int  stellapd_event_handler(PlaydateAPI*, PDSystemEvent, uint32_t, int dynamic);
extern void stellapd_run_tick(void);
extern void stella_force_full_repaint(void);

extern int           stella_init(const uint8_t* image, uint32_t size);
extern void          stella_set_paddle_mode(int on);
extern int           stella_get_paddle_mode(void);
extern void          stella_set_control_mode(int mode);
extern int           stella_get_control_mode(void);
extern void          stella_set_tv_type(int pal);
extern int           stella_get_tv_type(void);
extern void          stella_set_color_mode(int color);
extern void          stella_set_left_difficulty(int diff_a);
extern void          stella_set_right_difficulty(int diff_a);
extern void          stellapd_set_backdrop_white(int white);
extern int           stellapd_get_backdrop_white(void);
extern void          stella_set_dtcm_alloc(void* (*fn)(size_t));
extern const char*   stella_cart_name(void);

// --- frontend handle -------------------------------------------------------
static const ce_frontend_t* s_fe = NULL;

// Adapt the frontend's (size, alignment) dtcm allocator to stella_glue's
// single-arg signature. The stella side currently asks for naturally-aligned
// blocks, so we just pass 0 for alignment here.
static void* ce_dtcm_alloc_wrapper(size_t size)
{
    if (!s_fe || !s_fe->alloc_dtcm) return NULL;
    return s_fe->alloc_dtcm(size, 0);
}

// --- ce_* core API ---------------------------------------------------------

void ce_set_frontend(const ce_frontend_t* fe)
{
    s_fe = fe;
    if (fe && fe->get_buttons)
        stellapd_get_buttons_hook = fe->get_buttons;
    else
        stellapd_get_buttons_hook = NULL;
    if (fe && fe->alloc_dtcm)
        stella_set_dtcm_alloc(ce_dtcm_alloc_wrapper);
}

const char* ce_core_id(void)      { return "stella"; }
const char* ce_core_name(void)    { return "StellaPD"; }
const char* ce_core_version(void) { return "v0.1"; }

const char* ce_get_system_slugs(void) { return "a26"; }

const char* ce_get_system_name_from_slug(const char* system_slug)
{
    if (system_slug && strcmp(system_slug, "a26") == 0) return "Atari 2600";
    return NULL;
}

// Provide our own tiny strcmp so the build doesn't pull newlib's 736-byte
// optimised one (pdll_getsymbol_impl above and ce_get_system_name_from_slug
// are the only callers). Newlib's strcmp lives inside libc.a, so the linker
// picks ours over the archive's version. That ~700-byte saving keeps
// stella_emit_scanline / M6532::peek / cpu_set_nz in the I-cache hot path
// (worth ~1 fps on the device).
int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

bool ce_load_rom(uint8_t* rom, size_t size, const char* system_slug, const char* rom_basename)
{
    (void)system_slug; (void)rom_basename;
    if (!rom || size == 0) return false;
    stella_init(rom, (uint32_t)size);
    stella_force_full_repaint();
    return true;
}

// Stella holds onto the Console object across ce_play / ce_stop cycles --
// ce_unload_rom would normally drop it, but our existing code reuses the
// same Console for the next ce_load_rom; nothing to free here.
void ce_unload_rom(void) { }

// --- Optional ITCM relocation ----------------------------------------------
// The frontend can offer StellaPD an alloc_dtcm callback + an itcm_allowed
// flag in its config; when both are present we copy the smol interpreter's
// hot body (m6502_execute_smol_impl, marked .text.itcm.stella.execute_smol
// in link_map.ld) into the DTCM-allocated buffer and swap the dispatch
// function pointer to call the relocated copy. DTCM access is single-cycle
// vs. cached SRAM, so the per-instruction `[r7]` loads we anchored in BSS
// earlier (gSystemCycles, the smol register file, etc.) become unconditional
// hits regardless of D-cache contention from other workloads.
//
// Safety: only honored at ce_play time, only when the frontend explicitly
// allows it (frontend_config_t.itcm_allowed) AND provides alloc_dtcm. If
// either is missing we just keep running from flash.

extern uint8_t __stella_itcm_text_start[];
extern uint8_t __stella_itcm_text_end[];
extern void (*g_m6502_execute_smol)(void);   // defined in M6502Smol.cpp

// Read by the standalone update_cb (via stella_get_turbo) to decide whether
// to emulate two Atari NTSC frames per Playdate tick (turbo == native 60 Hz
// Atari on a 30 Hz host) or one (turbo off -- frontend has asked us to
// frame-rate-limit). Default is on; ce_play re-syncs it to the frontend's
// config each time the ROM is started.
int stella_turbo = 1;

static int s_itcm_relocated = 0;

static void maybe_relocate_to_itcm(void)
{
    if (s_itcm_relocated) return;
    if (!s_fe) return;
    if (!s_fe->alloc_dtcm) return;
    if (!s_fe->config) return;
    const ce_frontend_config_t* cfg = s_fe->config();
    if (!cfg || !cfg->itcm_allowed) return;

    size_t size = (size_t)(__stella_itcm_text_end - __stella_itcm_text_start);
    if (size == 0) return;

    // 32-byte alignment matches the section's alignment in the link script
    // (cache-line aligned) and keeps PC-relative literal pool offsets
    // word-aligned in the relocated copy.
    void* buf = s_fe->alloc_dtcm(size, 32);
    if (!buf) {
        if (s_fe->set_error) s_fe->set_error("ITCM: alloc_dtcm(%u) failed", (unsigned)size);
        return;
    }
    memcpy(buf, __stella_itcm_text_start, size);

    // Compute relocated address of m6502_execute_smol_impl. Thumb function
    // pointers have the LSB set ("thumb bit"); preserve it through the
    // arithmetic. The relocation offset is the same for every byte in the
    // span -- if we wanted to relocate more than one function in the future
    // each would get the same (buf - __stella_itcm_text_start) adjustment.
    uintptr_t orig = (uintptr_t)g_m6502_execute_smol;
    uintptr_t offset = (uintptr_t)buf - (uintptr_t)__stella_itcm_text_start;
    g_m6502_execute_smol = (void (*)(void))(orig + offset);

    // Make sure the I-cache sees the copy as code, not stale data lines.
    // pd_ is set by stellapd_event_handler at kEventInit.
    if (pd_ && pd_->system && pd_->system->clearICache)
        pd_->system->clearICache();

    s_itcm_relocated = 1;
    if (pd_ && pd_->system && pd_->system->logToConsole)
        pd_->system->logToConsole(
            "stella ITCM: relocated %u bytes -> %p (orig %p, offset 0x%lx)",
            (unsigned)size, buf, (void*)orig, (unsigned long)offset);
}

// play/stop are gating events for emulation. For StellaPD the Console
// is always ready once load_rom has run, and ticks come in from
// ce_update, so the play/stop pair is mostly a no-op -- with the new
// frontend-config protocol, ce_play is also our hook for optional ITCM
// relocation (idempotent: only runs once across multiple play cycles).
bool ce_play(void)
{
    // Latch the frontend's turbo flag for the standalone tick loop. Default
    // (no frontend, or no config()) is on -- StellaPD's natural cadence.
    if (s_fe && s_fe->config) {
        const ce_frontend_config_t* cfg = s_fe->config();
        if (cfg) stella_turbo = cfg->turbo ? 1 : 0;
    } else {
        stella_turbo = 1;
    }
    maybe_relocate_to_itcm();
    return true;
}
void ce_stop(void) { }

// Returns the number of *emulated* frames advanced per host tick. Stella
// runs two Atari NTSC frames per Playdate update so the user sees ~60 Hz
// gameplay on the 30 Hz host -- report 2 so the frontend's pacing logic
// (turbo / frame budgeting) has the right value.
int ce_update(void) { stellapd_run_tick(); return 2; }

// Tell the frontend what optional facilities this core uses.
//   itcm_allowed: StellaPD's hot interpreter benefits from ITCM placement
//                 (see commit 5275ec1's perf work).
//   turbo:        true == do not frame-rate-limit; we always run at the
//                 host's max update tick (the Atari frame pacing is the
//                 caller's job).
static const ce_frontend_config_t s_config = {
    .itcm_allowed = true,
    .turbo = true,
};
const ce_frontend_config_t* ce_config(void) { return &s_config; }

// The frontend scribbled over the framebuffer (showed a modal etc.) and
// wants us to repaint our entire image on the next tick. We just flip the
// existing force-full-repaint flag -- the next render_begin() / render_end()
// fills the margins, dithers every scanline, and markUpdatedRows the whole
// LCD.
void ce_full_redraw(void) { stella_force_full_repaint(); }

// Save data: stella core doesn't currently surface SRAM/battery state through
// this TU, so report none.
bool   ce_is_save_dirty(void)                            { return false; }
size_t ce_get_rom_save_size(const uint8_t* r, size_t s)  { (void)r; (void)s; return 0; }
void   ce_save(uint8_t* b, size_t s)                     { (void)b; (void)s; }
bool   ce_load(const uint8_t* b, size_t s)               { (void)b; (void)s; return true; }

// --- preferences -----------------------------------------------------------
//
// Layout (categories are bracketed):
//   [Input]
//     Control  (Auto / Joystick / Paddle)
//   [Miscellaneous]
//     Letterbox  (Black / White margin colour)
//     TV System  (NTSC / PAL)
//   [Switches]
//     TV Type             (B&W / Color)            default B&W
//     L. Difficulty       (B Novice / A Pro)       always-local
//     R. Difficulty       (B Novice / A Pro)       always-local
//
// CrankBoy's Settings_Scene case-insensitively merges "Input" and
// "Miscellaneous" into its built-in sections of the same names. "Switches"
// has no built-in counterpart, so it surfaces as a brand-new section
// (between General and Library/Misc per the section-order rule).

// "Input" category header.
static const char* pref_input_cat_name(ce_preference_t* self) { (void)self; return "Input"; }
static char s_pref_input_cat_id[] = "input";
static ce_preference_t s_pref_input_cat = {
    .type = CE_PREFERENCE_CATEGORY,
    .id   = s_pref_input_cat_id,
    .name = pref_input_cat_name,
};

// Control: Auto / Joystick / Paddle. Auto uses Joystick when the crank is
// docked, Paddle when undocked, resolved at stella_init time.
static const char* pref_control_name(ce_preference_t* self)        { (void)self; return "Control"; }
static const char* pref_control_description(ce_preference_t* self) { (void)self; return "Input device emulated for the Atari controller port.\n \nAuto picks Joystick when the crank is docked, Paddle when undocked."; }
static const char* const pref_control_values[] = { "Auto", "Joystick", "Paddle", NULL };
static unsigned pref_control_get(ce_preference_t* self) { (void)self; return (unsigned)stella_get_control_mode(); }
static bool pref_control_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 2) return false;
    stella_set_control_mode((int)v);
    stella_force_full_repaint();
    return true;
}

// Changing Control rebuilds the Console (controller objects are chosen at
// construction time), so any change needs a restart to take effect cleanly.
static uint32_t pref_control_flags(ce_preference_t* self) { (void)self; return CE_PREF_REQUIRES_RESTART; }

static char s_pref_control_id[] = "control";
static ce_preference_t s_pref_control = {
    .ud           = NULL,
    .type         = CE_PREFERENCE_STANDARD,
    .id           = s_pref_control_id,
    .name         = pref_control_name,
    .description  = pref_control_description,
    .values       = pref_control_values,
    .get          = pref_control_get,
    .set          = pref_control_set,
    .flags        = pref_control_flags,
};

// "Miscellaneous" category header.
static const char* pref_misc_cat_name(ce_preference_t* self) { (void)self; return "Miscellaneous"; }
static char s_pref_misc_cat_id[] = "miscellaneous";
static ce_preference_t s_pref_misc_cat = {
    .type = CE_PREFERENCE_CATEGORY,
    .id   = s_pref_misc_cat_id,
    .name = pref_misc_cat_name,
};

// Letterbox: colour for the static margins around the 320x210ish Atari image
// on the 400x240 Playdate LCD. (Persisted JSON key stays "backdrop" for
// continuity with already-saved values.)
static const char* pref_backdrop_name(ce_preference_t* self)        { (void)self; return "Letterbox"; }
static const char* pref_backdrop_description(ce_preference_t* self) { (void)self; return "Colour shown in the screen margins outside the Atari image."; }
static const char* const pref_backdrop_values[] = { "Black", "White", NULL };
static unsigned pref_backdrop_get(ce_preference_t* self) { (void)self; return stellapd_get_backdrop_white() ? 1u : 0u; }
static bool pref_backdrop_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 1) return false;
    stellapd_set_backdrop_white((int)v);
    return true;
}
static char s_pref_backdrop_id[] = "backdrop";
static ce_preference_t s_pref_backdrop = {
    .ud           = NULL,
    .type         = CE_PREFERENCE_STANDARD,
    .id           = s_pref_backdrop_id,
    .name         = pref_backdrop_name,
    .description  = pref_backdrop_description,
    .values       = pref_backdrop_values,
    .get          = pref_backdrop_get,
    .set          = pref_backdrop_set,
    .flags        = NULL,
};

// TV System: NTSC (default) or PAL. Affects palette and display timing;
// changing requires a Console reload.
static const char* pref_tv_name(ce_preference_t* self)        { (void)self; return "TV System"; }
static const char* pref_tv_description(ce_preference_t* self) { (void)self; return "Region the cart was made for. PAL carts assume 50 Hz / different palette."; }
static const char* const pref_tv_values[] = { "NTSC", "PAL", NULL };
static unsigned pref_tv_get(ce_preference_t* self) { (void)self; return stella_get_tv_type() ? 1u : 0u; }
static bool pref_tv_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 1) return false;
    stella_set_tv_type((int)v);
    return true;
}
static uint32_t pref_tv_flags(ce_preference_t* self) { (void)self; return CE_PREF_REQUIRES_RESTART; }
static char s_pref_tv_id[] = "tv_system";
static ce_preference_t s_pref_tv = {
    .ud           = NULL,
    .type         = CE_PREFERENCE_STANDARD,
    .id           = s_pref_tv_id,
    .name         = pref_tv_name,
    .description  = pref_tv_description,
    .values       = pref_tv_values,
    .get          = pref_tv_get,
    .set          = pref_tv_set,
    .flags        = pref_tv_flags,
};

// --- Switches category: stateful console switches ------------------------
// On a real Atari 2600 these are physical toggle switches on the console
// front. We surface them as libcrankemu prefs (under a "Switches" category)
// rather than OS-menu items so they survive across game runs and the single
// OS-menu slot stays free for the momentary Select/Reset pulses.
//
// Defaults match the bits set in Switches::Switches (mySwitches = 0x0F |
// 0x08 = 0x0F): Color, B Novice, B Novice.

static const char* pref_switches_cat_name(ce_preference_t* self) { (void)self; return "Switches"; }
static char s_pref_switches_cat_id[] = "switches";
static ce_preference_t s_pref_switches_cat = {
    .ud           = NULL,
    .type         = CE_PREFERENCE_CATEGORY,
    .id           = s_pref_switches_cat_id,
    .name         = pref_switches_cat_name,
};

// Currently-applied switch state, mirrored here so get() can answer without
// asking the Console (which may not exist yet during ce_set_frontend).
// TV Type defaults to B&W (0) per user preference -- a real Atari powers up
// in either position depending on where the user last left the physical
// switch, so this is purely a UX choice.
static int s_color_val  = 0;  // 0 = B&W, 1 = Color
static int s_ldiff_val  = 0;  // 0 = B (Novice), 1 = A (Pro)
static int s_rdiff_val  = 0;

static const char* const pref_color_values[] = { "B&W", "Color", NULL };
static const char* pref_color_name(ce_preference_t* self)        { (void)self; return "TV Type"; }
static const char* pref_color_description(ce_preference_t* self) { (void)self; return "The B&W vs Color toggle switch on the front of the Atari."; }
static unsigned pref_color_get(ce_preference_t* self) { (void)self; return s_color_val ? 1u : 0u; }
static bool pref_color_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 1) return false;
    s_color_val = (int)v;
    stella_set_color_mode(s_color_val);
    return true;
}
static char s_pref_color_id[] = "color_mode";
static ce_preference_t s_pref_color = {
    .type = CE_PREFERENCE_STANDARD, .id = s_pref_color_id,
    .name = pref_color_name, .description = pref_color_description,
    .values = pref_color_values, .get = pref_color_get, .set = pref_color_set,
};

// Difficulty switches are per-game: typical Atari players want different
// difficulty levels for different cartridges, so persisting them globally
// would be the wrong default.
static uint32_t pref_diff_flags(ce_preference_t* self) { (void)self; return CE_PREF_ALWAYS_LOCAL; }

static const char* const pref_diff_values[] = { "B (Novice)", "A (Pro)", NULL };
static const char* pref_ldiff_name(ce_preference_t* self)        { (void)self; return "L. Difficulty"; }
static const char* pref_ldiff_description(ce_preference_t* self) { (void)self; return "Player 1 difficulty switch. A is the harder \"Pro\" setting."; }
static unsigned pref_ldiff_get(ce_preference_t* self) { (void)self; return s_ldiff_val ? 1u : 0u; }
static bool pref_ldiff_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 1) return false;
    s_ldiff_val = (int)v;
    stella_set_left_difficulty(s_ldiff_val);
    return true;
}
static char s_pref_ldiff_id[] = "left_difficulty";
static ce_preference_t s_pref_ldiff = {
    .type = CE_PREFERENCE_STANDARD, .id = s_pref_ldiff_id,
    .name = pref_ldiff_name, .description = pref_ldiff_description,
    .values = pref_diff_values, .get = pref_ldiff_get, .set = pref_ldiff_set,
    .flags = pref_diff_flags,
};

static const char* pref_rdiff_name(ce_preference_t* self)        { (void)self; return "R. Difficulty"; }
static const char* pref_rdiff_description(ce_preference_t* self) { (void)self; return "Player 2 difficulty switch. A is the harder \"Pro\" setting."; }
static unsigned pref_rdiff_get(ce_preference_t* self) { (void)self; return s_rdiff_val ? 1u : 0u; }
static bool pref_rdiff_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    if (v > 1) return false;
    s_rdiff_val = (int)v;
    stella_set_right_difficulty(s_rdiff_val);
    return true;
}
static char s_pref_rdiff_id[] = "right_difficulty";
static ce_preference_t s_pref_rdiff = {
    .type = CE_PREFERENCE_STANDARD, .id = s_pref_rdiff_id,
    .name = pref_rdiff_name, .description = pref_rdiff_description,
    .values = pref_diff_values, .get = pref_rdiff_get, .set = pref_rdiff_set,
    .flags = pref_diff_flags,
};

static ce_preference_t* s_prefs[] = {
    // [Input]
    &s_pref_input_cat,
    &s_pref_control,
    // [Miscellaneous]
    &s_pref_misc_cat,
    &s_pref_backdrop,
    &s_pref_tv,
    // [Switches]
    &s_pref_switches_cat,
    &s_pref_color,
    &s_pref_ldiff,
    &s_pref_rdiff,
    NULL,
};
ce_preference_t** ce_get_preferences(void)
{
    return s_prefs;
}

// --- rom info --------------------------------------------------------------
// Manual string builder -- using snprintf here pulls the full printf chain
// (_vfiprintf_r, sprintf_aux, _flsbuf, ...) into the device .text, which adds
// ~16KB between memset and our hot helpers and pushes stella_emit_scanline /
// M6532::peek out of the I-cache footprint, costing ~4 fps. Stay newlib-free.
static char s_rom_info[128];
static char* sri_append(char* p, char* end, const char* s)
{
    while (*s && p < end - 1) *p++ = *s++;
    return p;
}
static char* sri_append_uint(char* p, char* end, size_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n-- && p < end - 1) *p++ = tmp[n];
    return p;
}
const char* get_rom_info(const uint8_t* rom, size_t size)
{
    (void)rom;
    const char* name = stella_cart_name();
    if (!name) name = "";
    char* p = s_rom_info;
    char* end = s_rom_info + sizeof(s_rom_info);
    p = sri_append(p, end, "System:\tAtari 2600\nSize:\t");
    p = sri_append_uint(p, end, size);
    p = sri_append(p, end, "\nCart:\t");
    p = sri_append(p, end, name);
    *p = '\0';
    return s_rom_info;
}

// --- pdll export table + eventHandler -------------------------------------
// PDLL_EXPORT must live in the same TU as PDLL_EVENT (the macro references
// the static pdll_getsymbol_impl it generates).
PDLL_EXPORT(
    ce_get_version,
    ce_set_frontend,
    ce_core_id,
    ce_core_name,
    ce_core_version,
    ce_get_system_slugs,
    ce_get_system_name_from_slug,
    ce_load_rom,
    ce_unload_rom,
    ce_play,
    ce_stop,
    ce_update,
    ce_config,
    ce_full_redraw,
    ce_is_save_dirty,
    ce_get_rom_save_size,
    ce_save,
    ce_load,
    ce_get_preferences,
    get_rom_info,
    eventHandler
)

#ifdef _WINDLL
__declspec(dllexport)
#endif
// Sticky: set on the kEventInit PDLL handshake (when the loader passes
// PDLL_DYNAMIC_INIT_ARG); stays set for the rest of the process so later
// events (kEventPause, kEventResume, kEventTerminate, ...) know we're
// running as a libcrankemu core rather than a standalone .pdx. Without
// this, only the init dispatch was tagged dynamic, so the kEventPause
// install_input_menu_item() refresh never fired.
static int s_is_dynamic = 0;

int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    {
        PDLL_EVENT(pd, event, arg);
        if (pdll) s_is_dynamic = 1;
    }
    return stellapd_event_handler(pd, event, arg, s_is_dynamic);
}

#ifdef __cplusplus
}
#endif
