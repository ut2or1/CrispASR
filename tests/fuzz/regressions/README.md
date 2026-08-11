# Fuzz regression seeds

Inputs that once crashed a parser, kept as permanent corpus entries. The
smoke-fuzz job copies this directory into its corpus, so every run replays them
— a fuzzing campaign rediscovers a crash only by luck, and a 45 s seeded run is
stochastic (the stb_vorbis crash below failed CI on a *documentation-only*
commit and passed on the four before it).

Each file is the minimal input that reproduces, crafted rather than harvested
where that was practical, so it is deterministic and small enough to read.

| File | Bug | Fixed in |
|---|---|---|
| `ogg-huge-comment-count.ogg` | 102 bytes. Ogg/Vorbis comment header declaring `comment_list_length = 0x3FFFFFFF`. The allocation of `sizeof(char*) * length` fails, and stb_vorbis returned from the error path with the length still set and `comment_list` NULL — `vorbis_deinit` then indexed the null array. ASAN: `SEGV in vorbis_deinit`, reached from `crispasr_audio_load`. | `examples/stb_vorbis.c`, guard in `vorbis_deinit` + reset the length on the error path |

## Adding one

Craft or minimise the input, drop it here, and add a row. Keep them small: they
run on every CI push. To check a seed still reproduces against an unpatched
build, revert the fix and run the single input directly — `libcrispasr` is a
shared library, so rebuild it, not just the harness, or you will test the new
code with an old-looking binary:

```
./build-fuzz/bin/crispasr-fuzz-audio tests/fuzz/regressions/<file>
```
