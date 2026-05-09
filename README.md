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

## Postgres / JSON extraction

See [`docs/FORGE-POSTGRES.md`](docs/FORGE-POSTGRES.md) — `libpq` and JSON are allowed **only** in this repository.

## Binary layout

Same as core: [`docs/SUBSTRATE-LAYOUT.md`](docs/SUBSTRATE-LAYOUT.md) (kept in sync with refinery-core).
