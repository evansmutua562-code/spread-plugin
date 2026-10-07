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
    virtual Vec GetAbsAngles() = 0;
    virtual Vec GetPlayerMins() = 0;
    virtual Vec GetPlayerMaxs() = 0;
    virtual const char* GetWeaponName() = 0;
    virtual const char* GetModelName() = 0;
    virtual int GetHealth() = 0;
    virtual int GetMaxHealth() = 0;
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

static const int MAXP = 1200;
static Vec g_path[MAXP];
static int g_np = 0;
static int g_state = 0;
static void* g_human = 0;
static void* g_bot = 0;
static Vec g_start, g_pos;
static bool g_moved = false;
static double g_lastSample = 0, g_stillSince = 0, g_lastT = 0, g_lastLog = 0, g_holdStart = 0;
static int g_idx = 0;
static int g_humanTeam = 0;
static int g_lastHp = -1;

static void AddEnt(void* e) {
    for (int i = 0; i < g_count; i++) if (g_ents[i] == e) return;
    if (g_count < 64) g_ents[g_count++] = e;
}

static void RemoveEnt(void* e) {
    for (int i = 0; i < g_count; i++) {
        if (g_ents[i] == e) { g_ents[i] = g_ents[--g_count]; return; }
    }
}

static IPlayerInfo* Info(void* e) {
    return (e && g_pim) ? g_pim->GetPlayerInfo(e) : 0;
}

static void Census() {
    Log("--- census (%d clients) ---", g_count);
    if (!g_pim) { Log("PlayerInfoManager not available"); return; }
    for (int i = 0; i < g_count; i++) {
        IPlayerInfo* p = Info(g_ents[i]);
        if (!p) { Log("  slot %d: no info", i); continue; }
        Log("  %s | team %d | %s | %s | hp %d", p->GetName(), p->GetTeamIndex(),
            p->IsFakeClient() ? "BOT" : "HUMAN", p->IsDead() ? "dead" : "alive", p->GetHealth());
    }
}

static void TrackHp(IPlayerInfo* b) {
    int hp = b->GetHealth();
    if (hp != g_lastHp) {
        Vec a = b->GetAbsOrigin();
        Log("HP: %d -> %d | route point %d/%d | actual (%.0f %.0f %.0f) | ours (%.0f %.0f %.0f)",
            g_lastHp, hp, g_idx, g_np, a.x, a.y, a.z, g_pos.x, g_pos.y, g_pos.z);
        g_lastHp = hp;
    }
}

