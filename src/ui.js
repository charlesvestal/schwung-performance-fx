/*
 * Performance FX Module UI — v2 Architecture
 *
 * 32 unified punch-in FX pads (hold=on, release=off)
 * Shift+hold = latch, Shift+hold latched = unlatch
 * E1: RPT Length  E2: RPT Speed  E3: RPT on/off
 * E4-E6: per-slot FX params (last touched pad)
 * E7: Tilt EQ  E8: DJ Filter  Shift+E8: Dry/Wet
 *
 * FX names and param labels come from the DSP descriptor table, not from here.
 */

import {
    Black, White, LightGrey, DarkGrey,
    BrightRed, OrangeRed, Bright, VividYellow,
    BrightGreen, ForestGreen, NeonGreen, TealGreen, Cyan,
    AzureBlue, RoyalBlue, Navy,
    BlueViolet, Violet, Purple, ElectricViolet,
    HotMagenta, NeonPink, Rose, BrightPink,
    Ochre, BurntOrange, Mustard,
    MintGreen, PaleCyan, SkyBlue, LightBlue,
    Lilac, Lime,
    MidiNoteOn, MidiNoteOff, MidiCC, MidiPolyAftertouch,
    MoveShift, MoveBack, MoveMainButton, MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveMaster, MovePads, MoveSteps,
    MoveCapture, MoveUndo, MoveLoop, MoveCopy, MoveDelete,
    MovePlay, MoveRec,
    MoveRow1, MoveRow2, MoveRow3, MoveRow4,
    WhiteLedOff, WhiteLedDim, WhiteLedMedium, WhiteLedBright,
    Pulse4th
} from '/data/UserData/schwung/shared/constants.mjs';

import {
    isCapacitiveTouchMessage, isNoiseMessage,
    setLED, setButtonLED, decodeDelta
} from '/data/UserData/schwung/shared/input_filter.mjs';

import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ================================================================
 * Constants
 * ================================================================ */

const SCREEN_W = 128;
const SCREEN_H = 64;
const LEDS_PER_FRAME = 8;
const NUM_SLOTS = 32;

/* Pad note → slot index mapping
 * Row 4 (top):    92-99  → slots 0-7
 * Row 3:          84-91  → slots 8-15
 * Row 2:          76-83  → slots 16-23
 * Row 1 (bottom): 68-75  → slots 24-31
 */
const PAD_NOTES = [
    92, 93, 94, 95, 96, 97, 98, 99,   /* Row 4: slots 0-7 */
    84, 85, 86, 87, 88, 89, 90, 91,   /* Row 3: slots 8-15 */
    76, 77, 78, 79, 80, 81, 82, 83,   /* Row 2: slots 16-23 */
    68, 69, 70, 71, 72, 73, 74, 75    /* Row 1: slots 24-31 */
];

/* Build reverse lookup: note → slot index */
const NOTE_TO_SLOT = {};
for (let i = 0; i < NUM_SLOTS; i++) {
    NOTE_TO_SLOT[PAD_NOTES[i]] = i;
}

/* Track buttons */
const TRACK_CCS = [MoveRow1, MoveRow2, MoveRow3, MoveRow4];

/* FX names and per-slot param names/defaults are NOT kept here.
 *
 * They are fetched from the DSP's pfx_fx_desc table (fx_names, fx_params_<n>),
 * which is the only place they are defined. This module used to carry its own
 * copy of both, the plugin wrapper carried a third, and all three disagreed \u2014
 * which is how the display came to advertise 91 knobs when only 19 were wired
 * to anything and 4 of those moved a different parameter than their label
 * claimed. A label that cannot drift from its implementation is the point. */
/* Repeat pads re-arm their first two knobs on each press; index 2 is left
 * alone. Mirrors the split in resetRepeatKnobs below. */
const PARAM_KEEP_FROM = 2;

let fxNames = null;                 /* string[32], or null until fetched */
const slotParamInfo = new Array(NUM_SLOTS).fill(null);  /* [[name,default],...] */

function fxName(slot) {
    if (fxNames && fxNames[slot]) return fxNames[slot];
    return `FX ${slot + 1}`;
}

function fetchFxNames() {
    try {
        const raw = getParam('fx_names');
        if (!raw) return;
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed) && parsed.length === NUM_SLOTS) fxNames = parsed;
    } catch (e) { /* keep the numeric fallback */ }
}

/* Param names and defaults for one slot, fetched once and cached. Lazy per
 * slot rather than all 32 up front: only the selected slot is ever displayed,
 * and the param mailbox is contended while pads are held.
 *
 * paramLabel() runs from drawMainView every frame, so a failed fetch must not
 * turn into a get_param call per tick — back off and retry occasionally
 * instead. */
const SLOT_PARAM_RETRY_TICKS = 30;
const slotParamRetry = new Array(NUM_SLOTS).fill(0);

function getSlotParams(slot) {
    if (slot < 0 || slot >= NUM_SLOTS) return null;
    if (slotParamInfo[slot]) return slotParamInfo[slot];
    if (slotParamRetry[slot] > 0) { slotParamRetry[slot]--; return null; }
    try {
        const raw = getParam(`fx_params_${slot}`);
        const parsed = raw ? JSON.parse(raw) : null;
        if (!Array.isArray(parsed) || parsed.length !== 3) {
            slotParamRetry[slot] = SLOT_PARAM_RETRY_TICKS;
            return null;
        }
        slotParamInfo[slot] = parsed;
        /* Mirror the DSP's defaults locally. The DSP already initialised them
         * from the same table, so nothing needs pushing back.
         *
         * Only seed when we have nothing: if the fetch was slow and the player
         * has already turned a knob for this slot, overwriting here would wipe
         * what they just dialled in. */
        if (!slotParams[slot]) slotParams[slot] = parsed.map(p => p[1]);
        return parsed;
    } catch (e) {
        slotParamRetry[slot] = SLOT_PARAM_RETRY_TICKS;
        return null;
    }
}

