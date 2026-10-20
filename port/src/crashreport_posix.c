/* crashreport_posix.c -- crashreport.h for hosts without the Windows crash reporter
 * (dbghelp minidumps, the report folder, the crash dialog). Headless bring-up relies
 * on the platform fault handler (xboxrecomp/src/platform/mmio_trap.c) and the
 * system's own crash report; nothing is written here. */
#include <stdio.h>
#include "crashreport.h"

void crashreport_install(void) {}
void crashreport_set_output(int player, const char *log_path, int output_redirected)
{ (void)player; (void)log_path; (void)output_redirected; }
void crashreport_game_start(void) {}
void crashreport_private_folder(const char *utf8) { (void)utf8; }
void crashreport_private_file_acp(const char *path) { (void)path; }
int crashreport_on_crash(EXCEPTION_POINTERS *ep, const char *details, wchar_t *dir_out, size_t dir_out_len)
{
    (void)ep;
    if (details) fprintf(stderr, "[crash] %s\n", details);
    if (dir_out && dir_out_len) dir_out[0] = 0;
    return 0;
}
void crashreport_fatal_overflow(EXCEPTION_POINTERS *ep) { (void)ep; }
void crashreport_crash_dialog(DWORD code, const wchar_t *dir) { (void)dir; fprintf(stderr, "[crash] exception 0x%08X\n", (unsigned)code); }
void crashreport_open_folder(const wchar_t *dir) { (void)dir; }
