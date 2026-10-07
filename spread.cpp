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

struct Pt { Vec p; float t; };

static IPlayerInfoManager* g_pim = 0;
static IBotManager* g_bm = 0;
static void* g_ents[64];
static int g_count = 0;
static double g_last = 0;
static bool g_firstFrame = true;
static double g_t0 = 0;
static char g_map[64] = "unknown";

static const int MAXP = 1200;
static Pt g_path[MAXP];
static int g_np = 0;
static int g_endIdx = 1;
static int g_state = 0;   // 0 wait, 1 record, 2 replay, 3 hold, 4 done
static void* g_human = 0;
static void* g_bot = 0;
static Vec g_start, g_prev, g_pos, g_endAng;
static bool g_moved = false;
static double g_lastSample = 0, g_stillSince = 0, g_recT0 = 0, g_lastLog = 0, g_holdStart = 0, g_rs = 0;
static int g_idx = 0;
static int g_humanTeam = 2;
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

static void RoutePath(char* buf, int n) {
    snprintf(buf, n, "/storage/emulated/0/counter strike/spread_route_%s.txt", g_map);
}

static void SaveRoute() {
    char fn[256]; RoutePath(fn, sizeof(fn));
    FILE* f = fopen(fn, "w");
    if (!f) { Log("ROUTE: cannot write %s", fn); return; }
    fprintf(f, "%d %d %.2f %.2f %.2f\n", g_np, g_humanTeam, g_endAng.x, g_endAng.y, g_endAng.z);
    for (int i = 0; i < g_np; i++)
        fprintf(f, "%.2f %.1f %.1f %.1f\n", g_path[i].t, g_path[i].p.x, g_path[i].p.y, g_path[i].p.z);
    fclose(f);
    Log("ROUTE: saved %d points to %s", g_np, fn);
}

static bool LoadRoute() {
    char fn[256]; RoutePath(fn, sizeof(fn));
    FILE* f = fopen(fn, "r");
    if (!f) return false;
    int n = 0, team = 2; float ax, ay, az;
    if (fscanf(f, "%d %d %f %f %f", &n, &team, &ax, &ay, &az) != 5 || n < 3 || n > MAXP) {
        fclose(f); Log("ROUTE: bad header in %s", fn); return false;
    }
    for (int i = 0; i < n; i++) {
        if (fscanf(f, "%f %f %f %f", &g_path[i].t, &g_path[i].p.x, &g_path[i].p.y, &g_path[i].p.z) != 4) {
            fclose(f); Log("ROUTE: bad data at line %d", i + 2); return false;
        }
    }
    fclose(f);
    g_np = n; g_humanTeam = team;
    g_endAng.x = ax; g_endAng.y = ay; g_endAng.z = az;
    Log("ROUTE: loaded %d points for team %d from %s", n, team, fn);
    return true;
}

static int ComputeEnd() {
    int e = g_np - 1;
    while (e > 1 && Dist(g_path[e].p, g_path[g_np - 1].p) < 70.0f) e--;
    return e;
}

static void Census() {
    Log("--- census (%d clients) ---", g_count);
    if (!g_pim) { Log("PlayerInfoManager not available"); return; }
    for (int i = 0; i < g_count; i++) {
        IPlayerInfo* p = Info(g_ents[i]);
        if (!p) { Log("  slot %d: no info", i); continue; }
        Log("  %s | team %d | %s | %s | hp %d | %s", p->GetName(), p->GetTeamIndex(),
            p->IsFakeClient() ? "BOT" : "HUMAN", p->IsDead() ? "dead" : "alive",
            p->GetHealth(), p->GetWeaponName() ? p->GetWeaponName() : "?");
    }
}

static void TrackHp(IPlayerInfo* b) {
    int hp = b->GetHealth();
    if (hp != g_lastHp) {
        Vec a = b->GetAbsOrigin();
        Log("HP: %d -> %d | route point %d/%d | actual (%.0f %.0f %.0f)",
            g_lastHp, hp, g_idx, g_np, a.x, a.y, a.z);
        g_lastHp = hp;
    }
}

static void FinishRecording(IPlayerInfo* h) {
    if (g_np > 24) g_np -= 12;   // drop the still seconds at the end
    g_humanTeam = h->GetTeamIndex();
    g_endAng = h->GetAbsAngles();
    Log("PATH: recorded %d points, %.1f seconds", g_np, g_path[g_np - 1].t);
    SaveRoute();
    g_state = 2; g_bot = 0; g_lastLog = Now();
}

