# refinery-forge

Tooling that **extracts** legacy probabilistic substrate (Postgres, JSON, float embeddings) and **freezes** deterministic `.bin` artifacts for **refinery-core**.

## Golden path (no database)

```bash
clang -std=c11 -O2 -c third_party/xxhash/xxhash.c -I third_party/xxhash -o build/xxhash.o
clang++ -std=c++20 -O2 -I include -I third_party/xxhash \
  src/forge/bitmask_generator.cpp build/xxhash.o -o build/refinery-forge
./build/refinery-forge --out marker.bin
```

Options: `--out <path>`, `--text "<utf-8 canon>"` (must contain at least seven whitespace-separated words).

## Postgres extraction and evidence

When built with `libpq`, Forge can extract a corpus directly from `canon_verses` and emit
distribution evidence before writing a lawful substrate:

```bash
cmake -S . -B build
cmake --build build
DATABASE_URL=postgres://... ./build/refinery-forge \
  --from-postgres \
  --limit 512 \
  --out golden-set.marker.bin \
  --stats \
  --stats-out golden-set.stats.json
```

Default SQL:

```sql
SELECT verse_text
FROM canon_verses
WHERE verse_text IS NOT NULL AND verse_text <> ''
ORDER BY id
```

Use `--sql "<query>"` to override the source query. The query must return exactly one text
column. If the generated marker set exceeds `REFINERY_MAX_MARKERS`, Forge refuses to emit an
invalid `.bin` and exits with `SILENCE` semantics after reporting the evidence.

## Postgres / JSON extraction

See [`docs/FORGE-POSTGRES.md`](docs/FORGE-POSTGRES.md) — `libpq` and JSON are allowed **only** in this repository.

## Binary layout

Same as core: [`docs/SUBSTRATE-LAYOUT.md`](docs/SUBSTRATE-LAYOUT.md) (kept in sync with refinery-core).
