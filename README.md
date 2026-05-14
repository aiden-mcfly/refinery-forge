# refinery-forge

Tooling that **extracts** legacy probabilistic substrate (Postgres, JSON, float embeddings) and **freezes** deterministic `.bin` artifacts for **refinery-core**.

## Golden path (no database)

```bash
clang -std=c11 -O2 -c third_party/xxhash/xxhash.c -I third_party/xxhash -o build/xxhash.o
clang++ -std=c++20 -O2 -I include -I third_party/xxhash \
  src/forge/bitmask_generator.cpp src/forge/ledger.cpp \
  src/forge/substrate_emit.cpp src/forge/tank.cpp \
  build/xxhash.o -o build/refinery-forge
./build/refinery-forge --out marker.bin
```

After changing tokenizer or header constants, optionally run `./scripts/verify-golden.sh`
(rebuilds forge and compares `golden.marker.bin` to the default embedded canon; standalone fixture, not a PAT gate requirement). If `golden.marker.bin` is missing at the repo root, copy the canonical file from a sibling checkout: `cp ../refinery-core/test/testdata/golden/golden.marker.bin ./golden.marker.bin`

Options: `--out <path>`, `--text "<utf-8 canon>"` (must yield at least seven tokens per
`refinery_word_byte()` in `include/refinery/substrate_interface.h`).

Protected-path guard: if an operator manifest marks the target path as active/live, forge
refuses to overwrite it and exits with code `4` after printing `L-F-02 VIOLATION...` to
stderr. If no manifest is found, forge emits a single `L-F-02: no active-substrate manifest
found; protected-path check skipped` warning and proceeds. Protection is deploy-opt-in via
`REFINERY_SUBSTRATE_MANIFEST`, `./.active-substrate-paths`, `./refinery-core/.active-substrate-paths`,
or the executable-relative sibling fallback.

## Evidence ledger and entropy tank

State-changing operations require an evidence ledger path and an entropy tank path.
Use `REFINERY_EVIDENCE_LEDGER` / `REFINERY_ENTROPY_TANK`, or copy the core repo
examples to `refinery-core/.evidence-ledger-path` and `refinery-core/.entropy-tank-path`.
The ledger is deterministic binary evidence; the tank is forge-owned producer storage.
Core never reads either artifact at runtime. **Tank v2** (`SELAHTN2`, see
`tank_interface.h`) stores the witnessed signal text per entry; for a **single**
`UN_REFUTED` entry, `--promote-tank-to-substrate` routes that text through the same
`refinery_emit_substrate_from_text` path as `--text`, so the promoted `.bin` is
**byte-identical** to `refinery-forge --text "<same canon>" --out …` and is
**ingress-spectroscopy-reachable** (kernel HIT). **v1** legacy tanks (`SELAHTNK`)
refuse new witness/refute/reject/evict (**exit 9**); promotion falls back to
identity-only mode with a stderr warning. Multi-entry v2 promotion is **exit 10**
until an aggregation rule is ratified.

```bash
export REFINERY_EVIDENCE_LEDGER=~/.config/refinery/evidence-ledger
export REFINERY_ENTROPY_TANK=~/.config/refinery/entropy-tank

./build/refinery-forge --witness "candidate signal"
SUBJECT=$(./build/refinery-forge --hash-text "candidate signal")
./build/refinery-forge --reject "$SUBJECT"
REFINERY_REFUTER_ID=refuter-A ./build/refinery-forge --refute "$SUBJECT" --evidence "challenge one"
REFINERY_REFUTER_ID=refuter-B ./build/refinery-forge --refute "$SUBJECT" --evidence "challenge two"
./build/refinery-forge --promote-tank-to-substrate "$REFINERY_ENTROPY_TANK" promoted.marker.bin
```

Return codes: `4` protected path refusal; `5` evidence-ledger failure; `6` tank failure;
`7` invalid refutation/eviction/evidence request; `8` promotion blocked (e.g. `PENDING`
or insufficient refutation coverage); `9` v1 tank — state ops refused (promote may warn
and use identity-only path); `10` multi-entry v2 tank promotion not specified; `11` tank
format/header/canon validation failure; `12` torn or malformed ledger (non-zero size
not a multiple of 88 bytes).

## Postgres extraction and evidence

When built with `libpq`, Forge can extract the canon directly from selah's `scripture_verses`
table and emit distribution evidence before writing a lawful substrate. Selah is the source
of truth — forge resolves `SELAH_DATABASE_URL` **only** (no generic `DATABASE_URL`, no
`AIDEN_DATABASE_URL`). Per project doctrine: *"A valid object in the wrong tenant is invalid"*.

Build with libpq linked:

```bash
clang++ -std=c++20 -O2 -Wall -Wextra \
  -DREFINERY_HAVE_LIBPQ=1 \
  -I include -I third_party/xxhash \
  -I /opt/homebrew/opt/libpq/include \
  src/forge/bitmask_generator.cpp src/forge/ledger.cpp \
  src/forge/substrate_emit.cpp src/forge/tank.cpp build/xxhash.o \
  -L /opt/homebrew/opt/libpq/lib -lpq \
  -o build/refinery-forge
```

Run (read SELAH_DATABASE_URL from a chmod-600 file, never inline):

```bash
set -a; source ~/.config/selah/db.env; set +a   # contents: SELAH_DATABASE_URL=...
./build/refinery-forge --from-postgres --stats
unset SELAH_DATABASE_URL
```

Without `--out`, `--from-postgres` defaults to **`canon.marker.bin`** in the working directory.
Override with `--out path` for a custom location. Add `--limit N` for a partial corpus,
`--stats-out path.json` for JSON stats.

Default SQL:

```sql
SELECT text_quote
FROM scripture_verses
WHERE text_quote IS NOT NULL AND text_quote <> ''
ORDER BY canonical_position
```

`canonical_position` is the canonical Selah ordering and is the default substrate read path.
If you intentionally want a lexical stress-test ordering instead, override with `--sql`:

```sql
SELECT text_quote
FROM scripture_verses
WHERE text_quote IS NOT NULL AND text_quote <> ''
ORDER BY ref
```

The query must return exactly one text column. If the generated marker set exceeds
`REFINERY_MAX_MARKERS`, Forge refuses to emit an invalid `.bin` and exits with `SILENCE`
semantics after reporting the evidence.

See [`docs/FORGE-POSTGRES.md`](docs/FORGE-POSTGRES.md) for the selah schema and override patterns.

## Postgres / JSON extraction

See [`docs/FORGE-POSTGRES.md`](docs/FORGE-POSTGRES.md) — `libpq` and JSON are allowed **only** in this repository.

## Binary layout

Same as core: [`docs/SUBSTRATE-LAYOUT.md`](docs/SUBSTRATE-LAYOUT.md) (kept in sync with refinery-core).
