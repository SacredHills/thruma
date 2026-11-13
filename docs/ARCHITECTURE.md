# Architecture

```
Source/
  Core/      pure C++17 DSP: no JUCE, no allocation or locks on the audio path; unit-tested directly
    Instruments        per-instrument detection profiles (bands, windows, decay, default MIDI note)
    TransientDetector  two-band onset detector (live + offline), fingerprints, sub-ms positioning
    Triggers           editable trigger lists, two-pass take analysis, Standard MIDI File writer
    Kit / SampleEngine sample data, velocity layers, round robin, polyphonic one-shot playback
    Resampler          Kaiser-windowed sinc SRC (~110 dB SNR), used offline when samples load
    ReplaceEngine      detection → lookahead-compensated triggering → dry/wet mix
    FFT / Debleed      STFT spectral debleed + offline analysis (reference detection, decay fitting)
    Rcu                lock-free publication of immutable objects to the audio thread
    LinkHub            timeline-keyed audio sharing between plugin instances
  Plugin/    JUCE layer: parameters, state, kit file I/O, capture, MIDI, UI
Tests/       DSP test suite (synthetic multitrack sessions), real-recording harness, VST3 host validator
packaging/   Inno Setup script, macOS pkg script, Linux install script
```

## Signal flow (per block)

```
input ──┬─► capture (host timeline) ─────────────────────────────► waveform editor / Analyze
        ├─► LinkHub publish (if "Share this track as")
        │
        └─► [link safety delay] ─► DEBLEED (STFT, N = 1024 @ ≤ 50 kHz) ─► REPLACE ─► output
                                     ▲ reference: sidechain L/R/L+R          │  ▲
                                     │ or LinkHub (other instance)           │  └─ MIDI in → kit slots
                                     └───────────────────────────────────────┴─► MIDI out (hits)
```

Reported latency = replacement lookahead (10 ms or 3 ms) + debleed FFT size (if on) + link safety delay (if linked). The host validator checks that the measured delay equals this value exactly.

## Detection

Each instrument profile defines a **body band**, used for level, velocity and confirmation (for example 140–2500 Hz for snare), and an **attack band**, used for onset rise (for example 1.2–5 kHz, the stick crack or beater click). The per-sample processing:

1. Both bands are filtered: 4th-order Butterworth high-pass plus a low-pass. Each band gets a peak-hold envelope; the hold is long enough to bridge zero crossings, so low frequencies produce no ripple.
2. A **candidate** starts when the attack-band (or body-band) envelope rises by more than the sensitivity-dependent threshold within the rise window and is above the bleed floor. The floor is a minimum statistic over about 400 ms.
3. The candidate is **confirmed** after the velocity window (2.5–6 ms) only if all of these hold:
   - **Level**: its body peak is within the instrument's level range of the reference level, which is either the adaptive running reference or, from Analyze, the 98th percentile of the take.
   - **Rise**: the body rose. While the previous hit still rings, a hit-like attack is also required, which rejects beating between overlapping decays. Alternatively, a strong fresh attack on a ringing drum confirms a double hit.
   - **Focus**: body vs. full-band level shows the energy is mostly in-band.
   - **Fingerprint**: for quieter, uncertain candidates, the 8-band spectral shape must match the instrument's fingerprint. The fingerprint is learned from confident hits, or from the whole take by Analyze.
4. **Positioning**: the detector walks back from the candidate's peak to the first threshold crossing of a 0.7 ms-held envelope of the signal high-passed at half the attack band. Low ringing from the previous hit is ignored.
5. **Velocity** is the body peak mapped over the dynamic range below the reference. A hit more than 14 dB below the reference is flagged as a ghost note.

The detector reports each hit at most `backtrack + velocity window` samples after its onset. The replace engine delays the dry signal by the lookahead L, which is at least that report delay, so the sample starts exactly on the onset in the output.

## Sample engine

Kits are immutable snapshots resampled to the host rate when they are built, so playback is a straight sample copy. Per hit:
- a velocity layer is chosen by range;
- a sample is chosen by cycle, or at random without immediate repeats;
- the level is set so that `peak = slotMax + (v − 1)·range·response`;
- leading silence is skipped (auto-align).

Voices can start at any future sample offset. Each instrument is limited to 8 voices, with 3 ms fades on steals.

## Debleed

The STFT uses a sqrt-Hann window at 75 % overlap, with N = 1024/2048/4096 by sample rate. Per bin:

- **Target model** `E`: at frames containing a detected onset, `E = max(E, P − B_ref)`; otherwise `E` decays with the bin's learned time constant τ. The detector runs on the input, which is N samples ahead of the STFT output, so onsets are always known in time (lookahead).
- **Bleed estimate**:
  - Reference mode: `B = |H|²·|R|²`, where `H = S_xr / S_rr` is learned with weights proportional to how bleed-dominated the bin is. Analyze can initialise it, blended by learning confidence. The estimate has a 60 ms release, and the 35 % of excess energy not explained by the target model is added.
  - Blind mode: `B = max(P − E, 0)`.
- **Gain**: decision-directed Wiener gain with over-subtraction `0.5 + 2.5·Amount`, raised to at least `√(E/P)` (the drum model is never cut), to at least the attack-window protection, and to at least the floor `−MaxReduction·Amount` dB. The gain is smoothed over a constant-Q frequency window and then in time: instant open, release set by the Release control.
- The same real gain is applied to all channels, so the output is zero-phase and phase-coherent across channels.

**Analyze** (offline):
1. Two-pass take detection.
2. Classifies inactive frames, falling back to shorter decay windows for dense playing.
3. Computes the bleed floor.
4. Scores each reference candidate by magnitude-squared coherence, weighted by target energy (100 Hz–8 kHz), and takes `|S_xr|²/S_rr²` from the best one as the transfer.
5. Fits per-band decay slopes after isolated hits; the median gives τ.
6. Suggests an Amount from the bleed level.

## Real-time safety

- Nothing is allocated, freed or locked on the audio thread; the test suite counts allocations through the whole chain.
- The UI publishes kits, trigger lists and models as immutable objects through `RcuPublisher`. The audio thread picks up the newest one at the start of each block and reports the oldest object a voice still uses. The writer frees only older objects.
- Parameter changes that alter latency (lookahead, debleed on/off, reference = linked) are applied on the message thread with processing suspended. Then `setLatencySamples` is called.
- Host blocks larger than the prepared size are processed in chunks. Output is bit-identical for any buffer size (tested).

## Limitations

See the README for the user-facing list. Technical notes:
- Linking requires a shared process and host timeline. Without a running transport the reference reads as silence, and debleed falls back to blind mode.
- The debleed's reference transfer is magnitude-only. Delays between mics larger than roughly the frame length (about 20 ms) reduce reference effectiveness.
- The listening buffer is allocated in prepareToPlay: 4 minutes per channel at the session rate, about 46 MB per channel at 48 kHz.
- Velocity mapping is level-based (dB). Velocity estimation does not use timbre.
