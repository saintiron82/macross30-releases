/* Synthetic filesystem/guest-memory check; no game, renderer or original data. */
#include "../cellGame.c"
#include <assert.h>

static uint8_t test_guest[8192];
uint8_t* vm_base = test_guest;
ps3_guest_caller_fn g_ps3_guest_caller = NULL;
const char* ppu_vfs_root = ".";
_Static_assert(CELL_GAME_PATH_MAX == 128, "Keep PS3 guest output buffer ABI");
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; abort(); }

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const char* root = argv[1];
    const char* mode = argv[2];
    _putenv_s("PS3_HDD0_GAME_ROOT", root);
    _putenv_s("PS3_HDD0_ROOT", "");
    strcpy((char*)vm_base + 0x100, "BLJS10184_INSTALL");
    const char* name = (const char*)(uintptr_t)0x100;
    if (!strcmp(mode, "fallback")) {
        _putenv_s("PS3_HDD0_GAME_ROOT", "");
        _putenv_s("PS3_HDD0_ROOT", root);
    }
    if (!strcmp(mode, "vfs-fallback")) {
        _putenv_s("PS3_HDD0_GAME_ROOT", "");
        ppu_vfs_root = root;
    }
    s32 result = cellGameDataCheck(CELL_GAME_GAMETYPE_GAMEDATA, name, NULL);
    printf("TEST_RESULT=%d ROOT_LENGTH=%zu RESOLVED_LENGTH=%zu\n", result, strlen(root), strlen(content_root()));
    if (!strcmp(mode, "missing")) return result == CELL_GAME_RET_NONE ? 0 : 3;
    if (!strcmp(mode, "create")) {
        if (result != CELL_GAME_RET_NONE) return 4;
        memset(vm_base + 0x400, 0xA5, 0x200);
        if (cellGameCreateGameData((CellGameSetInitParams*)(uintptr_t)0x200,
                (char*)(uintptr_t)0x400, (char*)(uintptr_t)0x500) != CELL_OK) return 5;
        if (strcmp((char*)vm_base + 0x400, "/dev_hdd0/game/BLJS10184_INSTALL")) return 6;
        if (strcmp((char*)vm_base + 0x500, "/dev_hdd0/game/BLJS10184_INSTALL/USRDIR")) return 7;
        if (vm_base[0x400 + CELL_GAME_PATH_MAX] != 0xA5 || vm_base[0x500 + CELL_GAME_PATH_MAX] != 0xA5) return 8;
        result = cellGameDataCheck(CELL_GAME_GAMETYPE_GAMEDATA, name, NULL);
        return result == CELL_OK ? 0 : 9;
    }
    if (result != CELL_OK) return 10;
    if (strcmp(mode, "fallback") && strcmp(mode, "vfs-fallback") && strlen(content_root()) != strlen(root)) return 11;
    if (!strcmp(mode, "permit")) {
        memset(vm_base + 0x400, 0xA5, 0x200);
        cellGameContentPermit((char*)(uintptr_t)0x400, (char*)(uintptr_t)0x500);
        if (strcmp((char*)vm_base + 0x400, "/dev_hdd0/game/BLJS10184_INSTALL")) return 12;
        if (strcmp((char*)vm_base + 0x500, "/dev_hdd0/game/BLJS10184_INSTALL/USRDIR")) return 13;
        if (vm_base[0x480] != 0xA5 || vm_base[0x580] != 0xA5) return 14;
    }
    return 0;
}
