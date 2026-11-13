# Thruma User Guide

Thruma has three pages: **DEBLEED**, **REPLACE** and **SAMPLES**. Everything works in real time on the track Thruma is inserted on: press play in your DAW and it listens. There is nothing to record or analyse first.

The header shows the plugin's latency (your DAW compensates it automatically), a hit light, and input/output meters. Hover any control for a short explanation; double-click a slider to reset it.

---

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
| Zoom / scroll | Mouse wheel zooms; Shift+wheel or the scrollbar scrolls; **F** shows the whole song |

The lane follows the playhead while the song plays.

**Live vs edited:** while you haven't edited anything, hits are detected live every time the song plays, and replaying a section re-detects it without duplicates. As soon as you edit a note, your edited notes are what plays back. **Clear** removes all notes and returns to live detection.

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
| **Drag MIDI** | Drag this into your DAW to drop the notes as a MIDI clip. Note positions are measured from the start of the song, so drop the clip at bar 1 to line it up |
| **Save MIDI…** | Saves the notes as a `.mid` file |
| **Clear** | Removes all notes and returns to live detection |

Thruma also learns in the background while the song plays: the level of your real hits and the drum's sound. Detection gets more consistent after the first few seconds.

**Too many false triggers?**
1. Drag the threshold above the bleed.
2. Turn on DEBLEED: it cleans the drum before detection.
3. Delete any remaining false notes in the MIDI lane.

**Missing quiet hits?** Drag the threshold down.

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
- Thruma's MIDI output sends every hit as a note on channel 10. Route it to a drum instrument in your DAW.
- MIDI notes sent *into* Thruma play the kit sound with that MIDI note.

## Saving
Your project stores everything: settings, the kit (as file paths), the notes, what Thruma learned, and the reference choice.
