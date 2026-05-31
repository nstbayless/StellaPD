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

bool ce_start_rom(uint8_t* rom, size_t size, const char* system_slug, const char* rom_basename)
{
    (void)system_slug; (void)rom_basename;
    if (!rom || size == 0) return false;
    stella_init(rom, (uint32_t)size);
    stella_force_full_repaint();
    return true;
}

void ce_end_rom(void) { /* nothing to free — stella core owns its state */ }

void ce_update(void) { stellapd_run_tick(); }

// Save data: stella core doesn't currently surface SRAM/battery state through
// this TU, so report none.
bool   ce_is_save_dirty(void)                            { return false; }
size_t ce_get_rom_save_size(const uint8_t* r, size_t s)  { (void)r; (void)s; return 0; }
void   ce_save(uint8_t* b, size_t s)                     { (void)b; (void)s; }
bool   ce_load(const uint8_t* b, size_t s)               { (void)b; (void)s; return true; }

// --- preferences -----------------------------------------------------------
static const char* pref_control_name(ce_preference_t* self)        { (void)self; return "Control"; }
static const char* pref_control_description(ce_preference_t* self) { (void)self; return "Input device emulated for the Atari controller port."; }
static const char* const pref_control_values[] = { "Joystick", "Paddle", NULL };
static int  pref_control_get(ce_preference_t* self)                 { (void)self; return stella_get_paddle_mode() ? 1 : 0; }
static void pref_control_set(ce_preference_t* self, unsigned v)
{
    (void)self;
    stella_set_paddle_mode(v ? 1 : 0);
    stella_force_full_repaint();
}

static char s_pref_control_id[] = "control";
static ce_preference_t s_pref_control = {
    .ud           = NULL,
    .is_category  = 0,
    .id           = s_pref_control_id,
    .name         = pref_control_name,
    .description  = pref_control_description,
    .values       = pref_control_values,
    .get          = pref_control_get,
    .set          = pref_control_set,
    .locked       = NULL,
    .hide         = NULL,
    .requires_restart = true,   // changing control rebuilds the console
};

static ce_preference_t* s_prefs[] = { &s_pref_control, NULL };
ce_preference_t** ce_get_preferences(void) { return s_prefs; }

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
    ce_start_rom,
    ce_end_rom,
    ce_update,
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
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    int dynamic = 0;
    {
        PDLL_EVENT(pd, event, arg);
        if (pdll) dynamic = 1;
    }
    return stellapd_event_handler(pd, event, arg, dynamic);
}

#ifdef __cplusplus
}
#endif
