/*
 * Performance FX DSP Engine v2
 *
 * 32 unified punch-in FX. All momentary by default, shift+pad to latch.
 * Animated filter sweeps, space throw tails, per-FX pressure mappings.
 */

#include "perf_fx_dsp.h"
#include "pfx_bungee.h"
#include "pfx_revsc.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================
 * FX descriptor table — the single source of truth
 *
 * Every default below is the value that reproduces the sound the effect had
 * before it had a knob, so wiring the knobs up did not change how the module
 * sounds out of the box. Changing one now is an audible decision.
 * ============================================================ */

const pfx_fx_desc_t pfx_fx_desc[PFX_NUM_FX] = {
/*  name           short      topology         param 0           param 1           param 2          wet */

  /* Time/repeat. No wet control on any of them: the loop is a delayed copy of
   * the input, so blending it back comb-filters instead of mixing. The knob
   * goes to Decay — successive repetitions falling away — which is what a beat
   * repeat actually wants. */
  { "RPT 1/4",     "RPT 1/4", PFX_TOPO_INSERT, {{"Filter",0.5f}, {"Gate",  0.0f}, {"Decay", 0.0f}}, -1 },
  { "RPT 1/8",     "RPT 1/8", PFX_TOPO_INSERT, {{"Filter",0.5f}, {"Gate",  0.0f}, {"Decay", 0.0f}}, -1 },
  { "RPT 1/16",    "RPT1/16", PFX_TOPO_INSERT, {{"Filter",0.5f}, {"Gate",  0.0f}, {"Decay", 0.0f}}, -1 },
  { "RPT Triplet", "RPT TRP", PFX_TOPO_INSERT, {{"Filter",0.5f}, {"Gate",  0.0f}, {"Decay", 0.0f}}, -1 },
  { "Stutter",     "STUTTER", PFX_TOPO_INSERT, {{"Filter",0.5f}, {"Gate",  0.0f}, {"Decay", 0.0f}}, -1 },
  { "Scatter",     "SCATTER", PFX_TOPO_INSERT, {{"Pattrn",0.0f}, {"Gate",  0.0f}, {"Revrse",0.5f}}, -1 },
  { "Reverse",     "REVERSE", PFX_TOPO_INSERT, {{"Length",0.5f}, {"Filter",0.5f}, {"Decay", 0.0f}}, -1 },
  /* Timestretch has two real controls and no honest third: it is time-domain
   * so Mix is out, and Bungee exposes nothing else worth a knob. An unused
   * slot the display marks "---" beats inventing a control that does little. */
  { "Timestretch", "STRETCH", PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Filter",0.5f}, {NULL,    0.5f}}, -1 },

  /* Sweeps: Depth is how far the sweep travels. Blending dry back into a
   * filter sweep just stops it sweeping, so these get Depth, not Mix. */
  { "LP Sweep",    "LP SWP",  PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Reso",  0.5f}, {"Depth", 0.5f}}, -1 },
  { "HP Sweep",    "HP SWP",  PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Reso",  0.5f}, {"Depth", 0.5f}}, -1 },
  { "BP Rise",     "BP RISE", PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Reso",  0.5f}, {"Depth", 0.5f}}, -1 },
  { "BP Fall",     "BP FALL", PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Reso",  0.5f}, {"Depth", 0.5f}}, -1 },
  /* These three do earn a Mix: a resonant peak or a comb over the dry signal
   * is the classic way all three are used. */
  { "Reso Sweep",  "RESO SW", PFX_TOPO_INSERT, {{"Speed", 0.5f}, {"Reso",  0.5f}, {"Mix",   0.7f}},  2 },
  { "Phaser",      "PHASER",  PFX_TOPO_INSERT, {{"Depth", 0.5f}, {"Feedbk",0.5f}, {"Mix",   0.7f}},  2 },
  { "Flanger",     "FLANGER", PFX_TOPO_INSERT, {{"Depth", 0.5f}, {"Feedbk",0.5f}, {"Mix",   0.6f}},  2 },
  { "Auto Filter", "AUTOFLT", PFX_TOPO_INSERT, {{"Depth", 0.5f}, {"Reso",  0.5f}, {"Center",0.5f}}, -1 },

  /* Sends. Level is not optional here — it is the send amount, and the dry
   * signal always survives underneath it. */
  { "Delay 1/4",   "DLY 1/4", PFX_TOPO_SEND,   {{"Feedbk",0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "Delay D8",    "DLY D8",  PFX_TOPO_SEND,   {{"Feedbk",0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "PingPong 1/4","PP 1/4",  PFX_TOPO_SEND,   {{"Feedbk",0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "PingPong D8", "PP D8",   PFX_TOPO_SEND,   {{"Feedbk",0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "Room",        "ROOM",    PFX_TOPO_SEND,   {{"Decay", 0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "Hall",        "HALL",    PFX_TOPO_SEND,   {{"Decay", 0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "Dark Verb",   "DK VERB", PFX_TOPO_SEND,   {{"Decay", 0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },
  { "Spring",      "SPRING",  PFX_TOPO_SEND,   {{"Decay", 0.5f}, {"Tone",  0.5f}, {"Level", 0.5f}},  2 },

  /* Distortion. Parallel processing is standard practice on all of these, so
   * Mix is real work rather than filler. */
  { "Bitcrush",    "CRUSH",   PFX_TOPO_INSERT, {{"Bits",  0.5f}, {"Tone",  1.0f}, {"Mix",   1.0f}},  2 },
  { "Downsample",  "DWNSMPL", PFX_TOPO_INSERT, {{"Rate",  0.5f}, {"Tone",  1.0f}, {"Mix",   1.0f}},  2 },
  { "Saturate",    "SATURATE",PFX_TOPO_INSERT, {{"Drive", 0.5f}, {"Tone",  0.5f}, {"Mix",   0.7f}},  2 },
  /* Gate's wet amount IS its depth — how far it ducks — so it is named for
   * what it does rather than dressed up as a mix. */
  { "Gate/Duck",   "GATE",    PFX_TOPO_INSERT, {{"Rate",  0.5f}, {"Duty",  0.5f}, {"Depth", 1.0f}}, -1 },
  /* Same trap avoided: a Mix here would have duplicated Depth. Shape morphs
   * the LFO from sine to square instead. */
  { "Tremolo",     "TREMOLO", PFX_TOPO_INSERT, {{"Rate",  0.5f}, {"Depth", 0.5f}, {"Shape", 0.0f}}, -1 },
  { "Octave Down", "OCT DN",  PFX_TOPO_INSERT, {{"Pitch", 0.5f}, {"Tone",  1.0f}, {"Mix",   1.0f}},  2 },
  { "Vinyl Sim",   "VINYL",   PFX_TOPO_INSERT, {{"Noise", 0.5f}, {"Warmth",0.5f}, {"Mix",   1.0f}},  2 },
  /* Time-domain like the repeats — a slowing copy against live audio flams. */
  { "Vinyl Brake", "VNL BRK", PFX_TOPO_INSERT, {{"Rate",  0.5f}, {"Noise", 0.5f}, {"Tone",  1.0f}}, -1 }
};


/* ============================================================
 * Utility helpers
 * ============================================================ */

#define clampf pfx_clampf

#define SAFE_SNPRINTF(buf, n, len, ...) do { \
    n += snprintf((buf) + (n), (n) < (len) ? (len) - (n) : 0, __VA_ARGS__); \
    if ((n) >= (len)) return (len) - 1; \
} while(0)

static inline float soft_clip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - (x * x * x) / 6.75f;
}

static inline float fast_tanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline float white_noise(unsigned int *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(int)(*seed) / 2147483648.0f;
}

/*
 * Pressure-relative volume gain (convenience wrapper).
 * Center (0.5) = 1.0x, max (1.0) = 2.0x, min (0.0) = 0.0x.
 */
static inline float pressure_volume_gain(float pressure, float initial,
                                          int settle_counter) {
    return pressure_relative(pressure, initial, settle_counter) * 2.0f;
}

static inline float flush_denormal(float x) {
    union { float f; uint32_t u; } v = { .f = x };
    return (v.u & 0x7F800000) == 0 ? 0.0f : x;
}

static inline float cutoff_to_f(float cutoff01) {
    float hz = 20.0f * powf(1000.0f, cutoff01);
    if (hz > 20000.0f) hz = 20000.0f;
    float f = 2.0f * sinf(M_PI * hz / PFX_SAMPLE_RATE);
    return clampf(f, 0.0f, 1.0f);
}

/* Resonance knob -> multiplier for an SVF q coefficient.
 *
 * Note the inversion: in this SVF, q is a damping term (hp = in - lp - q*bp),
 * so *smaller* q means *more* resonance. A Reso knob at 0.5 returns exactly
 * 1.0, which is what keeps each filter's hand-tuned base q intact at default.
 * Deliberately branch-and-multiply rather than powf() — this runs per sample
 * per active slot, and powf() on ARM is far too expensive for that. */
static inline float pfx_reso_scale(float reso) {
    if (reso < 0.5f) return 1.0f + (0.5f - reso) * 8.0f;   /* 1.0 -> 5.0 (tamer) */
    return 1.0f - (reso - 0.5f) * 1.7f;                     /* 1.0 -> 0.15 (sharper) */
}

/* Map rate knob (0..1) directly to repeat length in samples.
 * Free continuous sweep: rate01=0 → 2.0s, rate01=1 → ~12ms.
 * Pads preset to BPM-synced positions but the knob is free seconds. */
static int pfx_rate_to_samples(float rate01) {
    /* Exponential: 2.0 * 0.006^rate01 gives 2.0s → 0.012s */
    float seconds = 2.0f * powf(0.006f, rate01);
    int samples = (int)(seconds * PFX_SAMPLE_RATE);
    if (samples < 530) samples = 530;       /* ~12ms minimum */
    if (samples > PFX_SAMPLE_RATE * 2) samples = PFX_SAMPLE_RATE * 2;
    return samples;
}

/* Convert a time in seconds to the corresponding rate01 knob position */
static float pfx_seconds_to_rate(float seconds) {
    /* Inverse of: seconds = 2.0 * 0.006^rate01 */
    if (seconds <= 0.012f) return 1.0f;
    if (seconds >= 2.0f) return 0.0f;
    return logf(seconds / 2.0f) / logf(0.006f);
}

int pfx_bpm_to_samples(float bpm, float division) {
    if (bpm < 20.0f) bpm = 120.0f;
    float beat_samples = (60.0f / bpm) * PFX_SAMPLE_RATE;
    return (int)(beat_samples * division);
}

/* ============================================================
 * State Variable Filter
 * ============================================================ */

static void svf_reset(svf_t *s) {
    s->lp = s->bp = s->hp = 0.0f;
}

static void svf_process(svf_t *s, float input, float f, float q,
                         float *lp, float *hp, float *bp) {
    s->hp = input - s->lp - q * s->bp;
    s->bp += f * s->hp;
    s->lp += f * s->bp;
    s->bp = flush_denormal(s->bp);
    s->lp = flush_denormal(s->lp);
    if (lp) *lp = s->lp;
    if (hp) *hp = s->hp;
    if (bp) *bp = s->bp;
}

/* ============================================================
 * Delay line helpers
 * ============================================================ */

static void delay_init(delay_t *d, int max_len) {
    d->buf_l = (float *)calloc(max_len, sizeof(float));
    d->buf_r = (float *)calloc(max_len, sizeof(float));
    d->length = max_len;
    d->write_pos = 0;
    d->time = 0.3f;
    d->feedback = 0.4f;
    d->filter = 0.5f;
    d->mix = 0.3f;
    d->fb_lp_l = d->fb_lp_r = 0.0f;
}

static void delay_free(delay_t *d) {
    free(d->buf_l); free(d->buf_r);
    d->buf_l = d->buf_r = NULL;
}

static void delay_reset(delay_t *d) {
    if (d->buf_l) memset(d->buf_l, 0, d->length * sizeof(float));
    if (d->buf_r) memset(d->buf_r, 0, d->length * sizeof(float));
    d->write_pos = 0;
    d->fb_lp_l = d->fb_lp_r = 0.0f;
}

static void delay_write(delay_t *d, float l, float r) {
    d->buf_l[d->write_pos] = l;
    d->buf_r[d->write_pos] = r;
    d->write_pos = (d->write_pos + 1) % d->length;
}

static void delay_read(delay_t *d, int delay_samples, float *l, float *r) {
    int pos = (d->write_pos - delay_samples + d->length) % d->length;
    *l = d->buf_l[pos];
    *r = d->buf_r[pos];
}

/* ============================================================
 * Repeat buffer helpers
 * ============================================================ */

static void repeat_init(repeat_t *r, int max_len) {
    r->buf_l = (float *)calloc(max_len, sizeof(float));
    r->buf_r = (float *)calloc(max_len, sizeof(float));
    r->buf_len = max_len;
    r->write_pos = 0;
    r->read_pos = 0;
    r->repeat_len = PFX_SAMPLE_RATE / 4;
    r->repeat_pos = 0;
    r->capturing = 1;
    r->frames_captured = 0;
}

static void repeat_free(repeat_t *r) {
    free(r->buf_l); free(r->buf_r);
    r->buf_l = r->buf_r = NULL;
}

/* ============================================================
 * Reverb (Schroeder/Moorer)
 * ============================================================ */

/* Reverb now uses pfx_revsc.h (Costello/Soundpipe FDN reverb) */

/* reverb_init / reverb_process removed — using pfx_revsc_t instead */

/* ============================================================
 * Engine init / destroy / reset
 * ============================================================ */

void pfx_engine_init(perf_fx_engine_t *e) {
    memset(e, 0, sizeof(*e));

    /* Global defaults */
    e->dj_filter = 0.5f;
    e->tilt_eq = 0.5f;
    e->dry_wet = 1.0f;
    e->repeat_rate = 0.5f;
    e->repeat_speed = 0.5f;
    e->bpm = 120.0f;
    e->last_touched_slot = -1;

    /* Allocate shared capture buffer */
    e->capture_len = PFX_CAPTURE_BUF;
    e->capture_buf_l = (float *)calloc(PFX_CAPTURE_BUF, sizeof(float));
    e->capture_buf_r = (float *)calloc(PFX_CAPTURE_BUF, sizeof(float));
    if (!e->capture_buf_l || !e->capture_buf_r) {
        fprintf(stderr, "pfx: FATAL - capture buffer alloc failed\n");
        return;
    }

    /* Row 4 chain buffer — same size as capture buffer */
    e->row4_buf_len = PFX_CAPTURE_BUF;
    e->row4_buf_l = (float *)calloc(PFX_CAPTURE_BUF, sizeof(float));
    e->row4_buf_r = (float *)calloc(PFX_CAPTURE_BUF, sizeof(float));
    e->row4_write_pos = 0;

    /* Init all 32 slots based on type */
    for (int i = 0; i < PFX_NUM_FX; i++) {
        pfx_slot_t *s = &e->slots[i];

        /* Param defaults come from the descriptor table, never from a second
         * list kept in step by hand. */
        for (int j = 0; j < PFX_SLOT_PARAMS; j++)
            s->params[j] = pfx_fx_desc[i].params[j].def;

        if (FX_IS_REPEAT(i)) {
            /* Repeat slots: repeat buffer */
            repeat_init(&s->repeat, PFX_REPEAT_BUF);
            /* Tape stop / half speed buffer */
            s->tape.buf_l = (float *)calloc(PFX_REPEAT_BUF, sizeof(float));
            s->tape.buf_r = (float *)calloc(PFX_REPEAT_BUF, sizeof(float));
            s->tape.buf_len = PFX_REPEAT_BUF;
            s->tape.speed = 1.0f;
        } else if (FX_IS_FILTER(i)) {
            /* Filter slots: SVF state (inline, no alloc needed) */
            /* Phaser/Flanger need mod_delay */
            if (i == FX_FLANGER) {
                s->mod_delay.buf_l = (float *)calloc(PFX_CHORUS_BUF, sizeof(float));
                s->mod_delay.buf_r = (float *)calloc(PFX_CHORUS_BUF, sizeof(float));
                s->mod_delay.buf_len = PFX_CHORUS_BUF;
            }
        } else if (FX_IS_SPACE(i)) {
            /* Space slots: delay or reverb */
            if (i >= FX_DELAY && i <= FX_PING_PONG_DOT8) {
                delay_init(&s->delay, PFX_MAX_DELAY);
            }
            if (i >= FX_REVERB && i <= FX_SPRING) {
                pfx_revsc_t *rv = (pfx_revsc_t *)calloc(1, sizeof(pfx_revsc_t));
                if (rv) pfx_revsc_init(rv, PFX_SAMPLE_RATE);
                s->ext_instance = rv;
            }
        } else if (FX_IS_DISTORT(i)) {
            /* Distortion slots */
            if (i == FX_VINYL_BRAKE) {
                s->tape.buf_l = (float *)calloc(PFX_REPEAT_BUF, sizeof(float));
                s->tape.buf_r = (float *)calloc(PFX_REPEAT_BUF, sizeof(float));
                s->tape.buf_len = PFX_REPEAT_BUF;
                s->tape.speed = 1.0f;
            }
            if (i == FX_PITCH_DOWN) {
                s->bungee = pfx_bungee_create(PFX_SAMPLE_RATE);
            }
        }
    }

}

void pfx_engine_destroy(perf_fx_engine_t *e) {
    free(e->capture_buf_l);
    free(e->capture_buf_r);
    free(e->row4_buf_l);
    free(e->row4_buf_r);
    free(e->vinyl_crackle_buf);
    e->vinyl_crackle_buf = NULL;

    for (int i = 0; i < PFX_NUM_FX; i++) {
        pfx_slot_t *s = &e->slots[i];
        repeat_free(&s->repeat);
        free(s->tape.buf_l);
        free(s->tape.buf_r);
        s->tape.buf_l = s->tape.buf_r = NULL;
        delay_free(&s->delay);
        free(s->mod_delay.buf_l);
        free(s->mod_delay.buf_r);
        s->mod_delay.buf_l = s->mod_delay.buf_r = NULL;
        if (s->bungee) {
            pfx_bungee_destroy((pfx_bungee_t *)s->bungee);
            s->bungee = NULL;
        }
        /* Free revsc reverb instances */
        free(s->ext_instance);
        s->ext_instance = NULL;
    }
}

/* Simple WAV loader — reads mono/stereo 16-bit PCM WAV into a mono int16 buffer.
 * Properly skips non-data chunks (LIST, INFO, etc.) to find the 'data' chunk. */
void pfx_engine_load_vinyl_crackle(perf_fx_engine_t *e, const char *wav_path) {
    FILE *f = fopen(wav_path, "rb");
    if (!f) return;

    /* Read RIFF header (12 bytes) */
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12) { fclose(f); return; }
    if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F') { fclose(f); return; }
    if (riff[8] != 'W' || riff[9] != 'A' || riff[10] != 'V' || riff[11] != 'E') { fclose(f); return; }

    int channels = 0, bits = 0, data_size = 0;
    int found_fmt = 0, found_data = 0;

    /* Walk chunks to find 'fmt ' and 'data' */
    while (!found_data) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;

        uint32_t chunk_size = chunk_hdr[4] | (chunk_hdr[5] << 8) |
                              (chunk_hdr[6] << 16) | (chunk_hdr[7] << 24);

        if (chunk_hdr[0] == 'f' && chunk_hdr[1] == 'm' &&
            chunk_hdr[2] == 't' && chunk_hdr[3] == ' ') {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            channels = fmt[2] | (fmt[3] << 8);
            bits = fmt[14] | (fmt[15] << 8);
            found_fmt = 1;
            /* Skip rest of fmt chunk if > 16 bytes */
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (chunk_hdr[0] == 'd' && chunk_hdr[1] == 'a' &&
                   chunk_hdr[2] == 't' && chunk_hdr[3] == 'a') {
            data_size = (int)chunk_size;
            found_data = 1;
            /* File pointer is now at start of audio data */
        } else {
            /* Skip unknown chunk */
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data || bits != 16 || channels < 1) {
        fclose(f); return;
    }

    int total_samples = data_size / 2;
    int frames = total_samples / channels;

    int16_t *raw = (int16_t *)malloc(total_samples * sizeof(int16_t));
    if (!raw) { fclose(f); return; }
    if ((int)fread(raw, sizeof(int16_t), total_samples, f) != total_samples) {
        free(raw); fclose(f); return;
    }
    fclose(f);

    /* Convert to mono if stereo */
    e->vinyl_crackle_buf = (int16_t *)malloc(frames * sizeof(int16_t));
    if (!e->vinyl_crackle_buf) { free(raw); return; }

    if (channels == 1) {
        memcpy(e->vinyl_crackle_buf, raw, frames * sizeof(int16_t));
    } else {
        for (int i = 0; i < frames; i++) {
            e->vinyl_crackle_buf[i] = (int16_t)(((int)raw[i * channels] + (int)raw[i * channels + 1]) / 2);
        }
    }
    e->vinyl_crackle_len = frames;
    e->vinyl_crackle_pos = 0;
    free(raw);
}

void pfx_engine_reset(perf_fx_engine_t *e) {
    for (int i = 0; i < PFX_NUM_FX; i++) {
        pfx_slot_t *s = &e->slots[i];
        s->active = 0;
        s->latched = 0;
        s->tail_active = 0;
        s->pressure = 0.0f;
        s->fading_out = 0;
        s->phase = 0.0f;
        s->tail_silence_count = 0;
        svf_reset(&s->filter_l);
        svf_reset(&s->filter_r);
        svf_reset(&s->sat_filter_l);
        svf_reset(&s->sat_filter_r);
        if (s->delay.buf_l) delay_reset(&s->delay);
        if (s->ext_instance && i >= FX_REVERB && i <= FX_SPRING) {
            pfx_revsc_init((pfx_revsc_t *)s->ext_instance, PFX_SAMPLE_RATE);
        }
    }
    e->bypassed = 0;
    e->last_touched_slot = -1;
    svf_reset(&e->global_lp_l);
    svf_reset(&e->global_lp_r);
    svf_reset(&e->global_hp_l);
    svf_reset(&e->global_hp_r);
    svf_reset(&e->tilt_lp_l);
    svf_reset(&e->tilt_lp_r);
    svf_reset(&e->tilt_hp_l);
    svf_reset(&e->tilt_hp_r);
}

/* ============================================================
 * Unified FX control
 * ============================================================ */

/* Grab the tail of the capture buffer into the Reverse slot's private buffer
 * and park the read head at the end, ready to walk backwards.
 *
 * params[0] = Length: 0 -> 1/4 bar, 0.5 -> 1 bar (the old fixed behaviour),
 * 1.0 -> 2 bars. Called both on activation and on every loop wrap, so turning
 * the knob while it runs takes effect at the next wrap. */
static void pfx_reverse_recapture(perf_fx_engine_t *e, pfx_slot_t *s) {
    float k = s->params[0];
    float bars = (k < 0.5f) ? (0.25f + k * 1.5f) : (1.0f + (k - 0.5f) * 2.0f);

    int len = pfx_bpm_to_samples(e->bpm, 4.0f * bars);
    if (len > s->repeat.buf_len) len = s->repeat.buf_len;
    if (len > e->capture_len) len = e->capture_len;
    if (len < 128) len = 128;

    for (int i = 0; i < len; i++) {
        int src = (e->capture_write_pos - len + i + e->capture_len) % e->capture_len;
        s->repeat.buf_l[i] = e->capture_buf_l[src];
        s->repeat.buf_r[i] = e->capture_buf_r[src];
    }
    s->repeat.repeat_len = len;
    s->repeat.read_pos = len - 1;
    s->repeat.xfade_pos = 0;
    s->repeat.xfade_len = 128;
}

void pfx_activate(perf_fx_engine_t *e, int slot, float velocity) {
    /* Note-on velocity is deliberately ignored: it and pad pressure are not
     * on the same scale, so the pressure centre is captured from the first
     * aftertouch instead (see the settling window below). Kept in the
     * signature because the host still sends it. */
    (void)velocity;
    if (slot < 0 || slot >= PFX_NUM_FX) return;
    pfx_slot_t *s = &e->slots[slot];

    /* IMPORTANT: Set up ALL type-specific state BEFORE setting active = 1.
     * The render thread checks s->active to decide whether to process.
     * If active is set before repeat state (capturing=1), the render thread
     * may see active=1 + capturing=0 (stale) and read from garbage positions
     * in the capture buffer, causing an audible glitch. */

    s->fading_out = 0;
    s->tail_active = 0;
    s->tail_silence_count = 0;
    s->velocity = 0.5f;  /* temporary center until first aftertouch arrives */
    s->pressure = 0.0f;  /* no pressure yet — aftertouch comes separately */
    s->phase = 0.0f;

    /* Settling: -1 means "waiting for first aftertouch".
     * Once first aftertouch arrives, starts counting down from settle window.
     * During settling, velocity tracks pressure so pressure_relative = 0.5. */
    s->settle_counter = -1;
    e->last_touched_slot = slot;

    /* Reset filter state */
    svf_reset(&s->filter_l);
    svf_reset(&s->filter_r);

    /* Type-specific activation (all BEFORE s->active = 1) */
    if (FX_IS_REPEAT(slot)) {
        /* Decay is per punch-in: a pad that faded to silence last time must
         * come back at full level, not stay dead. */
        s->repeat.decay_gain = 1.0f;

        /* Beat repeat: pass through audio for one division, then loop.
         * The capture buffer records live audio continuously, so after
         * one division passes through, we loop from the capture buffer.
         * params[0] = Rate: 0.0 = 2 beats, 0.5 ≈ 1/4 note, 1.0 = 1/32 note
         * Set initial rate position based on which pad triggered it. */
        if (slot >= FX_RPT_1_4 && slot <= FX_STUTTER) {
            /* Set global repeat_rate to BPM-synced position for this pad.
             * The rate knob is free seconds — pads just preset it. */
            float bpm = e->bpm < 20.0f ? 120.0f : e->bpm;
            float beat_sec = 60.0f / bpm;
            float div = 1.0f;
            switch (slot) {
                case FX_RPT_1_4:  div = 1.0f; break;     /* quarter note */
                case FX_RPT_1_8:  div = 0.5f; break;     /* eighth note */
                case FX_RPT_1_16: div = 0.25f; break;    /* sixteenth note */
                case FX_RPT_TRIP: div = 2.0f/3.0f; break;/* triplet */
                case FX_STUTTER:  div = 0.125f; break;   /* thirty-second note */
                default: break;
            }
            float seconds = beat_sec * div;
            e->repeat_rate = pfx_seconds_to_rate(seconds);
            int rlen = pfx_rate_to_samples(e->repeat_rate);
            if (rlen < 64) rlen = 64;
            if (rlen > e->capture_len) rlen = e->capture_len;
            s->repeat.repeat_len = rlen;
            s->repeat.frames_captured = 0;
            s->repeat.read_pos = 0;
            s->repeat.repeat_pos = 0;  /* reset speed accumulator */
            s->repeat.capturing = 1;  /* Set LAST so render sees valid state */
        }

        /* Scatter: reset step counter and trigger first slice */
        if (slot == FX_SCATTER) {
            s->repeat.write_pos = -1;   /* step counter, advances to 0 first */
            s->repeat.repeat_pos = 0;   /* triggers slice setup on first call */
            s->repeat.frames_captured = 0;
            s->repeat.bar_start = 0;    /* latched on step 0, which is next */
        }

        /* Reverse: copy the tail of the capture buffer, play it backwards */
        if (slot == FX_REVERSE) {
            pfx_reverse_recapture(e, s);
        }

        /* Timestretch: create bungee stretcher and prime with capture audio */
        if (slot == FX_STRETCH) {
            if (!s->bungee) {
                s->bungee = pfx_bungee_create(PFX_SAMPLE_RATE);
            }
            pfx_bungee_set_speed((pfx_bungee_t *)s->bungee, 0.5f);
            pfx_bungee_reset((pfx_bungee_t *)s->bungee);

            /* Prime bungee with recent capture audio so it has data immediately */
            int prime_len = PFX_SAMPLE_RATE; /* 1 second */
            if (prime_len > e->capture_len) prime_len = e->capture_len;
            float prime_lr[2];
            for (int i = 0; i < prime_len; i++) {
                int src = (e->capture_write_pos - prime_len + i + e->capture_len) % e->capture_len;
                prime_lr[0] = e->capture_buf_l[src];
                prime_lr[1] = e->capture_buf_r[src];
                pfx_bungee_write((pfx_bungee_t *)s->bungee, prime_lr, 1);
            }
        }
    }

    if (FX_IS_FILTER(slot)) {
        /* Reset phase for animated sweeps */
        s->phase = 0.0f;
        if (slot == FX_PHASER) {
            memset(&s->phaser, 0, sizeof(s->phaser));
        }
        if (slot == FX_FLANGER && s->mod_delay.buf_l) {
            memset(s->mod_delay.buf_l, 0, s->mod_delay.buf_len * sizeof(float));
            memset(s->mod_delay.buf_r, 0, s->mod_delay.buf_len * sizeof(float));
            s->mod_delay.write_pos = 0;
            s->mod_delay.lfo_phase = 0.0f;
        }
    }

    if (FX_IS_SPACE(slot)) {
        /* Reset space FX state */
        if (slot >= FX_DELAY && slot <= FX_PING_PONG_DOT8) {
            delay_reset(&s->delay);
        }
        if (slot >= FX_REVERB && slot <= FX_SPRING && s->ext_instance) {
            pfx_revsc_init((pfx_revsc_t *)s->ext_instance, PFX_SAMPLE_RATE);
        }
    }

    if (FX_IS_DISTORT(slot)) {
        if (slot == FX_VINYL_BRAKE) {
            if (s->tape.buf_l)
                memset(s->tape.buf_l, 0, s->tape.buf_len * sizeof(float));
            if (s->tape.buf_r)
                memset(s->tape.buf_r, 0, s->tape.buf_len * sizeof(float));
            s->tape.speed = 1.0f;
            s->tape.read_pos = 0.0f;
            s->tape.write_pos = 0;
            s->tape.decel_rate = 0.00002f; /* slow vinyl brake style */
        }
        if (slot == FX_BITCRUSH || slot == FX_DOWNSAMPLE) {
            s->crush_count = 0;
            s->crush_hold_l = s->crush_hold_r = 0.0f;
        }
        if (slot == FX_GATE_DUCK) {
            s->ducker.phase = 0.0f;
            s->ducker.env = 1.0f;
        }
        if (slot == FX_TREMOLO) {
            s->trem_lfo_phase = 0.0f;
        }
        if (slot == FX_SATURATE) {
            svf_reset(&s->sat_filter_l);
            svf_reset(&s->sat_filter_r);
        }
        if (slot == FX_VINYL_SIM) {
            s->scatter_seed = 12345;  /* noise seed */
            s->trem_lfo_phase = 0.0f; /* wow LFO */
            /* Click envelope + polarity for the synthesised-crackle fallback */
            s->crush_hold_l = 1.0f;
            s->crush_hold_r = 0.0f;
            svf_reset(&s->sat_filter_l);
            svf_reset(&s->sat_filter_r);
        }
        if (slot == FX_PITCH_DOWN && s->bungee) {
            pfx_bungee_set_speed((pfx_bungee_t *)s->bungee, 1.0f);
            pfx_bungee_set_pitch((pfx_bungee_t *)s->bungee, 0.5f);
            pfx_bungee_reset((pfx_bungee_t *)s->bungee);
            /* Prime with capture audio so bungee has data immediately */
            int prime_len = PFX_SAMPLE_RATE / 2;
            if (prime_len > e->capture_len) prime_len = e->capture_len;
            float prime_lr[2];
            for (int i = 0; i < prime_len; i++) {
                int src = (e->capture_write_pos - prime_len + i + e->capture_len) % e->capture_len;
                prime_lr[0] = e->capture_buf_l[src];
                prime_lr[1] = e->capture_buf_r[src];
                pfx_bungee_write((pfx_bungee_t *)s->bungee, prime_lr, 1);
            }
        }
    }

    /* Set active LAST (belt-and-suspenders, even though set_param and
     * process_block are serialized via ioctl). */
    s->active = 1;

}

void pfx_deactivate(perf_fx_engine_t *e, int slot) {
    if (slot < 0 || slot >= PFX_NUM_FX) return;
    pfx_slot_t *s = &e->slots[slot];

    /* If latched, don't deactivate on release */
    if (s->latched) return;

    if (!s->active) return;

    /* Space FX: switch to tail mode instead of immediate cutoff */
    if (FX_IS_SPACE(slot)) {
        s->active = 0;
        s->tail_active = 1;
        s->tail_silence_count = 0;
        s->pressure = 0.0f;
        return;
    }

    /* Other FX: fade out */
    s->fading_out = 1;
    s->fade_pos = 0;
    s->fade_len = 256; /* ~5.8ms */
    s->pressure = 0.0f;
}

void pfx_set_pressure(perf_fx_engine_t *e, int slot, float pressure) {
    if (slot < 0 || slot >= PFX_NUM_FX) return;
    pfx_slot_t *s = &e->slots[slot];
    s->pressure = clampf(pressure, 0.0f, 1.0f);

    /* Settling: track pressure as center point so pressure_relative
     * returns 0.5 (neutral). Once settled, center locks.
     * -1 = waiting for first aftertouch (start settle window now).
     * >0 = settling in progress, track pressure as center. */
    if (s->settle_counter == -1) {
        /* First aftertouch arrived — start settling window */
        s->settle_counter = PFX_SAMPLE_RATE / 5;  /* 200ms */
        s->velocity = s->pressure;
    } else if (s->settle_counter > 0) {
        s->velocity = s->pressure;
    }
}

void pfx_set_param(perf_fx_engine_t *e, int slot, int idx, float val) {
    if (slot < 0 || slot >= PFX_NUM_FX) return;
    if (idx < 0 || idx >= PFX_SLOT_PARAMS) return;
    e->slots[slot].params[idx] = clampf(val, 0.0f, 1.0f);
}

void pfx_set_latched(perf_fx_engine_t *e, int slot, int latched) {
    if (slot < 0 || slot >= PFX_NUM_FX) return;
    pfx_slot_t *s = &e->slots[slot];
    s->latched = latched;

    if (latched && !s->active) {
        /* Latching an inactive slot: activate it */
        pfx_activate(e, slot, 0.7f);
    }
    if (!latched && s->active) {
        /* Unlatching: if pad is not physically held, deactivate.
         * For space FX in latched mode = continuous processing,
         * unlatching switches to tail decay. */
        if (s->pressure <= 0.0f) {
            /* Force deactivate by temporarily clearing latched */
            s->latched = 0;
            pfx_deactivate(e, slot);
        }
    }
}

/* ============================================================
 * Row 4: Time/Repeat FX processing (slots 0-7)
 * ============================================================ */

/* Beat repeat: let audio play through for one division (capturing into
 * the capture buffer naturally), then loop from the capture buffer.
 * Phase 1 (capturing=1): pass through live audio, count samples
 * Phase 2 (capturing=0): loop repeat_len samples from capture buffer
 *
 * repeat.capturing = 1 during pass-through, 0 during repeat
 * repeat.frames_captured = samples counted during pass-through
 * repeat.write_pos = capture buffer position saved when repeat begins
 * repeat.read_pos = offset within repeat region during playback */
/* Tone-shaping applied to a captured loop, shared by the beat repeats, Stutter
 * and Reverse — they differ in how they pick the loop, not in what they do to
 * it once they have it. Filter and gate are passed explicitly because the
 * callers keep them on different knobs (Reverse spends param 0 on Length).
 *
 * filt: 0 = dark LP, 0.5 = bypass, 1.0 = bright HP
 * gate: 0 = open, 1.0 = choppy (4 gates per loop cycle) */
static void repeat_shape(pfx_slot_t *s, float *l, float *r, int pos, int len,
                          float filt, float gate) {
    if (filt < 0.45f) {
        float cutoff = 0.1f + (filt / 0.45f) * 0.9f;
        float f = cutoff_to_f(cutoff);
        svf_process(&s->filter_l, *l, f, 0.4f, l, NULL, NULL);
        svf_process(&s->filter_r, *r, f, 0.4f, r, NULL, NULL);
    } else if (filt > 0.55f) {
        float cutoff = ((filt - 0.55f) / 0.45f) * 0.8f;
        float f = cutoff_to_f(cutoff);
        float hp_l, hp_r;
        svf_process(&s->filter_l, *l, f, 0.4f, NULL, &hp_l, NULL);
        svf_process(&s->filter_r, *r, f, 0.4f, NULL, &hp_r, NULL);
        *l = hp_l;
        *r = hp_r;
    }

    if (gate > 0.05f && len > 0) {
        float duty = 1.0f - gate * 0.85f;   /* 1.0 -> 0.15 */
        float sub_phase = ((float)pos / (float)len) * 4.0f;
        sub_phase = sub_phase - (int)sub_phase;
        if (sub_phase >= duty) { *l = 0.0f; *r = 0.0f; }
    }
}

/* Per-cycle decay for the looping effects: each pass through the captured
 * material comes back quieter, so a held repeat falls away instead of running
 * forever at full level. Knob at 0 means no decay at all, which is how these
 * behaved before the control existed. Called once per loop wrap. */
static void repeat_decay_step(repeat_t *rp, float decay) {
    if (decay <= 0.001f) { rp->decay_gain = 1.0f; return; }
    rp->decay_gain *= (1.0f - decay * 0.5f);
    if (rp->decay_gain < 0.0001f) rp->decay_gain = 0.0f;
}

static void process_beat_repeat(pfx_slot_t *s, int slot, float *l, float *r,
                                 perf_fx_engine_t *e) {
    repeat_t *rp = &s->repeat;
    (void)slot;

    if (rp->capturing) {
        /* Phase 1: pass through live audio, count down one division */
        rp->frames_captured++;
        if (rp->frames_captured >= rp->repeat_len) {
            /* Division complete — copy loop segment into private buffer
             * so it persists even as row4_buf gets overwritten. */
            rp->capturing = 0;
            int src_start = (e->row4_write_pos - rp->repeat_len + e->row4_buf_len) % e->row4_buf_len;
            int copy_len = rp->repeat_len;
            if (copy_len > rp->buf_len) copy_len = rp->buf_len;
            for (int i = 0; i < copy_len; i++) {
                int src = (src_start + i) % e->row4_buf_len;
                rp->buf_l[i] = e->row4_buf_l[src];
                rp->buf_r[i] = e->row4_buf_r[src];
            }
            rp->repeat_len = copy_len;
            rp->read_pos = 0;
            rp->xfade_pos = 0;
            rp->xfade_len = 64;  /* ~1.5ms crossfade at loop boundaries */
        }
        /* Output = live audio (unchanged *l, *r) */
        return;
    }

    /* Phase 2: loop from private buffer (frozen audio) */
    float gain = pressure_volume_gain(s->pressure, s->velocity, s->settle_counter);

    gain *= rp->decay_gain;                      /* params[2] = Decay */
    float loop_l = rp->buf_l[rp->read_pos] * gain;
    float loop_r = rp->buf_r[rp->read_pos] * gain;

    repeat_shape(s, &loop_l, &loop_r, rp->read_pos, rp->repeat_len,
                 s->params[0], s->params[1]);

    /* Crossfade from live to loop at initial transition only.
     * Skip crossfade at loop-back points (xfade_len=0 after first). */
    if (rp->xfade_pos < rp->xfade_len) {
        float t = (float)rp->xfade_pos / (float)rp->xfade_len;
        *l = *l * (1.0f - t) + loop_l * t;
        *r = *r * (1.0f - t) + loop_r * t;
        rp->xfade_pos++;
    } else {
        *l = loop_l;
        *r = loop_r;
    }

    /* Speed control: 0=stop, 0.5=normal, 1.0=2x */
    float speed = e->repeat_speed * 2.0f;
    rp->repeat_pos += (int)(speed * 256.0f);  /* 8.8 fixed-point accumulator */
    int advance = rp->repeat_pos >> 8;
    rp->repeat_pos &= 0xFF;
    rp->read_pos += advance;

    if (rp->read_pos >= rp->repeat_len) {
        rp->read_pos = 0;
        repeat_decay_step(rp, s->params[2]);
    }

    /* Check if rate knob changed — apply at loop boundary or wrap.
     * Changing mid-loop just adjusts repeat_len so the loop wraps sooner/later.
     * Only re-capture when crossing the boundary. */
    {
        int new_len = pfx_rate_to_samples(e->repeat_rate);
        if (new_len < 64) new_len = 64;
        if (new_len > rp->buf_len) new_len = rp->buf_len;
        if (new_len != rp->repeat_len) {
            if (rp->read_pos == 0) {
                /* At loop start — re-capture at new length */
                int src_start = (e->row4_write_pos - new_len + e->row4_buf_len) % e->row4_buf_len;
                for (int i = 0; i < new_len; i++) {
                    int src = (src_start + i) % e->row4_buf_len;
                    rp->buf_l[i] = e->row4_buf_l[src];
                    rp->buf_r[i] = e->row4_buf_r[src];
                }
            }
            rp->repeat_len = new_len;
            /* If read_pos is now past the new length, wrap immediately */
            if (rp->read_pos >= new_len) rp->read_pos = rp->read_pos % new_len;
        }
    }
}

static void process_stutter(pfx_slot_t *s, float *l, float *r,
                             perf_fx_engine_t *e) {
    repeat_t *rp = &s->repeat;

    if (rp->capturing) {
        /* Pass through, count down */
        rp->frames_captured++;
        if (rp->frames_captured >= rp->repeat_len) {
            /* Copy loop segment into private buffer */
            rp->capturing = 0;
            int src_start = (e->row4_write_pos - rp->repeat_len + e->row4_buf_len) % e->row4_buf_len;
            int copy_len = rp->repeat_len;
            if (copy_len > rp->buf_len) copy_len = rp->buf_len;
            for (int i = 0; i < copy_len; i++) {
                int src = (src_start + i) % e->row4_buf_len;
                rp->buf_l[i] = e->row4_buf_l[src];
                rp->buf_r[i] = e->row4_buf_r[src];
            }
            rp->repeat_len = copy_len;
            rp->read_pos = 0;
            rp->xfade_pos = 0;
            rp->xfade_len = 128;
        }
        return;
    }

    /* Pressure shrinks stutter length for faster glitchy repeats */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    int stutter_len = 64 + (int)((1.0f - pr) * (rp->repeat_len - 64));
    if (stutter_len < 64) stutter_len = 64;
    if (stutter_len > rp->repeat_len) stutter_len = rp->repeat_len;

    /* Read from private buffer (frozen audio) */
    float loop_l = rp->buf_l[rp->read_pos] * rp->decay_gain;
    float loop_r = rp->buf_r[rp->read_pos] * rp->decay_gain;

    repeat_shape(s, &loop_l, &loop_r, rp->read_pos, stutter_len,
                 s->params[0], s->params[1]);

    if (rp->xfade_pos < rp->xfade_len) {
        float t = (float)rp->xfade_pos / (float)rp->xfade_len;
        *l = *l * (1.0f - t) + loop_l * t;
        *r = *r * (1.0f - t) + loop_r * t;
        rp->xfade_pos++;
    } else {
        *l = loop_l;
        *r = loop_r;
    }

    rp->read_pos++;
    if (rp->read_pos >= stutter_len) {
        rp->read_pos = 0;
        rp->xfade_pos = 0;
        repeat_decay_step(rp, s->params[2]);
    }
}

/*
 * Scatter: tempo-synced slice rearrangement (SP-404 style).
 *
 * Fixed 8-step pattern of slice indices: [0,1,2,1,4,3,6,5]
 * Slices are 1/16th note, read from capture buffer.
 *
 * Pressure (0.0–1.0) continuously modulates:
 *   Gate ratio:     1.0 at p=0  →  0.4 at p=1 (rest of slice silent)
 *   Reverse weight: 0.0 at p=0  →  0.5 at p=1 (deterministic per step)
 *
 * 64-sample crossfade at every slice boundary.
 */

#define SCATTER_STEPS    8
#define SCATTER_PATTERNS 5

/* Slice orders, selected by params[0]. Index 0 is the order this had when it
 * was the only one, so the Pattern knob at rest reproduces the old arrangement.
 * Each entry says which slice of the latched bar to play on that step. */
static const int scatter_patterns[SCATTER_PATTERNS][SCATTER_STEPS] = {
    { 0, 1, 2, 1, 4, 3, 6, 5 },   /* Shuffle    — keeps the pulse, nudges it */
    { 0, 0, 2, 2, 4, 4, 6, 6 },   /* Stutter    — every slice played twice */
    { 0, 4, 1, 5, 2, 6, 3, 7 },   /* Interleave — halves of the bar alternate */
    { 0, 3, 1, 7, 2, 5, 4, 6 },   /* Scramble   — no two neighbours adjacent */
    { 7, 6, 5, 4, 3, 2, 1, 0 }    /* Retrograde — the bar back to front */
};

/* Deterministic reverse decision per step index.
 * Returns 1 if this step should be reversed at the given reverse weight.
 * Uses a fixed threshold table so behavior is stable while pressure holds. */
static inline int scatter_is_reversed(int step, float rev_weight) {
    /* Per-step thresholds: step must exceed this to reverse */
    static const float rev_thresh[SCATTER_STEPS] = {
        0.45f, 0.30f, 0.50f, 0.20f, 0.40f, 0.35f, 0.25f, 0.48f
    };
    return rev_weight > rev_thresh[step];
}

static void process_scatter(pfx_slot_t *s, float *l, float *r,
                             perf_fx_engine_t *e) {
    repeat_t *rp = &s->repeat;

    /* Slice length = 1/16th note */
    int slice_len = pfx_bpm_to_samples(e->bpm, 0.25f);
    if (slice_len < 128) slice_len = 128;
    if (slice_len > e->row4_buf_len / SCATTER_STEPS)
        slice_len = e->row4_buf_len / SCATTER_STEPS;

    /* Knobs set the base, pressure pushes either side of it.
     * params[0] = Pattern (which slice order)
     * params[1] = Gate    (how much of each slice is silenced)
     * params[2] = Revrse  (how many steps play backwards) */
    float p = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float gate_amt = pfx_mod(s->params[1], p, 0.6f);
    float rev_weight = pfx_mod(s->params[2], p, 0.5f) * 0.5f;
    float gate_ratio = 1.0f - gate_amt * 0.6f;  /* 1.0 → 0.4 */
    int gate_len = (int)(slice_len * gate_ratio);
    if (gate_len < 64) gate_len = 64;

    /* Slice boundary — set up next slice */
    if (rp->repeat_pos <= 0) {
        int step = (rp->write_pos + 1) % SCATTER_STEPS;
        rp->write_pos = step;

        /* Pattern is only consulted here, not per sample. */
        int pat = (int)(s->params[0] * (float)(SCATTER_PATTERNS - 1) + 0.5f);
        if (pat < 0) pat = 0;
        if (pat >= SCATTER_PATTERNS) pat = SCATTER_PATTERNS - 1;

        /* Latch the bar at the top of each cycle.
         *
         * This used to recompute the base from the live write position on
         * *every* step, so the window being rearranged slid forward with the
         * playhead — by the back half of the pattern it was playing nearly
         * current audio, which is why it read as the source with holes punched
         * in it rather than as rearrangement. Latching at step 0 means all
         * eight steps rearrange one fixed bar, and re-latching each cycle keeps
         * it musical and keeps the read inside the 4s row4 buffer. */
        if (step == 0) {
            rp->bar_start = (e->row4_write_pos - SCATTER_STEPS * slice_len
                            + e->row4_buf_len) % e->row4_buf_len;
        }

        /* Look up which slice index to play from the selected pattern */
        int slice_idx = scatter_patterns[pat][step];
        int slice_start = (rp->bar_start + slice_idx * slice_len) % e->row4_buf_len;

        /* Determine direction */
        int reversed = scatter_is_reversed(step, rev_weight);

        rp->repeat_pos = slice_len;     /* total time for this step */
        rp->repeat_len = gate_len;      /* audible portion */
        rp->frames_captured = 0;        /* samples consumed */
        rp->capturing = reversed;       /* 0=fwd, 1=rev */

        if (reversed)
            rp->read_pos = (slice_start + slice_len - 1) % e->row4_buf_len;
        else
            rp->read_pos = slice_start;

        /* Crossfade at slice boundary */
        rp->xfade_pos = 0;
        rp->xfade_len = 64;
    }

    if (rp->frames_captured < rp->repeat_len) {
        /* Audible portion — read from row4 buffer */
        float samp_l = e->row4_buf_l[rp->read_pos];
        float samp_r = e->row4_buf_r[rp->read_pos];

        /* Crossfade at slice start */
        if (rp->xfade_pos < rp->xfade_len) {
            float t = (float)rp->xfade_pos / (float)rp->xfade_len;
            *l = *l * (1.0f - t) + samp_l * t;
            *r = *r * (1.0f - t) + samp_r * t;
            rp->xfade_pos++;
        } else {
            *l = samp_l;
            *r = samp_r;
        }

        /* Advance read position */
        if (rp->capturing)
            rp->read_pos = (rp->read_pos - 1 + e->row4_buf_len) % e->row4_buf_len;
        else
            rp->read_pos = (rp->read_pos + 1) % e->row4_buf_len;
    } else {
        /* Silent gate portion */
        *l = 0.0f;
        *r = 0.0f;
    }

    rp->frames_captured++;
    rp->repeat_pos--;
}

static void process_reverse(pfx_slot_t *s, float *l, float *r,
                             perf_fx_engine_t *e) {
    repeat_t *rp = &s->repeat;

    /* Pressure → volume (relative to initial hit) */
    float gain = pressure_volume_gain(s->pressure, s->velocity, s->settle_counter);

    if (rp->read_pos >= 0 && rp->read_pos < rp->repeat_len) {
        gain *= rp->decay_gain;                  /* params[2] = Decay */
        float rev_l = rp->buf_l[rp->read_pos] * gain;
        float rev_r = rp->buf_r[rp->read_pos] * gain;

        /* params[1] = Filter (params[0] is Length here). No gate on Reverse. */
        repeat_shape(s, &rev_l, &rev_r, rp->read_pos, rp->repeat_len,
                     s->params[1], 0.0f);

        /* Crossfade from live to reverse at start */
        if (rp->xfade_pos < rp->xfade_len) {
            float t = (float)rp->xfade_pos / (float)rp->xfade_len;
            *l = *l * (1.0f - t) + rev_l * t;
            *r = *r * (1.0f - t) + rev_r * t;
            rp->xfade_pos++;
        } else {
            *l = rev_l;
            *r = rev_r;
        }
    }

    rp->read_pos--;
    if (rp->read_pos < 0) {
        /* Reached the start — re-capture at the current Length and loop */
        repeat_decay_step(rp, s->params[2]);
        pfx_reverse_recapture(e, s);
    }
}

/* Bungee timestretch: slow the audio down without changing its pitch.
 * Called per-sample. Accumulates input into bungee, reads stretched output.
 *
 * params[0] = Speed base, params[1] = Filter (tone of the stretched output) */
static void process_stretch(pfx_slot_t *s, float *l, float *r,
                             perf_fx_engine_t *e) {
    (void)e;
    pfx_bungee_t *b = (pfx_bungee_t *)s->bungee;
    if (!b) return;

    /* Knob picks the resting speed, pressure moves it: harder = slower.
     * The knob's own curve is 0 -> 1.0x, 0.5 -> 0.5x, 1.0 -> 0.25x. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float k = pfx_mod(s->params[0], pr, 1.0f);
    float speed = (k < 0.5f) ? (1.0f - k)            /* 1.0 -> 0.5 */
                             : (0.5f - (k - 0.5f) * 0.5f);  /* 0.5 -> 0.25 */
    pfx_bungee_set_speed(b, speed);

    /* Feed this sample to bungee */
    float in_lr[2] = { *l, *r };
    pfx_bungee_write(b, in_lr, 1);

    /* Read one stretched sample */
    float out_lr[2] = { 0.0f, 0.0f };
    pfx_bungee_read(b, out_lr, 1);
    *l = out_lr[0];
    *r = out_lr[1];

    /* params[1] = Filter, same LP/bypass/HP shape as the beat repeats */
    float filt = s->params[1];
    if (filt < 0.45f) {
        float f = cutoff_to_f(0.1f + (filt / 0.45f) * 0.9f);
        svf_process(&s->filter_l, *l, f, 0.4f, l, NULL, NULL);
        svf_process(&s->filter_r, *r, f, 0.4f, r, NULL, NULL);
    } else if (filt > 0.55f) {
        float f = cutoff_to_f(((filt - 0.55f) / 0.45f) * 0.8f);
        float hp_l, hp_r;
        svf_process(&s->filter_l, *l, f, 0.4f, NULL, &hp_l, NULL);
        svf_process(&s->filter_r, *r, f, 0.4f, NULL, &hp_r, NULL);
        *l = hp_l; *r = hp_r;
    }
}

/* ============================================================
 * Row 3: Filter Sweep FX (slots 8-15)
 * Phase counter 0->1 drives the sweep while active
 * ============================================================ */

/* Depth knob -> sweep excursion multiplier, centred so 0.5 returns exactly
 * 1.0 and the sweep keeps the full travel it had before the knob existed. */
static inline float pfx_depth_scale(float k) {
    if (k < 0.5f) return 0.2f + k * 1.6f;       /* 0.2x -> 1x */
    return 1.0f + (k - 0.5f) * 1.6f;            /* 1x -> 1.8x, clamped later */
}

/* Speed knob -> rate multiplier, centred so 0.5 returns exactly 1.0 and the
 * effect keeps whatever rate it was hand-tuned to before the knob existed. */
static inline float pfx_speed_mult(float k) {
    if (k < 0.5f) return 0.25f + k * 1.5f;      /* 0.25x -> 1x */
    return 1.0f + (k - 0.5f) * 6.0f;            /* 1x -> 4x */
}

/* Trapezoidal shape from phase 0→4:
 * 0→1: ramp up, 1→2: hold high, 2→3: ramp down, 3→4: hold low */
static inline float phase_to_trapezoid(float phase) {
    if (phase < 1.0f) return phase;        /* sweep 0→1 */
    if (phase < 2.0f) return 1.0f;         /* hold at 1 */
    if (phase < 3.0f) return 3.0f - phase; /* sweep 1→0 */
    return 0.0f;                           /* hold at 0 */
}

/* Advance phase per-sample. */
static void advance_filter_phase(pfx_slot_t *s, int slot, float bpm) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);

    /* Trapezoidal sweeps (LP, HP, BP, Reso):
     * Phase 0→4: sweep(1 bar) → hold(1 bar) → sweep back(1 bar) → hold(1 bar)
     * Rate = 1 phase unit per bar = bpm/(60*4) per second = bpm/(60*4*SR) per sample */
    if (slot == FX_LP_SWEEP_DOWN || slot == FX_HP_SWEEP_UP ||
        slot == FX_BP_RISE || slot == FX_BP_FALL || slot == FX_RESO_SWEEP) {
        float b = bpm < 20.0f ? 120.0f : bpm;
        /* params[0] = Speed: 0.5 keeps the one-bar-per-stage sweep these had
         * before the knob existed; either side scales it 0.25x .. 4x. */
        float rate = b / (60.0f * 4.0f * PFX_SAMPLE_RATE);
        rate *= pfx_speed_mult(s->params[0]);
        s->phase += rate;
        if (s->phase >= 4.0f) s->phase -= 4.0f;
        return;
    }

    /* Auto filter: beat-synced, pressure controls speed.
     * Default (neutral) = 1/4 note (1 cycle per beat).
     * Less pressure → 1/2 note (0.5 cycles/beat).
     * More pressure → 1/16 note (4 cycles/beat). */
    if (slot == FX_AUTO_FILTER) {
        float b = bpm < 20.0f ? 120.0f : bpm;
        /* 1 cycle per beat base rate */
        float beat_rate = b / (60.0f * PFX_SAMPLE_RATE);
        /* Pressure mapping: pr 0→0.5→1.0 maps to mult 0.5→1.0→4.0 */
        float rate_mult;
        if (pr < 0.5f) {
            rate_mult = 0.5f + pr;          /* 0.5 → 1.0 */
        } else {
            rate_mult = 1.0f + (pr - 0.5f) * 6.0f;  /* 1.0 → 4.0 */
        }
        s->phase += beat_rate * rate_mult;
        if (s->phase >= 1.0f) s->phase -= 1.0f;
        return;
    }

    /* LFO-based FX (phaser, flanger): ~2 second cycle */
    float base_rate = 1.0f / (2.0f * PFX_SAMPLE_RATE);
    switch (slot) {
        case FX_PHASER:
        case FX_FLANGER:
            base_rate *= (0.3f + pr * 4.0f);
            break;
        default:
            break;
    }
    s->phase += base_rate;
    if (s->phase >= 1.0f) s->phase -= 1.0f;
}

static void process_lp_sweep_down(pfx_slot_t *s, float *l, float *r) {
    /* Trapezoidal phase: sweep down, hold, sweep up, hold */
    float tri = phase_to_trapezoid(s->phase);

    /* Base sweep: open (tri=0) → nearly closed (tri=1).
     * params[2] = Depth scales the excursion; 0.5 is the original full travel. */
    float sweep_cutoff = 1.0f - tri * 0.95f * pfx_depth_scale(s->params[2]);

    /* Pressure opens the filter (inverse): harder press = higher cutoff.
     * At center (0.5) = no offset. Above center = pushes cutoff up. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float pressure_offset = (pr - 0.5f) * 1.5f;  /* -0.75 to +0.75 */

    float cutoff = sweep_cutoff + pressure_offset;
    if (cutoff < 0.02f) cutoff = 0.02f;
    if (cutoff > 1.0f) cutoff = 1.0f;

    float f = cutoff_to_f(cutoff);
    /* resonance increases as it sweeps down; params[1] = Reso scales it */
    float q = (0.2f + tri * 0.5f) * pfx_reso_scale(s->params[1]);
    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, q, &out_l, NULL, NULL);
    svf_process(&s->filter_r, *r, f, q, &out_r, NULL, NULL);
    *l = out_l; *r = out_r;
}

static void process_hp_sweep_up(pfx_slot_t *s, float *l, float *r) {
    /* Trapezoidal phase: sweep up, hold, sweep down, hold */
    float tri = phase_to_trapezoid(s->phase);

    /* Base sweep: open (tri=0) → high-passed (tri=1).
     * params[2] = Depth scales the excursion. */
    float sweep_cutoff = tri * 0.9f * pfx_depth_scale(s->params[2]);

    /* Pressure pushes cutoff higher (harder = more HP) */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float pressure_offset = (pr - 0.5f) * 1.0f;

    float cutoff = sweep_cutoff + pressure_offset;
    if (cutoff < 0.0f) cutoff = 0.0f;
    if (cutoff > 0.95f) cutoff = 0.95f;

    float f = cutoff_to_f(cutoff);
    float q = (0.2f + tri * 0.4f) * pfx_reso_scale(s->params[1]);
    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, q, NULL, &out_l, NULL);
    svf_process(&s->filter_r, *r, f, q, NULL, &out_r, NULL);
    *l = out_l; *r = out_r;
}

static void process_bp_rise(pfx_slot_t *s, float *l, float *r) {
    /* Trapezoidal phase: sweep low→high, hold, sweep high→low, hold */
    float tri = phase_to_trapezoid(s->phase);
    float sweep_cutoff = 0.1f + tri * 0.7f * pfx_depth_scale(s->params[2]);

    /* Pressure offsets cutoff */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float cutoff = sweep_cutoff + (pr - 0.5f) * 0.6f;
    cutoff = clampf(cutoff, 0.05f, 0.9f);

    float f = cutoff_to_f(cutoff);
    float q = (0.1f + (1.0f - tri) * 0.3f) * pfx_reso_scale(s->params[1]);
    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, q, NULL, NULL, &out_l);
    svf_process(&s->filter_r, *r, f, q, NULL, NULL, &out_r);
    *l = out_l; *r = out_r;
}

static void process_bp_fall(pfx_slot_t *s, float *l, float *r) {
    /* Trapezoidal phase: sweep high→low, hold, sweep low→high, hold */
    float tri = phase_to_trapezoid(s->phase);
    float sweep_cutoff = 0.8f - tri * 0.7f * pfx_depth_scale(s->params[2]);

    /* Pressure offsets cutoff */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float cutoff = sweep_cutoff + (pr - 0.5f) * 0.6f;
    cutoff = clampf(cutoff, 0.05f, 0.9f);

    float f = cutoff_to_f(cutoff);
    float q = (0.1f + tri * 0.3f) * pfx_reso_scale(s->params[1]);
    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, q, NULL, NULL, &out_l);
    svf_process(&s->filter_r, *r, f, q, NULL, NULL, &out_r);
    *l = out_l; *r = out_r;
}

static void process_reso_sweep(pfx_slot_t *s, float *l, float *r) {
    /* Trapezoidal phase: resonant peak sweeps up, hold, down, hold */
    float tri = phase_to_trapezoid(s->phase);
    float sweep_cutoff = 0.15f + tri * 0.6f;

    /* Pressure offsets cutoff */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float cutoff = sweep_cutoff + (pr - 0.5f) * 0.5f;
    cutoff = clampf(cutoff, 0.05f, 0.9f);

    float f = cutoff_to_f(cutoff);
    /* very high Q, pressure tightens further; params[1] = Reso scales it */
    float q = (0.05f + (1.0f - pr) * 0.03f) * pfx_reso_scale(s->params[1]);
    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, q, NULL, NULL, &out_l);
    svf_process(&s->filter_r, *r, f, q, NULL, NULL, &out_r);
    /* Dry blend is the Mix knob's job now — see apply_wet(). */
    *l = out_l;
    *r = out_r;
}

static void process_phaser_fx(pfx_slot_t *s, float *l, float *r) {
    phaser_t *ph = &s->phaser;
    float depth = 0.5f + s->params[0] * 0.5f;
    float fb = 0.3f + s->params[1] * 0.5f;

    float lfo = sinf(s->phase * 2.0f * M_PI);
    float mono = (*l + *r) * 0.5f + ph->ap[0].y1 * fb * 0.3f;
    float base_freq = 200.0f + depth * 3000.0f * (0.5f + 0.5f * lfo);
    float out = mono;

    for (int i = 0; i < PFX_NUM_ALLPASS; i++) {
        float freq = base_freq * (1.0f + (float)i * 0.3f);
        float w = 2.0f * M_PI * freq / PFX_SAMPLE_RATE;
        float cosw = cosf(w);
        float a = (cosw == 0.0f) ? 0.0f : (1.0f - sinf(w)) / cosw;
        a = clampf(a, -0.99f, 0.99f);
        float x = out;
        float y = -a * x + ph->ap[i].y1;
        ph->ap[i].y1 = flush_denormal(a * y + x);
        out = y;
    }

    /* Dry blend is the Mix knob's job now — see apply_wet(). Mix defaults to
     * 0.7 for this slot, which is the 0.3/0.7 split this used to hardcode. */
    *l = out;
    *r = out;
}

static void process_flanger_fx(pfx_slot_t *s, float *l, float *r) {
    mod_delay_t *md = &s->mod_delay;
    if (!md->buf_l) return;

    float depth = 1.0f + s->params[0] * 30.0f;
    float fb = 0.5f + s->params[1] * 0.4f;

    float lfo = sinf(s->phase * 2.0f * M_PI);
    float delay_samples = 5.0f + depth * (0.5f + 0.5f * lfo);

    int wp = md->write_pos;
    int rp = (wp - (int)delay_samples + md->buf_len) % md->buf_len;
    float dl = md->buf_l[rp];
    float dr = md->buf_r[rp];

    md->buf_l[wp] = *l + dl * fb;
    md->buf_r[wp] = *r + dr * fb;
    md->write_pos = (wp + 1) % md->buf_len;

    /* Dry blend is the Mix knob's job now (default 0.6 = the old 0.4/0.6). */
    *l = dl;
    *r = dr;
}

static void process_auto_filter(pfx_slot_t *s, float *l, float *r) {
    float lfo = sinf(s->phase * 2.0f * M_PI);
    float depth = 0.3f + s->params[0] * 0.3f;
    /* params[1] = Reso, params[2] = Center. Sweep rate stays on pressure. */
    float center = 0.2f + s->params[2] * 0.5f;
    float reso = 0.14f * pfx_reso_scale(s->params[1]);

    float cutoff = center + lfo * depth;
    cutoff = clampf(cutoff, 0.01f, 0.95f);
    float f = cutoff_to_f(cutoff);

    float out_l, out_r;
    svf_process(&s->filter_l, *l, f, reso, &out_l, NULL, NULL);
    svf_process(&s->filter_r, *r, f, reso, &out_r, NULL, NULL);
    *l = out_l; *r = out_r;
}

/* ============================================================
 * Row 2: Space Throw FX (slots 16-23)
 * Audio feeds in while held, tail decays on release
 * ============================================================ */

/* beat_mult: 1.0 = quarter note, 0.75 = dotted 8th */
static void process_delay_throw(pfx_slot_t *s, float *l, float *r,
                                 int feeding, float bpm, float beat_mult) {
    delay_t *d = &s->delay;
    /* params[1] = Tone. This used to read params[2], which the UI labelled
     * "Level" — so the tone control was on the wrong knob and the two knobs
     * labelled Feedbk and Tone did nothing at all. */
    float filt = s->params[1];

    float b = bpm < 20.0f ? 120.0f : bpm;
    int delay_samples = (int)(60.0f / b * beat_mult * PFX_SAMPLE_RATE);
    if (delay_samples < 100) delay_samples = 100;
    if (delay_samples > PFX_SAMPLE_RATE) delay_samples = PFX_SAMPLE_RATE;

    /* params[0] = Feedbk base, pressure moves it. Capped at 0.5 for quick decay. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float fb = 0.2f + pfx_mod(s->params[0], pr, 0.6f) * 0.3f;  /* 0.2 → 0.5 */

    float dl, dr;
    delay_read(d, delay_samples, &dl, &dr);

    float f_coeff = 0.1f + filt * 0.8f;
    d->fb_lp_l += f_coeff * (dl - d->fb_lp_l);
    d->fb_lp_r += f_coeff * (dr - d->fb_lp_r);

    if (feeding)
        delay_write(d, *l + d->fb_lp_l * fb, *r + d->fb_lp_r * fb);
    else
        delay_write(d, d->fb_lp_l * fb, d->fb_lp_r * fb);

    /* Send topology: add the wet at unity here, apply_wet() scales it by the
     * Level knob (default 0.5 -> 0.7x, the factor this used to hardcode). */
    *l += dl;
    *r += dr;
}

static void process_ping_pong_throw(pfx_slot_t *s, float *l, float *r,
                                     int feeding, float bpm, float beat_mult) {
    delay_t *d = &s->delay;

    /* True stereo ping-pong:
     * L tap at 1x delay, R tap at 2x delay.
     * Feedback crosses: L output feeds R input, R output feeds L input. */
    float b = bpm < 20.0f ? 120.0f : bpm;
    int half_delay = (int)(60.0f / b * beat_mult * PFX_SAMPLE_RATE);
    if (half_delay < 100) half_delay = 100;
    if (half_delay > PFX_SAMPLE_RATE / 2) half_delay = PFX_SAMPLE_RATE / 2;
    int full_delay = half_delay * 2;

    /* params[0] = Feedbk base, params[1] = Tone. Both were dead here: this
     * function read no params at all. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float fb = 0.2f + pfx_mod(s->params[0], pr, 0.6f) * 0.3f;

    /* Read L at 1x (short tap), R at 2x (long tap) */
    float dl_short, dr_short, dl_long, dr_long;
    delay_read(d, half_delay, &dl_short, &dr_short);
    delay_read(d, full_delay, &dl_long, &dr_long);

    /* Damping in the feedback path, same shape as the plain delays.
     *
     * This is the one place wiring the knobs up changed how something sounds:
     * ping-pong feedback was previously undamped, so repeats stayed bright all
     * the way down. Giving it a Tone knob means giving it a filter, and the
     * default matches the plain delays because DLY and PP are one family — a
     * player expects the same knob to do the same thing on both. */
    float f_coeff = 0.1f + s->params[1] * 0.8f;
    d->fb_lp_l += f_coeff * (dr_long - d->fb_lp_l);
    d->fb_lp_r += f_coeff * (dl_short - d->fb_lp_r);

    float in_l = feeding ? *l : 0.0f;
    float in_r = feeding ? *r : 0.0f;

    /* Cross-feed: R delay output feeds back into L, and vice versa */
    delay_write(d,
        in_l + d->fb_lp_l * fb,
        in_r + d->fb_lp_r * fb);

    /* L gets the short tap, R gets the long tap — creates the panning bounce.
     * Level scaling happens in apply_wet(). */
    *l += dl_short;
    *r += dr_long;
}

/* Reverb: per-sample processing via pfx_revsc (Costello/Soundpipe FDN reverb).
 * Pressure modulates feedback (decay). Each slot type has different base
 * feedback and LP cutoff to create Room, Hall, Dark Verb, Spring characters. */
static void process_reverb_throw(pfx_slot_t *s, int slot, float *l, float *r,
                                  int feeding) {
    pfx_revsc_t *rv = (pfx_revsc_t *)s->ext_instance;
    if (!rv) return;

    /* Per-slot character presets */
    float base_fb, lpfreq;
    switch (slot) {
        case FX_REVERB:    base_fb = 0.65f; lpfreq = 10000.0f; break; /* Room: tight, natural */
        case FX_HALL:      base_fb = 0.88f; lpfreq = 10000.0f; break; /* Hall: long, open */
        case FX_DARK_VERB: base_fb = 0.90f; lpfreq =  4000.0f; break; /* Dark: long, damped */
        case FX_SPRING:    base_fb = 0.82f; lpfreq = 6000.0f;  break; /* Spring: bright boing, mid-cut */
        default:           base_fb = 0.80f; lpfreq = 10000.0f; break;
    }

    /* params[0] = Decay base, pressure moves it: centre = the slot's own
     * character, full = 0.97. params[1] = Tone scales the damping cutoff.
     * Both knobs were dead here — all four reverbs ignored every param. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float decay = pfx_mod(s->params[0], pr, 1.0f);
    rv->feedback = base_fb + decay * (0.97f - base_fb);
    rv->lpfreq = lpfreq * (0.25f + s->params[1] * 1.5f);

    float in_l = feeding ? *l : 0.0f;
    float in_r = feeding ? *r : 0.0f;

    /* Spring: cut lows going in for that thin, metallic character */
    if (slot == FX_SPRING) {
        float hp_c = 0.05f; /* ~350 Hz highpass */
        s->filter_l.lp += hp_c * (in_l - s->filter_l.lp);
        s->filter_r.lp += hp_c * (in_r - s->filter_r.lp);
        in_l -= s->filter_l.lp;
        in_r -= s->filter_r.lp;
    }

    float out_l, out_r;
    pfx_revsc_process(rv, in_l, in_r, &out_l, &out_r);

    /* Send topology — apply_wet() applies the Level knob. */
    *l += out_l;
    *r += out_r;
}

/* ============================================================
 * Row 1: Distortion & Rhythm FX (slots 24-31)
 * ============================================================ */

/* Post-distortion tone control: a lowpass that is transparent at 1.0 and
 * progressively darker below it. Shared by Bitcrush, Downsample and Octave
 * Down, whose Tone knobs all default to 1.0 so wiring them up left the sound
 * of those effects unchanged. */
static void distort_tone(pfx_slot_t *s, float *l, float *r, float tone) {
    if (tone >= 0.95f) return;
    float f = cutoff_to_f(0.25f + tone * 0.7f);
    float fl, fr;
    svf_process(&s->sat_filter_l, *l, f, 0.6f, &fl, NULL, NULL);
    svf_process(&s->sat_filter_r, *r, f, 0.6f, &fr, NULL, NULL);
    *l = fl; *r = fr;
}

/* params[0] = Bits base (pressure moves it), params[1] = Tone */
static void process_bitcrush(pfx_slot_t *s, float *l, float *r) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    /* 8 bits (dramatic) down to 1 bit (destroyed) */
    float bits = 8.0f - pfx_mod(s->params[0], pr, 1.0f) * 7.0f;
    if (bits < 1.0f) bits = 1.0f;
    float levels = powf(2.0f, bits);
    *l = roundf(*l * levels) / levels;
    *r = roundf(*r * levels) / levels;
    distort_tone(s, l, r, s->params[1]);
}

/* params[0] = Rate base (pressure moves it), params[1] = Tone */
static void process_downsample(pfx_slot_t *s, float *l, float *r) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    /* Hold period 8 (noticeable) up to 64 (extreme) */
    int period = 8 + (int)(pfx_mod(s->params[0], pr, 1.0f) * 56.0f);

    s->crush_count++;
    if (s->crush_count >= (unsigned int)period) {
        s->crush_count = 0;
        s->crush_hold_l = *l;
        s->crush_hold_r = *r;
    }
    *l = s->crush_hold_l;
    *r = s->crush_hold_r;
    distort_tone(s, l, r, s->params[1]);
}

/* Vinyl brake: same as tape stop but slower with spindown character */
static void process_vinyl_brake(pfx_slot_t *s, float *l, float *r) {
    tape_stop_t *t = &s->tape;

    t->buf_l[t->write_pos] = *l;
    t->buf_r[t->write_pos] = *r;
    t->write_pos = (t->write_pos + 1) % t->buf_len;

    /* params[0] = Rate base, pressure moves it: harder = faster spindown. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float decel = 0.00002f + pfx_mod(s->params[0], pr, 1.0f) * 0.0002f;
    t->speed -= decel;
    if (t->speed < 0.0f) t->speed = 0.0f;

    if (t->speed > 0.01f) {
        int pos0 = ((int)t->read_pos) % t->buf_len;
        *l = t->buf_l[pos0];
        *r = t->buf_r[pos0];
        t->read_pos += t->speed;
        if (t->read_pos >= (float)t->buf_len)
            t->read_pos -= (float)t->buf_len;
    } else {
        int pos = ((int)t->read_pos) % t->buf_len;
        *l = t->buf_l[pos] * 0.97f;
        *r = t->buf_r[pos] * 0.97f;
    }

    /* params[2] = Tone: the platter dulls as it slows, tape-style */
    distort_tone(s, l, r, s->params[2]);

    /* params[1] = Noise: surface rumble as the platter slows */
    if (t->speed < 0.3f) {
        unsigned int seed = (unsigned int)(t->read_pos * 1000.0f);
        float noise = white_noise(&seed) * (0.3f - t->speed)
                    * (s->params[1] * 0.16f);
        *l += noise;
        *r += noise;
    }
}

/* params[0] = Drive base (pressure moves it), params[1] = Tone.
 * Drive used to be pressure-only while params[0] — labelled "Drive" in the UI —
 * silently drove the tone filter instead. */
static void process_saturate(pfx_slot_t *s, float *l, float *r) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    /* gentle (1.5x) up to heavy (12x) */
    float drive = 1.5f + pfx_mod(s->params[0], pr, 1.0f) * 10.5f;
    float tone = s->params[1];

    *l = fast_tanh(*l * drive);
    *r = fast_tanh(*r * drive);

    /* Tone filter */
    if (tone < 0.95f) {
        float f = cutoff_to_f(0.3f + tone * 0.65f);
        float fl, fr;
        svf_process(&s->sat_filter_l, *l, f, 0.5f, &fl, NULL, NULL);
        svf_process(&s->sat_filter_r, *r, f, 0.5f, &fr, NULL, NULL);
        *l = fl; *r = fr;
    }
    /* Dry blend is the Mix knob's job now (default 0.7 = the old 0.3/0.7). */
}

/* Pressure -> gate depth */
static void process_gate_duck(pfx_slot_t *s, float *l, float *r,
                               perf_fx_engine_t *e) {
    ducker_t *dk = &s->ducker;

    /* params[0] = Rate. BPM-synced; 0.5 is the quarter note this was fixed at.
     * Quantised to musical divisions rather than swept, because a gate that
     * drifts off the grid is not a gate. */
    static const float gate_div[5] = { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };
    int di = (int)(s->params[0] * 4.99f);
    if (di < 0) di = 0;
    if (di > 4) di = 4;
    float b = e->bpm < 20.0f ? 120.0f : e->bpm;
    float samples_per_cycle = (60.0f / b) * PFX_SAMPLE_RATE * (gate_div[di] * 4.0f);
    if (samples_per_cycle < 32.0f) samples_per_cycle = 32.0f;
    dk->phase += 1.0f / samples_per_cycle;
    if (dk->phase >= 1.0f) dk->phase -= 1.0f;

    /* params[1] = Duty base, pressure moves it: shorter = choppier. */
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    float duty = 0.15f + pfx_mod(s->params[1], pr, 1.0f) * 0.7f; /* 0.15 to 0.85 */

    /* Square gate: open when phase < duty, ducked otherwise.
     * params[2] = Depth is how far it ducks — this is the wet amount by its
     * real name, so the effect has no separate Mix. 1.0 = full gate. */
    float depth = s->params[2];
    float target_gain = (dk->phase < duty) ? 1.0f : (1.0f - depth);

    /* Very fast smoothing to avoid clicks (~1ms) */
    float coeff = 0.95f;
    dk->env = coeff * dk->env + (1.0f - coeff) * target_gain;

    *l *= dk->env;
    *r *= dk->env;
}

/* params[0] = Rate base (pressure moves it), params[1] = Depth.
 * Depth used to sit on params[0], which the UI labelled "Rate" — so the one
 * live knob on this effect moved the wrong thing. */
static void process_tremolo(pfx_slot_t *s, float *l, float *r) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    /* 2 Hz up to 20 Hz */
    float rate = 2.0f + pfx_mod(s->params[0], pr, 1.0f) * 18.0f;
    float depth = 0.5f + s->params[1] * 0.5f;

    s->trem_lfo_phase += rate / PFX_SAMPLE_RATE;
    if (s->trem_lfo_phase >= 1.0f) s->trem_lfo_phase -= 1.0f;

    /* params[2] = Shape morphs the LFO from sine to square. A Mix knob here
     * would only have duplicated Depth. */
    float lfo = sinf(s->trem_lfo_phase * 2.0f * M_PI);
    float shape = s->params[2];
    if (shape > 0.001f) {
        float sq = (lfo >= 0.0f) ? 1.0f : -1.0f;
        lfo = lfo + (sq - lfo) * shape;
    }
    float gain = 1.0f - depth * (0.5f + 0.5f * lfo);

    *l *= gain;
    *r *= gain;
}

/* Vinyl sim: SP-303/404-inspired.
 * Warm LP filter + light saturation + real vinyl crackle sample loop.
 * Pressure increases intensity. */
static void process_vinyl_sim(pfx_slot_t *s, float *l, float *r,
                               perf_fx_engine_t *e) {
    float pr = pressure_relative(s->pressure, s->velocity, s->settle_counter);
    /* params[1] = Warmth base (pressure moves it) drives the filter, the
     * saturation and the bass mono-ing; params[0] = Noise sets the crackle
     * independently, so you can have grit without mud or the reverse. */
    float intensity = 0.3f + pfx_mod(s->params[1], pr, 0.7f) * 0.7f;

    /* --- 1. Warmth: gentle LP at ~10kHz, more with pressure --- */
    float warmth_f = 0.85f - intensity * 0.25f;
    float warmth_q = 0.5f;
    float wl, wr;
    svf_process(&s->sat_filter_l, *l, warmth_f, warmth_q, &wl, NULL, NULL);
    svf_process(&s->sat_filter_r, *r, warmth_f, warmth_q, &wr, NULL, NULL);
    float blend = 0.3f + intensity * 0.4f;
    *l = *l * (1.0f - blend) + wl * blend;
    *r = *r * (1.0f - blend) + wr * blend;

    /* --- 2. Light saturation (soft clip for warmth/harmonics) --- */
    float sat_amt = 1.0f + intensity * 0.8f;
    *l = tanhf(*l * sat_amt) / sat_amt;
    *r = tanhf(*r * sat_amt) / sat_amt;

    /* --- 3. Bass mono-ification (M/S, collapse below ~200Hz) --- */
    float mid = (*l + *r) * 0.5f;
    float side = (*l - *r) * 0.5f;
    s->filter_l.lp += 0.03f * (side - s->filter_l.lp);
    side = side - s->filter_l.lp * intensity * 0.7f;
    *l = mid + side;
    *r = mid - side;

    /* --- 4. Crackle, from the sample loop when we have it --- */
    float crackle_vol = pfx_mod(s->params[0], pr, 0.7f) * 0.755f;
    float sample;

    if (e->vinyl_crackle_buf && e->vinyl_crackle_len > 0) {
        sample = (float)e->vinyl_crackle_buf[e->vinyl_crackle_pos] / 32768.0f;
        e->vinyl_crackle_pos++;
        if (e->vinyl_crackle_pos >= e->vinyl_crackle_len)
            e->vinyl_crackle_pos = 0;
    } else {
        /* Synthesised fallback. pfx_engine_load_vinyl_crackle() fails silently
         * on a missing or malformed WAV, and without this the effect lost its
         * defining feature and the Noise knob controlled nothing at all — with
         * no way to tell from the device that anything was wrong.
         *
         * Vinyl surface noise is mostly sparse ticks over a quiet hiss bed, so
         * that is what this generates: a ~0.3% chance per sample of a decaying
         * click, plus low-level noise. */
        float hiss = white_noise(&s->scatter_seed) * 0.05f;
        if (s->crush_hold_r > 0.0001f) {
            s->crush_hold_r *= 0.9994f;          /* click envelope */
        } else if ((s->scatter_seed >> 8) % 331u == 0u) {
            s->crush_hold_r = 0.35f + white_noise(&s->scatter_seed) * 0.2f;
            s->crush_hold_l = white_noise(&s->scatter_seed) > 0.0f ? 1.0f : -1.0f;
        }
        sample = hiss + s->crush_hold_r * s->crush_hold_l;
    }

    float crackle = sample * crackle_vol;
    *l += crackle;
    *r += crackle;
}

/* Pitch shift down via bungee (speed=1.0, pitch<1.0).
 * params[0] = Pitch: 0 -> two octaves down, 0.5 -> one octave (the fixed
 * behaviour this had), 1.0 -> unshifted. params[1] = Tone. */
static void process_pitch_down(pfx_slot_t *s, float *l, float *r) {
    pfx_bungee_t *b = (pfx_bungee_t *)s->bungee;
    if (!b) return;

    float k = s->params[0];
    float pitch = (k < 0.5f) ? (0.25f + k * 0.5f)          /* 0.25 -> 0.5 */
                             : (0.5f + (k - 0.5f) * 1.0f); /* 0.5  -> 1.0 */
    pfx_bungee_set_pitch(b, pitch);

    /* Feed live audio into bungee */
    float in_lr[2] = { *l, *r };
    pfx_bungee_write(b, in_lr, 1);

    /* Read pitch-shifted output */
    float out_lr[2] = { 0.0f, 0.0f };
    pfx_bungee_read(b, out_lr, 1);

    *l = out_lr[0];
    *r = out_lr[1];

    distort_tone(s, l, r, s->params[1]);
}

/* ============================================================
 * Process all active FX for one sample
 * ============================================================ */

static void process_slot(perf_fx_engine_t *e, int slot, float *l, float *r,
                          int feeding) {
    pfx_slot_t *s = &e->slots[slot];

    switch (slot) {
        /* Row 4: Time/Repeat */
        case FX_RPT_1_4:
        case FX_RPT_1_8:
        case FX_RPT_1_16:
        case FX_RPT_TRIP:
            process_beat_repeat(s, slot, l, r, e);
            break;
        case FX_STUTTER:
            process_stutter(s, l, r, e);
            break;
        case FX_SCATTER:
            process_scatter(s, l, r, e);
            break;
        case FX_REVERSE:
            process_reverse(s, l, r, e);
            break;
        case FX_STRETCH:
            process_stretch(s, l, r, e);
            break;

        /* Row 3: Filter Sweeps */
        case FX_LP_SWEEP_DOWN:
            advance_filter_phase(s, slot, e->bpm);
            process_lp_sweep_down(s, l, r);
            break;
        case FX_HP_SWEEP_UP:
            advance_filter_phase(s, slot, e->bpm);
            process_hp_sweep_up(s, l, r);
            break;
        case FX_BP_RISE:
            advance_filter_phase(s, slot, e->bpm);
            process_bp_rise(s, l, r);
            break;
        case FX_BP_FALL:
            advance_filter_phase(s, slot, e->bpm);
            process_bp_fall(s, l, r);
            break;
        case FX_RESO_SWEEP:
            advance_filter_phase(s, slot, e->bpm);
            process_reso_sweep(s, l, r);
            break;
        case FX_PHASER:
            advance_filter_phase(s, slot, e->bpm);
            process_phaser_fx(s, l, r);
            break;
        case FX_FLANGER:
            advance_filter_phase(s, slot, e->bpm);
            process_flanger_fx(s, l, r);
            break;
        case FX_AUTO_FILTER:
            advance_filter_phase(s, slot, e->bpm);
            process_auto_filter(s, l, r);
            break;

        /* Row 2: Space Throws */
        case FX_DELAY:
            process_delay_throw(s, l, r, feeding, e->bpm, 1.0f);   /* quarter note */
            break;
        case FX_DELAY_DOT8:
            process_delay_throw(s, l, r, feeding, e->bpm, 0.75f);  /* dotted 8th */
            break;
        case FX_PING_PONG:
            process_ping_pong_throw(s, l, r, feeding, e->bpm, 1.0f);   /* quarter note */
            break;
        case FX_PING_PONG_DOT8:
            process_ping_pong_throw(s, l, r, feeding, e->bpm, 0.75f);  /* dotted 8th */
            break;
        case FX_REVERB:   /* Room */
        case FX_HALL:
        case FX_DARK_VERB:
        case FX_SPRING:
            process_reverb_throw(s, slot, l, r, feeding);
            break;

        /* Row 1: Distortion & Rhythm */
        case FX_BITCRUSH:
            process_bitcrush(s, l, r);
            break;
        case FX_DOWNSAMPLE:
            process_downsample(s, l, r);
            break;
        case FX_SATURATE:
            process_saturate(s, l, r);
            break;
        case FX_GATE_DUCK:
            process_gate_duck(s, l, r, e);
            break;
        case FX_TREMOLO:
            process_tremolo(s, l, r);
            break;
        case FX_VINYL_SIM:
            process_vinyl_sim(s, l, r, e);
            break;
        case FX_PITCH_DOWN:
            process_pitch_down(s, l, r);
            break;
        case FX_VINYL_BRAKE:
            process_vinyl_brake(s, l, r);
            break;
    }
}

/* Wet amount, for the sixteen slots that declare one (see wet_param).
 *
 * Implemented once here rather than inside each effect, which is why the
 * process_* functions above no longer carry hardcoded 0.3/0.7-style blends —
 * those constants moved into the descriptor table as per-slot defaults.
 *
 * The two topologies are genuinely different and must not be conflated:
 *
 *   INSERT — the effect replaced the signal, so crossfade dry against wet.
 *            Mix 0 = bypass, Mix 1 = effect only.
 *
 *   SEND   — the effect added to the signal (delays, reverbs), so scale only
 *            what it added and always keep the dry. Level 0 = no send,
 *            Level 0.5 = the 0.7x these used to hardcode, Level 1 = 1.4x.
 *            Crossfading here instead would make a delay throw at full wet
 *            mute the track it is thrown from, which is not a delay throw. */
static void apply_wet(int slot, const float *params,
                      float dry_l, float dry_r, float *l, float *r) {
    /* Only slots that declare a wet control get one. Everywhere else every
     * param is an ordinary parameter and must not be read as a mix amount. */
    int wp = pfx_fx_desc[slot].wet_param;
    if (wp < 0) return;
    float wet_amt = params[wp];

    if (pfx_fx_desc[slot].topology == PFX_TOPO_SEND) {
        float g = wet_amt * 1.4f;
        *l = dry_l + (*l - dry_l) * g;
        *r = dry_r + (*r - dry_r) * g;
    } else {
        *l = dry_l + (*l - dry_l) * wet_amt;
        *r = dry_r + (*r - dry_r) * wet_amt;
    }
}

/* Process a single active slot with wet mix, fade-out and tail handling */
static void process_active_slot(perf_fx_engine_t *e, int i, float *l, float *r) {
    pfx_slot_t *s = &e->slots[i];

    int is_active = s->active || s->fading_out;
    int is_tail = s->tail_active;

    if (!is_active && !is_tail) return;

    /* Count down settling window */
    if (s->settle_counter > 0) s->settle_counter--;

    float dry_l = *l;
    float dry_r = *r;

    int feeding = s->active;
    process_slot(e, i, l, r, feeding);

    apply_wet(i, s->params, dry_l, dry_r, l, r);

    /* Apply fade-out crossfade for non-space FX */
    if (s->fading_out) {
        float fade = 1.0f - (float)s->fade_pos / (float)s->fade_len;
        *l = dry_l * (1.0f - fade) + *l * fade;
        *r = dry_r * (1.0f - fade) + *r * fade;
        s->fade_pos++;
        if (s->fade_pos >= s->fade_len) {
            s->active = 0;
            s->fading_out = 0;
        }
    }

    /* Check tail silence for space FX */
    if (is_tail && !is_active) {
        float max_out = fabsf(*l - dry_l);
        float max_out_r = fabsf(*r - dry_r);
        if (max_out_r > max_out) max_out = max_out_r;
        if (max_out < PFX_TAIL_THRESHOLD) {
            s->tail_silence_count++;
            if (s->tail_silence_count >= PFX_TAIL_SILENCE_FRAMES) {
                s->tail_active = 0;
            }
        } else {
            s->tail_silence_count = 0;
        }
    }
}

/*
 * Row 4 chain: reverse → repeats → scatter → half_speed
 *
 * Processed in fixed order so effects chain into each other.
 * After reverse runs, the signal is written to row4_buf so that
 * repeats and scatter loop from post-reverse audio.
 */
static void process_row4_chain(perf_fx_engine_t *e, float *l, float *r) {
    /* Stage 1: Reverse */
    process_active_slot(e, FX_REVERSE, l, r);

    /* Write post-reverse signal to Row 4 capture buffer.
     * This is what repeats/scatter will loop from. */
    e->row4_buf_l[e->row4_write_pos] = *l;
    e->row4_buf_r[e->row4_write_pos] = *r;
    e->row4_write_pos = (e->row4_write_pos + 1) % e->row4_buf_len;

    /* Stage 2: Beat repeats and stutter */
    process_active_slot(e, FX_RPT_1_4, l, r);
    process_active_slot(e, FX_RPT_1_8, l, r);
    process_active_slot(e, FX_RPT_1_16, l, r);
    process_active_slot(e, FX_RPT_TRIP, l, r);
    process_active_slot(e, FX_STUTTER, l, r);

    /* Stage 3: Scatter */
    process_active_slot(e, FX_SCATTER, l, r);

    /* Stage 4: Timestretch */
    process_active_slot(e, FX_STRETCH, l, r);
}

static void process_all_slots(perf_fx_engine_t *e, float *l, float *r) {
    /* Row 4 processed as a dedicated chain */
    process_row4_chain(e, l, r);

    /* Rows 3, 2, 1 (slots 8-31) */
    for (int i = FX_LP_SWEEP_DOWN; i < PFX_NUM_FX; i++) {
        process_active_slot(e, i, l, r);
    }
}

/* ============================================================
 * Main render
 * ============================================================ */

void pfx_engine_render(perf_fx_engine_t *e, int16_t *out_lr, int frames) {
    /* Read input audio: the FX-chain input when the host hands us one,
     * otherwise the shared output buffer. */
    int16_t *audio_src = NULL;
    if (e->direct_input) {
        audio_src = e->direct_input;
    } else if (e->mapped_memory) {
        audio_src = (int16_t *)(e->mapped_memory + e->audio_out_offset);
    }

    /* Convert input to float */
    for (int i = 0; i < frames; i++) {
        if (audio_src) {
            e->work_l[i] = (float)audio_src[i * 2] / 32768.0f;
            e->work_r[i] = (float)audio_src[i * 2 + 1] / 32768.0f;
        } else {
            e->work_l[i] = 0.0f;
            e->work_r[i] = 0.0f;
        }

        /* Save dry signal */
        e->dry_l[i] = e->work_l[i];
        e->dry_r[i] = e->work_r[i];
    }

    /* Single-pass: per-sample FX processing, global filters, and output */
    for (int i = 0; i < frames; i++) {
        float l = e->work_l[i];
        float r = e->work_r[i];

        /* Update capture buffer */
        e->capture_buf_l[e->capture_write_pos] = l;
        e->capture_buf_r[e->capture_write_pos] = r;
        e->capture_write_pos = (e->capture_write_pos + 1) % e->capture_len;

        if (!e->bypassed) {
            process_all_slots(e, &l, &r);

            /* E4: DJ Filter (center=off, CCW=LPF, CW=HPF) */
            if (e->dj_filter < 0.48f) {
                /* LPF side: 0.0 = fully closed, 0.48 = open */
                float cut = e->dj_filter / 0.48f; /* 0..1 */
                float f = cutoff_to_f(cut);
                float reso = 0.4f + (1.0f - cut) * 0.4f; /* more reso when closed */
                float lp_l, lp_r;
                svf_process(&e->global_lp_l, l, f, reso, &lp_l, NULL, NULL);
                svf_process(&e->global_lp_r, r, f, reso, &lp_r, NULL, NULL);
                l = lp_l;
                r = lp_r;
            } else if (e->dj_filter > 0.52f) {
                /* HPF side: 0.52 = open, 1.0 = fully closed */
                float cut = (e->dj_filter - 0.52f) / 0.48f; /* 0..1 */
                float f = cutoff_to_f(cut);
                float reso = 0.4f + cut * 0.4f; /* more reso when closed */
                float hp_l, hp_r;
                svf_process(&e->global_hp_l, l, f, reso, NULL, &hp_l, NULL);
                svf_process(&e->global_hp_r, r, f, reso, NULL, &hp_r, NULL);
                l = hp_l;
                r = hp_r;
            }

            /* E7: Tilt EQ (center=flat, CCW=bass boost+treble cut, CW=treble boost+bass cut) */
            if (e->tilt_eq != 0.5f) {
                float tilt = (e->tilt_eq - 0.5f) * 2.0f; /* -1 to +1 */
                float f_low = cutoff_to_f(0.15f);  /* ~200 Hz */
                float f_hi = cutoff_to_f(0.75f);   /* ~8 kHz */
                float lp_l, lp_r, hp_l, hp_r;
                svf_process(&e->tilt_lp_l, l, f_low, 0.5f, &lp_l, NULL, NULL);
                svf_process(&e->tilt_lp_r, r, f_low, 0.5f, &lp_r, NULL, NULL);
                svf_process(&e->tilt_hp_l, l, f_hi, 0.5f, NULL, &hp_l, NULL);
                svf_process(&e->tilt_hp_r, r, f_hi, 0.5f, NULL, &hp_r, NULL);
                /* Boost one end, cut the other */
                l += lp_l * (-tilt) * 0.5f + hp_l * tilt * 0.5f;
                r += lp_r * (-tilt) * 0.5f + hp_r * tilt * 0.5f;
            }

            /* E8: Dry/wet mix */
            l = e->dry_l[i] * (1.0f - e->dry_wet) + l * e->dry_wet;
            r = e->dry_r[i] * (1.0f - e->dry_wet) + r * e->dry_wet;
        }

        /* Soft clip output */
        l = soft_clip(l);
        r = soft_clip(r);

        /* Convert to int16 */
        int32_t sl = (int32_t)(l * 32767.0f);
        int32_t sr = (int32_t)(r * 32767.0f);
        if (sl > 32767) sl = 32767;
        if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767;
        if (sr < -32768) sr = -32768;

        out_lr[i * 2] = (int16_t)sl;
        out_lr[i * 2 + 1] = (int16_t)sr;
    }
}

/* ============================================================
 * State serialization (JSON)
 * ============================================================ */

int pfx_serialize_state(perf_fx_engine_t *e, char *buf, int buf_len) {
    int n = 0;
    SAFE_SNPRINTF(buf, n, buf_len, "{\"bpm\":%.1f", e->bpm);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"dj_filter\":%.3f", e->dj_filter);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"tilt_eq\":%.3f", e->tilt_eq);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"dry_wet\":%.3f", e->dry_wet);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"repeat_rate\":%.3f", e->repeat_rate);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"repeat_speed\":%.3f", e->repeat_speed);
    SAFE_SNPRINTF(buf, n, buf_len, ",\"last_touched\":%d", e->last_touched_slot);

    /* FX slots state */
    SAFE_SNPRINTF(buf, n, buf_len, ",\"slots\":[");
    for (int i = 0; i < PFX_NUM_FX; i++) {
        pfx_slot_t *s = &e->slots[i];
        if (i > 0) SAFE_SNPRINTF(buf, n, buf_len, ",");
        SAFE_SNPRINTF(buf, n, buf_len, "{\"a\":%d,\"l\":%d,\"t\":%d,\"p\":[",
                      s->active || s->tail_active, s->latched,
                      s->tail_active);
        for (int j = 0; j < PFX_SLOT_PARAMS; j++) {
            if (j > 0) SAFE_SNPRINTF(buf, n, buf_len, ",");
            SAFE_SNPRINTF(buf, n, buf_len, "%.3f", s->params[j]);
        }
        SAFE_SNPRINTF(buf, n, buf_len, "]}");
    }
    SAFE_SNPRINTF(buf, n, buf_len, "]}");
    return n;
}
