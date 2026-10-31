// HeavyWeaponSprintIII.cpp - GTA III 1.0 (PC, gta3.exe)   [NOT COMPILED, NOT TESTED]
// While the player holds one of the chosen weapons (default: baseball bat, shotgun, AK47, M16, sniper rifle, rocket launcher,
// flamethrower) and holds the sprint key (keyboard or a GInput controller: GInput feeds the game's CPad), the run speed is
// multiplied by SpeedMultiplier (default 2.0, fractional values like 1.5 are fine).
//
// What was found by reading gta3.exe:
//   - CPad::GetSprint = 0x493A70 (thiscall on the pad, returns al). Pad 0 = 0x6F0360 (CPad::GetPad(0)).
//   - The ped control code skips the sprint check when (CWeaponInfo[weapon] + 0x50) & 0x200 (heavy weapons), so those weapons
//     only ever reach move state 3 (run); the sprint state is 4. CPed::m_nMoveState is at ped+0x22C.
//   - CPed::SetMoveAnim = 0x4F0880 (thiscall, no args) is called every frame from the on-foot control code (5 call sites);
//     it picks/blends the walk (anim id 0), run (id 1) and sprint (id 2) animations.
//   - The ped's movement comes from the animation, so scaling CAnimBlendAssociation::speed (+0x24) speeds up both the legs
//     and the movement. RpAnimBlendClumpGetAssociation = 0x4055C0 (cdecl: clump, animId), clump = ped+0x4C.
//   - We hook the 5 calls to SetMoveAnim: run the original, then (player only, weapon in the list, sprint key held,
//     move state run/sprint) multiply the speed of the matching animation; otherwise restore the original speed.
//   - Stamina: the boost only works while the player is not out of breath (stamina > TiredBelow). While boosting, the stamina is
//     drained (and the maximum stamina trained) exactly like the game does for a normal sprint. When the player gets out of breath
//     the boost stays off until the stamina is restored to ResumeAt.
// Configuration in HeavyWeaponSprintIII.ini (reloaded when saved). Compile as Win32 (x86), output HeavyWeaponSprintIII.asi
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

// ======================= Addresses (GTA III 1.0) =======================
static const uintptr_t VERSION_ADDR = 0x5C1E70;
static const uint32_t  VERSION_OK   = 0x53E58955;
static const uintptr_t FN_SET_MOVE_ANIM = 0x4F0880;      // CPed::SetMoveAnim (thiscall, no args)
static const uintptr_t SITES_SET_MOVE_ANIM[] = { 0x4EFD68, 0x4F01E6, 0x4F03A2, 0x4F1731, 0x4F1BFF };
static const uintptr_t FN_GET_SPRINT    = 0x493A70;      // CPad::GetSprint (thiscall, bool)
static const uintptr_t FN_GET_ASSOC     = 0x4055C0;      // RpAnimBlendClumpGetAssociation(clump, animId) (cdecl)
static const uintptr_t ADDR_PAD0        = 0x6F0360;      // CPad::Pads[0]
static const uintptr_t ADDR_PLAYERS     = 0x9412F0;      // CWorld::Players[] (stride 0x13C, ped pointer at offset 0)
static const uintptr_t ADDR_PLAYER_FOCUS= 0x95CD61;      // CWorld::PlayerInFocus (byte)
static const uintptr_t OFF_PED_CLUMP    = 0x4C;          // RpClump*
static const uintptr_t OFF_PED_MOVESTATE= 0x22C;         // 3 = run, 4 = sprint
static const uintptr_t OFF_PED_CURSLOT   = 0x498;         // signed byte: current weapon slot
static const uintptr_t OFF_PED_WEAPONS   = 0x35C;         // CWeapon[13], 0x18 each, type = first dword
static const uintptr_t OFF_ASSOC_SPEED  = 0x24;          // CAnimBlendAssociation::speed (float)
static const int ANIM_RUN = 1, ANIM_SPRINT = 2;
// Stamina (breath), the same fields the game's own sprint code uses (CPed::SetMoveAnim)
static const uintptr_t OFF_STAMINA      = 0x548;         // float: current stamina (falls while sprinting, below 0 = out of breath)
static const uintptr_t OFF_MAXSTAMINA   = 0x54C;         // float: maximum stamina (starts at 150, grows with sprinting up to 1000)
static const uintptr_t OFF_STAMPROGRESS = 0x550;         // float: sprint time accumulated towards the next max stamina increase
static const uintptr_t OFF_NODRAIN      = 0x57C;         // byte: when non-zero the game does not drain stamina
static const uintptr_t OFF_PLAYER_INFSPRINT = 0x114;  // byte inside the Players[] entry: infinite sprint cheat
static const uintptr_t ADDR_TIMESTEP    = 0x8E2CB4;         // CTimer::ms_fTimeStep (float)
static const uintptr_t ADDR_TIME_MS     = 0x885B48;         // CTimer::m_snTimeInMilliseconds (stops when paused)
static const int SPRINT_STATE = 4;                   // move state of the game's own sprint (it drains stamina by itself)
static const float STAMINA_FLOOR = -150.0f, PROGRESS_LIMIT = 500.0f, MAXSTAMINA_CAP = 1000.0f, MAXSTAMINA_STEP = 10.0f;  // the game's constants

