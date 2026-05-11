# Step C-2 handover — `metal: kernel_aa_snake_beta` for IndexTTS BigVGAN v2

This is the picked-up-from-context doc for the next session that lands
the fused Metal AA-SnakeBeta kernel. The PR draft itself is in
`07-metal-aa-snake-beta.md`; this file holds the implementation plan,
file-by-file touch list, kernel skeleton, validation pipeline, and the
exact bench commands that gate "ship it" vs "iterate."

If you're a freshly-spawned Claude reading this — start with §0 then go
to §1; the bench oracle in §3 protects you from shipping a wrong-output
kernel.

---

## §0 — What's already on `main`

The IndexTTS BigVGAN AA path now has three working implementations on `main`:

| Path                              | Default? | Backend       | Output vs CPU oracle |
| --------------------------------- | -------- | ------------- | -------------------- |
| `aa_snake_beta`     (custom op)   | ✅       | CPU           | reference            |
| `aa_snake_beta_native` (Step B-v2)| Opt-in   | CPU (default) | bit-equiv on CPU     |
| `aa_snake_beta_native` (Step B-v2)| Opt-in   | Metal         | noise-floor delta    |

Step C-2 adds a fourth: `GGML_OP_AA_SNAKE_BETA` — a real ggml op with a
fused Metal kernel that beats the native-ggml-ops path's concat/reshape
graph overhead.

Last-pushed M1 numbers (q8_0 GPT, JFK voice prompt, ≈ 6.7 s of audio):

| Path                          | voc-only |
| ----------------------------- | -------- |
| Step A custom op  (CPU)       | 7.87 s   |
| Step B-v2 native  (CPU)       | 7.57 s   |
| Step B-v2 native  (Metal)     | 8.01 s   |
| **Step C-2 target (Metal)**   | **1.5–2 s**   |

The 1.5–2 s target is extrapolated from upstream IndexTTS's CUDA speedup
on A100 — fold every per-AA-site round-trip into one launch, hold
intermediates in registers / threadgroup memory.

## §1 — File-by-file touch list

Touch the ggml subtree carefully. Per `CLAUDE.md` ("Notes that bit hard")
every CrispASR-fork hunk in `ggml/` MUST carry the marker comment so the
next ggml bump re-applies them all. Pattern:

```c
// CrispASR patch (issue/PR #07-metal-aa-snake-beta): <one-line why>
// MUST RE-APPLY after every ggml bump.
```

The grep that finds them: `git grep -n "CrispASR patch\|CrispASR fork"`.
Add `07-metal-aa-snake-beta` to the audit table in `MASTER-AUDIT.md`
when shipping.

### 1.1  ggml/include/ggml.h

```c
// In `enum ggml_op` after GGML_OP_RWKV_WKV7 (or anywhere — keep alphabetical-ish):
GGML_OP_AA_SNAKE_BETA,

// In the public API section near ggml_conv_transpose_1d:
// BigVGAN v2 anti-aliased SnakeBeta — fused upsample 2× + sin²(α·x)/β + downsample 2×.
// All inputs F32; output same shape as `x`.
//   x        : [T, C]      — time-fastest, channel-major
//   log_alpha: [C]         — per-channel α frequency, log-scale
//   log_beta : [C]         — per-channel β amplitude, log-scale
//   us_filter: [K, 1, 1]   — Kaiser-windowed sinc, sum=1
//   ds_filter: [K, 1, 1]   — Kaiser-windowed sinc, sum=1
// K must be 12 in the current kernel — see ggml-cpu/ops.cpp for the assertion.
GGML_API struct ggml_tensor * ggml_aa_snake_beta(
        struct ggml_context * ctx,
        struct ggml_tensor  * x,
        struct ggml_tensor  * log_alpha,
        struct ggml_tensor  * log_beta,
        struct ggml_tensor  * us_filter,
        struct ggml_tensor  * ds_filter);
```

### 1.2  ggml/src/ggml.c

- Op-name string: add `"aa_snake_beta(x)"` (or `"aa_snake_beta"`) to the
  string table near `"conv_transpose_1d(x)"` (search for that literal —
  the array indexes by `enum ggml_op` value).
- Builder:

```c
// ggml_aa_snake_beta
GGML_API struct ggml_tensor * ggml_aa_snake_beta(
        struct ggml_context * ctx,
        struct ggml_tensor  * x,
        struct ggml_tensor  * log_alpha,
        struct ggml_tensor  * log_beta,
        struct ggml_tensor  * us_filter,
        struct ggml_tensor  * ds_filter) {
    GGML_ASSERT(ggml_is_matrix(x));                  // [T, C]
    GGML_ASSERT(log_alpha->ne[0] == x->ne[1]);       // C matches
    GGML_ASSERT(log_beta->ne[0]  == x->ne[1]);
    GGML_ASSERT(us_filter->ne[0] == 12);             // K fixed at 12 for now
    GGML_ASSERT(ds_filter->ne[0] == 12);
    GGML_ASSERT(x->type == GGML_TYPE_F32);

    struct ggml_tensor * result = ggml_dup_tensor(ctx, x);
    result->op     = GGML_OP_AA_SNAKE_BETA;
    result->src[0] = x;
    result->src[1] = log_alpha;
    result->src[2] = log_beta;
    result->src[3] = us_filter;
    result->src[4] = ds_filter;
    return result;
}
```

### 1.3  ggml/src/ggml-cpu/ops.h + ops.cpp

Declaration (`ops.h`):
```c
void ggml_compute_forward_aa_snake_beta(const struct ggml_compute_params * params,
                                        struct ggml_tensor * dst);
```