function paramLabel(slot, idx) {
    const info = getSlotParams(slot);
    if (!info || !info[idx] || info[idx][0] === null) return '---';
    return info[idx][0];
}

/* Current value of one param. Read-only: falls back to the declared default,
 * then to a neutral, without inventing a row. Anything that writes goes via
 * handleKnob, which bails on a '---' label and so only proceeds once the
 * descriptor fetch has succeeded and seeded slotParams[slot]. */
function paramValue(slot, idx) {
    const info = getSlotParams(slot);
    const p = slotParams[slot];
    if (p) return p[idx];
    if (info && info[idx]) return info[idx][1];
    return 0.5;
}

/* Engine-level knobs: E1, E2, E3 (UI-only latch toggle), E7, E8, Shift+E8 */
const GLOBAL_KEYS = ['repeat_rate', 'repeat_speed', 'rpt_toggle', 'tilt_eq', 'dj_filter', 'dry_wet'];
const GLOBAL_DEFAULTS = [0.5, 0.5, 0.0, 0.5, 0.5, 1.0];
const NUM_GLOBALS = 6;

/* LED color mapping per slot */
const BRIGHT_COLORS = [];
const DIM_COLORS = [];

/* Row 4 (slots 0-7): Orange */
for (let i = 0; i < 8; i++) {
    BRIGHT_COLORS.push(OrangeRed);
    DIM_COLORS.push(Ochre);
}
/* Row 3 (slots 8-15): Blue */
for (let i = 0; i < 8; i++) {
    BRIGHT_COLORS.push(AzureBlue);
    DIM_COLORS.push(RoyalBlue);
}
/* Row 2 (slots 16-23): Purple */
for (let i = 0; i < 8; i++) {
    BRIGHT_COLORS.push(ElectricViolet);
    DIM_COLORS.push(Violet);
}
/* Row 1 (slots 24-31): grouped by function */
/* Crush/Downsample/Saturate (24-26): Pink */
for (let i = 0; i < 3; i++) {
    BRIGHT_COLORS.push(BrightPink);
    DIM_COLORS.push(Rose);
}
/* Gate/Tremolo (27-28): Green */
for (let i = 0; i < 2; i++) {
    BRIGHT_COLORS.push(BrightGreen);
    DIM_COLORS.push(ForestGreen);
}
/* Pitch Down/Vinyl Sim/Vinyl Brake (29-31): Yellow */
for (let i = 0; i < 3; i++) {
    BRIGHT_COLORS.push(VividYellow);
    DIM_COLORS.push(Mustard);
}

/* ================================================================
 * State
 * ================================================================ */

let shiftHeld = false;
let bypassed = false;
let undoHeld = false;
let undoWasBypassed = false;

/* FX state */
let fxActive = new Array(NUM_SLOTS).fill(false);
let fxLatched = new Array(NUM_SLOTS).fill(false);
let fxHeld = new Array(NUM_SLOTS).fill(false); /* physically held (finger on pad) */
/* Per-slot param values, mirrored from the DSP on first touch of each slot
 * (see getSlotParams). Null until then — the DSP owns the defaults. */
let slotParams = new Array(NUM_SLOTS).fill(null);
/* Last touched slot for E1-E3 mapping */
let lastTouchedSlot = -1;
/* Last repeat slot used (for step button toggle) */
let lastRepeatSlot = 0; /* default to RPT 1/4 (slot 0) */
/* Global param values (0.0-1.0) */
let globalValues = GLOBAL_DEFAULTS.slice();

/* Display overlay */
let overlayText = '';
let overlayParam = '';
let overlayValue = '';
let overlayTimer = 0;
const OVERLAY_DURATION = 66;

/* Throttle screen reader announce to prevent D-Bus flood on rapid knob turns */
let lastAnnounceTime = 0;
const ANNOUNCE_THROTTLE_MS = 150;

/* LED init */
let ledInitPending = true;
let ledInitIndex = 0;
/* Repainting after a resume is a timing problem: the host's 0->2 entry queues
 * an all-LEDs-off sweep on the audio side and exposes no "sweep finished"
 * signal on the resume path (a fresh load gets one — deferred init waits for
 * clearLedBatch() AND OVERTAKE_INIT_DELAY_TICKS=30 before calling init()).
 *
 * So: wait at least as long as a fresh load does, then repaint — and repaint
 * again twice more, because the host's flushLedQueue only drains 16 writes per
 * tick and our ~55 take several ticks, any of which can still be overrun.
 * Cheap insurance; a forced repaint of an already-correct surface is invisible. */
const LED_RESUME_DELAY_TICKS = 30;
const LED_RESUME_REPEAT_TICKS = 20;
const LED_RESUME_REPEATS = 2;
let ledResumeDelay = 0;
let ledResumeRepeatsLeft = 0;

/* BPM and tap tempo */
let bpm = 120.0;
let tapTimes = [];
const TAP_TIMEOUT = 2000;
const TAP_MIN_TAPS = 2;

/* Host tempo follow lives in the DSP now: it polls the host's get_bpm()
 * (internal transport → live MIDI clock → last clock → current Set tempo →
 * settings → 120), so it works with no clock running and without MIDI sync
 * enabled. The UI just mirrors whatever the DSP settled on, because
 * bpmSyncedRate() and the header both need an accurate figure.
 *
 * Sending an explicit 'bpm' (tap tempo / tempo knob) tells the DSP to stop
 * following; 'bpm_follow_host' re-arms it. */
let lastHostBpm = 0;
const HOST_BPM_POLL_TICKS = 30;
let hostBpmPollCounter = 0;