// ======================= Configuration =======================
static char  g_iniPath[MAX_PATH] = "HeavyWeaponSprintIII.ini";
static bool  g_enabled = true;
static bool  g_logEnabled = false;
static float g_mult = 2.0f;
static float g_tiredBelow = -150.0f;   // stamina at or below this = out of breath: no boost (-150 = where the game itself ends a sprint)
static float g_resumeAt = 0.0f;        // after being out of breath, the boost comes back when stamina >= this value
static bool  g_drain = true;           // the boosted run drains stamina like a normal sprint
static bool  g_train = true;           // ... and also trains the maximum stamina like a normal sprint
static bool  g_weaponOn[16] = {};      // indexed by weapon type (0..15)
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;

static void Log(const char* fmt, ...)
{
    if (!g_logEnabled) return;
    FILE* f = fopen("HeavyWeaponSprintIII.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static float ReadF(const char* key, float def)
{
    char b[64];
    GetPrivateProfileStringA("Settings", key, "", b, sizeof(b), g_iniPath);
    if (!b[0]) return def;
    for (char* p = b; *p; ++p) if (*p == ',') *p = '.';
    return (float)atof(b);
}

static void LoadConfig()
{
    g_enabled = GetPrivateProfileIntA("Settings", "Enabled", 1, g_iniPath) != 0;
    g_logEnabled = GetPrivateProfileIntA("Settings", "EnableLog", 0, g_iniPath) != 0;
    char buf[128];
    g_mult = ReadF("SpeedMultiplier", 2.0f);
    if (!(g_mult > 0.0f)) g_mult = 1.0f;
    if (g_mult > 10.0f) g_mult = 10.0f;

    g_tiredBelow = ReadF("TiredBelow", -150.0f);
    g_resumeAt = ReadF("ResumeAt", 0.0f);
    g_drain = GetPrivateProfileIntA("Settings", "DrainStamina", 1, g_iniPath) != 0;
    g_train = GetPrivateProfileIntA("Settings", "TrainStamina", 1, g_iniPath) != 0;

    GetPrivateProfileStringA("Settings", "Weapons", "1,4,5,6,7,8,9", buf, sizeof(buf), g_iniPath);
    memset(g_weaponOn, 0, sizeof(g_weaponOn));
    for (char* p = buf; *p; )
    {
        while (*p && (*p < '0' || *p > '9')) ++p;
        if (!*p) break;
        const int v = atoi(p);
        if (v >= 0 && v < 16) g_weaponOn[v] = true;
        while (*p >= '0' && *p <= '9') ++p;
    }
}

static void CheckReload()
{
    const DWORD now = GetTickCount();
    if (now - g_lastCheck < 500) return;
    g_lastCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastWrite) != 0)
    {
        g_lastWrite = fad.ftLastWriteTime;
        LoadConfig();
        Log("Configuration reloaded (SpeedMultiplier=%.2f).", g_mult);
    }
}

// ======================= Game helpers =======================
typedef void* (__cdecl *GetAssoc_t)(void* clump, int animId);
typedef uint8_t (__fastcall *GetSprint_t)(void* pad, void* edx);
typedef void (__fastcall *SetMoveAnim_t)(void* ped, void* edx);

static void* PlayerPed()
{
    const uint8_t idx = *(const uint8_t*)ADDR_PLAYER_FOCUS;
    return *(void* const*)(ADDR_PLAYERS + (uintptr_t)idx * 0x13C);
}

static SetMoveAnim_t g_origSetMoveAnim = (SetMoveAnim_t)FN_SET_MOVE_ANIM;

// Speed bookkeeping per animation (index 0 = run, 1 = sprint)
static void*  g_assoc[2]   = { NULL, NULL };   // association we modified
static float  g_origSpeed[2] = { 1.0f, 1.0f }; // its speed before we touched it
static float  g_setSpeed[2]  = { 0.0f, 0.0f }; // the speed we wrote last

