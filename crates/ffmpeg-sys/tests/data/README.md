# Test data

Made with this repository's FFmpeg (`target/debug/ffmpeg`), from a 440 Hz sine,
48 kHz, stereo, 16-bit, 4813 samples (120 access units of 40 samples and a
last one of 13):

- `sine_4813.thd`: `-c:a truehd -strict -2 -f truehd` (121 access units, 8 major
  syncs; the last one ends the stream, shortened by 27 samples)
- `sine_4813.mlp`: `-c:a mlp -strict -2 -f mlp` (121 access units; FFmpeg's MLP
  encoder writes no end-of-stream marker)

Full command (TrueHD):

    ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000,aformat=channel_layouts=stereo:sample_fmts=s16,atrim=end_sample=4813" -c:a truehd -strict -2 -f truehd sine_4813.thd

- `sine_5120.dts`: `-c:a dca -strict -2 -f dts` from the same sine, 5120 samples
  (10 DTS core frames of 512 samples, 1884 bytes each, 48 kHz)