function syncHostBpm(force) {
    const raw = getParam('bpm');
    const v = parseFloat(raw);
    if (!(v >= 20 && v <= 300)) return;
    if (!force && Math.abs(v - bpm) < 0.05) return;
    lastHostBpm = v;
    bpm = Math.round(v * 10) / 10;
}

/* ================================================================
 * Tap Tempo
 * ================================================================ */

function handleTapTempo() {
    const now = Date.now();

    if (tapTimes.length > 0 && (now - tapTimes[tapTimes.length - 1]) > TAP_TIMEOUT) {
        tapTimes = [];
    }

    tapTimes.push(now);
    if (tapTimes.length > 8) tapTimes.shift();

    if (tapTimes.length >= TAP_MIN_TAPS) {
        let totalInterval = 0;
        for (let i = 1; i < tapTimes.length; i++) {
            totalInterval += tapTimes[i] - tapTimes[i - 1];
        }
        const avgInterval = totalInterval / (tapTimes.length - 1);
        const tapBpm = 60000.0 / avgInterval;

        if (tapBpm >= 20 && tapBpm <= 300) {
            bpm = Math.round(tapBpm * 10) / 10;
            sendParam('bpm', bpm.toFixed(1));
            showOverlay('Tap Tempo', `${bpm.toFixed(1)} BPM`, (bpm / 300).toFixed(2));
        }
    } else {
        showOverlay('Tap Tempo', 'Tap again...', '');
    }
}

/* ================================================================
 * LED Management
 * ================================================================ */

function getPadColor(slot) {
    if (fxLatched[slot]) {
        return BrightRed;
    }
    if (fxActive[slot]) {
        return BRIGHT_COLORS[slot];
    }
    return DIM_COLORS[slot];
}

function buildLedList() {
    const leds = [];

    /* All 32 pads */
    for (let i = 0; i < NUM_SLOTS; i++) {
        leds.push({
            note: PAD_NOTES[i],
            color: getPadColor(i)
        });
    }

    /* Step buttons (unused — all off) */
    for (let i = 0; i < 16; i++) {
        leds.push({ note: MoveSteps[i], color: Black });
    }

    return leds;
}

function setupLedBatch() {
    const leds = buildLedList();
    const start = ledInitIndex;
    const end = Math.min(start + LEDS_PER_FRAME, leds.length);

    for (let i = start; i < end; i++) {
        setLED(leds[i].note, leds[i].color);
    }

    ledInitIndex = end;
    if (ledInitIndex >= leds.length) {
        ledInitPending = false;
        /* Button LEDs */
        setButtonLED(MoveUndo, bypassed ? WhiteLedBright : WhiteLedDim);
        setButtonLED(MoveBack, WhiteLedDim);
        setButtonLED(MoveShift, WhiteLedDim);

        /* Track button LEDs */
        for (let i = 0; i < 4; i++) {
            setButtonLED(TRACK_CCS[i], WhiteLedDim);
        }
    }
}

function refreshPadLED(slot) {
    if (slot >= 0 && slot < NUM_SLOTS) {
        setLED(PAD_NOTES[slot], getPadColor(slot));
    }
}

function refreshAllPadLEDs() {
    for (let i = 0; i < NUM_SLOTS; i++) {
        setLED(PAD_NOTES[i], getPadColor(i));
    }
}

/* Repaint every LED this module owns, bypassing the cache.
 * Needed on resume: init() is not re-run, and setLED/setButtonLED are
 * cache-guarded by a module-scope cache in the shared helper that survives the
 * park — so an ordinary repaint would be silently skipped even though Move has
 * painted over the hardware in the meantime. */
function repaintAllModuleLEDs() {
    for (let i = 0; i < NUM_SLOTS; i++) setLED(PAD_NOTES[i], getPadColor(i), true);
    for (let i = 0; i < 16; i++) setLED(MoveSteps[i], Black, true);
    for (let i = 0; i < 4; i++) setButtonLED(TRACK_CCS[i], WhiteLedDim, true);
    setButtonLED(MoveUndo, bypassed ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveBack, WhiteLedDim, true);
    setButtonLED(MoveShift, WhiteLedDim, true);
}

/* Turn off every LED this module owns. Must run before host_exit_module():
 * the host's C-side snapshot restore does not reliably repaint surfaces an
 * overtake module lit, so each module blacks out its own (dAVEBOx and
 * song-mode do the same on their exit paths).
 *
 * force=true on every write is required — setLED/setButtonLED are cache-guarded
 * and would silently skip any surface the cache already believes is off. The
 * cache is module-scope in the shared helper and survives across sessions, so
 * without force the second visit clears nothing. */
function clearAllModuleLEDs() {
    for (let i = 0; i < NUM_SLOTS; i++) setLED(PAD_NOTES[i], Black, true);
    for (let i = 0; i < 16; i++) setLED(MoveSteps[i], Black, true);
    for (let i = 0; i < 4; i++) setButtonLED(TRACK_CCS[i], WhiteLedOff, true);
    setButtonLED(MoveUndo, WhiteLedOff, true);
    setButtonLED(MoveBack, WhiteLedOff, true);
    setButtonLED(MoveShift, WhiteLedOff, true);
}

/* ================================================================
 * Display
 * ================================================================ */

