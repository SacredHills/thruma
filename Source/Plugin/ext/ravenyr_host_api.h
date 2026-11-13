/*  RAVENYR host extension API (plain C, version 1).

    Plugins running inside RAVENYR can find this table at run time and use it for things VST3 has no standard
    way to ask: which track they are on, moving the play position, and reading the track's audio for the whole
    song (like ARA, much simpler). Outside RAVENYR the symbol does not exist, so plugins simply do without.

    Finding it from a plugin:
      Windows : GetProcAddress (GetModuleHandleW (nullptr), "ravenyr_host_api_v1")
      macOS / Linux : dlsym (RTLD_DEFAULT, "ravenyr_host_api_v1")
    then call it: const RavenyrHostApiV1* api = fn();

    This header is copied as-is into plugins that use it: keep it self-contained and only ever append members
    (bump `version` when you do). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RavenyrHostApiV1
{
    int32_t version;     /* 1 */
    int32_t size;        /* sizeof (RavenyrHostApiV1) */

    /* Audio thread, from inside the plugin's processBlock: the track being processed (stable across the session
       while the track exists), 0 if not on a track (master bus, offline export of another kind, ...). */
    int64_t (*currentTrack) (void);

    /* Any thread. Transport. */
    void    (*locate)       (int64_t samples);   /* engine samples from the song start */
    void    (*play)         (void);
    void    (*stop)         (void);
    int32_t (*isPlaying)    (void);
    double  (*sampleRate)   (void);              /* engine rate (0 = no device) */

    /* Message thread only. The track's own audio (its clips, before its effects and fader) in engine samples
       from the song start. trackLength: end of the last clip, 0 if none / unknown track.
       readTrack: adds nothing, overwrites left/right (right may be null: mono sum); returns samples written. */
    int64_t (*trackLength)  (int64_t trackKey);
    int64_t (*readTrack)    (int64_t trackKey, int64_t start, int64_t numSamples, float* left, float* right);

    /* Any thread. Keep processing the track's plugins for the next few seconds even without input (a plugin
       playing back audio of its own). Call it at least once a second while needed. */
    void    (*wakeTrack)    (int64_t trackKey);
} RavenyrHostApiV1;

typedef const RavenyrHostApiV1* (*RavenyrHostApiV1Fn) (void);

#ifdef __cplusplus
}
#endif