static void PathTest(double t) {
    if (!g_pim || !g_bm || g_state >= 4) return;

    if (g_state == 0) {
        if (t - g_t0 < 5.0) return;
        for (int i = 0; i < g_count; i++) {
            IPlayerInfo* p = Info(g_ents[i]);
            if (p && !p->IsFakeClient() && !p->IsDead() && p->GetTeamIndex() >= 2) {
                g_human = g_ents[i];
                g_start = p->GetAbsOrigin();
                g_np = 0;
                g_moved = false;
                g_lastSample = t;
                g_stillSince = t;
                g_state = 1;
                Log("PATH: recording started. Walk your route and stop at the hold spot for 3 seconds.");
                return;
            }
        }
        return;
    }

    if (g_state == 1) {
        if (t - g_lastSample < 0.25) return;
        g_lastSample = t;
        IPlayerInfo* h = Info(g_human);
        if (!h || h->IsDead()) { Log("PATH: you died or left, recording aborted"); g_state = 4; return; }
        Vec pos = h->GetAbsOrigin();
        if (!g_moved) {
            if (Dist(pos, g_start) < 60) return;
            g_moved = true;
            g_path[g_np++] = g_start;
            g_stillSince = t;
        }
        Vec last = g_path[g_np - 1];
        if (Dist(pos, last) >= 20) {
            if (g_np < MAXP) g_path[g_np++] = pos;
            g_stillSince = t;
        } else if (t - g_stillSince >= 3.0 && Dist(pos, g_path[0]) >= 500 && g_np >= 20) {
            g_humanTeam = h->GetTeamIndex();
            Log("PATH: recorded %d points, ending at (%.0f %.0f %.0f)", g_np, pos.x, pos.y, pos.z);
            g_state = 2; g_bot = 0; g_lastT = t; g_lastLog = t;
            return;
        }
        if (g_np >= MAXP) {
            g_humanTeam = h->GetTeamIndex();
            Log("PATH: path buffer full, using %d points", g_np);
            g_state = 2; g_bot = 0; g_lastT = t; g_lastLog = t;
        }
        return;
    }

    if (g_state == 2) {
        if (!g_bot) {
            for (int i = 0; i < g_count; i++) {
                IPlayerInfo* p = Info(g_ents[i]);
                if (p && p->IsFakeClient() && !p->IsDead() && p->GetTeamIndex() == g_humanTeam) {
                    IBotController* bc0 = g_bm->GetBotController(g_ents[i]);
                    if (!bc0) { Log("PATH: GetBotController returned null"); g_state = 4; return; }
                    g_bot = g_ents[i];
                    Vec was = p->GetAbsOrigin();
                    g_pos = g_path[0];
                    bc0->SetAbsOrigin(g_pos);
                    g_idx = 1;
                    g_lastT = t;
                    g_lastLog = t;
                    g_lastHp = -1;
                    Log("PATH: bot %s jumped from (%.0f %.0f %.0f) to route start (%.0f %.0f %.0f), hp %d",
                        p->GetName(), was.x, was.y, was.z, g_pos.x, g_pos.y, g_pos.z, p->GetHealth());
                    break;
                }
            }
            if (!g_bot) {
                if (t - g_lastLog >= 5.0) { Log("PATH: no living bot on your team, waiting"); g_lastLog = t; }
                return;
            }
        }
        IPlayerInfo* b = Info(g_bot);
        if (!b || b->IsDead()) {
            Log("PATH: bot died or left at route point %d of %d, last hp %d, last pos (%.0f %.0f %.0f)",
                g_idx, g_np, g_lastHp, g_pos.x, g_pos.y, g_pos.z);
            g_state = 4; return;
        }
        TrackHp(b);
        IBotController* bc = g_bm->GetBotController(g_bot);
        if (!bc) { Log("PATH: GetBotController returned null"); g_state = 4; return; }
        double dt = t - g_lastT;
        g_lastT = t;
        if (dt > 0.2) dt = 0.2;
        float budget = 250.0f * (float)dt;
        while (budget > 0 && g_idx < g_np) {
            Vec tgt = g_path[g_idx];
            float d = Dist(g_pos, tgt);
            if (d <= budget) { g_pos = tgt; budget -= d; g_idx++; }
            else {
                float f = budget / d;
                g_pos.x += (tgt.x - g_pos.x) * f;
                g_pos.y += (tgt.y - g_pos.y) * f;
                g_pos.z += (tgt.z - g_pos.z) * f;
                budget = 0;
            }
        }
        bc->SetAbsOrigin(g_pos);
        if (t - g_lastLog >= 2.0) {
            Log("PATH: replay point %d/%d, bot at (%.0f %.0f %.0f)", g_idx, g_np, g_pos.x, g_pos.y, g_pos.z);
            g_lastLog = t;
        }
        if (g_idx >= g_np) { Log("PATH: arrived, holding 15s"); g_holdStart = t; g_state = 3; }
        return;
    }

    if (g_state == 3) {
        IPlayerInfo* b = Info(g_bot);
        if (!b || b->IsDead()) { Log("PATH: bot died while holding, last hp %d", g_lastHp); g_state = 4; return; }
        TrackHp(b);
        IBotController* bc = g_bm->GetBotController(g_bot);
        if (bc) bc->SetAbsOrigin(g_pos);
        if (t - g_holdStart >= 15.0) { Log("PATH: released"); g_state = 4; }
    }
}

class SpreadPlugin {
public:
    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn gameFactory) {
        Log("=== Spread v0.5 loaded ===");
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
    virtual const char* GetPluginDescription() { return "Spread v0.5"; }
    virtual void LevelInit(const char* map) {
        Log("map: %s", map ? map : "?");
        g_state = 0; g_np = 0; g_human = 0; g_bot = 0; g_firstFrame = true; g_lastHp = -1;
    }
    virtual void ServerActivate(void*, int, int maxc) { Log("server activate, maxclients %d", maxc); }
    virtual void GameFrame(bool) {
        double t = Now();
        if (g_firstFrame) { g_firstFrame = false; g_t0 = t; Log("GameFrame is running"); }
        PathTest(t);
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
