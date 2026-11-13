# Thruma User Guide

Thruma has three pages: **REPLACE**, **DEBLEED** and **SAMPLES**. The header shows the plug-in's total latency, which your DAW compensates automatically, a hit LED that flashes on every triggered hit, and input/output meters.

Most controls have tooltips: hover over them for a short explanation. Double-click a slider to reset it.

---

## 1. Typical workflows

### Live replacement (quickest)
1. Insert Thruma on the drum track and choose its **Instrument** (Kick, Snare, Tom 1–4, Hi-Hat, Crash, Ride, China, Splash).
2. Give that instrument some samples: drop WAV files onto the REPLACE page, or use the SAMPLES page.
3. Play. Adjust **Sensitivity** until every hit lights the LED and bleed does not.
4. Choose **Mode**:
   - **Replace** crossfades the original and the sample with **Blend**.
   - **Layer** keeps the original and adds the sample at the **Blend** level.

If the selected instrument has no samples, the original passes through untouched, so the track never goes silent.

### Tracker-style editing (most accurate)
1. Press **CAPTURE** and play the section (up to 4 minutes per instance). Thruma records its input on the host timeline. Nothing in your project is changed.
2. Press **ANALYZE**. Thruma runs two passes over the whole take: it learns the loudest hits and the drum's spectral fingerprint, then detects again with those settings. Velocities are therefore consistent across the take, and bleed that does not look like the drum is rejected. The trigger source switches to **Edited triggers** automatically.
3. Correct anything you need in the waveform editor (see below). Playback now uses your edited list.
4. Optionally use **Export MIDI** to save the performance as a `.mid` file.

### Debleed
1. Turn on **Debleed** (on the DEBLEED page). This adds about 21 ms of compensated latency.
2. Choose a **Reference** if you have one (see section 3). Otherwise, *None (blind)* works from the drum's own hits.
3. Capture a section on the REPLACE page, then press **ANALYZE** on DEBLEED. Thruma learns:
   - the drum's decay per frequency
   - its fingerprint
   - the bleed spectrum
   - how the reference leaks into this mic

   It then picks the best reference and sets a suitable Amount.
4. Fine-tune with **Amount** and **Release**, and watch the gain-reduction display.

Debleed runs **before** replacement, so live detection also benefits from the cleaner signal.

---

## 2. REPLACE page

### Waveform editor
| Action | How |
|---|---|
| Add a trigger | Double-click on empty waveform. Velocity is estimated from the audio |
| Select | Click a trigger; Shift-click to add to the selection; drag on empty space to box-select; Ctrl/Cmd+A selects all |
| Move | Drag a selected trigger left or right; ←/→ nudge by one sample (Shift: 1 ms) |
| Velocity | Ctrl/Cmd-drag up or down on a trigger. The square marker's height shows velocity |
| Delete | Delete/Backspace, or right-click → Delete |
| Audition | Click a trigger to hear the original hit; Alt-click or double-click to hear the replacement |
| Zoom / scroll | Mouse wheel zooms at the cursor; Shift+wheel or the scrollbar scrolls; **+ / − / Fit** buttons; **F** fits |
| Undo / Redo | **Undo/Redo** buttons, or Ctrl/Cmd+Z and Ctrl/Cmd+Shift+Z |

Ghost notes (more than 14 dB below the loudest hits) are drawn dimmer.

### Main controls
| Control | What it does |
|---|---|
| **Instrument** | The drum on this track. Selects a detection profile: frequency bands, timing windows, decay and default MIDI note. Shared with DEBLEED |
| **Sample** | Which kit slot plays for detected hits. *Same as instrument* by default; set it to another slot to, for example, trigger a kick sample from a snare track |
| **Triggers** | *Live detection*, *Edited triggers* (the list in the editor, following the host timeline) or *MIDI only* |
| **Mode** | *Replace* or *Layer* (see above) |
| **Replace** switch | Turns triggering off. MIDI-triggered samples still play |
| **Sensitivity** | Higher catches quieter hits; lower rejects more bleed |
| **Velocity Response** | 0 % plays every hit at full level; 100 % follows the played dynamics across the *Dynamic Range* |
| **Blend** | Replace: original ↔ sample balance. Layer: sample level |
| **Output** | Output level |
| **Audition** | Plays the replacement sample |

### Advanced
| Control | What it does |
|---|---|
| **Ghost Notes** | Extends detection downward for quiet hits. Most effective on snare and hi-hat |
| **Retrigger** | Minimum time between hits. *0 ms* uses the instrument's default (kick 28 ms, snare 30 ms, …) |
| **Dynamic Range** | The level range (dB) that maps to velocity 1–127, and the range Velocity Response works over |
| **Sample Gain** | Gain of the replacement samples |
| **Sample Start** | Skips into the sample to trim its attack, with a 1 ms de-click fade |
| **Align** | Shifts the replacement against the original by −5…+20 ms, for phase alignment with the other mics |
| **Double hits** | Detects flams, drags and fast double strokes while the previous hit is still ringing |
| **Frequency focus** | Rejects hits whose spectrum does not match the instrument (bleed rejection) |
| **Invert polarity** | Flips the replacement's polarity |
| **Auto align** | Lines up each sample's own transient with the detected hit |
| **MIDI out** | Sends hits as MIDI notes on channel 10, using the kit slot's MIDI note |
| **Lookahead** | *Accurate (10 ms)* gives the best timing and velocity. *Low latency (3 ms)* is for tracking; it uses a shorter velocity window |

