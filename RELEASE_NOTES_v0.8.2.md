# CrispASR v0.8.2

Patch release — chatterbox robustness + chatterbox-turbo emotion tags.

## Fixes

- **chatterbox: segfault on very long text (#182).** A prompt longer than the
  model's text-position table (2050 positions for base T3; the char-level base
  tokenizer hits this on a ~4.5 KB paragraph, multibyte scripts with far less)
  read past the table and crashed. The token sequence is now bounded to the
  model's positional capacity (with a warning), with a defensive clamp at the
  embedding site. Five prefill sites (cond + uncond CFG, base + GPT-2 paths) were
  affected.

- **chatterbox: use-after-free in the long-form chunk loop.** Reallocating the KV
  cache for a longer chunk left the cached decode step-graphs pointing at freed
  tensors → intermittent crash on the 2nd+ chunk (also affected the server
  `/v1/audio/speech` chunk loop). Cached bucket graphs are now invalidated on KV
  realloc.

## Improvements

- **chatterbox CLI long-form `--tts` (§218).** Long input is now sentence-chunked
  before synthesis (the same pipeline the server already used) — each chunk
  synthesises within the model's healthy horizon and the audio is concatenated
  with short pauses, so long prompts render in full instead of being truncated.

- **chatterbox-turbo emotion/style tags (§217).** You can drive turbo prosody with
  bracketed tags in the input text: `[laugh] [chuckle] [sigh] [gasp] [cough]
  [groan] [sniff] [shush] [clear throat] [whispering] [angry] [happy] [crying]
  [fear] [surprised] [sarcastic] [dramatic] [narration] [advertisement]`. The
  tokenizer now emits these as their special token id (requires the re-uploaded
  `cstr/chatterbox-turbo-GGUF` files with the full 50276-token vocab; the
  converter now includes `added_tokens.json`).

## Upgrade

Drop-in for v0.8.1. No model re-download required for the #182 fixes; the turbo
emotion tags need the refreshed turbo T3 GGUFs (re-uploaded to HF).
