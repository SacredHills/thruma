# Thruma

[![Build, test & package](https://github.com/SacredHills/thruma/actions/workflows/build.yml/badge.svg)](https://github.com/SacredHills/thruma/actions/workflows/build.yml)

**Drum replacement and debleed in one VST3 plugin** for Windows, macOS and Linux.

Thruma detects the hits on a drum mic and replaces or layers them with your own WAV samples. It also reduces bleed from the rest of the kit without gating away the drum's attack, decay or ghost notes. Most of the DSP runs on its own; the interface shows only a few controls.

| | |
|---|---|
| **Format** | VST3 (64-bit). Windows 10/11, macOS 11+ (universal: Apple Silicon + Intel), Linux x86-64 |
| **Sections** | DEBLEED, REPLACE, SAMPLES |
| **Instruments** | Kick, snare, toms 1-4, hi-hat, crash, ride, china, splash |
| **Samples** | Standard WAV (plus AIFF/FLAC). Your original files are never modified or copied |
| **Version** | 0.1.0 (first public build) |

## Features

**Replace** (tracker-style, all real time)
- A track lane shows the drum's level with a draggable threshold, and a MIDI lane with the detected hits that you edit directly: move, change velocity, add, delete, undo/redo.
- Hits are detected live from the track Thruma is on. There is no recording or analysis step, and detection learns in the background.
- Transient detection with a profile for each instrument. It catches ghost notes, flams, drags and fast double bass, places onsets to within a fraction of a millisecond, and rejects bleed by its spectral fingerprint.
- Samples: velocity layers, round-robin or random selection, velocity response, blend.
- **Analyze** the whole song at once (RAVENYR reads the track for Thruma; elsewhere, what has been played), or **Import Audio** and play it inside Thruma. Results stay adjustable: change Sensitivity and the song is re-analysed, keeping hand edits. **Finalize** freezes the notes.
- Click the time ruler to play from there (imported audio, or RAVENYR's transport).
- MIDI: drag the notes straight into your DAW, save a `.mid` file (placed on the DAW's beat grid), send hits on the MIDI output, and play the kit from MIDI input.

**Drum sample player**
- Thruma is one VST3 instrument with an audio input: use it as an insert on a drum track, or on a MIDI track where MIDI clips play the kit.
- Import a MIDI file into Thruma: it plays the kit in time with the song, no analysis.
- **PLAYER** page: the kit as a picture (two crashes). Click to play, right-click or drop files to load samples, drums light up when hit; extra sounds go on up to 32 pads.
- **MIXER** page: volume, pan, mute and solo for every drum and pad, with meters, typed-in values, and routing to 16 extra stereo outputs (multi-out instrument).
- **TDE files:** save one drum's samples, velocity layers and round robin as a single `.tde` file (like Drumagog GOG) and load it onto any drum.

**Debleed** (spectral processing, not a gate)
- Models the target drum per frequency bin: its spectrum is captured at each hit and then decays at the drum's learned rate. Everything this model explains is kept. Bleed is estimated from a reference mic when one is available, otherwise from energy the model cannot explain (blind mode).
- Preserves transients through look-ahead protection of the attack, and protects ghost notes through hit detection.
- Uses zero-phase gains that are the same for every channel, so phase stays coherent.
- Releases slowly and smooths across frequency, which avoids pumping and musical-noise artifacts.
- References can come from the **sidechain** (L, R or L+R) or from **any other track with Thruma on it**, listed by DAW track name, with no setup.
- Debleed runs **before** trigger detection, so it also cleans up what the detector hears.
- Learns the drum's decay times, its fingerprint, the bleed spectrum and the transfer from the reference mic automatically while the track plays.

**Samples**
- WAV browser with preview.
- Import a whole folder: files are assigned to instruments by name, then sorted into velocity layers and round-robin groups by their measured loudness.
- Drag and drop files onto layers.
- Save and load kits as `.thrumakit` files (JSON that references your original files).

## Install

Download the package for your system from the [Releases](https://github.com/SacredHills/thruma/releases) page. Every push also produces packages as CI artifacts.

| OS | Package | Installs to |
|---|---|---|
| Windows | `Thruma-x.y.z-Windows-x64-Setup.exe` (or the portable `.zip`) | `C:\Program Files\Common Files\VST3\Thruma.vst3` |
| macOS | `Thruma-x.y.z-macOS-Installer.pkg` (or the `.zip`) | `/Library/Audio/Plug-Ins/VST3/Thruma.vst3` |
| Linux | `.deb` (`sudo apt install ./Thruma-*.deb`) or `.tar.gz` + `install.sh` | `/usr/lib/vst3` or `~/.vst3` |

After installing, rescan plug-ins in your DAW. For manual installation, unsigned builds and Gatekeeper/SmartScreen notes, see [docs/BUILDING.md](docs/BUILDING.md#installing).

## Quick start

1. Insert Thruma on a drum track (for example the snare top mic) and set **Instrument** on the REPLACE page.
2. Drop WAV files onto the page, or use **SAMPLES**.
3. Press play. Hits are replaced in real time and appear as notes in the MIDI lane.
4. If bleed triggers notes, drag the amber threshold line above it and/or switch on **DEBLEED**.
5. Drag **Drag MIDI** into your DAW to get the performance as MIDI.

The [User Guide](docs/USER_GUIDE.md) explains every control.

## Build from source

```bash
git clone https://github.com/SacredHills/thruma.git && cd thruma
cmake -B build -DCMAKE_BUILD_TYPE=Release        # downloads JUCE 8.0.15 automatically
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

[docs/BUILDING.md](docs/BUILDING.md) covers per-platform dependencies, the installers, code signing and notarization.

## Quality and testing

The automated suite (`Tests/`) runs on every CI build on all three operating systems. It checks:

- Detection accuracy on synthetic multitrack metal sessions with realistic bleed: kick double-bass at 190 BPM gets 100 % F1 with 0.00 ms timing error, and snare backbeats get 100 % recall with a mean error of 0.04 ms.
- Velocity tracking, double hits and retrigger behavior.
- Round-robin, random and velocity-layer selection.
- Sample-accurate triggering and replacement timing within 0.05 ms at 44.1, 48 and 96 kHz.
- Bit-identical output for any host buffer size.
- MIDI files.
- Debleed effectiveness: more than 10 dB of bleed reduction with transients within 0.4 dB, and a perfect null at Amount 0.
- Phase alignment, with zero sample lag.
- No heap allocation in the audio path, and CPU load (about 2-3 % of one core for the full stereo chain).
- State restoration.

A host validator loads the **built VST3 binary** and checks:

- bus layouts, sample rates from 44.1 to 192 kHz, and odd block sizes
- that reported latency equals the measured delay
- automation sweeps and MIDI
- state round-trips into a fresh instance
- the editor

CI also runs [pluginval](https://github.com/Tracktion/pluginval) at strictness 5. You can drop annotated real recordings into `Tests/data` to measure accuracy on your own material. [docs/TESTING.md](docs/TESTING.md) has the details.

## Known limitations

This is an honest list. See also [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#limitations).

- **DAW testing:** the CI validates the actual VST3 binary with a JUCE host and pluginval on all three operating systems. Testing inside specific commercial DAWs (Cubase, Reaper, Studio One, Ableton Live, Bitwig, Logic via VST3 wrappers, and so on) still has to be done by hand.
- **Built-in test material:** the bundled tests use physically modeled, synthetic drum recordings with known ground truth. Real recordings can be added to `Tests/data` and are scored automatically.
- **Live detection on sparse instruments:** with toms that only play in a fill, live detection can trigger on loud bleed before the first real hit. Raise the threshold, or delete the stray notes in the MIDI lane.
- **Ghost notes vs bleed:** ghost notes that are exactly as loud as the bleed and spectrally similar to it can still produce false or missed triggers. Lower *Sensitivity* or edit them in the editor.
- **Debleed latency:** about 21 ms at 44.1/48 kHz, compensated by the host. It is meant for mixing, not for live monitoring. Replacement alone uses 4.5–8 ms depending on the drum (kick 8 ms, snare 6 ms), or 3 ms with the *Lookahead* parameter set to *Lowest*.
- **Linked instances:** these share audio inside one plugin process only. DAWs that sandbox or bridge plugins into separate processes cannot link; use the sidechain input there. Linking needs the transport running and equal upstream latency on both tracks.
- **One target drum per instance:** for a multi-mic kit, use one instance per track.
- **Waveform history:** the editor keeps the first 4 minutes heard per instance; live triggering continues beyond that. The heard audio itself is not saved with the project; triggers, learned settings and a waveform overview are.
- **Not yet implemented:** hi-hat articulations and choke groups, and continuous phase rotation. The *phase adjustment* features are polarity, sample-accurate align and auto-align.
- **MIDI export:** uses a constant tempo, the host tempo at the time of export.
- **Signing:** builds are unsigned unless signing secrets are configured (see BUILDING.md).

## License

No license has been chosen for Thruma's own code yet; all rights are reserved by the repository owner. Thruma is built with [JUCE](https://juce.com), which is dual-licensed under AGPLv3 or a commercial JUCE license. Distributing Thruma binaries requires complying with one of those, for example by publishing the source under AGPLv3 or by holding a JUCE commercial license. VST is a trademark of Steinberg Media Technologies GmbH.
