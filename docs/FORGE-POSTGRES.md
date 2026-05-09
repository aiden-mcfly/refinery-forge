# Forge: Postgres / JSON extraction (non-golden path)

**Invariant:** `libpq`, JSON parsers, and legacy `verse-embeddings.bin` float reads exist **only** in refinery-forge — never in refinery-core.

## Alignment with OPP-49 sandbox

The OPP-49 canon pipeline lives under aiden-prime:

- Node exporter: `INTELLIGENCE-CYCLE/bets/active/OPP-49-Jesus-Canon-Root/sandbox/way/canon-export.js`
- C++ exporter: `sandbox/life/canon-export.cpp` → `life/substrate/verse-embeddings.bin`, `canon-index.bin`

**Next implementation step (not shipped in v1 skeleton):**

1. Read `DATABASE_URL` / `AIDEN_DATABASE_URL` (same hopper pattern as `scripts/opp49-restricted-olive-palm-cosine-harness.js`).
2. Export verse text in corpus order (35528 rows) into a single contiguous UTF-8 blob (or rebuild blob from `canon_verses` query).
3. Run the same 7-word window + XXH128 logic as `bitmask_generator.cpp`, dedupe to static markers, emit `marker-index.bin` per `docs/SUBSTRATE-LAYOUT.md` in refinery-core.

## Outputs

- `marker-index.bin` — substrate file consumed by refinery-core
- Optional sidecar manifest (JSON) with xxh64, row counts, git sha — allowed **only** in forge tooling, not on core hot path
