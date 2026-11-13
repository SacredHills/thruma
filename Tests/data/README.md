# Real-recording test data

Put annotated drum takes here to measure detection accuracy on real material:

```
mysong_snaretop.snare.wav     # audio (16/24/32-bit PCM or 32-bit float, any channel count)
mysong_snaretop.snare.txt     # one onset time in seconds per line (Audacity label export works)
```

Instrument tags: `kick snare tom1 tom2 tom3 tom4 hihat crash ride china splash`.

`thruma_tests "Real recordings"` runs Analyze on every annotated file and reports precision, recall, F1 and timing. WAV files in this folder are git-ignored by default; commit them deliberately if their license allows it.
