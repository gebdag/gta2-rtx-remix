#include "settings.h"

#include "frame_limiter.h"
#include "live_geometry.h"
#include "log.h"
#include "remix_lights.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"

#include "../../src/renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gta2dx9 {
namespace {

char g_path[MAX_PATH] = "gta2dx9_settings.ini";
bool g_dirty = false;

// One table, walked in both directions. Adding a setting means adding a line
// here and nothing else; a save/load pair that can disagree is the usual way
// these files rot.
struct FloatField {
    const char* section;
    const char* key;
    float* value;
};
struct BoolField {
    const char* section;
    const char* key;
    bool* value;
};
struct IntField {
    const char* section;
    const char* key;
    int* value;
};

// Built fresh each call: the settings live in their owning modules, and taking
// pointers once at startup would break the moment one of them reallocated.
void Fields(FloatField** floats, int* floatCount, BoolField** bools, int* boolCount,
            IntField** ints, int* intCount) {
    static FloatField f[64];
    static BoolField b[32];
    static IntField i[32];
    int nf = 0, nb = 0, ni = 0;

    RemixLightSettings& ls = LightsSettings();
    b[nb++] = {"lights", "Enabled", &ls.enabled};
    b[nb++] = {"lights", "InjectStatic", &ls.injectStatic};
    b[nb++] = {"lights", "InjectMoving", &ls.injectMoving};
    b[nb++] = {"lights", "AmbientFill", &ls.ambientFill};
    f[nf++] = {"lights", "Brightness", &ls.radianceScale};
    f[nf++] = {"lights", "MovingScale", &ls.movingScale};
    f[nf++] = {"lights", "ReferenceRadius", &ls.referenceRadius};
    f[nf++] = {"lights", "RadiusExponent", &ls.radiusExponent};
    f[nf++] = {"lights", "Saturation", &ls.saturation};
    f[nf++] = {"lights", "EmitterRadius", &ls.emitterRadius};
    f[nf++] = {"lights", "MinIntensity", &ls.minIntensity};
    f[nf++] = {"lights", "AmbientFillScale", &ls.ambientFillScale};
    f[nf++] = {"lights", "ConeSoftness", &ls.coneSoftness};

    for (int c = 0; c < kGameLightClassCount; ++c) {
        // The section name is the class name, so the file reads as
        // [maplights.Street] Enabled / DaylightGate / GateOffAbove / GateOnBelow.
        static char sections[kGameLightClassCount][32];
        _snprintf(sections[c], sizeof(sections[c]) - 1, "maplights.%s", GameLightClassName(c));
        sections[c][sizeof(sections[c]) - 1] = '\0';
        GameLightClassSettings& gc = ls.gameClass[c];
        b[nb++] = {sections[c], "Enabled", &gc.enabled};
        b[nb++] = {sections[c], "DaylightGate", &gc.gate.enabled};
        f[nf++] = {sections[c], "GateOffAbove", &gc.gate.offAboveDeg};
        f[nf++] = {sections[c], "GateOnBelow", &gc.gate.onBelowDeg};
    }

    TimeOfDaySettings& tod = TimeOfDay();
    b[nb++] = {"timeofday", "Enabled", &tod.enabled};
    b[nb++] = {"timeofday", "Paused", &tod.paused};
    b[nb++] = {"timeofday", "RotationClockwise", &tod.rotationClockwise};
    f[nf++] = {"timeofday", "StartHour", &tod.startHour};
    f[nf++] = {"timeofday", "MinutesPerSecond", &tod.minutesPerSecond};
    f[nf++] = {"timeofday", "Latitude", &tod.latitudeDeg};
    f[nf++] = {"timeofday", "Declination", &tod.declinationDeg};
    f[nf++] = {"timeofday", "RotationOffset", &tod.rotationOffsetDeg};
    f[nf++] = {"timeofday", "ElevationOffset", &tod.elevationOffsetDeg};
    f[nf++] = {"timeofday", "PushIntervalMs", &tod.pushIntervalMs};

    EffectSpriteSettings& fx = EffectSprites();
    f[nf++] = {"effectsprites", "EdgeLuma", &fx.edgeLuma};
    f[nf++] = {"effectsprites", "FaintLuma", &fx.faintLuma};
    f[nf++] = {"effectsprites", "CutoutGain", &fx.cutoutGain};

    *floats = f;
    *floatCount = nf;
    *bools = b;
    *boolCount = nb;
    *ints = i;
    *intCount = ni;
}

char* Trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

}  // namespace

