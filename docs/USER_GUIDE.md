# Thruma User Guide

Thruma has three pages: **DEBLEED**, **REPLACE** and **SAMPLES**. Everything works in real time on the track Thruma is inserted on: press play in your DAW and it listens. There is nothing to record or analyse first.

The header shows the plugin's latency (your DAW compensates it automatically), a hit light, and input/output meters. Hover any control for a short explanation; double-click a slider to reset it.

---

## Where to put Thruma

Thruma is one plugin (a VST3 *instrument* with an audio input) that you can use two ways:
- **On a drum audio track** (as an insert): it tracks the drum, replaces or layers it, and makes MIDI.
- **On a MIDI / instrument track**: it is a drum sample player. MIDI clips on the track play the kit (SAMPLES page). You can also import a MIDI file into Thruma itself (see below).

Some DAWs (e.g. Studio One) only offer instruments on instrument tracks, with no main audio input. There, send the drum track to Thruma's **sidechain**: with no main input, Thruma tracks the sidechain. RAVENYR lists Thruma as an insert on audio tracks too.

**Blend** mixes the original drum (0 %) with the samples (100 %). It needs samples loaded for the selected drum (SAMPLES page). With no samples, the original always passes through unchanged.

## REPLACE

The Replace page works like a tracker. It has a track lane and a few controls.

### The track lane
- **AUDIO lane:** the drum's level as the song plays (teal).
  - The dashed amber line is the **threshold**. **Drag it up or down.** Hits that rise above it become notes; bleed that stays under it is ignored.
  - **Trigger points:** dim **rings** are possible hits Thruma noticed but didn't use. **Hover one** to see why: *too quiet* (under the threshold), *no clear attack* (sounded like the last hit still ringing), *sounded like bleed* from another drum, or *too soon* after the previous hit. Each reason tells you which control to turn. **Click a ring** to add it as a note.
- **MIDI lane:** every detected hit, as a note with its velocity.

Editing in the MIDI lane:

| Action | How |
|---|---|
| Move notes | Drag a note sideways |
| Change velocity | Drag the dot on top of a note up or down |
| Add a note | Double-click an empty spot (height = velocity) |
| Select | Click; Shift-click to add; drag on empty space to box-select; Ctrl/Cmd+A selects all |
| Delete | Delete / Backspace |
| Undo / Redo | Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z |
| Play from here | **Click the time ruler** at the top. With imported audio this sets where Thruma's own playback starts; in RAVENYR it moves the DAW's play position. Other DAWs don't let plugins move their playhead |
| Zoom / scroll | **ZOOM slider** (bottom right): all the way left shows the whole song. Mouse wheel zooms too; Shift+wheel or the scrollbar scrolls; **F** shows the whole song |

The lane follows the playhead while the song plays.

**Live vs Finalized:**
- **Live** (default): hits are detected every time the song plays. Replaying a section re-detects it without duplicates. Notes you edited by hand are kept.
- **Finalize:** press it when you are happy with the notes. They are frozen: playback plays exactly these notes and nothing is re-detected. Press it again to go back to live detection.
- **Clear** removes all notes and returns to live detection.

**Analyze (no need to play the whole song):** in RAVENYR, Analyze reads the track's whole audio straight from the DAW and tracks it in a few seconds. In other DAWs, plugins can't read the track, so Analyze analyses everything played so far (or use Import Audio). The result is finalized, and it stays adjustable: change **Sensitivity** or drag the threshold and the whole song is re-analysed (notes you edited by hand are kept).