function drawMainView() {
    clear_screen();

    /* Line 1: header */
    let activeCount = 0;
    for (let i = 0; i < NUM_SLOTS; i++) {
        if (fxActive[i]) activeCount++;
    }
    print(0, 0, `PFX ${bpm.toFixed(0)} [${activeCount}]`, 1);
    draw_line(0, 9, SCREEN_W, 9, 1);

    /* Lines 2-3: names of active/latched FX */
    let activeLine1 = '';
    let activeLine2 = '';
    for (let i = 0; i < NUM_SLOTS; i++) {
        if (fxActive[i] || fxLatched[i]) {
            const name = fxName(i);
            const tag = fxLatched[i] ? '*' : '';
            const entry = name + tag;
            if (activeLine1.length === 0) {
                activeLine1 = entry;
            } else if (activeLine1.length + entry.length + 1 <= 21) {
                activeLine1 += ' ' + entry;
            } else if (activeLine2.length === 0) {
                activeLine2 = entry;
            } else if (activeLine2.length + entry.length + 1 <= 21) {
                activeLine2 += ' ' + entry;
            }
        }
    }
    if (activeLine1.length > 0) {
        print(0, 12, activeLine1, 1);
    } else {
        print(0, 12, 'No FX active', 1);
    }
    if (activeLine2.length > 0) {
        print(0, 21, activeLine2, 1);
    }

    /* Separator */
    draw_line(0, 30, SCREEN_W, 30, 1);

    /* Line 4: E1-E3 (RPT controls) + E4 (per-slot param 1) */
    const timeLabel = getTimeLabel(globalValues[0]);
    print(0, 33, timeLabel, 1);     /* E1: RPT Length */
    const spd = globalValues[1];
    print(32, 33, spd < 0.48 ? 'Slow' : spd > 0.52 ? 'Fast' : 'Nrml', 1); /* E2: Speed */
    const rptOn = fxActive[lastRepeatSlot] || fxLatched[lastRepeatSlot];
    print(64, 33, rptOn ? 'LP*' : 'Loop', 1);  /* E3: Loop SW */
    /* E4: first per-slot param */
    if (lastTouchedSlot >= 0 && lastTouchedSlot < NUM_SLOTS) {
        print(96, 33, paramLabel(lastTouchedSlot, 0), 1);
    } else {
        print(96, 33, '---', 1);
    }

    /* Line 5: E5-E8 labels */
    if (lastTouchedSlot >= 0 && lastTouchedSlot < NUM_SLOTS) {
        print(0, 44, paramLabel(lastTouchedSlot, 1), 1);  /* E5 */
        print(32, 44, paramLabel(lastTouchedSlot, 2), 1); /* E6 */
    } else {
        print(0, 44, '---', 1);
        print(32, 44, '---', 1);
    }
    print(64, 44, 'Tilt', 1);       /* E7: Tilt EQ */
    print(96, 44, 'DJ', 1);         /* E8: DJ Filter */

    /* Bypass overlay */
    if (bypassed) {
        draw_rect(30, 16, 68, 14, 1);
        fill_rect(31, 17, 66, 12, 0);
        print(38, 19, 'BYPASSED', 1);
    }
}

function drawOverlay() {
    if (overlayTimer <= 0) return;

    clear_screen();
    print(0, 0, overlayText, 1);
    draw_line(0, 10, SCREEN_W, 10, 1);

    print(0, 16, overlayParam, 1);

    let numVal = parseFloat(overlayValue);
    if (!isNaN(numVal)) {
        let barWidth = Math.floor(numVal * 110);
        if (barWidth < 0) barWidth = 0;
        if (barWidth > 110) barWidth = 110;
        fill_rect(8, 30, barWidth, 10, 1);
        draw_rect(8, 30, 110, 10, 1);
        let pct = Math.round(numVal * 100);
        print(50, 45, `${pct}%`, 1);
    } else {
        print(0, 30, overlayValue, 1);
    }

    overlayTimer--;
}

/* ================================================================
 * Rate label helper (maps 0..1 to musical division name)
 * ================================================================ */

/* Tapping a repeat pad re-arms its shaping controls (Filter bypassed, Gate off)
 * so each punch-in starts from a predictable place.
 *
 * Index 2 is deliberately NOT reset. It is whatever the player dialled in for
 * the performance — Decay on the repeats, a wet amount elsewhere — not part of
 * the per-hit shaping. Snapping it back on every press meant it could never be
 * dialled in at all. */
function resetRepeatKnobs(slot) {
    const info = getSlotParams(slot);
    if (info && slotParams[slot]) {
        for (let i = 0; i < PARAM_KEEP_FROM; i++) {
            slotParams[slot][i] = info[i][1];
            sendParam(`punch_${slot}_param_${i}`, info[i][1].toFixed(3));
        }
    }
    globalValues[1] = 0.5;      /* speed = normal */
    sendParam('repeat_speed', '0.500');
}

function bpmSyncedRate(slot) {
    /* Convert BPM-synced beat division to rate01 position (free seconds).
     * Matches DSP: seconds = 2.0 * 0.006^rate01
     * Inverse: rate01 = ln(seconds/2.0) / ln(0.006) */
    const beatSec = 60.0 / (bpm > 20 ? bpm : 120);
    const divMap = [1.0, 0.5, 0.25, 2.0/3.0, 0.125]; /* 1/4, 1/8, 1/16, trip, 1/32 */
    const seconds = beatSec * (divMap[slot] || 0.5);
    if (seconds >= 2.0) return 0.0;
    if (seconds <= 0.012) return 1.0;
    return Math.log(seconds / 2.0) / Math.log(0.006);
}

function getTimeLabel(rate01) {
    /* Matches DSP: seconds = 2.0 * 0.006^rate01 */
    const seconds = 2.0 * Math.pow(0.006, rate01);
    if (seconds >= 1.0) return seconds.toFixed(1) + 's';
    const ms = Math.round(seconds * 1000);
    return ms + 'ms';
}

/* ================================================================
 * Parameter handling
 * ================================================================ */

/* ---- Param queue for overtake mode ----
 * In overtake mode, shadow_set_param is fire-and-forget into a single
 * shared memory slot. Rapid calls within the same tick clobber each other.
 * Queue non-critical params and drain them 1 per tick.
 * Critical params (on/off/latch) use the blocking variant. */
const paramQueue = [];
/* [key, value, attempts] for critical sends the host refused. */
let criticalRetries = [];
/* Reconcile local FX state against the DSP's own view periodically. A dropped
 * punch_N_off cannot be detected locally: handlePadOff() clears fxActive before
 * the send, so the UI already believes the effect is off while the DSP still
 * has it running. The DSP is the only authority. */
