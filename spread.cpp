#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

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

struct Vec { float x, y, z; };

static float Dist(const Vec& a, const Vec& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
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
    virtual bool IsInAVehicle() = 0;
    virtual bool IsObserver() = 0;
    virtual Vec GetAbsOrigin() = 0;
};

class IPlayerInfoManager {
public:
    virtual IPlayerInfo* GetPlayerInfo(void* edict) = 0;
    virtual void* GetGlobalVars() = 0;
};

class IBotController {
public:
    virtual ~IBotController() {}
    virtual void SetAbsOrigin(const Vec&) = 0;
    virtual void SetAbsAngles(const Vec&) = 0;
    virtual void SetLocalAngles(const Vec&) = 0;
    virtual void RemoveAllItems(bool) = 0;
    virtual void SetActiveWeapon(const char*) = 0;
    virtual bool IsEFlagSet(int) = 0;
    virtual void RunPlayerMove(void*) = 0;
    virtual void SetLastUserCommand(void*) = 0;
};

class IBotManager {
public:
    virtual IBotController* GetBotController(void* edict) = 0;
    virtual void* CreateBot(const char*) = 0;
};

static IPlayerInfoManager* g_pim = 0;
static IBotManager* g_bm = 0;
static void* g_ents[64];
static int g_count = 0;
static double g_last = 0;
static bool g_firstFrame = true;
static double g_t0 = 0;

// steering test state
static int g_steerState = 0;   // 0 wait, 1 holding, 2 released/watching, 3 done
static void* g_target = 0;
static Vec g_p0;
static double g_t1 = 0;
static float g_maxDrift = 0;
static int g_frames = 0;

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

static void SteerTest(double t) {
    if (!g_pim || !g_bm || g_steerState == 3) return;

    if (g_steerState == 0) {
        if (t - g_t0 < 25.0) return;
        for (int i = 0; i < g_count; i++) {
            IPlayerInfo* p = g_pim->GetPlayerInfo(g_ents[i]);
            if (p && p->IsFakeClient() && p->GetTeamIndex() == 2 && !p->IsDead()) {
                g_target = g_ents[i];
                g_p0 = p->GetAbsOrigin();
                g_t1 = t;
                g_maxDrift = 0;
                g_frames = 0;
                g_steerState = 1;
                Log("STEER: target %s at (%.1f %.1f %.1f) - holding 8s",
                    p->GetName(), g_p0.x, g_p0.y, g_p0.z);
                return;
            }
        }
        return;
    }

    IPlayerInfo* p = g_pim->GetPlayerInfo(g_target);
    if (!p || p->IsDead()) { Log("STEER: target gone/dead, aborting"); g_steerState = 3; return; }

    if (g_steerState == 1) {
        Vec cur = p->GetAbsOrigin();
        float d = Dist(cur, g_p0);
        if (d > g_maxDrift) g_maxDrift = d;
        IBotController* bc = g_bm->GetBotController(g_target);
        if (!bc) { Log("STEER: GetBotController returned null"); g_steerState = 3; return; }
        bc->SetAbsOrigin(g_p0);
        g_frames++;
        if (t - g_t1 >= 8.0) {
            Vec after = p->GetAbsOrigin();
            Log("STEER: hold over. frames %d, max drift before reset %.1f, final dist from hold point %.1f",
                g_frames, g_maxDrift, Dist(after, g_p0));
            g_t1 = t;
            g_steerState = 2;
        }
        return;
    }

    if (g_steerState == 2) {
        if (t - g_t1 >= 4.0) {
            Vec now = p->GetAbsOrigin();
            Log("STEER: 4s after release, bot is %.1f units from hold point", Dist(now, g_p0));
            g_steerState = 3;
        }
    }
}

class SpreadPlugin {
public:
    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn gameFactory) {
        Log("=== Spread v0.3 loaded ===");
        if (gameFactory) {
            g_pim = (IPlayerInfoManager*)gameFactory("PlayerInfoManager002", 0);
            g_bm = (IBotManager*)gameFactory("BotManager001", 0);
        }
        Log("PlayerInfoManager: %s", g_pim ? "found" : "NOT found");
        Log("BotManager: %s", g_bm ? "found" : "NOT found");
        return true;
    }
    virtual void Unload() { Log("Unload called"); g_pim = 0; g_bm = 0; g_count = 0; }
    virtual void Pause() {}
    virtual void UnPause() {}
    virtual const char* GetPluginDescription() { return "Spread v0.3"; }
    virtual void LevelInit(const char* map) {
        Log("map: %s", map ? map : "?");
        g_steerState = 0; g_target = 0; g_firstFrame = true;
    }
    virtual void ServerActivate(void*, int, int maxc) { Log("server activate, maxclients %d", maxc); }
    virtual void GameFrame(bool) {
        double t = Now();
        if (g_firstFrame) { g_firstFrame = false; g_t0 = t; Log("GameFrame is running"); }
        SteerTest(t);
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
