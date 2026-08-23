# Move Everything Performance FX

Live punch-in audio effects processor for [Move Everything](https://github.com/charlesvestal/move-everything) on Ableton Move hardware. 32 pressure-sensitive FX pads with latching.

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-everything) v0.7.10+ installed on your Ableton Move

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Open **Overtake Modules** (Shift+Vol+Jog Click)
3. Select **Module Store**
4. Navigate to **Performance FX** and install

### Manual Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Features

- **32 punch-in FX pads** — hold to activate, release to stop
- **Pressure sensitivity** — press harder for more effect intensity
- **Shift+hold latching** — lock FX on, Shift+hold again to unlatch
- **Global EQ** — tilt EQ and DJ filter on dedicated knobs

### FX Layout

#### Row 4 — Time/Repeat (Orange)

| Pad | Effect | Description |
|-----|--------|-------------|
| 1 | RPT 1/4 | Beat repeat, quarter notes |
| 2 | RPT 1/8 | Beat repeat, eighth notes |
| 3 | RPT 1/16 | Beat repeat, sixteenth notes |
| 4 | RPT TRP | Beat repeat, triplet |
| 5 | STUTTER | Glitch stutter |
| 6 | SCATTER | Rhythmic slice rearrangement |
| 7 | REVERSE | Reverse playback |
| 8 | STRETCH | Time-stretch slowdown |

#### Row 3 — Filter Sweeps (Blue)

| Pad | Effect | Description |
|-----|--------|-------------|
| 1 | LP SWP | Low-pass filter sweep down |
| 2 | HP SWP | High-pass filter sweep up |
| 3 | BP RISE | Band-pass filter sweep up |
| 4 | BP FALL | Band-pass filter sweep down |
| 5 | RESO SW | Resonant sweep |
| 6 | PHASER | Phaser |
| 7 | FLANGER | Flanger |
| 8 | AUTOFLT | Auto-filter (LFO-swept SVF) |

#### Row 2 — Space/Delay (Purple)

| Pad | Effect | Description |
|-----|--------|-------------|
| 1 | DLY 1/4 | Delay, quarter note |
| 2 | DLY D8 | Delay, dotted eighth |
| 3 | PP 1/4 | Ping-pong delay, quarter note |
| 4 | PP D8 | Ping-pong delay, dotted eighth |
| 5 | ROOM | Room reverb |
| 6 | HALL | Hall reverb |
| 7 | DK VERB | Dark reverb |
| 8 | SPRING | Spring reverb |

#### Row 1 — Distortion/Rhythm (Pink/Green/Yellow)

| Pad | Effect | Description |
|-----|--------|-------------|
| 1 | CRUSH | Bit crusher |
| 2 | DWNSMPL | Sample rate reduction |
| 3 | SATURATE | Saturation/drive |
| 4 | GATE | Rhythmic gate |
| 5 | TREMOLO | Tremolo/autopan |
| 6 | OCT DN | Octave down pitch shift |
| 7 | VINYL | Vinyl noise overlay |
| 8 | VNL BRK | Vinyl brake slowdown |

### Encoders

| Knob | Function |
|------|----------|
| E1 | Repeat Length |
| E2 | Repeat Speed |
| E3 | Repeat Loop on/off |
| E4-E6 | Per-FX params for the last touched pad (see below) |
| E7 | Tilt EQ |
| E8 | DJ Filter |
| Shift+E8 | Global Dry/Wet |

Touch any knob to see its current value. Tap a latched pad to select it for
E4-E6 editing without unlatching. The display always shows the selected pad's
own three labels, read from the DSP, so what you see is what that effect
actually implements.

#### Per-FX Parameters (E4 / E5 / E6)

Knobs set the base value and pressure moves it either side of that, so a knob
never takes expression away from the pad.

| Effect | E4 | E5 | E6 |
|--------|----|----|-----|
| RPT 1/4 – RPT TRP | Filter | Gate | Decay |
| STUTTER | Filter | Gate | Decay |
| SCATTER | Pattern | Gate | Reverse |
| REVERSE | Length | Filter | Decay |
| STRETCH | Speed | Filter | — |
| LP SWP / HP SWP | Speed | Reso | Depth |
| BP RISE / BP FALL | Speed | Reso | Depth |
| RESO SW | Speed | Reso | Mix |
| PHASER | Depth | Feedback | Mix |
| FLANGER | Depth | Feedback | Mix |
| AUTOFLT | Depth | Reso | Center |
| DLY 1/4 / DLY D8 | Feedback | Tone | Level |
| PP 1/4 / PP D8 | Feedback | Tone | Level |
| ROOM / HALL / DK VERB / SPRING | Decay | Tone | Level |
| CRUSH | Bits | Tone | Mix |
| DWNSMPL | Rate | Tone | Mix |
| SATURATE | Drive | Tone | Mix |
| GATE | Rate | Duty | Depth |
| TREMOLO | Rate | Depth | Shape |
| OCT DN | Pitch | Tone | Mix |
| VINYL | Noise | Warmth | Mix |
| VNL BRK | Rate | Noise | Tone |

A wet control (Mix, or Level on the sends) only appears where blending against
the dry signal is a real technique. Effects that move audio in time get
something more useful instead — their wet signal is a delayed copy of the dry,
so mixing the two comb-filters rather than blends. SCATTER's Pattern selects one
of five slice orders: Shuffle, Stutter, Interleave, Scramble, Retrograde.

### Controls

| Control | Function |
|---------|----------|
| Pads | Hold = FX on, release = off |
| Pressure | Modulates the selected parameter around its knob position |
| Shift+Pad | Latch/unlatch FX |
| Undo (tap) | Bypass toggle |
| Undo (hold) | Momentary bypass |
| Jog turn | Coarse BPM |
| Shift+Jog turn | Fine BPM |
| Back | Exit |

## Building

Requires Docker for ARM64 cross-compilation:

```bash
./scripts/build.sh      # Build via Docker
./scripts/install.sh    # Deploy to Move
```

## Third-Party Libraries

- **[Bungee](https://github.com/kupix/bungee)** — Time-stretching library (MPL-2.0)
- **[Eigen](https://eigen.tuxfamily.org/)** — Linear algebra (MPL-2.0)
- **[PFFFT](https://bitbucket.org/jpommier/pffft/)** — FFT library (BSD-like, FFTPACK license)
- **[cxxopts](https://github.com/jarro2783/cxxopts)** — Command-line parser (MIT) (build-time dependency of Bungee)
- **RevSC** — Costello/Soundpipe-style FDN reverb (MIT, clean reimplementation)

## License

MIT