static float* SpeedPtr(void* assoc) { return (float*)((uint8_t*)assoc + OFF_ASSOC_SPEED); }

static void RestoreAll(void* clump)
{
    for (int i = 0; i < 2; ++i)
    {
        if (!g_assoc[i]) continue;
        // only touch it if the same association is still alive for this ped
        void* a = clump ? ((GetAssoc_t)FN_GET_ASSOC)(clump, i == 0 ? ANIM_RUN : ANIM_SPRINT) : NULL;
        if (a == g_assoc[i] && fabsf(*SpeedPtr(a) - g_setSpeed[i]) < 0.001f) *SpeedPtr(a) = g_origSpeed[i];
        g_assoc[i] = NULL;
    }
}

static void Apply(void* clump, int idx, int animId)
{
    void* a = ((GetAssoc_t)FN_GET_ASSOC)(clump, animId);
    if (!a) return;
    float* sp = SpeedPtr(a);
    if (g_assoc[idx] != a || fabsf(*sp - g_setSpeed[idx]) > 0.001f)
    {
        g_assoc[idx] = a;
        g_origSpeed[idx] = *sp;                // new association, or the game changed the speed: take it as the base
    }
    g_setSpeed[idx] = g_origSpeed[idx] * g_mult;
    *sp = g_setSpeed[idx];
}

static bool     g_loggedActive = false;
static bool     g_tired = false;          // out of breath: stays set until the stamina is restored (hysteresis)
static bool     g_wasBoosting = false;
static float    g_lastStamina = 0.0f;     // stamina we wrote on the last boosted frame
static uint32_t g_lastDrainMs = 0;

// What the game does every frame of a normal sprint (see SetMoveAnim): stamina -= timestep, progress += timestep,
// and when progress reaches 500 the maximum stamina grows by 10 (up to 1000). The heavy-weapon run does not do this by itself.
static bool InfiniteSprint()
{
    const uint8_t idx = *(const uint8_t*)ADDR_PLAYER_FOCUS;
    return *(const uint8_t*)(ADDR_PLAYERS + (uintptr_t)idx * 0x13C + OFF_PLAYER_INFSPRINT) != 0;
}

static void DrainLikeSprint(uint8_t* ped)
{
    const uint32_t now = *(const uint32_t*)ADDR_TIME_MS;
    if (now == g_lastDrainMs) return;                  // once per game frame (the timer is frozen while paused)
    g_lastDrainMs = now;
    float* st = (float*)(ped + OFF_STAMINA);
    float* mx = (float*)(ped + OFF_MAXSTAMINA);
    float* pr = (float*)(ped + OFF_STAMPROGRESS);
    const float dt = *(const float*)ADDR_TIMESTEP;
    const bool infinite = InfiniteSprint();
    // the game also regenerates a little while running; a real sprint does not, so start from what we wrote last frame
    const float base = g_wasBoosting ? g_lastStamina : *st;
    if (base > STAMINA_FLOOR && !infinite && ped[OFF_NODRAIN] == 0)
    {
        *st = base - dt;
        if (g_train) *pr += dt;
    }
    if (g_train && *pr >= PROGRESS_LIMIT)
    {
        *pr = 0.0f;
        if (*mx < MAXSTAMINA_CAP) *mx += MAXSTAMINA_STEP;
    }
    g_lastStamina = *st;
}

