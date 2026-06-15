# Unattended night — CA op-count + soak fixes (2026-06-15 → )

**Mandate (verbatim intent):** finish spec+plan and **implement #1 (head-after-put → drop the follow-up HEAD) to completion** with tests+reviews. **#2 GC single-leader/retire-contention → backlog only.** **#3 RustFS overwrite-leak mitigation, #4 root_shards congestion (widen fanout), #5 fsck per-LIST timeout+progress → brainstorm/spec/plan/implement** likewise. Don't ask, don't stop; defer→backlog. Then a **12h soak with hourly healthchecks + reports**.

Branch: `cas-mergetree-poc`. Evidence base: `docs/superpowers/reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md`; backlog `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (B157/B158/B159).

## Status board
- [ ] #1 head-after-put → PUT-response ETag (spec → plan → implement → tests → review)
- [ ] #5 fsck per-LIST timeout + progress (brainstorm → spec → plan → implement → tests → review)
- [ ] #4 root_shards widen fanout (brainstorm → spec → plan → implement → tests → review)
- [ ] #3 RustFS overwrite-leak mitigation (brainstorm → spec → plan → implement → tests → review)
- [ ] #2 GC single-leader / retire-contention → BACKLOG ONLY
- [ ] 12h soak with hourly reports

## Log
(appended chronologically below)

### Start
Design for #1 approved by user (head-after-put → return the PUT/CompleteMultipartUpload object ETag as the WCreate token; drop the post-write HEAD; dedup-reuse `observeAndAdmit` HEAD untouched). Model-checked against `CaIncarnationCore.tla` (`WCreate` records `nextTok`, never a HEAD; `SabotageNoReobserve` proves the gate token is load-bearing → can't drop the token, only the HEAD). Beginning spec.