const RECONCILE_TICKS = 120;   /* ~2s */
let reconcileCounter = 0;
const PARAMS_PER_TICK = 2;  /* drain up to 2 queued params per tick */

/* Critical sends (punch on/off/latch) go out blocking, because a dropped one
 * leaves an effect stuck on with nothing to correct it.
 *
 * host_module_set_param_blocking -> shadow_set_param_timeout returns FALSE when
 * the single param mailbox is busy, and the 50ms timeout makes that likelier
 * exactly when it matters: our own pressure stream writes to the same mailbox
 * fire-and-forget, and it is busiest while a pad is held hard. The return value
 * used to be discarded. Retry instead, with a longer timeout — the failure is
 * contention, so a second attempt a tick later usually lands. */
function sendCritical(key, v, timeoutMs) {
    if (typeof host_module_set_param_blocking === 'function') {
        /* Older hosts return undefined rather than a bool — treat only an
         * explicit false as failure so we never retry-storm against them. */
        return host_module_set_param_blocking(key, v, timeoutMs || 50) !== false;
    }
    host_module_set_param(key, v);
    return true;
}

function drainCriticalRetries() {
    if (criticalRetries.length === 0) return;
    const pending = criticalRetries;
    criticalRetries = [];
    for (let i = 0; i < pending.length; i++) {
        const key = pending[i][0], v = pending[i][1], attempts = pending[i][2];
        if (sendCritical(key, v, 200)) continue;
        if (attempts < 4) {
            criticalRetries.push([key, v, attempts + 1]);
        } else {
            console.log(`[pfx] critical param dropped after 5 tries: ${key}=${v}`);
        }
    }
}

function sendParam(key, value) {
    const v = String(value);
    /* Critical: note on/off/latch must be delivered immediately */
    if (key.endsWith('_on') || key.endsWith('_off') || key.endsWith('_latch')) {
        if (!sendCritical(key, v)) {
            console.log(`[pfx] critical param send failed, queued for retry: ${key}=${v}`);
            criticalRetries.push([key, v, 0]);
        }
        return;
    }
    /* Non-critical: queue and deduplicate (keep latest value per key) */
    const existing = paramQueue.findIndex(p => p[0] === key);
    if (existing >= 0) {
        paramQueue[existing][1] = v;
    } else {
        paramQueue.push([key, v]);
    }
}

function drainParamQueue() {
    let sent = 0;
    while (paramQueue.length > 0 && sent < PARAMS_PER_TICK) {
        const [key, value] = paramQueue.shift();
        host_module_set_param(key, value);
        sent++;
    }
}

function getParam(key) {
    return host_module_get_param(key);
}

function showOverlay(title, param, value) {
    overlayText = title;
    overlayParam = param;
    overlayValue = String(value);
    overlayTimer = OVERLAY_DURATION;

    const now = Date.now();
    if (now - lastAnnounceTime >= ANNOUNCE_THROTTLE_MS) {
        lastAnnounceTime = now;
        const parts = [title, param, value].filter(s => s && s.length > 0);
        announce(parts.join(', '));
    }
}

/* ================================================================
 * MIDI input handling
 * ================================================================ */

function handlePadOn(note, velocity) {
    const slot = NOTE_TO_SLOT[note];
    if (slot === undefined) return;

    const velNorm = (velocity / 127.0).toFixed(3);

    if (shiftHeld) {
        /* Shift+hold = latch toggle */
        if (fxLatched[slot]) {
            /* Unlatch */
            fxLatched[slot] = false;
            fxActive[slot] = false;
            sendParam(`punch_${slot}_latch`, '0');
            sendParam(`punch_${slot}_off`, '1');
            showOverlay(fxName(slot), 'Unlatched', '');
        } else {
            /* Latch on */
            fxLatched[slot] = true;
            fxActive[slot] = true;
            lastTouchedSlot = slot;
            if (slot <= 4) {
                lastRepeatSlot = slot;
                globalValues[0] = bpmSyncedRate(slot);
                resetRepeatKnobs(slot);
            }
            sendParam(`punch_${slot}_on`, velNorm);
            sendParam(`punch_${slot}_latch`, '1');
            showOverlay(fxName(slot), 'Latched', '');
        }
    } else {
        /* Normal tap on latched pad = select for knob editing (don't unlatch) */
        if (fxLatched[slot]) {
            lastTouchedSlot = slot;
            showOverlay(fxName(slot),
                `${paramLabel(slot, 0)} | ${paramLabel(slot, 1)} | ${paramLabel(slot, 2)}`, '');
            refreshPadLED(slot);
            return;
        }
        /* Normal punch-in: hold = on.
         * If already active (missed note-off), deactivate first. */
        if (fxActive[slot]) {
            sendParam(`punch_${slot}_off`, '1');
        }
        fxActive[slot] = true;
        fxHeld[slot] = true;
        lastTouchedSlot = slot;
        if (slot <= 4) {
            lastRepeatSlot = slot;
            globalValues[0] = bpmSyncedRate(slot);
            resetRepeatKnobs(slot);
        }
        sendParam(`punch_${slot}_on`, velNorm);
    }

    refreshPadLED(slot);
}

function handlePadOff(note) {
    const slot = NOTE_TO_SLOT[note];
    if (slot === undefined) return;

    fxHeld[slot] = false;

    /* If latched, pad release does nothing */
    if (fxLatched[slot]) return;

    /* Normal release */
    fxActive[slot] = false;
    sendParam(`punch_${slot}_off`, '1');
    refreshPadLED(slot);
}

/* Per-slot pressure throttle so simultaneous pad presses don't starve each other */
const lastPressureTime = new Array(NUM_SLOTS).fill(0);
const PRESSURE_THROTTLE_MS = 30; /* Don't send pressure faster than ~33Hz */

