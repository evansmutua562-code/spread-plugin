#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

typedef void* (*CreateInterfaceFn)(const char*, int*);

static void Log(const char* fmt, ...) {
    FILE* f = fopen("/storage/emulated/0/counter strike/spread_log.txt", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static double Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

class IPlayerInfo {
public:
    virtual const char* GetName() = 0;
    virtual int GetUserID() = 0;
    virtual const char* GetNetworkIDString() = 0;
    virtual int GetTeamIndex() = 0;
    virtual void ChangeTeam(int) = 0;
    virtual int GetFragCount() = 0;
    virtual int GetDeathCount() = 0;
    virtual bool IsConnected() = 0;
    virtual int GetArmorValue() = 0;
    virtual bool IsHLTV() = 0;
    virtual bool IsPlayer() = 0;
    virtual bool IsFakeClient() = 0;
    virtual bool IsDead() = 0;
};

class IPlayerInfoManager {
public:
    virtual IPlayerInfo* GetPlayerInfo(void* edict) = 0;
    virtual void* GetGlobalVars() = 0;
};

static IPlayerInfoManager* g_pim = 0;
static void* g_ents[64];
static int g_count = 0;
static double g_last = 0;
static bool g_firstFrame = true;

static void AddEnt(void* e) {
    for (int i = 0; i < g_count; i++) if (g_ents[i] == e) return;
    if (g_count < 64) g_ents[g_count++] = e;
}

static void RemoveEnt(void* e) {
    for (int i = 0; i < g_count; i++) {
        if (g_ents[i] == e) { g_ents[i] = g_ents[--g_count]; return; }
    }
}

static void Census() {
    Log("--- census (%d clients) ---", g_count);
    if (!g_pim) { Log("PlayerInfoManager not available"); return; }
    for (int i = 0; i < g_count; i++) {
        IPlayerInfo* p = g_pim->GetPlayerInfo(g_ents[i]);
        if (!p) { Log("  slot %d: no info", i); continue; }
        Log("  %s | team %d | %s | %s", p->GetName(), p->GetTeamIndex(),
            p->IsFakeClient() ? "BOT" : "HUMAN", p->IsDead() ? "dead" : "alive");
    }
}

class SpreadPlugin {
public:
    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn gameFactory) {
        Log("=== Spread v0.2 loaded ===");
        if (gameFactory) g_pim = (IPlayerInfoManager*)gameFactory("PlayerInfoManager002", 0);
        Log("PlayerInfoManager: %s", g_pim ? "found" : "NOT found");
        return true;
    }
    virtual void Unload() { Log("Unload called"); g_pim = 0; g_count = 0; }
    virtual void Pause() {}
    virtual void UnPause() {}
    virtual const char* GetPluginDescription() { return "Spread v0.2"; }
    virtual void LevelInit(const char* map) { Log("map: %s", map ? map : "?"); }
    virtual void ServerActivate(void*, int, int maxc) { Log("server activate, maxclients %d", maxc); }
    virtual void GameFrame(bool) {
        if (g_firstFrame) { g_firstFrame = false; Log("GameFrame is running"); }
        double t = Now();
        if (t - g_last < 15.0) return;
        g_last = t;
        Census();
    }
    virtual void LevelShutdown() { Log("level shutdown"); g_count = 0; }
    virtual void ClientActive(void* e) { AddEnt(e); }
    virtual void ClientDisconnect(void* e) { RemoveEnt(e); }
    virtual void ClientPutInServer(void* e, const char* name) {
        AddEnt(e);
        Log("join: %s", name ? name : "?");
    }
    virtual void SetCommandClient(int) {}
    virtual void ClientSettingsChanged(void*) {}
    virtual int ClientConnect(bool*, void*, const char*, const char*, char*, int) { return 0; }
    virtual int ClientCommand(void*, const void*) { return 0; }
    virtual int NetworkIDValidated(const char*, const char*) { return 0; }
    virtual void OnQueryCvarValueFinished(int, void*, int, const char*, const char*) {}
    virtual void OnEdictAllocated(void*) {}
    virtual void OnEdictFreed(const void*) {}
};

static SpreadPlugin g_plugin;

__attribute__((constructor)) static void OnLoad() { Log("library loaded"); }

extern "C" __attribute__((visibility("default")))
void* CreateInterface(const char* name, int* rc) {
    if (name && strncmp(name, "ISERVERPLUGINCALLBACKS", 22) == 0) {
        Log("engine asked for %s", name);
        if (rc) *rc = 0;
        return &g_plugin;
    }
    if (rc) *rc = 1;
    return 0;
}
