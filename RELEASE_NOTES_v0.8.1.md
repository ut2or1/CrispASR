# CrispASR v0.8.1

Patch release — fixes a chatterbox-turbo loading regression introduced in v0.8.0.

## Fixes

- **chatterbox-turbo: vocab-mismatch load regression (#181).** v0.8.0 added a
  strict tokenizer↔embedding consistency check that rejected the published
  `cstr/chatterbox-turbo-GGUF` models with
  `tokenizer has 50257 tokens, T3 text_vocab_size=50276`. These files ship the
  stock 50257-token GPT-2 tokenizer against a 50276-row text embedding (19
  reserved rows) and loaded fine on v0.7.x. The mismatch is **benign in this
  direction** — BPE only emits ids `< 50257`, and special text tokens are added
  by id and bounds-checked against `text_vocab_size`, so the extra rows are never
  indexed out of range. The check is now **directional**: it hard-errors only
  when `tokenizer > text_vocab_size` (a real out-of-bounds risk) and warns +
  loads when `tokenizer < text_vocab_size`. **No re-download needed** — the turbo
  GGUFs already on disk load again. Reported by @niksedk (Subtitle Edit).

## Upgrade

Drop-in for v0.8.0. chatterbox-turbo users blocked by the v0.8.0 load failure
should upgrade; everything else is unchanged.
