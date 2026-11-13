# Thruma User Guide

Thruma has three pages: **DEBLEED**, **REPLACE** and **SAMPLES**. Everything works in real time on the track Thruma is inserted on: press play in your DAW and it listens. There is nothing to record or analyse first.

The header shows the plugin's latency (your DAW compensates it automatically), a hit light, and input/output meters. Hover any control for a short explanation; double-click a slider to reset it.

---

## Where to put Thruma

Thruma is one plugin (a VST3 *instrument* with an audio input) that you can use two ways:
- **On a drum audio track** (as an insert): it tracks the drum, replaces or layers it, and makes MIDI.
- **On a MIDI / instrument track**: it is a drum sample player. MIDI clips on the track play the kit (SAMPLES page). You can also import a MIDI file into Thruma itself (see below).

Some DAWs only list instruments on MIDI tracks; in those, route the drum track into Thruma's input or sidechain. RAVENYR lists Thruma as an insert on audio tracks too.

## REPLACE

The Replace page works like a tracker. It has a track lane and a few controls.

### The track lane
- **AUDIO lane:** the drum's level as the song plays (teal).
  - The dashed amber line is the **threshold**. **Drag it up or down.** Hits that rise above it become notes; bleed that stays under it is ignored.
  - Grey **×** marks show moments that crossed the threshold but were ignored because they didn't sound like this drum (bleed from the kick, hats, cymbals and so on).
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

**Import Audio… (non-live mode):** choose an audio file of the drum track (WAV, AIFF, FLAC, …, up to 10 minutes), or drop it onto the lane. Thruma analyses the whole file at once, shows its waveform and notes, and finalizes the result. Start the file at the song start so the notes line up with your DAW. Press **Play** to hear the imported track with its replacement inside Thruma (no DAW playback needed), click the ruler to jump, and adjust Sensitivity, Velocity, Blend and Output while it plays.

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

Thruma also learns in the background while the song plays: the level of your real hits and the drum's sound. Detection gets more consistent after the first few seconds.

**Too many false triggers?**
1. Drag the threshold above the bleed.
2. Turn on DEBLEED: it cleans the drum before detection.
3. Delete any remaining false notes in the MIDI lane.

**Missing quiet hits?** Drag the threshold down.

**Fast playing** (rolls, blast beats, fast double bass): detection follows hits down to about 40 ms apart (16ths at 180+ BPM). Very fast snare rolls under ~40 ms may lose some notes; check the MIDI lane and add any missing ones.

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

## SAMPLES

- **WAV Browser:** click a file to preview it. Select files and press **Add to instrument** (or double-click) to add them to the selected instrument. Files are sorted into velocity layers by loudness; samples of similar loudness alternate as round-robin.
- **Import Folder…:** assigns every WAV in a folder to instruments by name (kick, snare, tom/floor, hat, crash, ride, china, splash).
- **Kit list:** select an instrument to edit it.
- **Layers:** each row is a velocity layer (velocity range 1–127) with its round-robin samples. Drop files on a row to add them to that layer. Shift-click a sample to remove it. **Round robin / Random** sets how the samples alternate. **MIDI note** is the note used for MIDI.
- **Load Kit… / Save Kit… / New Kit:** kits are `.thrumakit` files that point to your original WAV files. Samples are never copied or changed.

---

## MIDI
- On a MIDI / instrument track, the track's MIDI clips play the kit (Thruma is a drum sample player there).
- Thruma's MIDI output sends every hit as a note on channel 10. Route it to a drum instrument in your DAW.
- MIDI notes sent *into* Thruma play the kit sound with that MIDI note.

## Saving
Your project stores everything: settings, the kit (as file paths), the notes, what Thruma learned, and the reference choice.
