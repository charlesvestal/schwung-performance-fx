# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

Performance FX is a live audio effects processor overtake module for [Move Anything](https://github.com/charlesvestal/move-everything) on Ableton Move hardware. It provides 32 pressure-sensitive punch-in audio effects with latching and three
knobs per effect. (There are no scenes and no step sequencer — earlier versions
of this file and of `module.json` claimed both; neither was ever built.)

Accessed via the Overtake Modules menu (Shift+Vol+Jog Click).

## Architecture

```
src/
  module.json           # Module metadata (tool, API v2)
  ui.js                 # JavaScript UI (pads, knobs, display)
  help.json             # On-device help text
  dsp/
    perf_fx_dsp.h       # DSP engine types and API
    perf_fx_dsp.c       # DSP engine implementation (all 32 effects)
    perf_fx_plugin.c    # Plugin API v2 wrapper (params, MIDI, rendering)
    test_perf_fx.c      # Unit tests (compile & run on host)
    plugin_api_v1.h     # Vendored from move-anything (host API ABI)
    pfx_track_shm.h     # Vendored from move-anything (Link Audio shm ABI)
scripts/
  build.sh              # Cross-compile via Docker
  install.sh            # Deploy to Move device
  Dockerfile            # ARM64 cross-compilation environment
```

### Vendored Headers

One header is vendored from `move-anything/src/host/`:

- **`plugin_api_v1.h`** — Host/plugin ABI. Defines `host_api_v1_t`, `plugin_api_v2_t`. Source of truth: `move-anything/src/host/plugin_api_v1.h`

This is a stable ABI contract. If the host changes it, both repos must be updated.

`pfx_track_shm.h` used to be vendored here too, for a per-track Link Audio input
mode. Nothing selected that mode after tracks mode was removed from the UI, but
the wrapper still mapped the shared memory and copied four buffers per block for
data no code read. Both the header and the path are gone.

### Parameters: one source of truth

`pfx_fx_desc[]` in `perf_fx_dsp.c` is the only place FX names, param names,
param defaults and signal topology are defined. `perf_fx_plugin.c` exports it
(`fx_names`, `fx_params_<slot>`) and `ui.js` reads it.

Do not add a label table anywhere else. There used to be three — one per file —
and they disagreed: the UI advertised 91 knobs of which 72 controlled nothing,
and 4 more moved a different parameter than the label claimed.

**A wet control is earned, not automatic.** `wet_param` in the descriptor names
the index of the wet amount, or -1 for none, and `apply_wet()` is the single
place it is applied — insert FX crossfade dry against wet, sends scale only what
they added so the dry always survives. Individual effects must not carry their
own dry/wet blend.

Sixteen slots have one; sixteen do not. It belongs on sends and on effects used
in parallel (saturation, bitcrush, octave down, vinyl, phaser, flanger, resonant
peak). It does not belong on anything that moves audio in time — repeats,
stutter, scatter, reverse, timestretch, vinyl brake — because the wet signal
there is a delayed copy of the dry and blending them comb-filters. Nor where it
would duplicate an existing knob: Mix on a tremolo is just its Depth again.
Those slots spend the knob on something real (Decay, Depth, Center, Shape), and
one, Timestretch, honestly declares its third param unused rather than carrying
filler.

`test_params_are_live()` renders every declared param at 0.0 and at 1.0 and
fails if the audio is identical, so a name with nothing behind it cannot ship.

### DSP Engine

All 32 effects are implemented in C with Bungee (C++ time-stretch library) for the stretch FX:

Every effect has three knobs (E4-E6) on top of its pressure response, with E6
always the wet amount. See `pfx_fx_desc[]` for the exact per-slot assignment.

**Row 4 — Time/Repeat**: RPT 1/4, RPT 1/8, RPT 1/16, RPT Triplet, Stutter, Scatter, Reverse, Stretch
**Row 3 — Filter Sweeps**: LP Sweep, HP Sweep, BP Rise, BP Fall, Reso Sweep, Phaser, Flanger, AutoFilter
**Row 2 — Space/Delay**: Delay 1/4, Delay D8, PingPong 1/4, PingPong D8, Room, Hall, Dark Verb, Spring
**Row 1 — Distortion/Rhythm**: Crush, Downsample, Saturate, Gate, Tremolo, Octave Down, Vinyl, Vinyl Break

All FX are punch-in (hold=on, release=off) with pressure sensitivity and Shift+hold latching.

### UI Module

`ui.js` is a standalone JavaScript module following the Move Anything overtake module pattern:

- Exports `init()`, `tick()`, `onMidiMessageInternal()` via `globalThis`
- Uses shared utilities from `move-anything/src/shared/` (constants, display, input filter)
- Communicates with DSP via `host_module_set_param()` / `host_module_get_param()`
- Always starts fresh (no state persistence)

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

The build compiles `perf_fx_plugin.c` and `perf_fx_dsp.c` together into `dsp.so`, linked with `-lm`. It links with `g++` because Bungee is C++, so the module needs `libstdc++` and `libgcc_s` on the device (both present). `-lrt` was dropped along with the Link Audio shared-memory path.

### Running Tests Locally

```bash
cc -o test_pfx src/dsp/test_perf_fx.c src/dsp/perf_fx_dsp.c \
   src/dsp/pfx_bungee_stub.c -Isrc/dsp -lm && ./test_pfx
```

`pfx_bungee_stub.c` is required: the real stretcher is C++ and pulls in Bungee
and pffft, which do not link into a plain `cc` host binary. Without it the
command in this file simply failed to link, which is why a suite that had been
red for several commits went unnoticed.

## Code Style

- **C**: Snake_case. Prefix engine functions with `pfx_`. Log with `pfx:` prefix.
- **JavaScript**: Follows Move Anything module conventions. Host functions are `snake_case`.
- **Parameters**: Keys are `snake_case` (e.g. `punch_5_param_2`, `dry_wet`).

## Key Design Decisions

- **All punch-in**: Every FX is hold=on, release=off for live performance feel
- **Knob sets base, pressure modulates**: `pfx_mod()` centres pressure on the
  knob position, so adding knobs took no expression away from the pads
- **Immediate rate changes**: Repeat rate knob takes effect immediately (no waiting for loop quantum)
- **No state persistence**: Always starts fresh — removed to simplify

## Release

1. Update version in `src/module.json`
2. Commit: `git commit -am "bump version to X.Y.Z"`
3. Tag and push: `git tag vX.Y.Z && git push --tags`
4. GitHub Actions builds and uploads tarball
5. Add release notes: `gh release edit vX.Y.Z --notes "- Changes here"`

## Host Requirements

- **Minimum host version**: 0.7.10
- Uses `audio_fx_api_v2` interface (available since early host versions)

### End-of-chain FX placement

`"end_of_chain": true` in `capabilities` asks the host to run this module on the
final Move+ME mix instead of the ME bus alone, so Move's own tracks are
processed without Link Audio routing.

Hosts that predate the capability **ignore it** and there is no fallback needed:

- **Move→Schwung ON** — unchanged. The host sets `fx_target = mailbox_audio`
  under `rebuild_from_la`, which already is the full mix, so Move's audio
  reaches this module through the existing path.
- **Move→Schwung OFF** — ME bus only, the pre-0.1.0 behaviour.

So on an older host the module still works; it just needs Move→Schwung enabled
to hear Move, which is the ~16ms Link Audio round trip that `end_of_chain`
exists to avoid. Requires host **0.12.0+** (first release containing the
capability) for the routing-free path.

### Host tempo

The DSP follows the host's project tempo via `host_api_v1_t.get_bpm()`, which
has been present and populated since well before 0.11.6 — no host upgrade
needed. MIDI clock never reaches the module (`onMidiMessageInternal` drops
0xF8), so `get_bpm()` is the only route; it resolves through the host's
`sampler_get_bpm()` chain and works with no clock running.

Keep `src/dsp/plugin_api_v1.h` in sync with `schwung/src/host/plugin_api_v1.h`.
It drifted once and silently hid four host capabilities, `get_bpm` among them.
