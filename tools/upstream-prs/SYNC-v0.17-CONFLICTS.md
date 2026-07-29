# ggml sync v0.10.2 → v0.17.0 — conflict inventory (2026-07-29)

Merge attempted: `CrispStrobe/ggml` `crispstrobe-ops` (bfe8ea22 + 2 SVE cherry-picks)
← `ggml-org/ggml` master (9be31331). **435 commits, 19 conflicted files, 53 hunks.**

I resolved 3 files and then STOPPED, because two of the remaining problems are
design decisions with silent-failure modes, not mechanical merges. Writing the
inventory down so the analysis is not lost and whoever finishes it starts from
evidence rather than from scratch.

## Resolved (3)

| file | resolution |
| --- | --- |
| `src/gguf.cpp` | took upstream — it implemented our empty-key rejection independently (`1dc4cb93` superseded, only the log wording differs) |
| `tests/test-quantize-fns.cpp` | took upstream (adds `Q2_0`; superset of ours) |
| `src/ggml-metal/ggml-metal-device.m` | kept BOTH — our 154-line MTLBinaryArchive pipeline cache and upstream's new 42-line `ggml_metal_device_id_parse` are adjacent additions, not an overlap |

## BLOCKER 1 — the `GGML_OP_*` enum must be re-derived by hand

`src/ggml.c` carries `static_assert(GGML_OP_COUNT == 99)` on our side and `== 101`
on upstream's, at two sites each. Both sides inserted ops into an ORDERED enum that
has parallel name and symbol tables:

    ours:     GGML_OP_NORM_AFFINE (500), GGML_OP_COL2IM_1D (530), GGML_OP_AA_SNAKE_BETA (585)
    upstream: its own additions, different positions

This cannot be auto-merged and it cannot be eyeballed. If the enum order and the
two tables in `ggml.c` disagree, ops dispatch to the WRONG KERNEL with no compile
error — the same silent-failure shape as everything else this week. Whoever
finishes must rebuild the enum + both tables entry-by-entry and assert the count.

## BLOCKER 2 — `col2im_1d` is two different ops sharing one name

Already documented in `UPSTREAM-SYNC.md`, repeated here because it drives four of
the conflicted files (`ggml-cuda/col2im-1d.cu`, `ggml-metal.metal`,
`ggml-vulkan/vulkan-shaders/col2im_1d.comp`, `ggml.c`):

    upstream   scatter-add, p0 = crop BOTH sides, T_out = (T_in-1)*s0 + K - 2*p0
    ours #160  gather,      p0 = LEFT offset,     T_out = (T_in-1)*s0 + K -   p0

`src/core/conv.h:166` depends on ours. Taking upstream's silently changes
ConvTranspose1d output length in every TTS decoder. Resolving it means either
renaming ours, or rewriting `convt1d_decomp` to pass p0=0 and crop with views —
and then validating against real TTS audio (f5-tts, cosyvoice3, TADA, vocoders).

## Remaining 16 files — per-file guidance

Classification from the hunk analysis: `OURS-only` = pure addition of ours, keep;
`overlap` = both sides edited the same lines, needs judgement.

| file | hunks | notes |
| --- | --- | --- |
| `ggml-cuda/ggml-cuda.cu` | 2 | hunk 2 is **588 lines OURS-only** (peer-access / VMM pool) — keep. Hunk 1 overlaps our runtime-vs-compile CUDA version check (#152) |
| `ggml-metal/ggml-metal.metal` | 4 | hunk 4 is **432 lines ours vs 40 theirs** — our col2im/snake kernels vs upstream's; blocked on BLOCKER 2 |
| `ggml-metal/ggml-metal-ops.cpp` | 8 | our im2col batch-1 occupancy (`23695065`) vs upstream's dispatch rework; also col2im + snake dispatch |
| `ggml-vulkan/ggml-vulkan.cpp` | 8 | our conv_transpose_1d_f16 + col2im pipelines vs upstream's col2im + snake pipelines |
| `ggml-cuda/fattn.cu` | 1 | 41 ours vs 3 theirs — our RDNA4 WMMA gate vs upstream's MFMA batch-size note |
| `ggml-cuda/cpy.cu` | 2 | our GH #65 grid_y clamp vs upstream's `cpy.cu` rework (`00504b04`) — reconcile, both are real fixes |
| `ggml-cuda/im2col.cu` | 1 | **OURS-only** (grid.y clamp, upstream-prs 02) — keep |
| `ggml-cuda/col2im-1d.cu` | 5 | blocked on BLOCKER 2; upstream also added fastdiv |
| `ggml-cuda/conv-transpose-1d.cu` | 1 | ours (F16 template, upstream-prs 14) vs upstream's loop rework |
| `ggml-metal/ggml-metal-device.cpp` | 5 | our #83 F32-precision gate + col2im + snake registrations vs upstream |
| `ggml.c` | 6 | 2 hunks are the OP_COUNT asserts (BLOCKER 1); 3 are our issue-#38 im2col_type selection vs upstream |
| `ggml-webgpu/*` (2) | 6 | our NORM/LayerNorm patch vs upstream's f16 rework of `row_norm.wgsl` |
| `ggml-vulkan/*` (3) | 3 | col2im shader + gen + CMakeLists — BLOCKER 2 |

## Recommendation

This is a multi-session project, not a task tail. Suggested order:

1. Settle BLOCKER 2 first (it decides 4 files). Preferred: rename ours to
   `ggml_col2im_1d_gather` so upstream's canonical name stays canonical and nothing
   changes silently; update `src/core/conv.h` accordingly.
2. Rebuild the enum + `ggml.c` tables (BLOCKER 1) and assert the count.
3. Work the remaining files with `test-backend-ops` per op as the gate.
4. Only then bump CrispASR's pin, and run the TTS regression before merging.

Do NOT bump the CrispASR submodule pin from a partially-resolved merge. The
current pin (`52165e4c`) is good: it has both SVE fixes and is otherwise the
known-good tree.
