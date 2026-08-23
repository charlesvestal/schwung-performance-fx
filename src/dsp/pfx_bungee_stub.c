/*
 * pfx_bungee_stub.c — host-build test double for the Bungee time-stretcher.
 *
 * The real stretcher is C++ (src/dsp/pfx_bungee.cpp) and pulls in Bungee and
 * pffft, which do not cross-compile into a plain `cc` host test binary. Link
 * this instead when building test_perf_fx on the dev machine.
 *
 * It is deliberately not a no-op passthrough: it is a ring buffer whose read
 * pointer advances at speed*pitch with linear interpolation. That is not
 * pitch-preserving time-stretching, so it sounds wrong — but it honours the
 * part of the contract the tests care about, namely that changing speed or
 * pitch changes the output. A passthrough stub would make every STRETCH and
 * OCT DN parameter look dead to the characterization tests.
 */

#include "pfx_bungee.h"
#include <stdlib.h>
#include <string.h>

#define STUB_BUF_FRAMES 65536

struct pfx_bungee {
    float buf_l[STUB_BUF_FRAMES];
    float buf_r[STUB_BUF_FRAMES];
    int write_pos;
    double read_pos;
    float speed;
    float pitch;
};

pfx_bungee_t *pfx_bungee_create(int sample_rate) {
    (void)sample_rate;
    pfx_bungee_t *b = (pfx_bungee_t *)calloc(1, sizeof(pfx_bungee_t));
    if (!b) return NULL;
    b->speed = 1.0f;
    b->pitch = 1.0f;
    return b;
}

void pfx_bungee_destroy(pfx_bungee_t *b) {
    free(b);
}

void pfx_bungee_set_speed(pfx_bungee_t *b, float speed) {
    if (b) b->speed = speed;
}

void pfx_bungee_set_pitch(pfx_bungee_t *b, float pitch) {
    if (b) b->pitch = pitch;
}

void pfx_bungee_reset(pfx_bungee_t *b) {
    if (!b) return;
    memset(b->buf_l, 0, sizeof(b->buf_l));
    memset(b->buf_r, 0, sizeof(b->buf_r));
    b->write_pos = 0;
    b->read_pos = 0.0;
}

void pfx_bungee_write(pfx_bungee_t *b, const float *in_lr, int frames) {
    if (!b || !in_lr) return;
    for (int i = 0; i < frames; i++) {
        b->buf_l[b->write_pos] = in_lr[i * 2];
        b->buf_r[b->write_pos] = in_lr[i * 2 + 1];
        b->write_pos = (b->write_pos + 1) % STUB_BUF_FRAMES;
    }
}

int pfx_bungee_read(pfx_bungee_t *b, float *out_lr, int max_frames) {
    if (!b || !out_lr) return 0;

    double rate = (double)b->speed * (double)b->pitch;
    if (rate <= 0.0) rate = 0.0;

    for (int i = 0; i < max_frames; i++) {
        int i0 = (int)b->read_pos % STUB_BUF_FRAMES;
        int i1 = (i0 + 1) % STUB_BUF_FRAMES;
        float frac = (float)(b->read_pos - (double)(long)b->read_pos);

        out_lr[i * 2]     = b->buf_l[i0] + frac * (b->buf_l[i1] - b->buf_l[i0]);
        out_lr[i * 2 + 1] = b->buf_r[i0] + frac * (b->buf_r[i1] - b->buf_r[i0]);

        b->read_pos += rate;
        if (b->read_pos >= (double)STUB_BUF_FRAMES)
            b->read_pos -= (double)STUB_BUF_FRAMES;
    }
    return max_frames;
}
