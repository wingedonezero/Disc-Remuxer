# Test data

Made with this repository's FFmpeg (`dist/debug/ffmpeg`), from a 440 Hz sine,
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
- `sine_11520.mp2` (`-c:a mp2 -b:a 192k -f mp2`), `sine_11520.aac` (`-c:a aac -b:a 128k -f adts`),
  `sine_11520.latm` (`-c:a aac -b:a 128k -f latm`): the same sine, 11520 samples
- `testsrc_1200.m2v`: `-f lavfi -i testsrc=size=64x48:rate=30000/1001 -frames:v 1200 -c:v mpeg2video -g 12 -bf 2
  -q:v 20 -f mpeg2video` (MPEG-2, 29.97 fps, GOP of 12 with two B pictures between references)
- `sine_7680.ac3`: the same sine (float planar), 7680 samples, `-c:a ac3 -b:a 192k -f ac3` (5 AC-3 frames of 768
  bytes, 48 kHz)
