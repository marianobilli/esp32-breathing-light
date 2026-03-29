Audio files for Moni breathing app
====================================

Place WAV files here, then upload to the board's SPIFFS partition:

    pio run --target uploadfs

Required format
---------------
  Format:      WAV (RIFF PCM, uncompressed)
  Bit depth:   16-bit signed
  Channels:    Mono
  Sample rate: 16000 Hz

Files
-----
  breath_in.wav   -- plays when inhale phase starts  (up to ~4 s)
  breath_out.wav  -- plays when exhale phase starts  (up to ~6 s)

If a file is missing the app plays silence for that phase.
The SD card root is checked first; SPIFFS is the fallback.

Tips (Audacity)
---------------
  Tracks > Mix > Mix Stereo Down to Mono
  Tracks > Resample... > 16000 Hz
  File > Export > Export as WAV > Signed 16-bit PCM
