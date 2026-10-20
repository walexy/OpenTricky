/* Stand-ins for the symbols the runtime expects from the renderer (0.5) and from
 * the translated game, so the memory-layout test links without either. */
#include <stdint.h>

volatile unsigned g_last_loc, g_main_loc;
int g_fe_cursor;
volatile int g_suppress_present;
int fe_menu_get_pb_state(void) { return 0; }

int  pgraph_d3d11_init()                 { return 0; }
int  pgraph_d3d11_method()               { return 0; }
void pgraph_d3d11_flush()                {}
void pgraph_d3d11_present()              {}
void pgraph_d3d11_get_stats()            {}
void pgraph_d3d11_perf_counts()          {}
int  pgraph_d3d11_draws_pending()        { return 0; }
int  pgraph_d3d11_reports_pending()      { return 0; }
void pgraph_d3d11_set_chyron_scroll()    {}
void nv2a_live_pb_tick()                 {}
void nv2a_live_pb_capture()              {}
void nv2a_live_pb_note_ring()            {}
void d3d8_SetDacPalette()                {}
int  d3d8_pump_alt_off()                 { return 0; }

/* Translated-game entry points and low-memory access logging (recomp_types.h). */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va)        { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
int g_lowacc_enabled;
void xbox_lowaccess_log() {}