Definition (`ops.cpp`): port `aa_snake_beta_op` body from
`src/indextts_voc.cpp:189-321` (the file's current line numbers). The
new function takes `params` + `dst` instead of `dst, src, ith, nth,
userdata`. Five things change:

1. **Pull operands from `dst->src[]`** instead of `userdata`:
   ```c
   const struct ggml_tensor * x         = dst->src[0];
   const struct ggml_tensor * log_alpha = dst->src[1];
   const struct ggml_tensor * log_beta  = dst->src[2];
   const struct ggml_tensor * us_filter = dst->src[3];
   const struct ggml_tensor * ds_filter = dst->src[4];
   ```
2. **Compute α/β/baked filters inline** (no `aa_snake_params` struct):
   read each channel's `expf(log_alpha[c])` per call. ggml-CPU-CPU paths
   are not inside a hot multi-loop the way the indextts custom op was;
   the alpha/beta arrays can be small allocas or local vectors per call.
3. **Use `params->ith / params->nth`** for the worker split (same shape
   as the existing `aa_snake_beta_op`).
4. **`ggml_set_n_tasks` is via the op's `n_tasks_op` callback** —
   ggml-cpu.c case (see §1.4). Keep the AA scratch buffers local to the
   forward fn or use ggml-cpu's `wdata` scratch ring.
5. **Drop `INDEXTTS_AA_SCALAR` opt-out** — we already have a separate
   scalar fallback in `src/indextts_voc.cpp:aa_snake_beta_op`. The ggml
   op's CPU forward is the Accelerate-vectorised version; INDEXTTS_AA_SCALAR
   stays in `indextts_voc.cpp` for the legacy path.

### 1.4  ggml/src/ggml-cpu/ggml-cpu.c

Three switch cases — search for `case GGML_OP_CONV_TRANSPOSE_1D:` (there
are three of them) and add an `aa_snake_beta` case in each block.

```c
// ~line 1895 — compute_forward dispatch
case GGML_OP_AA_SNAKE_BETA:
    {
        ggml_compute_forward_aa_snake_beta(params, tensor);
    } break;

// ~line 2342 — n_tasks dispatch (parallelism hint)
case GGML_OP_AA_SNAKE_BETA:
    {
        n_tasks = n_threads;   // splits by channel — fully parallel
    } break;

// ~line 2849 — op_offload (if you see a backend-offload switch in this file)
case GGML_OP_AA_SNAKE_BETA:
    return false;   // CPU forward only; backends can override via supports_op
```

### 1.5  ggml/src/ggml-metal/ggml-metal-impl.h

Kargs struct (mirrors upstream CUDA's kernel signature):

```c
typedef struct {
    int32_t T;            // sequence length per channel
    int32_t C;            // channels
    int32_t K;            // filter taps (always 12 in current kernel)
    int32_t up_pad;       // 5 — replicate-pad before upsample
    int32_t up_pad_left;  // 15
    int32_t up_pad_right; // 15
    int32_t ds_pad_left;  // 5
    int32_t ds_pad_right; // 6
} ggml_metal_kargs_aa_snake_beta;
```

### 1.6  ggml/src/ggml-metal/ggml-metal.metal

The kernel. See §2 for the full skeleton; it goes near
`kernel_conv_transpose_1d` (line ~4856) and follows the same
template-host-name registration:

```cpp
typedef void (aa_snake_beta_t)(
        constant ggml_metal_kargs_aa_snake_beta & args [[buffer(0)]],
        device const float                        * x         [[buffer(1)]],
        device const float                        * log_alpha [[buffer(2)]],
        device const float                        * log_beta  [[buffer(3)]],
        device const float                        * us_filter [[buffer(4)]],
        device const float                        * ds_filter [[buffer(5)]],
        device float                              * dst       [[buffer(6)]],
        uint3                                       tgpig     [[threadgroup_position_in_grid]],
        uint3                                       tpitg     [[thread_position_in_threadgroup]]);

template [[host_name("kernel_aa_snake_beta_f32")]]
kernel aa_snake_beta_t kernel_aa_snake_beta_impl<float>;
```

### 1.7  ggml/src/ggml-metal/ggml-metal-device.{h,cpp}

- `supports_op`: return true for `GGML_OP_AA_SNAKE_BETA` when all srcs
  are F32. Other backends (CUDA, Vulkan, SYCL, OpenCL, RPC) return
  false → ggml-backend-sched falls back to the CPU forward we just
  added.
- `get_pipeline_aa_snake_beta`: look up `kernel_aa_snake_beta_f32` from
  the library. Mirror `get_pipeline_conv_transpose_1d` at line 1756.

### 1.8  ggml/src/ggml-metal/ggml-metal-ops.cpp

- Top-level dispatch case (search for `case GGML_OP_CONV_TRANSPOSE_1D:`
  ~line 389):
  ```cpp
  case GGML_OP_AA_SNAKE_BETA:
      n_fuse = ggml_metal_op_aa_snake_beta(ctx, idx);
      break;
  ```
- Op implementation function (mirror `ggml_metal_op_conv_transpose_1d`
  at line 3929):
  - Build kargs from `op->ne` and the operand shapes.
  - Compute `dim3 blocks(seq_blocks, channels, batches)` equivalent for Metal:
    - threadgroups: `((T + BUFFER_SIZE - 1) / BUFFER_SIZE, C, 1)` —
      well, with the upstream's 128-thread layout, it's
      `((T + 128 * BUFFER_SIZE - 1) / (128 * BUFFER_SIZE), C, 1)`.
    - threadgroup size: `(128, 1, 1)`.
  - Bind buffers, dispatch.

### 1.9  src/indextts_voc.cpp

Replace the three call sites currently calling `aa_snake_beta(...)`
(custom op) or `aa_snake_beta_native(...)` (Step B-v2 native ggml ops)
with a new dispatch arm:

```cpp
if (aa_use_native()) {
    x = aa_snake_beta_native(ctx0, x, alpha, beta, usf, dsf);
} else if (aa_use_opvariant()) {                       // NEW
    x = ggml_aa_snake_beta(ctx0, x, alpha, beta, usf, dsf);
} else {
    x = aa_snake_beta(ctx0, x, alpha, beta, usf, dsf, c->aa_params);
}
```

Gate behind `INDEXTTS_AA_BACKEND=op` (or `=metal` — pick one). The Step
A `aa_blocks_gpu` early-exit must also let the new op through:
```cpp
const bool aa_blocks_gpu = c->use_aa
                           && !aa_use_native()
                           && !aa_use_opvariant()    // NEW
                           && !force_gpu_with_aa;
```

Once the bench shows the new op wins on Metal, switch the default to
`aa_use_opvariant()` and demote the others to opt-in. Keep them
compiled — they're still bench oracles.

---

## §2 — Metal kernel skeleton (port from CUDA)

### 2.1  Upstream reference

`/Users/christianstrobele/code/index-tts/indextts/BigVGAN/alias_free_activation/cuda/anti_alias_activation_cuda.cu`

Read lines 40–181 — that's the `__global__ void anti_alias_activation_forward`
kernel and `dispatch_anti_alias_activation_forward` host stub. Total
~140 lines. Apache 2.0 license; clean-room re-derivation into MSL.

Key constants (lines 36–42):
```c
constexpr int FILTER_SIZE                     = 12;
constexpr int HALF_FILTER_SIZE                = 6;
constexpr int BUFFER_SIZE                     = ?;  // see §2.2
constexpr int UPSAMPLE_REPLICATION_PAD        = 5;
constexpr int DOWNSAMPLE_REPLICATION_PAD_LEFT = 5;
constexpr int DOWNSAMPLE_REPLICATION_PAD_RIGHT= 6;
```

Threadgroup layout (lines 196–209):
```c
constexpr int threads_per_block  = 128;
constexpr int seq_len_per_block  = 4096;       // → BUFFER_SIZE = 32
int blocks_per_seq_len = (seq_len + seq_len_per_block - 1) / seq_len_per_block;
dim3 blocks(blocks_per_seq_len, channels, batch_size);
dim3 threads(threads_per_block, 1, 1);
```

Buffer-size math: 128 threads × `BUFFER_SIZE` samples/thread = `seq_len_per_block`.
At `seq_len_per_block = 4096`, `BUFFER_SIZE = 32`. On M1 with typical
BigVGAN AA-site `T` in the 200–6000 range, this means most invocations
land in 1–2 threadgroups per channel. Worth tuning `BUFFER_SIZE` down
to 8 or 16 to spread short sequences across more SMs.

### 2.2  MSL kernel skeleton

```cpp
// In ggml-metal.metal, near kernel_conv_transpose_1d.
//
// One threadgroup processes (channel × seq_chunk × batch).
// Each thread owns BUFFER_SIZE output samples; intermediates live in
// thread-local arrays / threadgroup memory.

constant constexpr int AA_FILTER_SIZE          = 12;
constant constexpr int AA_HALF_FILTER          = 6;
constant constexpr int AA_BUFFER_SIZE          = 32;       // tune: 8/16/32
constant constexpr int AA_UPSAMPLE_PAD         = 5;
constant constexpr int AA_DOWN_PAD_L           = 5;
constant constexpr int AA_DOWN_PAD_R           = 6;
constant constexpr int AA_THREADS_PER_TGRP     = 128;

template <typename T>
kernel void kernel_aa_snake_beta_impl(
        constant ggml_metal_kargs_aa_snake_beta & args      [[buffer(0)]],
        device const T                          * x         [[buffer(1)]],
        device const T                          * log_alpha [[buffer(2)]],
        device const T                          * log_beta  [[buffer(3)]],
        device const T                          * us_filter [[buffer(4)]],
        device const T                          * ds_filter [[buffer(5)]],
        device T                                * dst       [[buffer(6)]],
        uint3                                     tgpig     [[threadgroup_position_in_grid]],
        uint3                                     tpitg     [[thread_position_in_threadgroup]])
{
    // tgpig.x = seq_chunk index, tgpig.y = channel, tgpig.z = batch
    // tpitg.x = thread within threadgroup
    const int seq_blk = (int)tgpig.x;
    const int c       = (int)tgpig.y;
    const int b       = (int)tgpig.z;
    const int tid     = (int)tpitg.x;

    // Load α/β for this channel once
    const float alpha_val = exp(log_alpha[c]);
    const float beta_val  = exp(log_beta[c]);
    const float inv_beta  = 1.0f / (beta_val + 1e-9f);

    // Load filter taps into registers (copy from device → local).
    float up_f[AA_FILTER_SIZE];
    float dn_f[AA_FILTER_SIZE];
    #pragma unroll
    for (int k = 0; k < AA_FILTER_SIZE; ++k) {
        up_f[k] = (float)us_filter[k] * 2.0f;     // pre-bake the ×2 zero-stuff gain
        dn_f[k] = (float)ds_filter[k];
    }

    // Each thread owns AA_BUFFER_SIZE output samples (in the original T frame).
    // Indexing: seq_offset = seq_blk * 128 * BUFFER_SIZE + tid * BUFFER_SIZE.
    const int seq_offset      = seq_blk * AA_THREADS_PER_TGRP * AA_BUFFER_SIZE + tid * AA_BUFFER_SIZE;
    const int channel_offset  = (b * args.C + c) * args.T;
    device const T * src_ch   = x   + channel_offset;
    device T       * dst_ch   = dst + channel_offset;

    // Edge values for replicate-padding.
    const float seq_left  = (float)src_ch[0];
    const float seq_right = (float)src_ch[args.T - 1];

    // ── 1. Build the upsample-input window into stack array `elements[]`.
    // Each thread gathers AA_BUFFER_SIZE + 2 * AA_HALF_FILTER positions
    // around its slice of the input.
    float elements[2 * AA_FILTER_SIZE + 2 * AA_BUFFER_SIZE + 2 * AA_UPSAMPLE_PAD];
    #pragma unroll
    for (int i = -AA_HALF_FILTER; i < AA_BUFFER_SIZE + AA_HALF_FILTER; ++i) {
        const int idx = seq_offset + i;
        float val = 0.0f;
        if (idx < 0 && idx >= -AA_UPSAMPLE_PAD) {
            val = seq_left;
        } else if (idx >= args.T && idx < args.T + AA_UPSAMPLE_PAD) {
            val = seq_right;
        } else if (idx >= 0 && idx < args.T) {
            val = (float)src_ch[idx];
        }
        // zero-stuff: write at 2*offset, every other slot stays 0
        elements[2 * (AA_HALF_FILTER + i)] = val;
    }

    // ── 2. Upsample FIR — convolve `elements[]` with up_f[]. Writes
    // into `intermediates[]` which has DOWNSAMPLE_REPLICATION_PAD_LEFT
    // headroom reserved on the left.
    float intermediates[2 * AA_FILTER_SIZE + 2 * AA_BUFFER_SIZE
                        + AA_DOWN_PAD_L + AA_DOWN_PAD_R];
    #pragma unroll
    for (int i = 0; i < 2 * AA_BUFFER_SIZE + 2 * AA_FILTER_SIZE; ++i) {
        float acc = 0.0f;
        #pragma unroll
        for (int k = 0; k < AA_FILTER_SIZE; ++k) {
            acc += up_f[k] * elements[i + k];
        }
        intermediates[i + AA_DOWN_PAD_L] = acc;
    }

    // ── 3. SnakeBeta in place on intermediates[]: x + sin²(α·x) / β.
    #pragma unroll
    for (int i = 0; i < 2 * AA_BUFFER_SIZE + 2 * AA_FILTER_SIZE; ++i) {
        const float v = intermediates[i + AA_DOWN_PAD_L];
        const float s = sin(alpha_val * v);
        intermediates[i + AA_DOWN_PAD_L] = v + inv_beta * s * s;
    }

    // ── 4. Replicate-pad downsample left/right.
    #pragma unroll
    for (int i = 0; i < AA_DOWN_PAD_L; ++i) {
        intermediates[i] = intermediates[AA_DOWN_PAD_L];
    }
    const int tail = AA_DOWN_PAD_L + 2 * AA_BUFFER_SIZE + 2 * AA_FILTER_SIZE;
    #pragma unroll
    for (int i = 0; i < AA_DOWN_PAD_R; ++i) {
        intermediates[tail + i] = intermediates[tail - 1];
    }

    // ── 5. Downsample FIR (stride 2).
    float output[AA_BUFFER_SIZE];
    #pragma unroll
    for (int i = 0; i < AA_BUFFER_SIZE; ++i) {
        float acc = 0.0f;
        #pragma unroll
        for (int k = 0; k < AA_FILTER_SIZE; ++k) {
            acc += dn_f[k] * intermediates[i * 2 + k + AA_DOWN_PAD_R];
        }
        output[i] = acc;
    }

    // ── 6. Write output (bounds-check the last thread of the last block).
    #pragma unroll
    for (int i = 0; i < AA_BUFFER_SIZE; ++i) {
        const int dst_idx = seq_offset + i;
        if (dst_idx < args.T) {
            dst_ch[dst_idx] = (T)output[i];
        }
    }
}

typedef decltype(kernel_aa_snake_beta_impl<float>) aa_snake_beta_t;
template [[host_name("kernel_aa_snake_beta_f32")]]
kernel aa_snake_beta_t kernel_aa_snake_beta_impl<float>;
```

**Watch-outs:**

- The zero-stuff trick at step 1 (`elements[2 * (HALF_FILTER + i)] = val`)
  is the same as upstream's CUDA — works because the FIR convolution
  in step 2 reads `elements[i + k]` and the zero-stuffed positions
  contribute zero. Don't "optimize" by removing the doubling.
- The `#pragma unroll`s matter; without them the compiler can't keep
  `elements[]` and `intermediates[]` in registers. Expect ~ 2 KB stack
  per thread.
- BUFFER_SIZE × threads_per_threadgroup × sizeof(float) for the dst write
  needs to fit in the L1/L2 cache. 32 × 128 × 4 = 16 KB per threadgroup
  — fine on Apple M1's 32 KB threadgroup memory budget.
- Apple Silicon doesn't have a watchdog issue with conv-style kernels
  the way the original `kernel_conv_transpose_1d` did (see PR 04). But
  long-running kernels still cap at ~ 2 s before the macOS GPU watchdog
  fires `kIOGPUCommandBufferCallbackErrorImpactingInteractivity`. If
  the kernel is slow on first compile, the second invocation will hang
  — see PR 04's writeup.

---

## §3 — Bench oracle + validation pipeline

### 3.1  Smoke-test the existing oracles before changing anything

```bash
cd /Users/christianstrobele/code/CrispASR
git checkout main && git pull

# Build
cmake --build build-ninja-compile --target crispasr-cli

# Step A baseline (custom op, CPU)
INDEXTTS_BENCH=1 build-ninja-compile/bin/crispasr --backend indextts \
    --model /Volumes/backups/ai/crispasr/indextts-gpt-q8_0.gguf \
    --codec-model /Volumes/backups/ai/crispasr/indextts-bigvgan.gguf \
    --voice samples/jfk.wav \
    --tts "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs." \
    --tts-output /tmp/oracle_stepA.wav 2>&1 | grep BigVGAN

# Step B-v2 native ggml (CPU — bit-equiv to A)
INDEXTTS_BENCH=1 INDEXTTS_AA_BACKEND=native build-ninja-compile/bin/crispasr \
    --backend indextts --no-gpu \
    --model /Volumes/backups/ai/crispasr/indextts-gpt-q8_0.gguf \
    --codec-model /Volumes/backups/ai/crispasr/indextts-bigvgan.gguf \
    --voice samples/jfk.wav \
    --tts "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs." \
    --tts-output /tmp/oracle_stepB.wav 2>&1 | grep BigVGAN
```

Both should report `BigVGAN compute ~ 7.5-8 s (AA/CPU)`.

### 3.2  Click-detector (the cheap aliasing oracle)

A clean BigVGAN AA output has zero (or single-digit) inter-sample jumps
> 0.3 of full scale and max\|Δ\| < 0.4. The raw aliased path has
~2000 jumps > 0.3 and max\|Δ\| > 1.0 (impossible for a band-limited
signal — that's the signature).

```python
# /tmp/click_check.py
import wave, sys, numpy as np
def stats(path):
    with wave.open(path) as w:
        n = w.getnframes()
        b = w.readframes(n)
    x = np.frombuffer(b, dtype=np.int16).astype(np.float32) / 32768.0
    d = np.diff(x)
    return {
        "peak":   float(np.abs(x).max()),
        "delta_gt_0p3": int((np.abs(d) > 0.3).sum()),
        "delta_gt_0p5": int((np.abs(d) > 0.5).sum()),
        "max_delta":    float(np.abs(d).max()),
    }
for p in sys.argv[1:]:
    print(p, stats(p))
```

Run after every kernel change: `python /tmp/click_check.py /tmp/oracle_stepA.wav /tmp/oracle_stepC.wav`.

Targets for Step C-2 output (Metal kernel):
- `delta_gt_0p3 ≤ 30` (matches Step B-v2 GPU's noise-floor delta)
- `max_delta ≤ 0.4`
- `peak ≤ 1.0` (no clipping)

### 3.3  Numerical-diff against Step B-v2 (the CPU oracle)

Step B-v2 on CPU is bit-equivalent to Step A. Use it as the per-sample
ground truth for the new op's CPU forward:

```python
# /tmp/diff_paths.py
import wave, numpy as np, sys
def load(p):
    with wave.open(p) as w: b = w.readframes(w.getnframes())
    return np.frombuffer(b, dtype=np.int16).astype(np.float32) / 32768.0

a, b = load(sys.argv[1]), load(sys.argv[2])
n = min(len(a), len(b))
d = a[:n] - b[:n]
print(f"rmsdiff={np.sqrt((d*d).mean()):.2e}  max|Δ|={np.abs(d).max():.2e}  len_a={len(a)} len_b={len(b)}")
```

- CPU-forward of new op vs Step B-v2 CPU: target `rmsdiff < 1e-5,
  max|Δ| < 1e-4`. Above that, the CPU forward is wrong; fix before
  even trying the Metal kernel.
- Metal forward of new op vs Step B-v2 CPU: target `rmsdiff < 1e-4,
  max|Δ| < 1e-3` (Metal vs CPU fp order-of-ops gives some noise; this
  bound matches what Step B-v2 GPU drifts from Step B-v2 CPU today).

### 3.4  ASR roundtrip (end-to-end correctness)

```bash
ffmpeg -y -loglevel error -i /tmp/oracle_stepC.wav -ar 16000 -ac 1 /tmp/oracle_stepC-16k.wav
build-ninja-compile/bin/crispasr --backend parakeet \
    --model ~/.cache/crispasr/parakeet-tdt-0.6b-v3.gguf \
    -nt -np /tmp/oracle_stepC-16k.wav
```

Expected: `The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.` (modulo trailing whitespace; comma vs period variations are OK).

If the transcript skips a word or substitutes `Pack→Tack`, the kernel
output has audible artifacts that ASR's noise-floor model is rejecting.
Don't ship.

### 3.5  Wall-clock bench

Run 3-5 iterations after a warm-up to filter cache effects:

```bash
TEXT='The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.'
mkdir -p /tmp/stepC-bench
for label in stepA stepB_native stepC_op_cpu stepC_op_metal; do
    case $label in
      stepA)            ENV="" GPU="";;
      stepB_native)     ENV="INDEXTTS_AA_BACKEND=native" GPU="";;
      stepC_op_cpu)     ENV="INDEXTTS_AA_BACKEND=op" GPU="--no-gpu";;
      stepC_op_metal)   ENV="INDEXTTS_AA_BACKEND=op" GPU="";;
    esac
    for i in 1 2 3; do
        /usr/bin/time -p env $ENV INDEXTTS_BENCH=1 build-ninja-compile/bin/crispasr \
            --backend indextts \
            --model /Volumes/backups/ai/crispasr/indextts-gpt-q8_0.gguf \
            --codec-model /Volumes/backups/ai/crispasr/indextts-bigvgan.gguf \
            --voice samples/jfk.wav --tts "$TEXT" $GPU \
            --tts-output /tmp/stepC-bench/${label}_$i.wav \
            2>/tmp/stepC-bench/${label}_$i.log
        voc=$(grep BigVGAN /tmp/stepC-bench/${label}_$i.log | awk -F"compute " '{print $2}' | awk '{print $1}')
        echo "$label run$i: $voc ms"
    done
done
```

Decision rule: ship Step C-2 if `stepC_op_metal` < `min(stepA, stepB_native)`
*and* the click-detector + ASR pass. Otherwise iterate the kernel.

### 3.6  crispasr-diff harness (deeper validation, optional)

For tensor-level diff against the Python reference (catches bugs the
end-to-end click-detector misses):

```bash
# Dump Python ref (CPU; takes a few minutes the first time)
HF_HOME=/Volumes/backups/ai/huggingface-hub \
HUGGINGFACE_HUB_CACHE=/Volumes/backups/ai/huggingface-hub \
TRANSFORMERS_OFFLINE=1 \
python tools/dump_reference.py \
    --backend indextts \
    --model-dir /Volumes/backups/ai/huggingface-hub/IndexTTS-1.5 \
    --output /tmp/indextts-ref.gguf \
    --text "Hello"

# Diff C++ vs ref
build-ninja-compile/bin/crispasr-diff indextts \
    /Volumes/backups/ai/crispasr/indextts-gpt-q8_0.gguf \
    /tmp/indextts-ref.gguf samples/jfk.wav
```

The current indextts ref dump (`tools/reference_backends/indextts.py`)
captures GPT-side stages: `text_tokens`, `prefix_embeds`, `gpt_layer_0`,
`gpt_layer_23`, `prefill_logits`, `mel_codes`, `latent_output`. It does
NOT capture per-AA-site BigVGAN intermediates. To validate Step C-2
deeply, extend the dump with `bigvgan_conv_pre`, `bigvgan_amp_0_aa_0`
(post-AA-snakebeta), etc. — pattern: `tools/reference_backends/qwen3_tts_codec.py`
(cleanest small backend) or `tools/reference_backends/kokoro.py`
(most stages). The C++ side adds matching `else if (backend_name == "indextts-voc")`
branches in `examples/cli/crispasr_diff_main.cpp`.

If you only need to ship Step C-2, skip §3.6 — the §3.1–3.5 oracles
are sufficient.

---

## §4 — File / location cheat sheet

| Thing | Path |
| ---- | ---- |
| Current CPU AA op (port from here) | `src/indextts_voc.cpp:189-321` |
| Step B-v2 native graph (validation oracle on CPU) | `src/indextts_voc.cpp:418-525` (approx; grep `aa_snake_beta_native`) |
| Upstream CUDA reference | `/Users/christianstrobele/code/index-tts/indextts/BigVGAN/alias_free_activation/cuda/anti_alias_activation_cuda.cu` |
| Upstream torch reference (no fused, pure ops) | `/Users/christianstrobele/code/index-tts/indextts/BigVGAN/alias_free_activation/torch/{act,resample,filter}.py` |
| PR draft body | `tools/upstream-prs/07-metal-aa-snake-beta.md` |
| PR ordering / audit | `tools/upstream-prs/README.md`, `tools/upstream-prs/MASTER-AUDIT.md` |
| Existing Metal-perf-patch precedent | `tools/upstream-prs/04-metal-conv-transpose-1d.{md,patch}` (merged upstream as #1477) |
| Models | `/Volumes/backups/ai/crispasr/indextts-gpt-q8_0.gguf` + `indextts-bigvgan.gguf` |
| Voice prompt | `samples/jfk.wav` (16 kHz mono PCM16) |
| ASR roundtrip model | `~/.cache/crispasr/parakeet-tdt-0.6b-v3.gguf` |
| Build dir | `build-ninja-compile/` — `cmake --build build-ninja-compile --target crispasr-cli` |
| Format wrapper (clang-format-18 only!) | `tools/format.sh` or `/opt/homebrew/opt/llvm@18/bin/clang-format -i <file>` |
| Click-detector script | write `/tmp/click_check.py` per §3.2 |
| Numerical-diff script | write `/tmp/diff_paths.py` per §3.3 |

## §5 — Env-knob inventory (don't break these)

| Env | Effect | Owner |
| --- | --- | --- |
| `INDEXTTS_BENCH=1` | Unlock vocoder timing log | repo convention |
| `INDEXTTS_AA_BACKEND=native` | Step B-v2 ggml-native graph | shipped |
| `INDEXTTS_AA_BACKEND=op` | **NEW**: Step C-2 new ggml op | this PR |
| `INDEXTTS_AA_SCALAR=1` | Disable vDSP inner loops | shipped |
| `INDEXTTS_VOCODER_RAW=1` | Aliased fast path (legacy) | shipped |
| `INDEXTTS_VOCODER_AA=0` | Same | shipped |
| `INDEXTTS_VOC_FORCE_GPU=1` | Keep custom-op on Metal (slow mixed) | shipped |
| `INDEXTTS_DEBUG=1` | Per-stage tensor dumps | shipped |

All new perf-timing logs MUST be gated under `INDEXTTS_BENCH || verbosity >= 1`
to match the repo's `<BACKEND>_BENCH` convention.

## §6 — Project-level reminders (from `CLAUDE.md`)

- **clang-format MUST be v18.** v22 (Homebrew/Xcode default) silently
  re-wraps lines and breaks CI lint. Use `tools/format.sh` or the
  explicit `/opt/homebrew/opt/llvm@18/bin/clang-format` binary.
- **Models live at `/Volumes/backups/ai/crispasr/`** — don't redownload
  to `~/.cache`. The HF cache mirror is at `/Volumes/backups/ai/huggingface-hub/`.
- **Use `python` (not `python3`)** — `python` resolves to the conda env
  with `transformers`/`torch`/`gguf`; `python3` is Homebrew bare.
- **The ggml subtree has CrispASR patches** — mark every new hunk with
  the standard `// CrispASR patch ... MUST RE-APPLY` comment so the
  next `ggml-bump.sh` re-applies them.
- **Integrate directly to `main`** — feature branches are rebased +
  fast-forwarded and deleted; never park work on a feature branch "for
  review."
- **Commits end with `Co-Authored-By: Claude Opus 4.7 (1M context)
  <noreply@anthropic.com>`** (sonnet/haiku if applicable).
- **Strip local markers from outbound artifacts** before opening any
  upstream PR — `// CrispASR patch`, `MUST RE-APPLY`, and any
  `Co-Authored-By: Claude` lines need to go.

## §7 — Git state when handed off

Last commit on `main` at handoff is `4b1307c3` (Step B-v2 lands as
opt-in). All A/B/C-1/B-v2 work is in. The next commit should be Step
C-2 phase 1 (machinery + CPU forward) — landable on its own as a no-op
regression because the new op's CPU forward is byte-equivalent to the
existing custom op. Phase 2 (Metal kernel) follows.

Branches to know:
- `main` — current; Step A + B-v2 + C-1 landed.
- `issue81-phase1-uar-wip` — CUDA work (PRs 05 + 06); don't merge into
  main yet, those depend on PR 01 landing upstream first.

---

## §8 — Why I'm handing this off vs grinding it out

Step C-2 is genuinely a multi-hour project with:

1. A real upstream-ggml dependency (touching the subtree means re-apply
   on every bump),
2. Kernel debug iterations (MSL is harder to print-debug than C),
3. Cross-backend stubs (`supports_op` returns false for CUDA/Vulkan/SYCL/
   OpenCL/RPC), and
4. The need for fresh prompt-cache context — long sessions accumulate
   stale grep results and burned reasoning.

Doing it in a fresh session with this handover as the prompt gives you
~3 hours of clean context against a precisely-scoped problem. The
oracles in §3 protect against shipping a wrong-output kernel.