static void __fastcall Hook_SetMoveAnim(void* ped, void* edx)
{
    g_origSetMoveAnim(ped, edx);
    CheckReload();
    if (!ped || ped != PlayerPed()) return;
    uint8_t* b = (uint8_t*)ped;
    void* clump = *(void**)(b + OFF_PED_CLUMP);
    if (!clump) return;

    // out of breath? (hysteresis: once tired, no boost until the stamina is restored)
    const float stamina = *(const float*)(b + OFF_STAMINA);
    const float maxStamina = *(const float*)(b + OFF_MAXSTAMINA);
    const float resumeAt = g_resumeAt;
    const bool infinite = InfiniteSprint();
    if (infinite) { if (g_tired) Log("Infinite sprint is on: never out of breath."); g_tired = false; }
    else if (!g_tired && stamina <= g_tiredBelow) { g_tired = true; Log("Out of breath (stamina=%.1f): boost off until stamina >= %.1f.", stamina, resumeAt); }
    else if (g_tired && stamina > g_tiredBelow && stamina >= resumeAt - 0.5f) { g_tired = false; Log("Breath restored (stamina=%.1f): boost available again.", stamina); }

    // diagnostics: every ~500 ms (real time) while the log is on
    static DWORD s_lastDiag = 0;
    if (g_logEnabled && GetTickCount() - s_lastDiag >= 500)
    {
        s_lastDiag = GetTickCount();
        Log("  [stamina] %.1f / max %.1f, resumeAt %.1f, tired=%d, infinite=%d, moveState=%d, boosting=%d",
            stamina, maxStamina, resumeAt, g_tired ? 1 : 0, infinite ? 1 : 0, *(const int*)(b + OFF_PED_MOVESTATE), g_wasBoosting ? 1 : 0);
    }

    bool active = false;
    int moveState = 0, weapon = 0;
    if (g_enabled)
    {
        const int slot = *(int8_t*)(b + OFF_PED_CURSLOT);                 // read the type stored in the slot (the game does the same)
        weapon = (slot >= 0 && slot < 13) ? *(int*)(b + OFF_PED_WEAPONS + (uintptr_t)slot * 0x18) : -1;
        moveState = *(int*)(b + OFF_PED_MOVESTATE);
        if (!g_tired && weapon >= 0 && weapon < 16 && g_weaponOn[weapon] && (moveState == 3 || moveState == SPRINT_STATE))
            active = ((GetSprint_t)FN_GET_SPRINT)((void*)ADDR_PAD0, NULL) != 0;
    }

    if (active)
    {
        if (moveState == SPRINT_STATE) Apply(clump, 1, ANIM_SPRINT);   // the game drains the stamina itself in this state
        else
        {
            Apply(clump, 0, ANIM_RUN);
            if (g_drain) DrainLikeSprint(b);
        }
        g_wasBoosting = true;
        if (!g_loggedActive) { g_loggedActive = true; Log("Boost ON (weapon=%d, moveState=%d, stamina=%.1f/%.1f, x%.2f).", weapon, moveState, stamina, maxStamina, g_mult); }
    }
    else
    {
        RestoreAll(clump);
        g_wasBoosting = false;
        if (g_loggedActive) { g_loggedActive = false; Log("Boost OFF (stamina=%.1f/%.1f, tired=%d).", stamina, maxStamina, g_tired ? 1 : 0); }
    }
}

// ======================= Patching =======================
static uintptr_t CallTarget(uintptr_t site)
{
    const uint8_t* p = (const uint8_t*)site;
    if (p[0] != 0xE8) return 0;
    return site + 5 + (uintptr_t)(intptr_t)(*(const int32_t*)(p + 1));
}

static uintptr_t PatchCall(uintptr_t site, void* newTarget)
{
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return 0;
    const uintptr_t orig = site + 5 + (uintptr_t)(intptr_t)(*(int32_t*)(p + 1));
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)newTarget - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return orig;
}

BOOL APIENTRY DllMain(HMODULE hm, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hm);

    GetModuleFileNameA(hm, g_iniPath, MAX_PATH);
    char* dot = strrchr(g_iniPath, '.');
    if (dot) strcpy(dot, ".ini");

    g_logEnabled = GetPrivateProfileIntA("Settings", "EnableLog", 0, g_iniPath) != 0;
    if (g_logEnabled) { FILE* f = fopen("HeavyWeaponSprintIII.log", "w"); if (f) fclose(f); }

    if (*(const uint32_t*)VERSION_ADDR != VERSION_OK)
    {
        Log("Exe version not recognized (expected GTA III 1.0). Mod disabled.");
        return TRUE;
    }

    LoadConfig();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) g_lastWrite = fad.ftLastWriteTime;

    int hooked = 0;
    for (size_t i = 0; i < sizeof(SITES_SET_MOVE_ANIM) / sizeof(SITES_SET_MOVE_ANIM[0]); ++i)
    {
        const uintptr_t s = SITES_SET_MOVE_ANIM[i];
        const uintptr_t t = CallTarget(s);
        if (!t) { Log("Site %X is not a call. Skipped.", (unsigned)s); continue; }
        if (t != FN_SET_MOVE_ANIM) Log("Site %X: target %X (expected %X) - another plugin hooked it; joining its chain.", (unsigned)s, (unsigned)t, (unsigned)FN_SET_MOVE_ANIM);
        const uintptr_t orig = PatchCall(s, (void*)&Hook_SetMoveAnim);
        if (!orig) continue;
        if (hooked == 0) g_origSetMoveAnim = (SetMoveAnim_t)orig;
        ++hooked;
    }
    Log("OK: %d of %d SetMoveAnim call sites hooked (SpeedMultiplier=%.2f, Enabled=%d).",
        hooked, (int)(sizeof(SITES_SET_MOVE_ANIM) / sizeof(SITES_SET_MOVE_ANIM[0])), g_mult, g_enabled ? 1 : 0);
    return TRUE;
}