function handleAftertouch(note, pressure) {
    const slot = NOTE_TO_SLOT[note];
    if (slot === undefined) return;
    if (!fxActive[slot]) return;

    /* Per-slot throttle — each pad has its own timer */
    const now = Date.now();
    if (now - lastPressureTime[slot] < PRESSURE_THROTTLE_MS) return;
    lastPressureTime[slot] = now;

    sendParam(`punch_${slot}_pressure`, (pressure / 127.0).toFixed(3));
}

function handleKnob(knobIndex, delta) {
    if (knobIndex === 0) {
        /* E1: RPT Length — free seconds */
        let v = globalValues[0] + delta * 0.01;
        v = Math.max(0.0, Math.min(1.0, v));
        globalValues[0] = v;
        sendParam('repeat_rate', v.toFixed(3));
        const timeLabel = getTimeLabel(v);
        showOverlay('Repeat', `Length: ${timeLabel}`, v.toFixed(2));
    } else if (knobIndex === 1) {
        /* E2: RPT Speed — detent around 0.5 (normal) */
        let v = globalValues[1] + delta * 0.01;
        v = Math.max(0.0, Math.min(1.0, v));
        /* Snap to 0.5 when crossing through the detent zone */
        if (v >= 0.49 && v <= 0.51) v = 0.5;
        globalValues[1] = v;
        sendParam('repeat_speed', v.toFixed(3));
        const label = v < 0.49 ? 'Slow' : v > 0.51 ? 'Fast' : 'Normal';
        showOverlay('Repeat', `Speed: ${label}`, v.toFixed(2));
    } else if (knobIndex === 2) {
        /* E3: RPT on/off — turn right = on, turn left = off */
        const slot = lastRepeatSlot;
        if (delta > 0 && !fxActive[slot]) {
            fxLatched[slot] = true;
            fxActive[slot] = true;
            lastTouchedSlot = slot;
            sendParam(`punch_${slot}_on`, '0.700');
            sendParam(`punch_${slot}_latch`, '1');
            globalValues[2] = 1.0;
            showOverlay(fxName(slot), 'Loop ON', '1.00');
        } else if (delta < 0 && fxActive[slot]) {
            fxLatched[slot] = false;
            fxActive[slot] = false;
            sendParam(`punch_${slot}_latch`, '0');
            sendParam(`punch_${slot}_off`, '1');
            globalValues[2] = 0.0;
            showOverlay(fxName(slot), 'Loop OFF', '0.00');
        }
        refreshPadLED(slot);
    } else if (knobIndex >= 3 && knobIndex <= 5) {
        /* E4-E6: per-slot params for last touched pad */
        if (lastTouchedSlot < 0 || lastTouchedSlot >= NUM_SLOTS) {
            showOverlay('No FX', 'Tap a pad first', '');
            return;
        }
        const slot = lastTouchedSlot;
        const pi = knobIndex - 3;
        const label = paramLabel(slot, pi);
        if (label === '---') return;   /* this FX declares no knob here */
        let v = paramValue(slot, pi) + delta * 0.01;
        v = Math.max(0.0, Math.min(1.0, v));
        /* Must land locally, or the next turn reads the old value back and the
         * knob appears to snap. paramValue() guarantees the row exists. */
        slotParams[slot][pi] = v;
        sendParam(`punch_${slot}_param_${pi}`, v.toFixed(3));
        showOverlay(fxName(slot), label, v.toFixed(2));
    } else if (knobIndex === 6) {
        /* E7: Tilt EQ */
        let v = globalValues[3] + delta * 0.01;
        v = Math.max(0.0, Math.min(1.0, v));
        globalValues[3] = v;
        sendParam('tilt_eq', v.toFixed(3));
        showOverlay('Global', 'Tilt', v.toFixed(2));
    } else if (knobIndex === 7) {
        if (shiftHeld) {
            /* Shift+E8: global dry/wet. The DSP has always had this stage;
             * until now nothing sent the param, so it was stuck at full wet. */
            let v = globalValues[5] + delta * 0.01;
            v = Math.max(0.0, Math.min(1.0, v));
            globalValues[5] = v;
            sendParam('dry_wet', v.toFixed(3));
            showOverlay('Global', 'Dry/Wet', v.toFixed(2));
            return;
        }
        /* E8: DJ Filter */
        let v = globalValues[4] + delta * 0.01;
        v = Math.max(0.0, Math.min(1.0, v));
        globalValues[4] = v;
        sendParam('dj_filter', v.toFixed(3));
        showOverlay('Global', 'DJ Flt', v.toFixed(2));
    }
}

function handleKnobPeek(knobNote) {
    /* Capacitive touch notes: 0=E1 .. 7=E8, 8=Master, 9=Jog */
    if (knobNote === 9) return;
    if (knobNote === 8) return; /* Master knob = volume passthrough, no peek */

    if (knobNote === 0) {
        /* E1: RPT Length */
        const timeLabel = getTimeLabel(globalValues[0]);
        showOverlay('Repeat', `Length: ${timeLabel}`, globalValues[0].toFixed(2));
    } else if (knobNote === 1) {
        /* E2: RPT Speed */
        showOverlay('Repeat', 'Speed', globalValues[1].toFixed(2));
    } else if (knobNote === 2) {
        /* E3: RPT on/off */
        const rptActive = fxActive[lastRepeatSlot] || fxLatched[lastRepeatSlot];
        showOverlay(fxName(lastRepeatSlot), rptActive ? 'Loop ON' : 'Loop OFF', rptActive ? '1.00' : '0.00');
    } else if (knobNote >= 3 && knobNote <= 5) {
        /* E4-E6: per-slot params */
        const pi = knobNote - 3;
        if (lastTouchedSlot >= 0 && lastTouchedSlot < NUM_SLOTS) {
            const slot = lastTouchedSlot;
            showOverlay(fxName(slot), paramLabel(slot, pi),
                       paramValue(slot, pi).toFixed(2));
        } else {
            showOverlay('No FX', 'Tap a pad first', '');
        }
    } else if (knobNote === 6) {
        /* E7: Tilt EQ */
        showOverlay('Global', 'Tilt', globalValues[3].toFixed(2));
    } else if (knobNote === 7) {
        if (shiftHeld) {
            showOverlay('Global', 'Dry/Wet', globalValues[5].toFixed(2));
            return;
        }
        /* E8: DJ Filter */
        showOverlay('Global', 'DJ Flt', globalValues[4].toFixed(2));
    }
}

