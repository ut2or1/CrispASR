#!/usr/bin/env python3
"""Kaggle GPU kernel: CrispASR full-fleet benchmark + onnx-asr head-to-head (#81).

Tests all CrispASR backends that fit in Kaggle's time/disk budget.
Head-to-head with onnx-asr for overlapping models (whisper, parakeet, canary).
CrispASR-only RTF for the 20+ backends onnx-asr doesn't support.

Push (under chr1s4):
  export KAGGLE_API_TOKEN=<chr1s4 token>
  python -m kaggle kernels push -p tools/kaggle/issue81-onnx-bench
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

WORK = Path("/kaggle/working")
REPO = WORK / "CrispASR"
TEMP = Path("/kaggle/temp") if Path("/kaggle/temp").is_dir() else Path("/tmp")

# ── Phase 0: Clone + build CrispASR with CUDA ───────────────────────────────
print("=== Phase 0: clone + build CrispASR ===", flush=True)
if not REPO.exists():
    subprocess.check_call([
        "git", "clone", "--depth", "1", "-b", "main",
        "https://github.com/CrispStrobe/CrispASR", str(REPO),
    ])
if (REPO / "ggml").is_dir() and not (REPO / "ggml" / "CMakeLists.txt").exists():
    subprocess.check_call(["git", "submodule", "update", "--init", "ggml"], cwd=str(REPO))

if (REPO / "tools" / "kaggle").is_dir():
    sys.path.insert(0, os.path.join(str(REPO), "tools", "kaggle"))
import kaggle_harness as kh  # noqa: E402

BUILD = TEMP / "build"
BUILD.mkdir(parents=True, exist_ok=True)

has_cuda = Path("/usr/local/cuda/bin/nvcc").exists()
print(f"  CUDA available: {has_cuda}")

if has_cuda:
    arch = kh.detect_cuda_arch()
    flags = kh.cuda_build_flags(arch) + kh.cache_and_link_flags()
    cmake_flags = "-DCMAKE_BUILD_TYPE=Release " + " ".join(flags)
    ret = subprocess.call(f"cmake -G Ninja -B {BUILD} -S {REPO} {cmake_flags}", shell=True)
    if ret != 0:
        print("  CUDA cmake failed, falling back to CPU")
        has_cuda = False
        import shutil
        if BUILD.exists(): shutil.rmtree(BUILD); BUILD.mkdir(parents=True, exist_ok=True)

if not has_cuda:
    subprocess.check_call(
        f"cmake -G Ninja -B {BUILD} -S {REPO} -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF",
        shell=True, stdout=subprocess.DEVNULL)

n_jobs = min(os.cpu_count() or 2, 4)
with kh.build_heartbeat("cmake.build"):
    subprocess.check_call(f"cmake --build {BUILD} -j{n_jobs} --target crispasr-cli", shell=True,
                          stdout=subprocess.DEVNULL)
CRISPASR = BUILD / "bin" / "crispasr"
print(f"  built: {CRISPASR}")

# ── Phase 1: Install onnx-asr ───────────────────────────────────────────────
print("\n=== Phase 1: install onnx-asr ===", flush=True)
subprocess.check_call([sys.executable, "-m", "pip", "install", "-q",
                        "onnx-asr", "soundfile", "huggingface_hub", "onnxruntime"])

# ── Phase 2: Prepare audio ──────────────────────────────────────────────────
print("\n=== Phase 2: prepare audio ===", flush=True)
import soundfile as sf
import numpy as np

jfk_wav = REPO / "samples" / "jfk.wav"
pcm, sr = sf.read(str(jfk_wav), dtype="float32")
duration = len(pcm) / sr

# 60s version
repeats = max(1, int(60 / duration))
long_pcm = np.tile(pcm, repeats)
long_wav = TEMP / "long_60s.wav"
sf.write(str(long_wav), long_pcm, sr)
long_dur = len(long_pcm) / sr
print(f"  JFK: {duration:.1f}s, Long: {long_dur:.1f}s")

# ── Phase 3: Download all GGUF models ───────────────────────────────────────
print("\n=== Phase 3: download models ===", flush=True)
from huggingface_hub import hf_hub_download

MODELS_DIR = TEMP / "models"
MODELS_DIR.mkdir(parents=True, exist_ok=True)

# Models to benchmark: (backend, hf_repo, filename, label)
CRISPASR_MODELS = [
    # Head-to-head with onnx-asr.
    # NOTE: parakeet-ctc is a pure CTC (EncDecCTCModelBPE) — it must run on the
    # fastconformer-ctc backend, NOT the `parakeet` transducer backend (which
    # rejects it: "no RNN-T decoder/joint tensors — failed to load"). Using the
    # wrong backend here is exactly the #81 bug that produced the bogus 102.4×/
    # 127.7× (a ~0.5 s failed load timed as if it were inference). See the guard
    # in bench_crispasr() below.
    ("fastconformer-ctc", "cstr/parakeet-ctc-0.6b-GGUF",   "parakeet-ctc-0.6b-q8_0.gguf",     "parakeet-ctc-0.6b"),
    ("parakeet",  "cstr/parakeet-tdt-0.6b-v2-GGUF", "parakeet-tdt-0.6b-v2-q8_0.gguf",  "parakeet-tdt-0.6b"),
    # CrispASR-only (small models that fit in budget)
    ("moonshine", "cstr/moonshine-tiny-GGUF",       "moonshine-tiny-q8_0.gguf",         "moonshine-tiny"),
    ("cohere",    "cstr/cohere-transcribe-GGUF",    "cohere-transcribe-q4_k.gguf",      "cohere-transcribe"),
    ("kyutai-stt","cstr/kyutai-stt-1b-GGUF",        "kyutai-stt-1b-q4_k.gguf",          "kyutai-stt-1b"),
    ("firered-asr","cstr/firered-asr2-aed-GGUF",    "firered-asr2-aed-q4_k.gguf",      "firered-asr2"),
    ("sensevoice","cstr/sensevoice-small-GGUF",     "sensevoice-small-q8_0.gguf",       "sensevoice-small"),
    ("funasr",    "cstr/fun-asr-nano-GGUF",         "fun-asr-nano-q8_0.gguf",           "funasr-nano"),
    ("paraformer","cstr/paraformer-zh-GGUF",        "paraformer-zh-q8_0.gguf",          "paraformer-zh"),
    ("glm-asr",   "cstr/glm-asr-nano-GGUF",        "glm-asr-nano-q4_k.gguf",           "glm-asr-nano"),
]

# Add moonshine tokenizer
MOONSHINE_TOK = None

model_paths = {}
for backend, repo, fname, label in CRISPASR_MODELS:
    try:
        p = hf_hub_download(repo, fname, cache_dir=str(TEMP / "hf"))
        model_paths[label] = p
        sz = Path(p).stat().st_size / (1024**2)
        print(f"  {label}: {sz:.0f} MB")
        # Download moonshine tokenizer alongside
        if "moonshine" in label:
            try:
                MOONSHINE_TOK = hf_hub_download(repo, "tokenizer.bin", cache_dir=str(TEMP / "hf"))
            except Exception:
                pass
    except Exception as e:
        print(f"  {label}: SKIP ({e})")

# ── Phase 4: Benchmark CrispASR fleet ───────────────────────────────────────
print("\n=== Phase 4: benchmark CrispASR fleet ===", flush=True)

gpu_flag = "--gpu-backend cuda" if has_cuda else ""

def bench_crispasr(backend, model_path, audio_path, audio_dur, label, extra_flags="",
                   n_warmup=1, n_runs=3):
    """Benchmark one CrispASR backend. Returns (mean_time, rtf, transcript)."""
    cmd = f"{CRISPASR} --backend {backend} {gpu_flag} -m {model_path} -f {audio_path} {extra_flags} --no-prints"

    # Warmup
    for _ in range(n_warmup):
        subprocess.run(cmd, shell=True, capture_output=True, timeout=120)

    times = []
    text = ""
    ok = True
    for i in range(n_runs):
        try:
            t0 = time.perf_counter()
            r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=120)
            t1 = time.perf_counter()
            times.append(t1 - t0)
            out = (r.stdout or "").strip()
            if r.returncode != 0 or not out:
                ok = False
                if i == 0:
                    tail = (r.stderr or "").strip().split("\n")[-1] if r.stderr else ""
                    text = f"FAILED (rc={r.returncode}): {tail}"[:120]
            elif i == 0:
                text = out[:120]
        except subprocess.TimeoutExpired:
            times.append(120.0)
            ok = False

    # A crash / empty transcript must NOT be reported as a fast run — that was
    # the #81 bug: `--backend parakeet` on a CTC model exits in ~0.5 s, which the
    # old code timed and turned into `audio_dur / 0.5` = 102.4× / 127.7×. Only a
    # run that exited 0 with a non-empty transcript counts as a real measurement.
    if not times or not ok:
        return None, None, (text or "FAILED")
    mean = sum(times) / len(times)
    rtf = audio_dur / mean
    return mean, rtf, text

results = {}
for backend, repo, fname, label in CRISPASR_MODELS:
    if label not in model_paths:
        continue
    mp = model_paths[label]
    extra = ""
    if "moonshine" in label and MOONSHINE_TOK:
        extra = f"--moonshine-tokenizer {MOONSHINE_TOK}"

    print(f"\n  [{label}]")
    mean, rtf, text = bench_crispasr(backend, mp, jfk_wav, duration, label, extra)
    if mean:
        print(f"    JFK {duration:.0f}s: {mean:.3f}s = {rtf:.1f}x realtime")
        results[label] = {"mean_jfk": mean, "rtf_jfk": rtf, "text": text}

        # Also test long audio for the head-to-head models
        if "parakeet" in label:
            mean_l, rtf_l, _ = bench_crispasr(backend, mp, long_wav, long_dur, label, extra,
                                               n_warmup=0, n_runs=2)
            if mean_l:
                print(f"    Long {long_dur:.0f}s: {mean_l:.3f}s = {rtf_l:.1f}x realtime")
                results[label]["mean_long"] = mean_l
                results[label]["rtf_long"] = rtf_l
    else:
        print(f"    FAILED")
        results[label] = {"mean_jfk": None, "rtf_jfk": None, "text": "FAILED"}

# ── Phase 5: Benchmark onnx-asr (head-to-head models) ───────────────────────
print("\n=== Phase 5: benchmark onnx-asr ===", flush=True)
import onnx_asr

ONNX_MODELS = [
    ("nemo-parakeet-ctc-0.6b",    "int8", "parakeet-ctc-0.6b"),
    ("nemo-parakeet-tdt-0.6b-v2", "int8", "parakeet-tdt-0.6b"),
]

onnx_results = {}
for onnx_name, quant, label in ONNX_MODELS:
    print(f"\n  [{label} via onnx-asr]")
    try:
        t_load = time.perf_counter()
        model = onnx_asr.load_model(onnx_name, quantization=quant,
                                     providers=["CPUExecutionProvider"])
        t_load = time.perf_counter() - t_load
        print(f"    load: {t_load:.3f}s")

        # Warmup
        model.recognize(str(jfk_wav))

        times = []
        text = ""
        for i in range(3):
            t0 = time.perf_counter()
            r = model.recognize(str(jfk_wav))
            t1 = time.perf_counter()
            times.append(t1 - t0)
            if i == 0: text = str(r)[:120]

        mean = sum(times) / len(times)
        rtf = duration / mean
        print(f"    JFK {duration:.0f}s: {mean:.3f}s = {rtf:.1f}x realtime")
        onnx_results[label] = {"mean_jfk": mean, "rtf_jfk": rtf, "text": text, "load": t_load}

        # Long audio
        times_l = []
        for i in range(2):
            t0 = time.perf_counter()
            r = model.recognize(str(long_wav))
            t1 = time.perf_counter()
            times_l.append(t1 - t0)
        mean_l = sum(times_l) / len(times_l)
        rtf_l = long_dur / mean_l
        print(f"    Long {long_dur:.0f}s: {mean_l:.3f}s = {rtf_l:.1f}x realtime")
        onnx_results[label]["mean_long"] = mean_l
        onnx_results[label]["rtf_long"] = rtf_l

        del model
    except Exception as e:
        print(f"    FAILED: {e}")
        onnx_results[label] = {"mean_jfk": None, "rtf_jfk": None, "text": f"ERROR: {e}"}

# ── Phase 6: Summary ────────────────────────────────────────────────────────
gpu_name = "unknown"
try:
    gpu_name = subprocess.check_output(
        "nvidia-smi --query-gpu=name --format=csv,noheader", shell=True).decode().strip()
except Exception: pass

print("\n" + "=" * 78)
print("  CrispASR FULL-FLEET BENCHMARK + onnx-asr HEAD-TO-HEAD")
print("=" * 78)
print(f"  GPU: {gpu_name}  |  CUDA build: {has_cuda}  |  Audio: {duration:.0f}s JFK")
print()
print(f"  {'Backend':<22s} {'Engine':>10s} {'JFK RTF':>10s} {'Long RTF':>10s} {'Notes':>20s}")
print(f"  {'-'*74}")

# Head-to-head
for label in ["parakeet-ctc-0.6b", "parakeet-tdt-0.6b"]:
    ca = results.get(label, {})
    ox = onnx_results.get(label, {})
    ca_rtf = f"{ca.get('rtf_jfk', 0):.1f}x" if ca.get('rtf_jfk') else "FAIL"
    ox_rtf = f"{ox.get('rtf_jfk', 0):.1f}x" if ox.get('rtf_jfk') else "FAIL"
    ca_long = f"{ca.get('rtf_long', 0):.1f}x" if ca.get('rtf_long') else "-"
    ox_long = f"{ox.get('rtf_long', 0):.1f}x" if ox.get('rtf_long') else "-"

    print(f"  {label:<22s} {'CrispASR':>10s} {ca_rtf:>10s} {ca_long:>10s} {'CUDA Q8_0':>20s}")
    print(f"  {'':.<22s} {'onnx-asr':>10s} {ox_rtf:>10s} {ox_long:>10s} {'CPU int8':>20s}")

print(f"  {'-'*74}")
print(f"  {'CrispASR-only backends':}")

# CrispASR-only
for backend, repo, fname, label in CRISPASR_MODELS:
    if "parakeet" in label:
        continue  # already shown above
    ca = results.get(label, {})
    ca_rtf = f"{ca.get('rtf_jfk', 0):.1f}x" if ca.get('rtf_jfk') else "FAIL"
    print(f"  {label:<22s} {'CrispASR':>10s} {ca_rtf:>10s} {'':>10s} {'CUDA':>20s}")

print(f"\n  Total backends tested: {len(results)} CrispASR + {len(onnx_results)} onnx-asr")
print(f"  CrispASR supports 25+ backends; onnx-asr supports ~10")

# Save JSON
all_results = {
    "gpu": gpu_name, "cuda_build": has_cuda, "audio_duration": duration,
    "crispasr": results, "onnx_asr": onnx_results,
}
with open(WORK / "benchmark_results.json", "w") as f:
    json.dump(all_results, f, indent=2)
print(f"\n  Results saved to {WORK / 'benchmark_results.json'}")
print("\n=== Done ===", flush=True)
