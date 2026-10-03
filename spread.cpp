#include <stdio.h>
#include <string.h>

typedef void* (*CreateInterfaceFn)(const char*, int*);

static void Log(const char* a, const char* b = "") {
    FILE* f = fopen("/storage/emulated/0/counter strike/spread_log.txt", "a");
    if (f) { fprintf(f, "%s %s\n", a, b); fclose(f); }
}

#define SLOT(n) virtual int S##n() { return 0; }

class SpreadPlugin {
public:
    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn) { Log("Load called"); return true; }
    virtual void Unload() { Log("Unload called"); }
    virtual void Pause() {}
    virtual void UnPause() {}
    virtual const char* GetPluginDescription() { return "Spread test v0.1"; }
    SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12)
    SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20)
    SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28)
};

static SpreadPlugin g_plugin;

__attribute__((constructor)) static void OnLoad() { Log("library loaded"); }

extern "C" __attribute__((visibility("default")))
void* CreateInterface(const char* name, int* rc) {
    if (name && strncmp(name, "ISERVERPLUGINCALLBACKS", 22) == 0) {
        Log("engine asked for", name);
        if (rc) *rc = 0;
        return &g_plugin;
    }
    if (rc) *rc = 1;
    return 0;
}