**Import Audio… (non-live mode):** choose an audio file of the drum track (WAV, AIFF, FLAC, …, up to 10 minutes), or drop it onto the lane. Thruma analyses the whole file at once, shows its waveform and notes, and finalizes the result. Start the file at the song start so the notes line up with your DAW. The project remembers the file: when you reopen it, the audio and waveform come back with your notes unchanged (keep the file where it is). Press **Play** to hear the imported track with its replacement inside Thruma (no DAW playback needed), click the ruler to jump, and adjust Sensitivity, Velocity, Blend and Output while it plays. When you play the song from the DAW instead, the imported drum plays along from the song start and Blend mixes it with the samples the same way; the DAW decides the position then, so move it in the DAW (RAVENYR also follows Thruma's ruler).

**Import MIDI… (sample player):** load a `.mid` file (or drop it on the lane). Its notes play the kit in time with your DAW's tempo from bar 1, through the kit's MIDI note mapping (SAMPLES page). Nothing is analysed. **Clear** removes it.

### Track panel
| Control | What it does |
|---|---|
| **Instrument** | The drum on this track (Kick, Snare, Tom 1–4, Hi-Hat, Crash, Ride, China, Splash). Selects tuned detection settings. Shared with DEBLEED |
| **Sample** | Which kit sound plays for the hits. *Same as instrument* by default |

The panel also shows the note count, whether Thruma is listening, and how many samples are loaded. If the instrument has no samples yet, the original drum plays unchanged. Drop WAV files onto the page to add samples quickly.

### Controls
| Control | What it does |
|---|---|
| **Sensitivity** | The same as dragging the threshold. Higher catches quieter hits; lower ignores more bleed. At 0 % only hits close to your loudest ones count |
| **Velocity** | 0 % = every hit at full level, 100 % = follow the played dynamics |
| **Amount** (knob) | How easily a possible hit is noticed at all. Raise it when quiet or fast hits are missed (more rings appear); lower it if notes appear where nothing was played |
| **Match** (knob) | How much a hit must sound like this drum rather than bleed from the others. Lower it when real hits show as *sounded like bleed*; raise it when bleed turns into notes. 0 % switches the check off |
| **Advanced…** | Ghost Notes (quiet ghost notes), Retrigger (minimum time between hits, 0 = the drum's default; lower it for hits marked *too soon*), Dynamic Range (level range mapped onto velocity), Double hits (flams, drags, fast doubles), Frequency focus (bleed protection) |
| **Blend** | 0 % = original drum, 100 % = replacement sample |
| **Output** | Output level |
| **Analyze** | Tracks the whole song at once (see above) |
| **Import Audio…** | Analyses an audio file instead of listening live |
| **Import MIDI…** | Plays a MIDI file through the kit, in time with the song |
| **Play** | Plays imported audio inside Thruma; in RAVENYR it starts / stops the DAW |
| **Finalize** | Freezes the current notes (see above) |
| **Drag MIDI** | Drag this into your DAW to drop the notes as a MIDI clip. Notes are placed on your DAW's own beat grid (tempo changes included), measured from the song start, so drop the clip at bar 1 to line it up |
| **Save MIDI…** | Saves the notes as a `.mid` file, timed the same way |
| **Clear** | Removes all notes, the imported audio and MIDI file, and returns to live detection |

**Missing hits?** Hover the rings where the hits should be and follow the reason: *too quiet* → drag the threshold down or raise Sensitivity; *no clear attack* → raise Amount; *sounded like bleed* → lower Match; *too soon* → lower Retrigger in Advanced. Or just click the rings to add those notes. With imported or analysed audio, every change re-analyses the whole song and keeps the notes you added by hand.

Thruma also learns in the background while the song plays: the level of your real hits and the drum's sound. Detection gets more consistent after the first few seconds.

**Too many false triggers?**
1. Drag the threshold above the bleed.
2. Turn on DEBLEED: it cleans the drum before detection.
3. Delete any remaining false notes in the MIDI lane.

**Missing quiet hits?** Drag the threshold down.

**Fast playing** (rolls, blast beats, fast double bass): detection follows hits down to about 40 ms apart (16ths at 180+ BPM). Very fast snare rolls under ~40 ms may lose some notes; check the MIDI lane and add any missing ones.

**Track level and plugins before Thruma:** detection follows the track's own level, so quiet tracks, gain plugins, EQ and compressors before Thruma are fine. Live tracking settles on a new level within the first second or two of hits (and re-learns from what it hears every few seconds); **Analyze** reads the whole track and gets every hit from the start. Very heavy compression or clipping before Thruma flattens the hits and lifts the bleed, so put those *after* Thruma when you can. Stereo wideners or Haas-style delays before Thruma blur the transients (Thruma listens to left + right), so keep the track mono going into it.

**Latency:** replacement uses a short look-ahead so each sample starts exactly on the hit: 8 ms for kick and toms, 6 ms for snare, 4.5–5.5 ms for hats and cymbals (set the *Lookahead* parameter in your DAW's plugin parameter list to *Lowest* for 3 ms, slightly less accurate on kicks). Your DAW compensates it. Turning on DEBLEED adds about 21 ms.

---

## DEBLEED

Debleed runs **before** detection and replacement, so it also reduces what triggers the replacement. It is **not a gate**:
- It keeps the drum's attack, its natural ring-out and its ghost notes.
- It reduces bleed per frequency band, smoothly, without pumping or chopping cymbal tails.
- It learns automatically while the track plays.

| Control | What it does |
|---|---|
| **Debleed** | On/off. Adds about 21 ms of latency, compensated by your DAW |
| **Instrument** | Shared with REPLACE |
| **Reference** | Optional: another mic that carries the bleed. Choose the **sidechain input** (L, R or L+R), or any other track that has Thruma on it. Those are listed by their DAW track names, so there is nothing to set up |
| **Amount** | How strongly bleed is reduced |
| **Sensitivity** | How readily quiet hits (ghost notes) of the drum are recognised and kept |
| **Attack Preservation** | Keeps the start of every hit untouched |
| **Release** | How smoothly the reduction lets go after each hit |

The display shows the current reduction per frequency band.

**Reference tracks:** linking to another Thruma track works when both plugins run in the same DAW process, which is the default in most DAWs, and while the song plays. Otherwise, route the reference mic to Thruma's sidechain input.

---

## PLAYER

The kit as a picture, like a drum sampler.

- **Click a drum** to play it: harder towards the middle, softer at the edge (**Shift** = soft).
- **Right-click a drum** to load samples, add samples, load a whole folder, change its **MIDI note** (General MIDI list, or any note; a drum already on that note swaps with it), open its layers on the SAMPLES page, track it on REPLACE, **Load TDE… / Save as TDE…**, or clear it. The submenu lists its samples; click one to hear it.
- **Drop audio files** (or a folder, or a `.tde` file) on a drum to load them into it. Drop a folder anywhere else to import it as a whole kit.
- **Loading:** samples load in the background with a progress bar; the window stays responsive.
- **Drums light up** (white) when they are hit: by tracking, MIDI clips, an imported MIDI file or your clicks.
- **Two crashes:** the left one is **Crash** (MIDI 49), the right one **Crash 2** (MIDI 57). Kit folders with files named "Crash 2" / "Crash_B" / "Right Crash" load onto Crash 2.
- **Pads:** kit sounds that are not one of the drums (cowbell, tambourine, stacks, FX...) go on the pads under the picture, up to 32; more rows appear as you add them. **Import Kit Folder…** puts them there automatically, named after their folder or file, each with its own MIDI note. Click **+ Pad** to load a sound onto a new pad; right-click a pad to rename or remove it (the pads after it move up).

## MIXER

A channel strip for every drum and pad: **volume** fader (−inf to +12 dB), **pan** knob, **M**ute, **S**olo and a level meter. Double-click a fader or knob to reset it; **Reset Mixer** resets everything. Changes apply instantly, also to cymbals that are still ringing, and are saved with the kit (and the project).

- **Type a value:** click the pan number under the knob and type `L30`, `R15`, `C`, or a number from −100 (left) to 100 (right). Click the dB number under the fader and type a level such as `-6` (or `-inf`).
- **Strip order** follows the kit: Crash 2 sits right after Crash.

### Outputs (multi-out)

Thruma is a multi-output instrument: besides **Main** it has 16 extra stereo outputs, **Out 2 … Out 17**. Pick an output in the box under each strip's Mute/Solo to send that drum or pad there, so you can put kick, snare, toms, overheads etc. on separate DAW mixer channels with their own plugins.

- Turn the outputs on in your DAW first (in Studio One: the instrument's output list in the Instrument panel / Mix view; in Reaper: the plugin's I/O pins; in Cubase: Activate Outputs). Until an output is on, the box shows "(off in DAW)" and that drum keeps playing on **Main**, so you never lose a sound.
- Volume, pan, mute and solo still apply to drums on extra outputs. Blend and the original audio stay on Main.
- The output choice is saved with the kit and the project.

## SAMPLES

- **Which sample played:** when the selected drum is hit, the sample file that played lights up (with its velocity), and the drum's row flashes in the kit list.
- **WAV Browser:** click a file to preview it. Select files and press **Add to instrument** (or double-click) to add them to the selected instrument. Files are sorted into velocity layers by loudness; samples of similar loudness alternate as round-robin.
- **Import Folder…:** assigns every WAV in a folder to instruments by name (kick, snare, tom/floor, hat, crash, ride, china, splash); other sounds become pads.
- **Kit list:** select an instrument or pad to edit it. Double-click a pad's name above its layers to rename it.
- **Layers:** each row is a velocity layer (velocity range 1–127) with its round-robin samples. Drop files on a row to add them to that layer. Shift-click a sample to remove it. **Round robin / Random** sets how the samples alternate. **MIDI note** is the note used for MIDI.
- **Save TDE… / Load TDE…** (above the layers): a `.tde` file holds **one drum** – all its samples, velocity layers and round robin – in a single file, like a Drumagog GOG file. Save your snare as `My Snare.tde`, then load it onto any drum in any project or share it; the file carries the WAVs themselves, so it works on another computer. Loading replaces that drum's layers but keeps its MIDI note and mixer settings. You can also drop a `.tde` onto the layers. Loaded TDE samples are unpacked once to `Documents/Thruma/TDE`. Samples are stored as lossless FLAC inside the file (bit-identical sound, about half the size of WAV); 32-bit float WAVs are kept as they are.
- **Save All TDE… / Load TDEs…** (under the kit list): Save All writes every drum and pad as its own `.tde` into a folder you pick (`<Kit> - Snare.tde`, `<Kit> - Kick.tde`, …). Load TDEs lets you pick many `.tde` files at once; each goes back onto the drum it was saved from (a snare TDE onto Snare), pad TDEs go onto new pads, and a second file for the same drum goes onto a pad so nothing is lost. Dropping several `.tde` files on the SAMPLES or PLAYER page does the same.
- **Load Kit… / Save Kit… / New Kit:** kits are `.thrumakit` files that point to your original WAV files. Samples are never copied or changed.

---

## MIDI
- On a MIDI / instrument track, the track's MIDI clips play the kit (Thruma is a drum sample player there).
- Whenever MIDI plays the kit (MIDI notes coming in, or an imported MIDI file playing), Thruma is only a sample player: audio tracking and replacing pause, any audio input passes through untouched and MIDI-played samples play at full level (Blend does not apply). Tracking resumes two seconds after the MIDI stops.
- Thruma's MIDI output sends every hit as a note on channel 10. Route it to a drum instrument in your DAW.
- MIDI notes sent *into* Thruma play the kit sound with that MIDI note.

## Saving
Your project stores everything: settings, the kit (as file paths), the notes, what Thruma learned, and the reference choice.
