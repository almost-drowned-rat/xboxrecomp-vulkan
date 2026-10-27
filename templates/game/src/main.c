/**
 * @TITLE@ - recompiled game entry point (template: xboxrecomp templates/game).
 *
 * Fill in from docs/xbe.md (tools\m0_analyze.ps1): entry point, XDK builds, title ID.
 * Workflow and pitfalls: toolkit docs/porting_guide.md, docs/lessons_learned.md.
 * OS-specific parts (log, crash handler, paths) live in platform/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu.h"
#include "xinput_xbox.h"

#include "platform/host.h"
#include "input.h"
#if defined(@PREFIX@_VULKAN)
#include "nv2a_vk.h"
#endif

#define GAME_ENTRY_POINT 0x00000000u   /* TODO: XBE entry point (docs/xbe.md) */

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_OhciInit(void);
extern void xbe_entry_point(void);       /* generated */
extern int recomp_dispatch_init(void);   /* generated */

int main(int argc, char **argv)
{
    HostOptions opt;
    char xbe_path[HOST_PATH_MAX];
    void *xbe;
    size_t xbe_size = 0;

    if (host_init(argc, argv, &opt) != 0)
        return 1;
    printf("=== @TITLE@ - static recompilation ===\n");

    snprintf(xbe_path, sizeof(xbe_path), "%s/default.xbe", opt.game_dir);
    xbe = host_read_file(xbe_path, &xbe_size);
    if (!xbe) {
        host_fatal("Cannot read game/default.xbe - run tools\\lift.ps1 first.");
        return 1;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {    /* also sets g_xbox_title_id */
        host_fatal("Failed to map the Xbox memory layout.");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    host_debug_start();

#if defined(@PREFIX@_VULKAN)
    host_setenv_default("RECOMP_PB_EXEC", "1");     /* pushbuffer executor -> nv2a_vk */
#endif
    /* Input: the title's XPP driver on the OHCI model (LLE fallback). Check the
     * controller-port mapping of this XDK build (lessons_learned.md). */
    host_setenv_default("RECOMP_USB", "1");
    host_setenv_default("RECOMP_KEYBOARD", "1");
    game_input_init();
    if (*getenv("RECOMP_USB") != '0')
        xbox_OhciInit();

    xbox_kernel_init();
    {
        /* Storage layout (docs/storage.md). Decide TDATA after looking at what the
         * title writes there; default: saves. */
        XboxPathConfig cfg;
        xbox_path_config_default(&cfg);
        cfg.game_dir = opt.game_dir;
        cfg.save_dir = opt.save_dir;
        cfg.data_dir = opt.data_dir;
        cfg.tdata = XBOX_TDATA_SAVES;
        xbox_path_init_cfg(&cfg);
    }
    xbox_kernel_bridge_init();
#if defined(@PREFIX@_VULKAN)
    nv2a_vk_set_key_handler(xbox_InputKeyEvent);
    nv2a_vk_set_quit_handler(host_quit);
    nv2a_vk_install();
    /* TODO (M2): wrap the title's D3D fence wait in recomp_manual.c and register
     * its GPU semaphore (porting_guide.md, M2). */
#endif

    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable\n");
    xbox_WatchdogStart();

    printf("Entry point 0x%08X - starting game\n", GAME_ENTRY_POINT);
    fflush(stdout);
    xbe_entry_point();

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}