void SettingsInit(const char* path) {
    if (path && *path) {
        strncpy(g_path, path, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = '\0';
    }
}

const char* SettingsPath() { return g_path; }
bool SettingsDirty() { return g_dirty; }
void SettingsMarkDirty() { g_dirty = true; }

void SettingsSaveAll() {
    FloatField* f;
    BoolField* b;
    IntField* i;
    int nf, nb, ni;
    Fields(&f, &nf, &b, &nb, &i, &ni);

    FILE* out = fopen(g_path, "w");
    if (!out) {
        Log("settings: could not write %s", g_path);
        return;
    }
    fprintf(out, "; GTA2 RTX Remix -- everything the F4 menu can change.\n");
    fprintf(out, ";\n");
    fprintf(out, "; Written by the Save button, read at startup. Separate from gta2dx9.ini on\n");
    fprintf(out, "; purpose: deploy regenerates that one on every build, so anything saved into\n");
    fprintf(out, "; it would be lost. Where the two overlap this file wins.\n");
    fprintf(out, ";\n");
    fprintf(out, "; Per-light overrides are in gta2dx9_lights.ini and the effect categories in\n");
    fprintf(out, "; gta2dx9_effects.ini; the Save button writes all three.\n");

    const char* section = "";
    auto heading = [&](const char* want) {
        if (strcmp(section, want) != 0) {
            fprintf(out, "\n[%s]\n", want);
            section = want;
        }
    };
    for (int n = 0; n < nb; ++n) {
        heading(b[n].section);
        fprintf(out, "%s=%d\n", b[n].key, *b[n].value ? 1 : 0);
    }
    for (int n = 0; n < nf; ++n) {
        heading(f[n].section);
        fprintf(out, "%s=%.5f\n", f[n].key, *f[n].value);
    }
    for (int n = 0; n < ni; ++n) {
        heading(i[n].section);
        fprintf(out, "%s=%d\n", i[n].key, *i[n].value);
    }

    // Things that live outside a settings struct.
    fprintf(out, "\n[render]\n");
    fprintf(out, "SpriteConform=%d\n", SpriteConform() ? 1 : 0);
    fprintf(out, "SpriteHeight=%.4f\n", SpriteHeight());
    fprintf(out, "SpriteRoll=%.4f\n", SpriteRoll());
    fprintf(out, "SpriteLift=%.4f\n", SpriteLift());
    fprintf(out, "SpriteStackStep=%.4f\n", SpriteStackStep());
    fprintf(out, "SpriteFeather=%.4f\n", SpriteFeather());
    fprintf(out, "AlphaMode=%s\n", gta2::GetAlphaMode() == gta2::AlphaMode::Blend ? "blend" : "test");
    fprintf(out, "AlphaRef=%d\n", gta2::GetAlphaRef());
    fprintf(out, "FpsCap=%.4f\n", FrameLimitFps());
    fprintf(out, "DumpTextures=%d\n", TextureDumping() ? 1 : 0);
    const EffectSpriteMode mode = EffectSprites().mode;
    fprintf(out, "EffectSprites=%s\n",
            mode == EffectSpriteMode::Additive
                ? "additive"
                : (mode == EffectSpriteMode::Cutout ? "cutout" : "off"));
    fclose(out);

    // One button, everything saved - the three files are an implementation
    // detail and nobody should have to remember which is which.
    LightsSaveOverrides();
    SyntheticLightsSave();

    g_dirty = false;
    Log("settings: saved %s (plus light overrides and effect settings)", g_path);
}

void SettingsLoadAll() {
    FILE* in = fopen(g_path, "r");
    if (!in) return;   // first run

    FloatField* f;
    BoolField* b;
    IntField* i;
    int nf, nb, ni;
    Fields(&f, &nf, &b, &nb, &i, &ni);

    char sectionName[64] = "";
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        char* s = Trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            strncpy(sectionName, s + 1, sizeof(sectionName) - 1);
            sectionName[sizeof(sectionName) - 1] = '\0';
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = Trim(s);
        char* value = Trim(eq + 1);

        bool handled = false;
        for (int n = 0; n < nb && !handled; ++n) {
            if (!_stricmp(sectionName, b[n].section) && !_stricmp(key, b[n].key)) {
                *b[n].value = atoi(value) != 0;
                handled = true;
            }
        }
        for (int n = 0; n < nf && !handled; ++n) {
            if (!_stricmp(sectionName, f[n].section) && !_stricmp(key, f[n].key)) {
                *f[n].value = static_cast<float>(atof(value));
                handled = true;
            }
        }
        for (int n = 0; n < ni && !handled; ++n) {
            if (!_stricmp(sectionName, i[n].section) && !_stricmp(key, i[n].key)) {
                *i[n].value = atoi(value);
                handled = true;
            }
        }
        if (handled || _stricmp(sectionName, "render") != 0) continue;

        if (!_stricmp(key, "SpriteRoll")) {
            SetSpriteRoll(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "SpriteHeight")) {
            SetSpriteHeight(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "SpriteConform")) {
            SetSpriteConform(atoi(value) != 0);
        } else if (!_stricmp(key, "SpriteLift")) {
            SetSpriteLift(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "SpriteStackStep")) {
            SetSpriteStackStep(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "SpriteFeather")) {
            SetSpriteFeather(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "AlphaMode")) {
            gta2::SetAlphaMode(!_stricmp(value, "blend") ? gta2::AlphaMode::Blend
                                                         : gta2::AlphaMode::Test);
        } else if (!_stricmp(key, "AlphaRef")) {
            gta2::SetAlphaRef(atoi(value));
        } else if (!_stricmp(key, "FpsCap")) {
            FrameLimitSet(static_cast<float>(atof(value)));
        } else if (!_stricmp(key, "DumpTextures")) {
            SetTextureDumping(atoi(value) != 0);
        } else if (!_stricmp(key, "EffectSprites")) {
            EffectSprites().mode = !_stricmp(value, "off")
                                       ? EffectSpriteMode::Off
                                       : (!_stricmp(value, "cutout") ? EffectSpriteMode::Cutout
                                                                     : EffectSpriteMode::Additive);
        }
    }
    fclose(in);
    g_dirty = false;
    Log("settings: loaded %s", g_path);
}

}  // namespace gta2dx9
