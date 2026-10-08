---
description: 'ca-soak scenario results for the CAS semantic wire-key cut, with the triage of every failure.'
sidebar_label: 'Wire keys phase 3 soak'
sidebar_position: 97
slug: /superpowers/cas/wire-keys-phase3-soak
title: 'CAS wire keys phase 3 — the soak scenarios'
doc_type: 'reference'
---

# Wire-keys phase 3 — the soak scenarios {#wire-keys-phase-3-the-soak-scenarios}

The lanes prove the cut through a running server. The soak scenarios add two things the lanes cannot:
adversarial object-level injection, and cards that decode persisted bodies in their own code. That
second property is the whole reason a soak pass belongs in this phase — it is the only place outside
the unit battery where a wrong key spelling fails loudly instead of round-tripping silently.

All runs are at the freeze commit `e9f1c3d867c`. The soak compose mounts `build/programs/clickhouse`,
the same binary the lanes ran against, whose provenance was established by content rather than by the
version banner (see the lanes document). No commit between the freeze and these runs touches `src/`
or `tests/`.

## What the soak can and cannot convict {#what-the-soak-can-and-cannot-convict}

The card-to-format coverage table settles this precisely, and the answer is narrower than a green
suite suggests. Exactly two cards decode real CAS object bytes in their own Python — `S38` and `S43`,
both for `cas_ref_log` only, and S43 literally imports S38's helpers. Everything else asserts through
SQL, system tables, ProfileEvents, or the real `cas-fsck` binary, none of which can distinguish a
renamed key from a correct one as long as the server is self-consistent. One more reader sits outside
the cards: the `T8/S44` discrimination script parses `cas_ref_catalog` rows with its own regex.

So the soak's wire-key tripwire surface is three readers over two formats. That is what a green soak
proves about the cut. The rest of the suite proves the system still works, which is worth having and
is a different claim.

## The body-touching runs {#the-body-touching-runs}

These ran first, because they are the ones that can fail on a key spelling.

**S43 — PASS.** Builds and injects a survivor transaction into a freshly wiped pool using S38's
decode/encode helpers, and the pool refuses it.

**S44 — PASS**, followed by the `T8/S44` discrimination script, whose sequence `RUN_HISTORY.md` did
not record and which was therefore reconstructed and written down before running: one disposable S44
pass, then the script with no intervening `scenarios.run` invocation, because the runner's default
reset would destroy the dead incarnations the script looks for.

The script read the catalog: `2 total entries` immediately after the scenario, `0` after the GC
fixpoint. That is the result this task wanted. Its reader matches
`{"kind":"entry","ns":..,"state":..,"life":..}` against the raw `cas_ref_catalog` body, and phase 2
gave it a fail-loud branch that raises when entry rows are present but none match the pattern — so a
stale spelling could not have been mistaken for an empty catalog. It parsed two rows, and the branch
did not fire. The script also found no dead incarnations at all, consistent with `RUN_HISTORY`'s note
that the original S44 drain contradiction was resolved and was an Atomic drop-delay rather than a CAS
fault.

**S38 — INCONCLUSIVE, then PASS after a card fix.** This is the run that mattered most and the one
that was quietly proving nothing.

S38 requires exactly one direct child under `cas/ns/stream/` before it will inject. It found two, so
the injection — the entire point of the card — was skipped, while its other ten verdicts passed and
the scenario reported INCONCLUSIVE rather than failing.

The diagnosis came from a reproduction run's live pool rather than from the run artifacts, which
carry no listing. Two children, 51 and 114 objects. The catalog explains them:

```
{"kind":"entry","ns":"ca_soak_ch1/store/42a/42aad325-…@cas@","state":"live","life":"72374add…"}
{"kind":"entry","ns":"ca_soak_ch2/store/f5d/f5dfc269-…@cas@","state":"live","life":"834575…"}
```

Both `live`; no dead incarnation, so not a rebirth defect. `system.tables` shows the same replicated
table carrying different UUIDs on the two nodes, and namespaces are UUID-scoped, so a two-replica
compose necessarily has two live namespaces. One logical table is not one namespace. The card's
premise was wrong for its own topology, and no key rename can change a count of storage prefixes.

The fix makes discovery read the catalog through the server and select the single live entry whose
namespace carries the killed node's table UUID — the node whose epoch the successor sealed, which is
the one the injection targets. That follows the card's own rule that the catalog is the only
namespace-to-life mapping, instead of violating it by counting keys, and it carries the same
fail-loud branch the T8 script has.

With that, S38 reports 19 of 19 and the injection chain runs for the first time. It GETs a real
`cas_ref_log` object, zstd-decompresses it, rewrites its meta line, and PUTs it back above the epoch
seal. The store refuses a straggler's conditional create at the sealed id with HTTP 412, the seal
stays byte-for-byte identical at 150 bytes, the table checksum is unchanged by the injected log, and
a full re-recovery still ignores it. That is post-cut evidence for `cas_ref_log` that the INCONCLUSIVE
run was not producing.

## The breadth pass {#the-breadth-pass}

The card-to-format table was built first and chose the set: `{S16, S39, S45}` covers every format any
card reaches, with `S41` added because it is the deepest exercise of `cas_blob_meta`'s own state
machine rather than of its mere presence.

**S39 — PASS.** The only card that drives `cas_mount_lease` through a real fault-injected loss and
remount rather than passive steady-state renewal.

