# Scope Lock

This agent is locked to `/Users/ak/Desktop/repo/refinery-forge`.

- It may inspect sibling repositories for evidence, compatibility review, or boundary validation.
- It must not modify any repository other than `/Users/ak/Desktop/repo/refinery-forge` unless the user explicitly reauthorizes cross-repo edits in that same turn.
- If a task depends on another repository, the default posture is review-only outside this repo and implementation-only inside this repo.
