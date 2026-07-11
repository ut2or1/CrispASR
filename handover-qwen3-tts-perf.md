# Qwen3-TTS Code Predictor Perf Fix (#245) — Handover

## Problem

CrispASR's qwen3-tts code predictor runs **15 separate ggml graph dispatches** per audio frame (one per codebook 1-15). Each dispatch goes through full scheduler alloc → compute → read. This makes it ~40x slower per frame than competitors.

**Profiling output (`QWEN3_TTS_BENCH=1 QWEN3_TTS_O15=1`):**
```
qwen3_tts: code_pred_kv bench (15 calls): build=0.13ms reset=0.31ms alloc=9.34ms compute=12731.0ms read=0.24ms
```

The build/reset/alloc are fast (O15 persistent graph works), but 15 × ~680ms compute = ~10s/frame for the code predictor alone.

**Competitive landscape:**

| Engine | CodePred ms/frame | Talker ms/frame | Total RTF (CPU) |
|--------|-------------------|-----------------|-----------------|
| CrispASR | ~10,000 (15×680) | ~13,000 | 0.006x |
| predict-woo/qwen3-tts.cpp | ~225 | ~84 | ~0.5x |
| ServeurpersoCom/qwentts.cpp | ~111 (claimed) | no data | no data |

## Root Cause

In `src/qwen3_tts.cpp`, `code_pred_generate_15()` (around line 5800-5900) calls `run_code_pred_kv()` in a loop 15 times:

```cpp
for (int cb = 1; cb < n_q; cb++) {  // cb = 1..15
    // Each iteration: build graph, alloc, set inputs, compute, read logits, sample
    float* logits = run_code_pred_kv(ctx, embed, 1, n_past, &out_n, &out_vocab);
    // ... sample from logits, update embed for next cb ...
}
```

Each `run_code_pred_kv()` dispatches a **complete** 5-layer transformer graph (Q/K/V projections, attention, FFN for all 5 layers + KV cache write + lm_head matmul).

The competitors handle this differently:
- **predict-woo**: runs all 15 codebooks with shared KV state in fewer dispatches
- **qwentts.cpp**: uses "per-frame cache reset" where the code predictor's 5-layer KV cache is reset per frame but all 15 steps share a single pre-allocated graph

## Proposed Fix

### Option A: Fused multi-step graph (recommended)

Build a single graph that does 15 sequential AR steps within one `ggml_backend_sched_graph_compute` call. The code predictor has only 5 layers, head_dim=64, so the KV cache is tiny. The graph would:

1. Accept input embedding for step 0
2. For each of the 15 steps within the graph:
   - Write K/V to cache at position `step`
   - Run 5-layer attention + FFN
   - Project to logits via `lm_head[cb]`
   - Argmax (or feed back via a feedback tensor)
3. Output all 15 logits in one read

**Challenge:** ggml doesn't natively support loops within a graph. But since n_steps=15 is fixed, we can **unroll** — build a graph with 15 × 5 = 75 layer blocks. The KV cache grows from position 0 to 14. This is large but bounded.

**Simpler variant:** keep the loop but eliminate the per-iteration scheduler overhead by pre-allocating the graph once and only updating the input tensor + KV write position per iteration (the O15 path already does this for a single step — extend it to reuse across the 15 iterations without `ggml_backend_sched_reset`).

### Option B: Batch decode (parallel codebooks)

If the code predictor's 15 steps are architecturally independent (they're not — each cb conditions on the previous), this won't work. But checking the architecture: **codebook k depends on codebooks 0..k-1** (autoregressive), so the 15 steps must be sequential.

### Option C: Skip scheduler reset between steps

The simplest fix: the O15 path already has `skip_plan` logic that avoids rebuilding the graph. Extend this to also skip `ggml_backend_sched_reset()` between the 15 code_pred steps within the same frame. The key insight is that between steps, only the input embedding changes (1 token × 1024 dim) and the KV cache grows by 1 position — the graph topology is identical.

**This is likely what qwentts.cpp means by "~90x speedup with per-frame cache reset."**

## Key Code Locations

| File | Line | What |
|------|------|------|
| `src/qwen3_tts.cpp:~5800` | `code_pred_generate_15()` | The loop calling run_code_pred_kv 15× |
| `src/qwen3_tts.cpp:~1100` | `build_graph_code_pred_kv()` | Graph builder (O15 path) |
| `src/qwen3_tts.cpp:~1240` | `run_code_pred_kv()` | Single-step dispatcher |
| `src/qwen3_tts.cpp:~756` | O15 persistent graph fields | `cp_t1_sched`, `cp_t1_graph` |
| `src/qwen3_tts.cpp:~776` | `skip_plan` logic | Graph-reuse condition |

