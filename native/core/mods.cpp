#include "mods.h"
#include "json.h"
#include "paths.h"
#include "util.h"
#include <stdio.h>

namespace rn {
namespace mods {
namespace {

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// a mod's object: {"on": bool, "value": ...}
yyjson_val* mod(yyjson_val* root, const char* key) {
    yyjson_val* v = yyjson_obj_get(root, key);
    return yyjson_is_obj(v) ? v : nullptr;
}
void on(yyjson_val* root, const char* key, bool* out) {
    yyjson_val* o = mod(root, key);
    yyjson_val* v = o ? yyjson_obj_get(o, "on") : nullptr;
    if (yyjson_is_bool(v)) *out = yyjson_get_bool(v);
}
bool num(yyjson_val* root, const char* key, double* out) {
    yyjson_val* o = mod(root, key);
    yyjson_val* v = o ? yyjson_obj_get(o, "value") : nullptr;
    if (!yyjson_is_num(v)) return false;
    *out = yyjson_get_num(v);
    return true;
}
bool str(yyjson_val* root, const char* key, std::wstring* out) {
    yyjson_val* o = mod(root, key);
    yyjson_val* v = o ? yyjson_obj_get(o, "value") : nullptr;
    if (!yyjson_is_str(v)) return false;
    *out = json::wstr(v);
    return true;
}

} // namespace

std::wstring path() { return join(paths::launcherDir(), L"mods.json"); }

Mods load() {
    Mods m;
    std::string text;
    if (!readFile(path(), &text)) return m;
    yyjson_doc* doc = json::parse(text);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (yyjson_is_obj(root)) {
        double d;
        std::wstring s;
        on(root, "bodySliders", &m.body);
        on(root, "ragdoll", &m.ragdoll);
        on(root, "console", &m.console);
        if (str(root, "console", &s) && !s.empty() && s.size() < 24) m.consoleKey = s;
        on(root, "menu", &m.menu);
        on(root, "fov", &m.fov);
        if (num(root, "fov", &d)) m.fovValue = clampi((int)d, 60, 120);
        on(root, "fpsCap", &m.fps);
        if (num(root, "fpsCap", &d)) m.fpsValue = clampi((int)d, 0, 1000);
        on(root, "noPostEffects", &m.noPostFx);
        on(root, "slowMotion", &m.slomo);
        if (num(root, "slowMotion", &d)) m.slomoValue = clampf((float)d, 0.1f, 1.0f);
        on(root, "fly", &m.fly);
        if (str(root, "fly", &s)) m.ghost = s == L"ghost";
        on(root, "aimAssist", &m.aim);
        if (num(root, "aimAssist", &d)) m.aimStrength = clampi((int)d, 1, 10);
        if (num(root, "aimCone", &d)) m.aimCone = clampi((int)d, 5, 180);
        on(root, "god", &m.god);
        on(root, "cheatKeys", &m.cheatKeys);
        if (num(root, "spawnBots", &d)) m.spawnCount = clampi((int)d, 1, 20);
        yyjson_val* sb = mod(root, "spawnBots");
        yyjson_val* side = sb ? yyjson_obj_get(sb, "side") : nullptr;
        if (yyjson_is_str(side)) m.spawnTeam = json::wstr(side) == L"team";
    }
    if (doc) yyjson_doc_free(doc);
    return m;
}

bool save(const Mods& m) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    char num[64];
    std::string t = "{\r\n  \"version\": 1,\r\n";
    t += std::string("  \"bodySliders\": {\"on\": ") + b(m.body) + "},\r\n";
    t += std::string("  \"ragdoll\": {\"on\": ") + b(m.ragdoll) + "},\r\n";
    t += std::string("  \"console\": {\"on\": ") + b(m.console) + ", \"value\": " + json::strict(m.consoleKey) + "},\r\n";
    t += std::string("  \"menu\": {\"on\": ") + b(m.menu) + "},\r\n";
    snprintf(num, sizeof num, "%d", clampi(m.fovValue, 60, 120));
    t += std::string("  \"fov\": {\"on\": ") + b(m.fov) + ", \"value\": " + num + "},\r\n";
    snprintf(num, sizeof num, "%d", clampi(m.fpsValue, 0, 1000));
    t += std::string("  \"fpsCap\": {\"on\": ") + b(m.fps) + ", \"value\": " + num + "},\r\n";
    t += std::string("  \"noPostEffects\": {\"on\": ") + b(m.noPostFx) + "},\r\n";
    snprintf(num, sizeof num, "%.2f", clampf(m.slomoValue, 0.1f, 1.0f));
    t += std::string("  \"slowMotion\": {\"on\": ") + b(m.slomo) + ", \"value\": " + num + "},\r\n";
    t += std::string("  \"fly\": {\"on\": ") + b(m.fly) + ", \"value\": \"" + (m.ghost ? "ghost" : "fly") + "\"},\r\n";
    snprintf(num, sizeof num, "%d", clampi(m.aimStrength, 1, 10));
    t += std::string("  \"aimAssist\": {\"on\": ") + b(m.aim) + ", \"value\": " + num + "},\r\n";
    snprintf(num, sizeof num, "%d", clampi(m.aimCone, 5, 180));
    t += std::string("  \"aimCone\": {\"value\": ") + num + "},\r\n";
    t += std::string("  \"god\": {\"on\": ") + b(m.god) + "},\r\n";
    t += std::string("  \"cheatKeys\": {\"on\": ") + b(m.cheatKeys) + "},\r\n";
    snprintf(num, sizeof num, "%d", clampi(m.spawnCount, 1, 20));
    t += std::string("  \"spawnBots\": {\"value\": ") + num + ", \"side\": \"" + (m.spawnTeam ? "team" : "enemy") + "\"}\r\n}\r\n";
    // the game reads the file whenever it changes: it must never see half of it
    std::wstring file = path(), tmp = file + L".tmp";
    if (!writeFile(tmp, t)) return false;
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

} // namespace mods
} // namespace rn