function handleJogScroll(delta) {
    /* Jog scroll adjusts BPM in coarse steps */
    bpm = Math.max(20, Math.min(300, bpm + delta * 1.0));
    sendParam('bpm', bpm.toFixed(1));
    showOverlay('Tempo', `${bpm.toFixed(1)} BPM`, (bpm / 300).toFixed(2));
}

/* Ask the DSP which effects it actually has running and shut down any that
 * every local signal says should be off. Deliberately conservative: it only
 * acts when the slot is not held, not latched and not locally active, so a
 * legitimately-running effect can never be cut. */
function reconcileWithDsp() {
    let activeStr;
    try {
        activeStr = getParam('fx_active');
    } catch (e) { return; }
    if (!activeStr) return;
    let active;
    try { active = JSON.parse(activeStr); } catch (e) { return; }
    if (!Array.isArray(active)) return;

    for (let i = 0; i < NUM_SLOTS; i++) {
        if (active[i] === 1 && !fxActive[i] && !fxLatched[i] && !fxHeld[i]) {
            console.log(`[pfx] stuck FX repaired: slot ${i} (${fxName(i)}) — DSP had it on, UI did not`);
            sendParam(`punch_${i}_off`, '1');
            refreshPadLED(i);
        }
    }
}

function syncFxState() {
    try {
        const activeStr = getParam('fx_active');
        if (activeStr) {
            const active = JSON.parse(activeStr);
            for (let i = 0; i < NUM_SLOTS; i++) {
                fxActive[i] = active[i] === 1;
            }
        }
    } catch (e) { /* ignore */ }

    try {
        const latchedStr = getParam('fx_latched');
        if (latchedStr) {
            const latched = JSON.parse(latchedStr);
            for (let i = 0; i < NUM_SLOTS; i++) {
                fxLatched[i] = latched[i] === 1;
            }
        }
    } catch (e) { /* ignore */ }

}


/* ================================================================
 * Lifecycle
 * ================================================================ */

globalThis.init = function() {
    console.log('Performance FX v2 module initializing');

    /* State persistence disabled — always start fresh.
     * Deliberately no sendParam('bpm', ...) here: that would immediately tell
     * the DSP a tempo was chosen manually and kill host-tempo follow. */
    lastHostBpm = 0;
    hostBpmPollCounter = 0;
    fetchFxNames();
    sendParam('bpm_follow_host', '1');
    for (let i = 0; i < NUM_GLOBALS; i++) {
        if (GLOBAL_KEYS[i] === 'rpt_toggle') continue;
        sendParam(GLOBAL_KEYS[i], GLOBAL_DEFAULTS[i].toFixed(3));
    }

    ledInitPending = true;
    ledInitIndex = 0;
};

/* Called by the host at the top of exitOvertakeMode(), before the LED queue is
 * torn down. Covers exits this module did not initiate — the Tools shortcut and
 * the overtake menu both leave without routing through our Back handler. */
globalThis.onUnload = function() {
    clearAllModuleLEDs();
};

/* Called by the host when this module is un-parked. init() is deliberately not
 * re-run, so anything init() would have set up has to be re-established here.
 * Latched FX are still running in the DSP — we are only restoring the surface. */
globalThis.onResume = function() {
    /* Do NOT paint here. The host's 0->2 entry transition queues an
     * all-LEDs-off sweep on the audio side, which runs progressively over the
     * next few frames and would wipe anything painted now — on a fresh load the
     * deferred init() waits for that sweep to finish, but resume has no such
     * gate. Defer instead, and force the writes when they happen: setLED is
     * cache-guarded by a module-scope cache that survives the park, so an
     * ordinary repaint after the sweep would be skipped as "already set". */
    ledResumeDelay = LED_RESUME_DELAY_TICKS;
    ledResumeRepeatsLeft = LED_RESUME_REPEATS;
    overlayTimer = 0;          /* drop any overlay frozen from before the park */
    hostBpmPollCounter = 0;    /* re-check the project tempo promptly */
    syncHostBpm(true);
};

