# Forge: Postgres / JSON extraction (non-golden path)

**Invariant:** `libpq`, JSON parsers, and legacy `verse-embeddings.bin` float reads exist **only** in refinery-forge — never in refinery-core.

## Alignment with OPP-49 sandbox

The OPP-49 canon pipeline lives under aiden-prime:

- Node exporter: `INTELLIGENCE-CYCLE/bets/active/OPP-49-Jesus-Canon-Root/sandbox/way/canon-export.js`
- C++ exporter: `sandbox/life/canon-export.cpp` → `life/substrate/verse-embeddings.bin`, `canon-index.bin`

**Current forge surface:**

1. Read `DATABASE_URL` / `AIDEN_DATABASE_URL`.
2. Query a single text column from `canon_verses` in corpus order.
3. Rebuild one contiguous UTF-8 canon blob by joining rows with newline separators.
4. Run the same 7-word window + XXH128 logic as `bitmask_generator.cpp`.
5. Emit `marker-index.bin` only when the resulting marker set satisfies the frozen layout cap.
6. Optionally emit a JSON evidence sidecar with corpus size and hash-distribution stats.

**Default query:**

```sql
SELECT verse_text
FROM canon_verses
WHERE verse_text IS NOT NULL AND verse_text <> ''
ORDER BY id
```

Use `--sql` to supply a stricter or schema-specific extraction query when needed.

## Outputs

- `marker-index.bin` — substrate file consumed by refinery-core
- Optional sidecar manifest (JSON) with xxh64, row counts, git sha — allowed **only** in forge tooling, not on core hot path
