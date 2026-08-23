/*
 * Performance FX Plugin v2 (Audio FX API v2)
 *
 * Wrapper around the perf_fx_dsp engine.
 * 32 unified punch-in FX with latch support.
 * Exports as an audio FX plugin (in-place processing).
 */

#include "perf_fx_dsp.h"
#include "plugin_api_v1.h"
#include "audio_fx_api_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define clampf pfx_clampf

#define SAFE_SNPRINTF(buf, n, len, ...) do { \
    n += snprintf((buf) + (n), (n) < (len) ? (len) - (n) : 0, __VA_ARGS__); \
    if ((n) >= (len)) return (len) - 1; \
} while(0)

static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v2_t g_fx_api;

typedef struct {
    perf_fx_engine_t engine;
    char module_dir[256];
    /* Follow the host's project tempo until the user sets one explicitly.
     * The host resolves BPM through sampler_get_bpm(): internal transport →
     * live MIDI clock → last clock → current Set tempo → settings → 120, so
     * this works with no clock running and without MIDI sync enabled.
     * Cleared the moment a "bpm" param arrives (tap tempo / tempo knob), so
     * a manual tempo is never stomped by the host value. */
    int follow_host_bpm;
    int bpm_poll_countdown;
} pfx_instance_t;

/* Re-poll the host tempo every ~32 blocks (~93ms at 128 frames / 44.1kHz).
 * get_bpm() is a cheap float read, but there is no reason to call it per
 * block — tempo does not move that fast. */
#define PFX_BPM_POLL_BLOCKS 32

static void log_msg(const char *fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(buf);
}

/* ---- Lifecycle ---- */

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)config_json;
    pfx_instance_t *inst = (pfx_instance_t *)calloc(1, sizeof(pfx_instance_t));
    if (!inst) return NULL;

    if (module_dir)
        snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir);

    pfx_engine_init(&inst->engine);
    inst->follow_host_bpm = 1;
    inst->bpm_poll_countdown = 0;

    if (g_host) {
        inst->engine.mapped_memory = g_host->mapped_memory;
        inst->engine.audio_out_offset = g_host->audio_out_offset;
        inst->engine.audio_in_offset = g_host->audio_in_offset;
    }

    /* Load vinyl crackle sample */
    if (module_dir) {
        char crackle_path[512];
        snprintf(crackle_path, sizeof(crackle_path), "%s/vinyl_crackle.wav", module_dir);
        pfx_engine_load_vinyl_crackle(&inst->engine, crackle_path);
        log_msg("pfx: vinyl crackle: %s (%d samples)",
                inst->engine.vinyl_crackle_buf ? "loaded" : "not found",
                inst->engine.vinyl_crackle_len);
    }

    log_msg("pfx: Performance FX v2 engine initialized (32 unified FX)");

    return inst;
}

static void fx_destroy(void *instance) {
    pfx_instance_t *inst = (pfx_instance_t *)instance;
    if (!inst) return;
    pfx_engine_destroy(&inst->engine);
    free(inst);
    log_msg("pfx: engine destroyed");
}

/* ---- Audio (in-place processing) ---- */

static void fx_process_block(void *instance, int16_t *audio_inout, int frames) {
    pfx_instance_t *inst = (pfx_instance_t *)instance;
    if (!inst) return;

    /* Track the host's project tempo unless the user has set one. */
    if (inst->follow_host_bpm && g_host && g_host->get_bpm) {
        if (--inst->bpm_poll_countdown <= 0) {
            inst->bpm_poll_countdown = PFX_BPM_POLL_BLOCKS;
            float hb = g_host->get_bpm();
            if (hb >= 20.0f && hb <= 300.0f) inst->engine.bpm = hb;
        }
    }

    inst->engine.direct_input = audio_inout;
    pfx_engine_render(&inst->engine, audio_inout, frames);
    inst->engine.direct_input = NULL;
}

/* ---- Parameters ---- */

