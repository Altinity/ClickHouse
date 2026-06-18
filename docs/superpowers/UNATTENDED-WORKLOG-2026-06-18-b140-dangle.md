# Unattended work log — B140-dangle real fix (build-root/precommit redesign)

Started: 2026-06-18 evening (user asleep, I am in charge; no questions, no stops).

**Mission:** Fix B140-dangle properly via the approved build-root/precommit design. Follow the
superpowers flow: design spec → TLA+ (brainstorm/spec/plan/impl/test) → C++ (spec/plan/impl/test) →
12h soak. Record all deferred/debt items in the backlog. Keep this log current.

**Approved design (from the brainstorming dialogue):** protection becomes *reachability from a durable
build root*, not a revocable hint. A build precommits its manifest tree under a build root (references
blobs by content-hash, published before/while blobs upload — local staging only for now); everything
reachable from the build root has in-degree ≥ 1 and GC cannot collect it. The real commit publishes
the table-namespace ref only when the full closure is present (fail-closed), then removes the
precommit. Abandoned precommits (dead builds) are reclaimed via the existing per-server watermark
(repurposed from per-object protection). The fragile `cas_owner` / `protectedByLiveBuild` per-candidate
machinery is DELETED. Four invariants: INV-NO-DANGLE-COMMITTED, INV-BUILDROOT-PROTECTS,
INV-BUILDROOT-RECLAIM, INV-COMMIT-FAILCLOSED.

Root cause that motivated this (report 2026-06-18-ca-b140-dangle-trigger-pinned.md): `reuseBlob` adopt
doesn't transfer `cas_owner`; an adopted blob's protection stays bound to its (retired) byte-writer, so
GC reclaims it under a still-in-flight adopter → publish dangles. Pinned live in the soak via B170
`system.content_addressed_log`.

---

## Timeline

- **T0** — Set up tasks #137–#141, this work log, backlog entries. Soak stopped, cluster down, dangle
  evidence preserved under `utils/ca-soak/tmp/b140_dangle_soak_20260618/`. Starting the design spec.
