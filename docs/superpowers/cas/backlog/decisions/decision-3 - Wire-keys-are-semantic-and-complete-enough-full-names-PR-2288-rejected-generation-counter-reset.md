---
id: decision-3
title: >-
  Wire keys are semantic and complete enough; full names (PR #2288) rejected;
  generation counter reset
date: '2026-09-26 06:35'
status: accepted
---
## Context

The wire keys of manifests, ref logs and checkpoints were revised for readability; PR #2288 proposed full descriptive names.

## Decision

Keys stay semantic and "complete enough" (generation-11 document direction); full names from PR #2288 are rejected; the generation counter is reset; in-memory structs align with the wire vocabulary (2026-09-03).

## Consequences

No further key renames. Any change to key names is now a format-version change (decision-4).