static void PathTest(double t) {
    if (!g_pim || !g_bm || g_state >= 4) return;

    if (g_state == 0) {
        if (t - g_t0 < 5.0) return;
        if (LoadRoute()) { g_state = 2; g_bot = 0; g_lastLog = t; return; }
        for (int i = 0; i < g_count; i++) {
            IPlayerInfo* p = Info(g_ents[i]);
            if (p && !p->IsFakeClient() && !p->IsDead() && p->GetTeamIndex() >= 2) {
                g_human = g_ents[i];
                g_start = p->GetAbsOrigin();
                g_prev = g_start;
                g_np = 0; g_moved = false;
                g_lastSample = t; g_stillSince = t;
                g_state = 1;
                Log("PATH: no saved route, recording started. Walk your route, pause where you want him to pause, stop at the hold spot for 3 seconds.");
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
            g_recT0 = t;
            g_path[0].p = g_start; g_path[0].t = 0; g_np = 1;
            g_stillSince = t; g_prev = pos;
        }
        if (g_np < MAXP) { g_path[g_np].p = pos; g_path[g_np].t = (float)(t - g_recT0); g_np++; }
        if (Dist(pos, g_prev) >= 8.0f) g_stillSince = t;
        g_prev = pos;
        if (t - g_stillSince >= 3.0 && Dist(pos, g_path[0].p) >= 500 && g_np >= 20) { FinishRecording(h); return; }
        if (g_np >= MAXP) { Log("PATH: buffer full"); FinishRecording(h); }
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
                    g_endIdx = ComputeEnd();
                    g_pos = g_path[0].p;
                    bc0->SetAbsOrigin(g_pos);
                    g_idx = 0; g_rs = t; g_lastLog = t; g_lastHp = -1;
                    Log("PATH: bot %s jumped from (%.0f %.0f %.0f) to route start, end point %d of %d, hp %d",
                        p->GetName(), was.x, was.y, was.z, g_endIdx, g_np, p->GetHealth());
                    break;
                }
            }
            if (!g_bot) {
                if (t - g_lastLog >= 5.0) { Log("PATH: no living bot on team %d, waiting", g_humanTeam); g_lastLog = t; }
                return;
            }
        }
        IPlayerInfo* b = Info(g_bot);
        if (!b || b->IsDead()) {
            Log("PATH: bot died or left at route point %d of %d, last hp %d", g_idx, g_np, g_lastHp);
            g_state = 4; return;
        }
        TrackHp(b);
        IBotController* bc = g_bm->GetBotController(g_bot);
        if (!bc) { Log("PATH: GetBotController returned null"); g_state = 4; return; }
        float el = (float)(t - g_rs);
        if (el >= g_path[g_endIdx].t) {
            g_pos = g_path[g_endIdx].p; g_idx = g_endIdx;
            bc->SetAbsOrigin(g_pos);
            bc->SetAbsAngles(g_endAng);
            Log("PATH: arrived at (%.0f %.0f %.0f), holding 40s, facing yaw %.0f", g_pos.x, g_pos.y, g_pos.z, g_endAng.y);
            g_holdStart = t; g_state = 3;
            return;
        }
        while (g_idx + 1 <= g_endIdx && g_path[g_idx + 1].t <= el) g_idx++;
        Pt& a = g_path[g_idx];
        Pt& c = g_path[g_idx + 1];
        float span = c.t - a.t;
        float f = span > 0.001f ? (el - a.t) / span : 1.0f;
        if (f > 1.0f) f = 1.0f;
        g_pos.x = a.p.x + (c.p.x - a.p.x) * f;
        g_pos.y = a.p.y + (c.p.y - a.p.y) * f;
        g_pos.z = a.p.z + (c.p.z - a.p.z) * f;
        bc->SetAbsOrigin(g_pos);
        if (t - g_lastLog >= 2.0) {
            Log("PATH: replay %.1fs, point %d/%d, bot at (%.0f %.0f %.0f)", el, g_idx, g_endIdx, g_pos.x, g_pos.y, g_pos.z);
            g_lastLog = t;
        }
        return;
    }

    if (g_state == 3) {
        IPlayerInfo* b = Info(g_bot);
        if (!b || b->IsDead()) { Log("PATH: bot died while holding, last hp %d, weapon %s", g_lastHp, b && b->GetWeaponName() ? b->GetWeaponName() : "?"); g_state = 4; return; }
        TrackHp(b);
        IBotController* bc = g_bm->GetBotController(g_bot);
        if (bc) { bc->SetAbsOrigin(g_pos); bc->SetAbsAngles(g_endAng); }
        if (t - g_holdStart >= 40.0) { Log("PATH: released after 40s"); g_state = 4; }
    }
}

class SpreadPlugin {
public:
    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn gameFactory) {
        Log("=== Spread v0.6 loaded ===");
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
    virtual const char* GetPluginDescription() { return "Spread v0.6"; }
    virtual void LevelInit(const char* map) {
        Log("map: %s", map ? map : "?");
        snprintf(g_map, sizeof(g_map), "%s", map ? map : "unknown");
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
