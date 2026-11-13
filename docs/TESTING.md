# Testing

```bash
ctest --test-dir build -C Release --output-on-failure      # everything
build/Tests/thruma_tests Detection                          # filter by test-name substring
build/Tests/ThrumaHostValidator_artefacts/Release/ThrumaHostValidator <path>/Thruma.vst3
```

The DSP suite prints the measured numbers, not only pass/fail, so CI logs track quality over time.

## Test material

`Tests/TestSignals.h` synthesises **multitrack metal sessions** with sample-exact ground truth:
- **Drums**:
  - kick: a pitch-swept body plus a beater click
  - snare: 185/330 Hz modes plus band-limited wire noise
  - toms: pitched with a pitch drop
  - hi-hat: filtered noise
  - crash: 40 inharmonic partials plus noise
- **Performance** at 190 BPM: 16th-note double-bass blast sections, a backbeat with ghost notes about 20 dB down, 35 ms drags, hats on eighths, crashes on bar starts, and a tom fill.
- **Five close mics**: each source bleeds into every other mic with its own gain (−8 to −32 dB), delay (0.8–3 ms) and off-axis low-pass, plus a −84 dBFS noise floor.

### Real recordings
Add `<name>.<instrument>.wav` plus `<name>.<instrument>.txt` (one onset time in seconds per line) to `Tests/data/`. The *Real recordings* test then runs Analyze on each take and reports precision, recall, F1 and timing. It fails below an F1 of 0.8. The WAV reader handles 16/24/32-bit PCM and 32-bit float, mixed down to mono.

## What is tested

| Area | Test | Current result (48 kHz) |
|---|---|---|
| Detection: snare mic with kick/hat/crash/tom bleed | P/R/F1, timing | main hits R = 1.00, mean error 0.04 ms; incl. ghosts P = 0.95 / R = 0.95 |
| Detection: kick, 16th double bass | P/R/F1, timing | 1.00 / 1.00, 0.00 ms |
| Detection: tom mic with −10 dB snare bleed (Analyze) | P/R | 1.00 / 1.00 |
| Detection: hi-hat / crash (overhead) | P/R | 0.88 / 1.00, 1.00 / 1.00 |
| Velocity | correlation of velocity vs. played level | 0.96 |
| Double hits / retrigger / no retrigger on a ringing hit | hit counts | ✓ |
| Sample-rate consistency (44.1 / 96 kHz) | P/R/timing | ✓ |
| Block-size independence (detector, replace, debleed) | bit-exact output for block sizes 1…2048 | ✓ |
| Round robin, random (no repeats, uniform), velocity layers, loudness layering | selection order/statistics | ✓ |
| Sample-accurate start, polarity, gain, auto-align, velocity response, voice stealing | exact sample positions/levels | ✓ |
| Replacement timing (44.1/48/96 kHz) | onset → replacement error | ≤ 0.05 ms |
| Edited triggers and MIDI triggering | exact output sample | ✓ |
| MIDI file writer | header, ticks, velocities | ✓ |
| Debleed null (Amount 0) | max error | < 2·10⁻⁵ |
| Debleed: snare blind / hat reference / analysed model | bleed reduction, SDR, transient peak, ghost, decay | 10.8 / 11.3 / 11.6 dB reduction; transients within 0.4 dB |
| Debleed: tom (analysed) | bleed reduction | 13.1 dB, transient change −0.02 dB |
| Phase alignment | cross-correlation lag vs. clean source | 0 samples |
| Reference-mic detection | picks the hat mic over kick mic and noise | ✓ (coherence 0.80 vs 0.23 / 0.00) |
| Resampler | SNR | > 108 dB |
| Real-time safety | heap allocations during processing (all engines, parameter changes, linking) | 0 |
| CPU | full stereo chain incl. reference debleed | ≈ 2–3 % of one core |
| State / persistence | trigger serialisation, model remapping between rates, RCU lifetime | ✓ |

### Host validator (built VST3 binary)
The host validator instantiates the plugin through JUCE's VST3 hosting and checks:
- MIDI in/out and the parameter count (33, with no MIDI-CC proxy parameters);
- processing at 44.1/48/96/192 kHz with block sizes 37–1024;
- that **measured delay equals reported latency**, with and without debleed;
- mono layout, and stereo with a stereo sidechain;
- random automation of every parameter;
- a **state round-trip into a new instance**;
- editor creation.

CI runs it on all three operating systems, together with **pluginval** at strictness 5.

## Manual DAW checklist
Automated host checks cannot replace a pass through real DAWs before a release:
1. The plugin scans, loads and shows the editor. Resize the window.
2. Snare track with bleed:
   - live replace;
   - Capture → Analyze → edit;
   - Edited playback after moving the playhead and looping;
   - offline bounce/export matches realtime playback.
3. Debleed on/off and changing the reference: the DAW updates delay compensation, with no clicks.
4. Sidechain routing; two linked instances (Kick → Snare reference).
5. MIDI out to a drum instrument; MIDI in triggering; Export MIDI imports at the right positions.
6. Save the project, reopen it: kit, triggers, analysis and parameters are restored. Then render offline immediately after loading.
7. Run at 44.1/48/96 kHz, at 32 and 2048 buffer sizes, and in a long session (30+ minutes).