static void fx_set_param(void *instance, const char *key, const char *val) {
    pfx_instance_t *inst = (pfx_instance_t *)instance;
    if (!inst || !key || !val) return;
    perf_fx_engine_t *e = &inst->engine;
    float fval = (float)atof(val);
    int ival = atoi(val);

    /* Global params (E6-E8) */
    if (strcmp(key, "dj_filter") == 0) { e->dj_filter = clampf(fval, 0, 1); return; }
    if (strcmp(key, "tilt_eq") == 0) { e->tilt_eq = clampf(fval, 0, 1); return; }
    if (strcmp(key, "dry_wet") == 0) { e->dry_wet = clampf(fval, 0, 1); return; }
    if (strcmp(key, "repeat_rate") == 0) { e->repeat_rate = clampf(fval, 0, 1); return; }
    if (strcmp(key, "repeat_speed") == 0) { e->repeat_speed = clampf(fval, 0, 1); return; }

    /* Engine params */
    /* An explicit bpm means the user tapped or turned the tempo knob — stop
     * following the host from here on. "bpm_follow_host" re-arms it. */
    if (strcmp(key, "bpm") == 0) {
        e->bpm = clampf(fval, 20, 300);
        inst->follow_host_bpm = 0;
        return;
    }
    if (strcmp(key, "bpm_follow_host") == 0) {
        inst->follow_host_bpm = ival ? 1 : 0;
        inst->bpm_poll_countdown = 0;   /* re-poll on the next block */
        return;
    }
    if (strcmp(key, "bypass") == 0) { e->bypassed = ival; return; }

    /* Unified punch FX: punch_N_on, punch_N_off, punch_N_pressure,
     * punch_N_param_M, punch_N_latch */
    if (strncmp(key, "punch_", 6) == 0) {
        int slot = atoi(key + 6);
        const char *suffix = strchr(key + 6, '_');
        if (!suffix) return;
        suffix++;
        if (strcmp(suffix, "on") == 0) {
            pfx_activate(e, slot, fval > 0.0f ? fval : 0.7f);
            log_msg("pfx: ON slot=%d vel=%.3f", slot, fval);
        } else if (strcmp(suffix, "off") == 0) {
            pfx_deactivate(e, slot);
            log_msg("pfx: OFF slot=%d", slot);
        } else if (strcmp(suffix, "pressure") == 0) {
            pfx_set_pressure(e, slot, fval);
            /* Log pressure with settling state */
            pfx_slot_t *s = &e->slots[slot];
            log_msg("pfx: PRES slot=%d p=%.3f vel=%.3f settle=%d pr=%.3f",
                    slot, s->pressure, s->velocity, s->settle_counter,
                    pressure_relative(s->pressure, s->velocity, s->settle_counter));
        } else if (strcmp(suffix, "latch") == 0) {
            pfx_set_latched(e, slot, ival);
        } else if (strncmp(suffix, "param_", 6) == 0) {
            int param = atoi(suffix + 6);
            pfx_set_param(e, slot, param, fval);
        }
        return;
    }

}

