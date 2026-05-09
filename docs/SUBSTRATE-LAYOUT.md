# Refinery marker-index binary layout (v1)

Frozen contract shared by **refinery-core** (read/mmap) and **refinery-forge** (write/extract).

## Endianness

All multi-byte integers are **little-endian**.

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

## Validation

1. `magic == REFINERY_MAGIC` (`0x31465248414C4553` — ASCII `SELAHRF1` on little-endian as typed `uint64_t`/byte order; writers must emit the constant from `substrate_interface.h`).
2. `format_version == 1` (`REFINERY_FORMAT_VERSION_OPP51_1`).
3. `marker_count <= REFINERY_MAX_MARKERS` (35528). Smaller counts allowed for tests.
4. Regions fit within the memory-mapped file and do not overlap.
5. `canon_xxh64` equals **XXH64** over the canon bytes only, seed **0** (classic `XXH64()`, not XXH3).
6. `reserved` in header is zero.

## Markers table

- Entries are **sorted** by `hash_id` ascending (`high64`, then `low64`) for binary search in core.
- Duplicate `hash_id` values must not appear (forge dedupes).

## 32MB index note

Production may mmap a **fixed-size arena** (e.g. 32MB) larger than the file; the reader uses **actual file length** for bounds checks. Extra mapping space is implementation-defined padding, not part of the v1 contract.

## Hash algorithm (window identity)

**Spectroscopy** for a 7-word window: normalize words (see forge README), concatenate with single `0x20` between tokens, digest with **XXH3_128bits()** (xxHash), stored as `Hash128` matching `XXH128_hash_t` layout (`low64`, `high64` per xxHash C API).

## Related

- OPP-51: governed deterministic economics vs probabilistic LLM baselines.
- OPP-49 sandbox legacy: `verse-embeddings.bin` / Postgres — forge is the only component that may link `libpq` or parse JSON witness files.