---

## 3. DEBLEED page

The debleed is **not a gate**. For every frequency band it estimates how much of the signal is the target drum and how much is bleed, then applies smooth, phase-neutral attenuation:

- At each detected hit, the drum's spectrum is captured. It then decays at the drum's own rate, which is learned by Analyze. Everything this model explains is **kept**, so the natural sustain and tail survive. Bleed arriving during the decay, such as a crash over a snare's ring, is reduced.
- Just before and during each hit, an **attack window** stays fully open. The detector runs ahead of the audio, so this happens before the transient arrives.
- Bleed is attenuated down to a floor set by **Amount** and **Max Reduction**. It never drops to silence, so there is no pumping and no chopped cymbal tails.

| Control | What it does |
|---|---|
| **Debleed** | On/off (non-automatable, because it changes latency) |
| **Instrument** | Shared with REPLACE |
| **Reference** | *None (blind)*, *Sidechain L / R / L+R*, or *Linked instance* |
| **Share this track as** | Publishes this track's input under a name, so other Thruma instances can use it as a reference |
| **Linked source** | The shared track to use when Reference = *Linked instance* |
| **Amount** | How strongly bleed is reduced |
| **Sensitivity** | How readily quiet hits (ghost notes) of the target drum are recognised and protected |
| **Attack Preservation** | Length and strength of the untouched window at each hit (5–65 ms) |
| **Release** | How smoothly reduction returns after the drum. Longer is smoother |

**Advanced**: *Max Reduction* (the deepest cut at 100 % Amount), *Smoothing* (frequency smoothing from 1/24 to 1/3 octave), *Low Protect* (never cut below this frequency, useful to keep a kick's sub), *Decay Scale* (scales the preserved decay), *Adaptive learning* (keeps refining the bleed estimate while playing).

### Choosing a reference
- **Sidechain**: route the mic that causes the bleed (overheads for cymbals, the kick mic for kick bleed) to Thruma's sidechain input in your DAW.
- **Linked instances**: insert Thruma on the source track and type a name under *Share this track as*, for example "Kick In". On the target track choose *Reference = Linked instance* and pick "Kick In". Linking adds a compensated safety delay of two host buffers (at least 1024 samples), so the source instance has always written the audio before the target reads it. It works within one plugin process while the transport runs. Both tracks need the same latency before Thruma.
- **Analyze** compares every available reference (sidechain L, R and L+R) and selects the one that best explains the bleed. It reports the coherence it found.

The **gain-reduction display** shows the current reduction per frequency band.

---

## 4. SAMPLES page

- **WAV Browser**: click a file to preview it. Select several and press **Add to instrument** (or double-click) to add them to the selected instrument. Files are re-sorted into velocity layers by measured loudness: samples within 1.5 dB of each other form one round-robin group.
- **Import Folder…**: assigns every WAV in a folder tree to instruments by name. It recognises kick, snare, tom/floor, hat, crash, ride, china and splash, in file or folder names.
- **Kit list**: the instruments with their sample counts. Select one to edit it.
- **Layers**: each row is a velocity layer with its velocity range (1–127) and its round-robin samples.
  - Click a sample to preview it; Shift-click to remove it.
  - Drop files on a row to add round-robin samples to that layer.
  - Drop anywhere else on the page to auto-layer the files.
  - **Auto velocity** spreads the layer ranges evenly.
  - **Round robin / Random** sets how samples within a layer alternate. Random never repeats the same sample twice in a row.
  - **MIDI note** sets the note used for MIDI output and for triggering by MIDI input. General MIDI defaults are used.
- **Load Kit… / Save Kit… / New Kit**: kits are `.thrumakit` files, which are JSON listing your original file paths. Samples are never copied or converted on disk. Missing files are shown in red.

Samples are converted to the session's sample rate with a high-quality windowed-sinc resampler when they load, so playback has no interpolation artifacts.

---

## 5. MIDI
- **Out**: every detected or edited hit becomes a note-on (velocity 1–127) plus a note-off 50 ms later, on channel 10. In your DAW, route Thruma's MIDI output to an instrument track to drive a drum sampler.
- **In**: notes that match a kit slot's MIDI note trigger that slot. This works regardless of the Triggers setting, and MIDI-triggered notes are not echoed back out.
- **Export MIDI**: writes the edited trigger list as a Standard MIDI File at the current host tempo.

## 6. Saving, recall and automation
All parameters, the kit (as file paths), the trigger list, the analysis results, the link names and a waveform overview are stored in your project. Automatable parameters can be automated in the host. Lookahead, Debleed On and Reference change latency, so they are not automatable.