**S16 — FAIL, then PASS after a card fix.** It required `blob_reuse_resurrect` to fire across its
drop, GC-condemn, re-insert cycle. That event has had no emitter since `907c3b5ce7d` ("Publish CAS
blobs after mandatory `HEAD`"), which is an ancestor of the pre-cut baseline: once publication became
unconditional after a mandatory `HEAD`, a writer stopped splitting reuse into adopt versus resurrect
and always re-uploads from source. The enum member and its string survive with nothing raising them,
so the verdict could not pass at any scale, and no key rename can delete an emitter.

The GC log rules out the alternative reading. The card's own failure text offers "either GC did not
condemn before the re-insert or the resurrect event failed to fire", and rounds 2, 4, 6 and 8 each
record `entries_condemned=5` — the precondition held every cycle. `blob_delete=0` is explained by
`objects_spared` in the same rounds: the next cycle re-referenced the objects.

The verdict now requires that reuse happened at all, which is what the cycle is there to establish,
and passes with `blob_reuse_adopt=139`. What that loses — the only positive check that a condemned
token forces a re-upload — is recorded in the card and in the backlog.

**S41 — did not run.** Both attempts died at bring-up after the full 300-second health timeout, with
zero verdicts, because of how it was invoked and not because of anything it tests. `wait_healthy`
polls `localhost:8123` by the default replica convention while `docker-compose-s41.yml` publishes its
single node on 18123; the card's own docstring requires an isolated `ca-s41` project driven with
`--no-reset` and the framework pointed at it by environment.

**S45 — FAIL, and the most instructive of the four.** `cas-drop-member` reported
`namespaces_removed=0` deterministically, including on the seed that passed on 2026-08-03, with fsck
showing an empty pool and the tool exiting cleanly with `slot_removed=true`.

The card could not say why, so it was taught to sample the catalog on both sides of the lease-lapse
wait. That settled it in one run:

```
after_kill        = {"ca_soak_ch1:removing": 3, "ca_soak_ch2:removing": 3}
after_drop_member = {}
```

The premise held — the victim's three hidden `Removing` rows existed. By the time the tool returned,
all six rows were gone, **including the survivor's own three**, which `cas-drop-member` never touches
because it decommissions only the victim. The sweeper was therefore the survivor's GC, which is the
pool's GC leader and retires `Removing` namespaces pool-wide during exactly the wait the tool forces
by refusing to run while the victim's lease is alive. The card's "kill the victim before its own GC
settles" guard only ever covered the victim's GC.

That exonerates the tool, which correctly had nothing to sweep, and the cut, since a rename cannot
make GC retire rows. The verdict is now split into the precondition (the hidden rows exist at the
kill, so a later zero can never quietly be a setup failure) and the invariant the scenario actually
protects (no victim rows survive the decommission). The loss — that the tool's own sweep path goes
unexercised whenever GC wins the race — is filed with a fix direction.

**S41 — PASS, once invoked as its own docstring requires.** Brought up as an isolated `ca-s41`
compose project and driven with `--no-reset` and the framework pointed at port 18123, it came up in
one second and passed every verdict. It is also the most valuable single card for this campaign after
S38, because it checks the blob publication protocol by request class rather than by outcome: a fresh
wide insert shows `HEAD=3155 (1497 hit / 1658 miss)` with 1658 body publications, and the duplicate
of the same data shows `HEAD=3155 (3155 hit / 0 miss)` with zero publications and 3155 avoided. That
is `cas_blob_meta`'s state machine exercised end to end, which is why the card was added to the
covering set.

Its cost attribution — `s3_network 86%`, `dedup_head_gate 12%`, `ledger_manifest 1%` — and its
CA-over-plain wall ratios of 1.26 small and 1.61 wide are recorded as context, not as a claim about
this campaign: the card states explicitly that these are target-only observations and not a
code-version delta.

## Results {#results}

| № | Сценарий (что доказывает) | Результат | Найденные артефакты | Планируемый фикс |
|---|---|---|---|---|
| S38 | unclean handover: the epoch seal makes a late predecessor PUT lose | PASS (19/19, was INCONCLUSIVE 10/11) | card premise: one-namespace assumption in a two-replica pool | done — discovery now maps the killed node's table UUID through the catalog |
| S43 | same-uuid pool recreation refuses a residual survivor write | PASS | pre-existing quiescence note (`SYSTEM SYNC REPLICA` on a deliberately non-replicated table), identical to the 2026-07-29 row | none — not new, not this campaign |
| S44 + T8 | rebirth with concurrent namespace-file readers; catalog discrimination | PASS | none | — |
| S39 | mount-renewal retries and fail-closed recovery under degraded S3 | PASS | none | — |
| S16 | hot content cycle with GC | PASS (was FAIL) | stale assertion on `blob_reuse_resurrect`, an event with no emitter since `907c3b5ce7d` | done — asserts reuse; loss filed as `[blob-reuse-resurrect-no-emitter]` |
| S45 | decommission a victim member with hidden Removing catalog entries | PASS (was FAIL) | card asserted the outcome of a GC race it cannot win | done — split into precondition and invariant; loss filed as `[s45-drop-member-sweep-untested]` |
| S41 | CAS write-path performance and blob publication protocol | PASS (was FAIL) | operator error: plain invocation against a compose that publishes 18123 | done — run isolated with `--no-reset` and the documented env |

Three cards needed fixes and all three were stale assertions; none was a cut defect. Each was
attributed by evidence rather than by elimination: S38 by the live catalog and `system.tables` UUIDs,
S16 by `git log -S` placing the emitter's removal before the pre-cut baseline, S45 by sampling the
catalog on both sides of the lease-lapse wait. The two coverage losses the fixes cost are filed with
fix directions rather than absorbed silently.