static int fx_get_param(void *instance, const char *key, char *buf, int buf_len) {
    pfx_instance_t *inst = (pfx_instance_t *)instance;
    if (!inst || !key) return -1;
    perf_fx_engine_t *e = &inst->engine;

    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Performance FX");

    /* Global params */
    if (strcmp(key, "dj_filter") == 0) return snprintf(buf, buf_len, "%.3f", e->dj_filter);
    if (strcmp(key, "tilt_eq") == 0) return snprintf(buf, buf_len, "%.3f", e->tilt_eq);
    if (strcmp(key, "dry_wet") == 0) return snprintf(buf, buf_len, "%.3f", e->dry_wet);
    if (strcmp(key, "repeat_rate") == 0) return snprintf(buf, buf_len, "%.3f", e->repeat_rate);
    if (strcmp(key, "repeat_speed") == 0) return snprintf(buf, buf_len, "%.3f", e->repeat_speed);
    if (strcmp(key, "bpm") == 0) return snprintf(buf, buf_len, "%.1f", e->bpm);
    if (strcmp(key, "bypass") == 0) return snprintf(buf, buf_len, "%d", e->bypassed);
    if (strcmp(key, "last_touched") == 0) {
        return snprintf(buf, buf_len, "%d", e->last_touched_slot);
    }

    /* Short FX names for the 128px display (all 32), straight from the
     * descriptor table so the UI never keeps its own copy. */
    if (strcmp(key, "fx_names") == 0) {
        int n = snprintf(buf, buf_len, "[");
        for (int i = 0; i < PFX_NUM_FX; i++) {
            SAFE_SNPRINTF(buf, n, buf_len, "%s\"%s\"", i ? "," : "",
                          pfx_fx_desc[i].short_name);
        }
        SAFE_SNPRINTF(buf, n, buf_len, "]");
        return n;
    }

    /* FX active state (all 32) */
    if (strcmp(key, "fx_active") == 0) {
        int n = snprintf(buf, buf_len, "[");
        for (int i = 0; i < PFX_NUM_FX; i++) {
            pfx_slot_t *s = &e->slots[i];
            SAFE_SNPRINTF(buf, n, buf_len, "%s%d", i ? "," : "",
                          s->active || s->tail_active);
        }
        SAFE_SNPRINTF(buf, n, buf_len, "]");
        return n;
    }

    /* FX latched state (all 32) */
    if (strcmp(key, "fx_latched") == 0) {
        int n = snprintf(buf, buf_len, "[");
        for (int i = 0; i < PFX_NUM_FX; i++) {
            SAFE_SNPRINTF(buf, n, buf_len, "%s%d", i ? "," : "",
                          e->slots[i].latched);
        }
        SAFE_SNPRINTF(buf, n, buf_len, "]");
        return n;
    }

    /* Per-FX param names and defaults: [["Filter",0.500],...].
     * A null name means that knob is unused for this FX. The UI fetches this
     * for the selected slot rather than carrying its own label table — the
     * three-tables-that-disagree problem is what left most of these knobs
     * doing nothing while the display insisted otherwise. */
    if (strncmp(key, "fx_params_", 10) == 0) {
        int slot = atoi(key + 10);
        if (slot < 0 || slot >= PFX_NUM_FX) return -1;
        int n = snprintf(buf, buf_len, "[");
        for (int i = 0; i < PFX_SLOT_PARAMS; i++) {
            const pfx_param_desc_t *p = &pfx_fx_desc[slot].params[i];
            if (p->name)
                SAFE_SNPRINTF(buf, n, buf_len, "%s[\"%s\",%.3f]",
                              i ? "," : "", p->name, p->def);
            else
                SAFE_SNPRINTF(buf, n, buf_len, "%s[null,%.3f]", i ? "," : "", p->def);
        }
        SAFE_SNPRINTF(buf, n, buf_len, "]");
        return n;
    }

    /* Full state */
    if (strcmp(key, "state") == 0)
        return pfx_serialize_state(e, buf, buf_len);

    return -1;
}

/* ---- MIDI ---- */

static void fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    pfx_instance_t *inst = (pfx_instance_t *)instance;
    if (!inst || len < 3) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t d1 = msg[1];
    uint8_t d2 = msg[2];

    /* Polyphonic aftertouch for pad pressure — all 4 rows */
    if (status == 0xA0) {
        int note = d1;
        float pressure = (float)d2 / 127.0f;

        /* Row 4: pads 92-99 -> slots 0-7 */
        if (note >= 92 && note <= 99)
            pfx_set_pressure(&inst->engine, note - 92, pressure);
        /* Row 3: pads 84-91 -> slots 8-15 */
        else if (note >= 84 && note <= 91)
            pfx_set_pressure(&inst->engine, note - 84 + 8, pressure);
        /* Row 2: pads 76-83 -> slots 16-23 */
        else if (note >= 76 && note <= 83)
            pfx_set_pressure(&inst->engine, note - 76 + 16, pressure);
        /* Row 1: pads 68-75 -> slots 24-31 */
        else if (note >= 68 && note <= 75)
            pfx_set_pressure(&inst->engine, note - 68 + 24, pressure);
    }
}

/* ---- Entry point (Audio FX API v2) ---- */

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api.create_instance = fx_create;
    g_fx_api.destroy_instance = fx_destroy;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;
    g_fx_api.on_midi = fx_on_midi;
    return &g_fx_api;
}
