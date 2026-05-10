# Refinery marker-index binary layout (v1)

Frozen contract shared by **refinery-core** (read/mmap) and **refinery-forge** (write/extract). Both repos hold byte-identical copies of `include/refinery/substrate_interface.h`; this doc describes the on-disk format that header defines.

## Endianness

All multi-byte **integer** fields are stored in **little-endian** byte order. The header asserts at compile time that the host is LE; on a big-endian host the build fails by design (a v2 contract would be a deliberate spec change).

The `magic` field is **not** an integer — it is an explicit 8-byte ASCII array (`"SELAHRF1"`). Its on-wire bytes are unambiguous regardless of host byte order.

## File structure

| Region | Offset | Size |
|--------|--------|------|
| `SubstrateHeader` | 0 | 64 bytes |
| `Marker[]` | `header.markers_offset` | `header.markers_bytes` |
| Canon UTF-8 blob | `header.canon_offset` | `header.canon_size` |

**v1 writer rule:** `markers_offset` is **64** (immediately after header).
`canon_offset` **must** satisfy:

`canon_offset >= markers_offset + markers_bytes`, aligned to **8-byte** boundary (pad with zeros between markers region and canon if needed).

**Total file size:** `canon_offset + canon_size` (padding after canon is forbidden for v1).

## Validation (kernel `validate_mapped`, in order)

1. `memcmp(header->magic, "SELAHRF1", 8) == 0` — integrity seal.
2. `format_version == 1` (`REFINERY_FORMAT_VERSION_OPP51_1`).
3. `marker_count <= REFINERY_MAX_MARKERS` (1,200,000). Smaller counts allowed for tests.
4. `markers_offset >= sizeof(SubstrateHeader)`; `markers_offset` and `canon_offset` 8-byte aligned.
5. `markers_bytes == marker_count * sizeof(Marker)`.
6. Regions fit within the memory-mapped file and do not overlap (`canon_offset >= markers_offset + markers_bytes`).
7. `reserved` in header is zero.
8. `canon_xxh64` equals **XXH64** over the canon bytes only, seed **0** (classic `XXH64()`, not XXH3).
9. **L-K-05 sort invariant:** for every adjacent pair of markers, the prior marker's `(high64, low64)` is strictly less than the next. Unsorted or duplicate markers return `REFINERY_ERR_SORT`. Binary search is unsafe on unsorted input; the kernel refuses rather than serve wrong results.

## Markers table

- Entries are **sorted strictly ascending** by `hash_id` `(high64, low64)` for binary search in core.
- Duplicate `hash_id` values must not appear (forge dedupes by `Hash128`; kernel rejects on load).

## Tokenization (shared via `refinery_word_byte` in the header)

- Word class = ASCII `[0-9A-Za-z]`. All other bytes terminate a word.
- Case is **preserved** (no case folding in v1). `"Word"` and `"word"` are distinct tokens.
- Per-token cap: `REFINERY_MAX_WORD_BYTES` (128). A single token at or above the cap collapses to SILENCE (kernel returns `REFINERY_SPECTROSCOPY_ERR_WORD_TOO_LONG`; forge refuses to emit a `.bin`).

## Window hash algorithm (kernel + forge MUST agree byte-for-byte)

For a window of `REFINERY_WINDOW_WORDS = 7` consecutive tokens:

1. Build a flattened buffer:
   `token[i] ++ 0x20 ++ token[i+1] ++ 0x20 ++ ... ++ token[i+6]`
   — exactly 6 separator bytes between 7 tokens; no leading or trailing separator.
2. Hash that buffer with `XXH3_128bits()`. Store the digest as `Hash128` (`low64`, `high64` per the xxHash C API).
3. `marker.canon_offset` = byte offset of `token[i]` in the original canon.
   `marker.span_bytes` = `end_of_token[i+6] − start_of_token[i]` (in original canon bytes; includes any non-word bytes lying between the 7 tokens).

Any divergence on this construction is a v1 contract violation. The shared header constants and `refinery_word_byte()` are the only lawful source.

## 32MB index note

Production may mmap a fixed-size arena (e.g. 32MB) larger than the file; the reader uses **actual file length** for bounds checks. Extra mapping space is implementation-defined padding, not part of the v1 contract.

## Doc/code parity enforcement

The two copies of `include/refinery/substrate_interface.h` (in `refinery-core/` and `refinery-forge/`) MUST be byte-identical. Recommended pre-merge check:

```bash
diff -u refinery-core/include/refinery/substrate_interface.h \
        refinery-forge/include/refinery/substrate_interface.h
```

Non-empty diff blocks the merge. This doc and the header are sibling artifacts; updating one without the other is doc drift (D-5).

## Related

- OPP-51: governed deterministic economics vs probabilistic LLM baselines.
- OPP-49 sandbox legacy: `verse-embeddings.bin` / Postgres — forge is the only component that may link `libpq` or parse JSON witness files.
