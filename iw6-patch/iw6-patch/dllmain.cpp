#include "pch.h"
#include "MinHook.h"
#include <cstdio>

static const uintptr_t BD_LOGGER_RVA = 0x6F54D0;
static const uintptr_t DISPATCH_FUNC_RVA = 0x4E6DA0;
static const uintptr_t FUNC_4E7210_RVA = 0x4E7210;
static const uintptr_t COM_ERROR_RVA = 0x412740;

//crash on launch, bd_logger tries to log stuff to Demonware and just dies
//we nuke it with a RET so it does nothing and bails instantly, thanks iw6-mod : https://github.com/CBServers/iw6-mod
static void Patch_BD_Logger()
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    uintptr_t target = base + BD_LOGGER_RVA;
    DWORD oldProtect;
    if (!VirtualProtect((void*)target, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) return;
    unsigned char* p = (unsigned char*)target;
    p[0] = 0xC3; // RET 
    DWORD tmp;
    VirtualProtect((void*)target, 16, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 16);
}

// crash when loading a map, real name is StructuredData_GetLookupState (thanks the xbox one pdb lol). basically reads a
// pointer from the struct and dereferences it without checking anything, so
// when a stats table is empty it just yeets into garbage mem and crashes. we just sanity check the pointer before touching it and
// bail with a safe default if it's junk. gets called from LiveStorage_BuildExtendedLoadoutDefaults (sub_1403FB690) that's the caller that actually triggers this during map load

typedef int(__fastcall* sub_1404E6DA0_t)(void* rcx);
static sub_1404E6DA0_t original_sub_1404E6DA0 = nullptr;

static int __fastcall hk_sub_1404E6DA0(void* rcx)
{
    if (!rcx) return 1;

    DWORD flags = *(DWORD*)((uintptr_t)rcx + 0x14);
    if (flags != 0) return 2;

    uintptr_t rax = *(uintptr_t*)((uintptr_t)rcx + 0x8);

    if (!rax || IsBadReadPtr((void*)rax, sizeof(DWORD)))
    {
        return 1;
    }

    return original_sub_1404E6DA0(rcx);
}

// same deal, different function, same family, same fix
typedef int(__fastcall* sub_1404E7210_t)(void* a1, unsigned int a2);
static sub_1404E7210_t original_sub_1404E7210 = nullptr;

static int __fastcall hk_sub_1404E7210(void* a1, unsigned int a2)
{
    if (!a1) return 0;

    uintptr_t v3 = *(uintptr_t*)((uintptr_t)a1 + 0x8);

    // pointer's dead, bail out clean
    if (!v3 || IsBadReadPtr((void*)v3, sizeof(int)))
    {
        *(DWORD*)((uintptr_t)a1 + 0x14) = 0; // force flag back to 0
        return 0;
    }

    int res = original_sub_1404E7210(a1, a2);

    // if it raised the error flag, just quietly clear it
    if (*(DWORD*)((uintptr_t)a1 + 0x14) == 2)
    {
        *(DWORD*)((uintptr_t)a1 + 0x14) = 0;
    }

    return res;
}

// this one blocks you with a popup "Invalid structure definition... stats group etc..."
// whenever a stats table is empty. it's harmless, game runs fine without it,
// but you’d get kicked out of the game at the OnPlayerConnect stage lmao so we just filter that specific message out and let everything else through normally
typedef void(__cdecl* com_error_t)(int code, const char* message, ...);
static com_error_t original_com_error = nullptr;

void __cdecl hk_com_error(int code, const char* message, ...)
{
    if (!message)
    {
        original_com_error(code, "");
        return;
    }

    // ignore the "stats group of type X" spam, doesn't matter
    if (strstr(message, "stats group of type"))
        return;

    va_list args;
    va_start(args, message);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), message, args);
    va_end(args);
    original_com_error(code, buf);
}

static void InitializeHooks()
{
    if (MH_Initialize() != MH_OK)
    {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);

    void* dispatch_target = (void*)(base + DISPATCH_FUNC_RVA);
    if (MH_CreateHook(dispatch_target, &hk_sub_1404E6DA0, (void**)&original_sub_1404E6DA0) == MH_OK)
    {
        MH_EnableHook(dispatch_target);
    }

    void* target_4E7210 = (void*)(base + FUNC_4E7210_RVA);
    if (MH_CreateHook(target_4E7210, &hk_sub_1404E7210, (void**)&original_sub_1404E7210) == MH_OK)
    {
        MH_EnableHook(target_4E7210);
    }

    void* com_error_target = (void*)(base + COM_ERROR_RVA);
    if (MH_CreateHook(com_error_target, &hk_com_error, (void**)&original_com_error) == MH_OK)
    {
        MH_EnableHook(com_error_target);
    }
}

static DWORD WINAPI patch_thread(LPVOID)
{
    Sleep(200);
    Patch_BD_Logger();
    InitializeHooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, patch_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}