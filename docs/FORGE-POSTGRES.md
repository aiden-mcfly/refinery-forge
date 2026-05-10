# Forge: Postgres / JSON extraction (non-golden path)

**Invariant:** `libpq`, JSON parsers, and any external corpus reads exist **only** in refinery-forge — never in refinery-core.

**Tenant:** Selah is the source of truth. Forge resolves `SELAH_DATABASE_URL` only. Aiden was a downstream projection of selah and is no longer a lawful source for the substrate; the AIDEN_DATABASE_URL resolution branch was removed in v0.3 and must not be re-introduced. Per project doctrine: *"A valid object in the wrong tenant is invalid"*; *"Cross-tenant ... use is blocked by default."*

## Selah schema (relevant subset)

Source: `SELAH-ZION-DATABASE-SCHEMA.md`, exported 2026-05-10.

Table `scripture_verses` (primary key `ref`, text):

| Column              | Type          | Notes                          |
|---------------------|---------------|--------------------------------|
| `ref`               | text NOT NULL | primary key, e.g. "John 3:16"  |
| `book`              | text NOT NULL |                                |
| `chapter`           | integer NOT NULL |                             |
| `verse_start`       | integer NOT NULL |                             |
| `verse_end`         | integer NOT NULL | row may span verse_start..end |
| `text_quote`        | text NULL     | the verse text (substrate input) |
| `source_provenance` | text NOT NULL |                                |
| `source_path`       | text NOT NULL |                                |
| `created_at`        | timestamptz NULL | default `now()`             |

Unique index on `(book, chapter, verse_start, verse_end)` — same verse range cannot be duplicated under different refs.

## Default forge query

```sql
SELECT text_quote
FROM scripture_verses
WHERE text_quote IS NOT NULL AND text_quote <> ''
ORDER BY canonical_position
```

`canonical_position` is a dense `1..N` integer anchored to NABRE Catholic 73-book canon order. Populated by the `v1__anchor_canonical_position` migration applied against selah on 2026-05-10. Verified state: 35,543 rows, MIN=1, MAX=35,543, 0 NULLs, 0 gaps, 0 duplicates, `Gen 1:1 → 1`, `Rev 22:21 → 35,543`.

Source of truth for the 73-book NABRE order is the `CANONICAL_BOOKS` array in `aiden-prime/INTELLIGENCE-CYCLE/bets/active/OPP-28-Boundary-Containment/evidence/olive-fiasco/canon-export.js` (per L-11). Refinery does not carry an independent copy of this list; selah's `book_canon_order` table is seeded from it, and `scripture_verses.canonical_position` is derived from `book_canon_order` via the migration. Re-canonization to a different canon = full substrate rebuild; position values are immutable post-ratification within a canon.

If you need to bypass canonical order (e.g. for a stress test using lexical order), override `--sql`:

```sql
SELECT text_quote FROM scripture_verses
WHERE text_quote IS NOT NULL AND text_quote <> ''
ORDER BY ref
```

`ORDER BY ref` is deterministic but **lexical on the text primary key, not canonical scripture order** (per selah authenticity record). Use only when canonical order is intentionally not required.

## Operational flow

1. Read `SELAH_DATABASE_URL` (only — `DATABASE_URL` and `AIDEN_DATABASE_URL` are not honored).
2. Run the SELECT (default above, or `--sql` override).
3. Reject if zero rows or query does not return exactly one text column.
4. Concatenate non-NULL `text_quote` values with `\n` separators to form the canon UTF-8 blob (newline is non-word per `refinery_word_byte`, so windows do not span row boundaries).
5. Run shared 7-word window + XXH3_128 logic (per `substrate_interface.h` algorithm spec).
6. Emit `.bin` only if `marker_count <= REFINERY_MAX_MARKERS` (1,200,000); otherwise SILENCE — print evidence and exit non-zero without writing.
7. Optionally emit a JSON evidence sidecar with row count, word count, window count, unique markers, bucket distribution, and `canon_xxh64`.

## Outputs

- `marker-index.bin` — substrate file consumed by refinery-core (binary contract in `docs/SUBSTRATE-LAYOUT.md`).
- Optional sidecar manifest (JSON) with stats — allowed **only** in forge tooling, not on core hot path.
