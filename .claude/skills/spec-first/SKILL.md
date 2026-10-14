---
name: spec-first
description: Use before building any non-trivial Easel feature or behavior change. Writes a short spec in docs/specs/ (problem, behavior, acceptance criteria, test plan) and gets it agreed before code.
---

# Spec first

1. Find the next number: `ls docs/specs`. Copy `docs/specs/_template.md` to `docs/specs/NNNN-short-title.md`.
2. Fill it from what the user asked. Read the relevant code and `docs/architecture.md` first so "Design notes"
   names real files. For behavior, check how XP Paint / jspaint behaves (behavior only, never copy code).
3. Acceptance criteria must be checkable; each should map to an automated test or a numbered manual step.
4. Ask the user only about decisions that change behavior and that you cannot settle from the code or XP precedent.
   Present the spec (short) and wait for "go". Mark `Status: accepted`.
5. Implement in slices; tick the criteria; on completion set `Status: implemented` and add the commit hash.
6. If the change reverses or adds an architectural rule, add an ADR in `docs/decisions/`.

Skip the spec for: typo/one-line fixes, pure refactors with no behavior change (but still run verify-change).
