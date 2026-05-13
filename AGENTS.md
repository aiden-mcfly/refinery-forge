# Scope Lock

This agent is locked to `/Users/ak/Desktop/repo/refinery-forge`.

- It may inspect sibling repositories for evidence, compatibility review, or boundary validation.
- It must not modify any repository other than `/Users/ak/Desktop/repo/refinery-forge` unless the user explicitly reauthorizes cross-repo edits in that same turn.
- If a task depends on another repository, the default posture is review-only outside this repo and implementation-only inside this repo.

## OPP-51 PAT (product acceptance)

PAT mechanical checks that touch the gate live in sibling **`refinery-core`** (`scripts/refinery-gate.sh`); law and evidence rules are in that repo’s **`LAWS.md`** under **Executable gate** and **PAT evidence (OPP-51)**. For PAT-A3, `scripts/verify-golden.sh` expects **`golden.marker.bin`** at this repo’s **root** — canonical bytes are in `refinery-core/test/testdata/golden/golden.marker.bin` when repos are checked out side by side (see README). Editing `refinery-core` in the same task requires an explicit same-turn reauthorization beyond this scope lock.