## Quick Start

```bash
# Build
CCACHE_DIR=/path/to/.ccache cmake -G Ninja -B build \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

# Download models (if not cached)
./build/bin/crispasr --backend qwen3-tts -m auto --auto-download --dry-run-resolve

# Benchmark baseline (BEFORE fix)
QWEN3_TTS_BENCH=1 QWEN3_TTS_O15=1 ./build/bin/crispasr --backend qwen3-tts \
  -m qwen3-tts-12hz-0.6b-base-q8_0.gguf \
  --codec-model qwen3-tts-tokenizer-12hz.gguf \
  --voice qwen3-tts-voice-default.gguf \
  --tts "Hello world, this is a test." \
  --tts-output baseline.wav -v 2>&1 | grep -E "perf|bench|rtf"

# After fix, same command — compare ms/frame numbers
```

## What "Good" Looks Like

- Code predictor: **<500ms/frame** on M1 Metal (currently ~680ms × 15 = 10s)
- Total synthesis RTF: **>1x realtime** on M1 Metal for short utterances
- No regression in output quality (bit-identical codebook selections)

## Env Vars for Testing

| Var | Default | What |
|-----|---------|------|
| `QWEN3_TTS_O15` | `0` | Persistent code_pred graph (reuse topology) |
| `QWEN3_TTS_LK_BUCKET` | `0` | Bucketed Lk for talker (fixed-size mask) |
| `QWEN3_TTS_BENCH` | `0` | Print per-stage timing breakdown |
| `QWEN3_TTS_PROF` | `0` | More detailed profiling |
| `QWEN3_TTS_CP_BACKEND` | auto | Force code_pred backend (cpu/metal) |
| `QWEN3_TTS_CODEC_GPU` | auto | Force codec on GPU |

## Implementation Steps

1. **Measure baseline on Metal**: `QWEN3_TTS_BENCH=1 QWEN3_TTS_O15=1` — get ms/frame numbers
2. **Read `code_pred_generate_15()`** at ~line 5800 — understand the loop structure
3. **Implement Option C first** (simplest): skip scheduler reset between the 15 steps
   - In `run_code_pred_kv()`, when called from the 15-step loop with same graph topology:
     - Don't call `ggml_backend_sched_reset()`
     - Only update the input tensor data + KV write position
     - Reuse the same alloc plan
4. **Measure again** — expect 5-10x improvement from removing 14 alloc cycles
5. **If still slow**: implement Option A (unrolled 15-step graph)
6. **Enable O15 + LK_BUCKET by default** once validated stable
7. **Format**: `./tools/format.sh --fix src/qwen3_tts.cpp`
8. **Commit**: reference #245

## Architecture Notes

The code predictor is a **5-layer Qwen2 transformer** with:
- dim = 1024 (same as talker)
- n_heads = 16, n_kv_heads = 8 (GQA)
- head_dim = 64
- FFN dim = 3072 (SwiGLU)
- 15 separate `lm_head` projections (one per codebook)
- 15 separate `codec_embd` embeddings (one per codebook)
- KV cache: 15 positions max per frame (reset each frame)

Each step k (for codebook k):
1. Input: embedding of the token sampled at step k-1 (or talker hidden for k=0)
2. Run 5 layers with KV cache at positions [0..k-1]
3. Project hidden state through `lm_head[k]` → logits over 2048 entries
4. Sample (temperature + top-k + rep penalty)
5. Look up sampled token in `codec_embd[k]` → next input

The key property: **topology is identical across all 15 steps** — only the input data and KV write position change. This is exactly what graph reuse is designed for.

## Related Competitors (for A/B comparison)

```bash
# predict-woo (clone + build for local comparison)
git clone https://github.com/predict-woo/qwen3-tts.cpp /tmp/qwen3-tts-cpp
cd /tmp/qwen3-tts-cpp && mkdir build && cd build
cmake .. -DGGML_METAL=ON && make -j$(sysctl -n hw.ncpu)
# Their CLI: ./qwen3-tts --model <gguf> --text "Hello" --output out.wav

# qwentts.cpp
git clone https://github.com/ServeurpersoCom/qwentts.cpp /tmp/qwentts
cd /tmp/qwentts && ./buildmac.sh
# Their CLI: ./qwen-tts --talker <gguf> --tokenizer <gguf> --text "Hello"
```

Note: predict-woo uses a different GGUF format (their own converter). qwentts.cpp uses their own format too. Direct model-file comparison isn't possible — only RTF comparison on the same text with equivalent models.