globalThis.tick = function() {
    /* Parked in the background: the host has swapped the draw and LED bindings
     * for no-ops, but host_flush_display() is NOT one of them — flushing here
     * would push our stale buffer over whatever Move is showing. Keep the param
     * work (a latched effect is still running and still wants its knob values
     * and the stuck-FX reconcile) and skip the entire surface. */
    if (globalThis.overtakeParked) {
        drainCriticalRetries();
        drainParamQueue();
        if (++reconcileCounter >= RECONCILE_TICKS) {
            reconcileCounter = 0;
            reconcileWithDsp();
        }
        return;
    }

    if (ledResumeDelay > 0) {
        if (--ledResumeDelay === 0) {
            repaintAllModuleLEDs();
            if (ledResumeRepeatsLeft > 0) {
                ledResumeRepeatsLeft--;
                ledResumeDelay = LED_RESUME_REPEAT_TICKS;
            }
        }
        /* Keep the display alive while we wait — only the LEDs are deferred. */
        if (overlayTimer > 0) drawOverlay(); else drawMainView();
        host_flush_display();
        return;
    }

    if (ledInitPending) {
        setupLedBatch();
        return;
    }

    /* Retry any critical send the host refused, before anything else queues. */
    drainCriticalRetries();

    /* Drain queued params (pressure, knob values, etc.) */
    drainParamQueue();

    /* Backstop for a punch-off that never landed. */
    if (++reconcileCounter >= RECONCILE_TICKS) {
        reconcileCounter = 0;
        reconcileWithDsp();
    }

    /* Follow the project tempo (cheap poll — the host updates it as clock
     * arrives, and syncHostBpm() no-ops unless the value actually moved). */
    if (++hostBpmPollCounter >= HOST_BPM_POLL_TICKS) {
        hostBpmPollCounter = 0;
        syncHostBpm(false);
    }

    /* Render display */
    if (overlayTimer > 0) {
        drawOverlay();
    } else {
        drawMainView();
    }
    host_flush_display();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* Filter clock and sysex noise, but NOT aftertouch or capacitive touch */
    if (data[0] === 0xF8 || data[0] === 0xF0 || data[0] === 0xF7) return;

    /* Capacitive touch on knobs (notes 0-9) = knob peek */
    if (status === 0x90 && d1 < 10 && d2 > 0) {
        handleKnobPeek(d1);
        return;
    }
    if (status === 0x80 && d1 < 10) return;

    /* Polyphonic aftertouch - pad pressure */
    if (status === 0xA0) {
        handleAftertouch(d1, d2);
        return;
    }

    /* Channel aftertouch - broadcast to all active punch-ins (throttled) */
    if (status === 0xD0) {
        const now = Date.now();
        for (let i = 0; i < NUM_SLOTS; i++) {
            if (fxActive[i] && now - lastPressureTime[i] >= PRESSURE_THROTTLE_MS) {
                lastPressureTime[i] = now;
                sendParam(`punch_${i}_pressure`, (d1 / 127.0).toFixed(3));
            }
        }
        return;
    }

    /* Note On */
    if (status === 0x90) {
        if (d2 > 0) {
            if (d1 >= 68 && d1 <= 99) {
                handlePadOn(d1, d2);
                return;
            }
        } else {
            if (d1 >= 68 && d1 <= 99) {
                handlePadOff(d1);
                return;
            }
        }
    }

    /* Note Off */
    if (status === 0x80) {
        if (d1 >= 68 && d1 <= 99) {
            handlePadOff(d1);
        }
        return;
    }

    /* CC Messages */
    if (status === 0xB0) {
        /* Shift */
        if (d1 === MoveShift) {
            shiftHeld = d2 > 0;
            /* Shift pressed while holding pads → latch/unlatch them */
            if (shiftHeld) {
                for (let i = 0; i < NUM_SLOTS; i++) {
                    if (fxHeld[i]) {
                        if (fxLatched[i]) {
                            /* Already latched: unlatch (will release on pad-off) */
                            fxLatched[i] = false;
                            sendParam(`punch_${i}_latch`, '0');
                            showOverlay(fxName(i), 'Unlatched', '');
                        } else {
                            /* Not latched: latch it */
                            fxLatched[i] = true;
                            sendParam(`punch_${i}_latch`, '1');
                            showOverlay(fxName(i), 'Latched', '');
                        }
                        refreshPadLED(i);
                    }
                }
            }
            return;
        }

        /* Back - CLEAN EXIT */
        if (d1 === MoveBack && d2 > 0) {
            for (let i = 0; i < NUM_SLOTS; i++) {
                sendParam(`punch_${i}_off`, '1');
                sendParam(`punch_${i}_latch`, '0');
            }
            sendParam('bypass', '1');
            clearAllModuleLEDs();
            host_exit_module();
            return;
        }

        /* Undo - Bypass (tap=toggle, hold=momentary) */
        if (d1 === MoveUndo) {
            if (d2 > 0) {
                undoHeld = true;
                undoWasBypassed = bypassed;
                if (!bypassed) {
                    bypassed = true;
                    sendParam('bypass', '1');
                    setButtonLED(MoveUndo, WhiteLedBright);
                    showOverlay('FX', 'BYPASSED', '');
                } else {
                    bypassed = false;
                    sendParam('bypass', '0');
                    setButtonLED(MoveUndo, WhiteLedDim);
                    showOverlay('FX', 'ACTIVE', '');
                }
            } else {
                if (undoHeld && bypassed && !undoWasBypassed) {
                    bypassed = false;
                    sendParam('bypass', '0');
                    setButtonLED(MoveUndo, WhiteLedDim);
                }
                undoHeld = false;
            }
            return;
        }

        /* Copy - unused */
        if (d1 === MoveCopy && d2 > 0) {
            return;
        }

        /* Jog wheel turn */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (shiftHeld) {
                /* Shift+Turn = BPM fine */
                bpm = Math.max(20, Math.min(300, bpm + delta * 0.5));
                    sendParam('bpm', bpm.toFixed(1));
                showOverlay('Tempo', `${bpm.toFixed(1)} BPM`, (bpm / 300).toFixed(2));
            } else {
                /* Turn = scroll through active/latched FX */
                handleJogScroll(delta);
            }
            return;
        }

        /* Jog click */
        if (d1 === MoveMainButton && d2 > 0) {
            if (shiftHeld) {
                /* Shift+Click = unused */
            } else {
                /* Click = tap tempo */
                handleTapTempo();
            }
            return;
        }

        /* Knobs E1-E8 */
        if (d1 >= MoveKnob1 && d1 <= MoveKnob8) {
            const knobIdx = d1 - MoveKnob1;
            const delta = decodeDelta(d2);
            handleKnob(knobIdx, delta);
            return;
        }

        /* Master knob - DO NOT intercept CC 79, let it pass through for volume */

    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* Pass external MIDI through for potential future use */
};
