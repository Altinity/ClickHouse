---
description: 'Settled verdict on S3 LIST trust for CAS GC: what the 2026-07-26 RustFS incident proved and did not prove, which lie classes are legal on contract-compliant S3, which verbs stayed honest, and why a one-shot LIST-consistency probe cannot certify a backend. Written to stop re-litigating the same argument in every session.'
sidebar_label: 'LIST trust verdict'
sidebar_position: 20260803
slug: /superpowers/cas/list-trust-verdict
title: 'CAS: S3 LIST trust — settled verdict'
doc_type: 'reference'
---

# S3 LIST trust — settled verdict (2026-08-03) {#list-trust-verdict}

This document exists because the same argument has been re-derived from scratch in multiple
sessions. The facts below are ESTABLISHED — by measurement, by contract text, or by review — and are
not open for re-litigation. A session that wants to overturn one of them needs NEW evidence, not a
fresh chain of reasoning over the old evidence.

## 1. What the 2026-07-26 incident actually was {#incident}

On RustFS, four minutes into a soak, an enumeration of a ref-log stream returned `seq 0x1430e` while
omitting `0x1430c` and `0x1430d`. Established by measurement
(`docs/superpowers/reports/2026-07-26-list-incompleteness-investigation.md`, proof bundle
`.../2026-07-26-list-incompleteness-proof/`, `BACKLOG.md` anchors `{#probe-a-caught-live}`,
`{#probe-a-answered}`, `{#probe-a-proven-by-measurement}`):

- Both omitted objects were **durable for ~19 seconds** before the anomaly fired: uploads at
  16:47:19 (2.2 ms apart, strict id order per `system.blob_storage_log`), anomaly at 16:47:38.
- A `HEAD` of each omitted key **at the moment of the disagreement** confirmed `present`.
- Across 65,263 ref-log uploads in that namespace, partitioned by epoch, **zero** were out of order.
- All four alternative explanations (concurrent delete, stale-epoch minting, out-of-order appends, a
  late `Unresolved` PUT) were excluded on evidence, at three source levels.

**Classification (the part that keeps getting re-argued): the TAIL WAS CORRECT.** The lie was
omission of predecessors *below* the returned frontier — not tail under-reporting.

## 2. Which lie classes are legal on contract-compliant S3 {#legal-classes}

- **Predecessor omission below a correct tail is LEGAL** on any S3, including AWS, for writes
  concurrent with a paginated enumeration: the read-after-write guarantee covers lists *started
  after* a PUT completes; there is **no snapshot contract** for keys landing mid-walk (a key landing
  behind the pagination marker is invisible; a later key ahead of it is visible). The observed shape
  is therefore reachable on a fully compliant store — which is why arithmetic middle intake is
  mandatory **everywhere**, not a RustFS workaround.
- **The observed instance was nevertheless a contract violation** by AWS-style semantics: the
  omitted keys were durable ~19 s before the firing enumeration began. RustFS broke the contract; a
  compliant store could produce the same *shape* only under concurrency, not at 19 s.
- **Tail under-reporting of a completed write is ILLEGAL** on a compliant store (that is exactly the
  read-after-write clause). This is the one and only LIST property that a per-backend trust mode may
  ever lean on (`BACKLOG.md {#gc-frontier-one-list}` lever 1).
- **Not proven for AWS**: no violation of any kind has been observed on AWS S3. The incident is
  evidence about RustFS, and about what enumeration concurrency legally permits — not about AWS.

## 3. Which verbs stayed honest (trusted verbs) {#trusted-verbs}

During the incident, **exact-key `GET`/`HEAD` told the truth while LIST lied** (the `HEAD`-at-firing
-time is the proof), and conditional writes are trusted by construction — a store cannot answer
`putIfAbsent` with `Created` over an existing key without destroying its own data. The design's
trust chain is therefore: catalog cut (universe) → `_ckpt` (acked frontier) → exact `GET`s (records)
→ conditional writes (fences); LIST is a hint (work bound, witness set, genesis hint) whose lie in
any direction can only *delay* reclamation, never *authorize* destruction.

The asymmetry that decides everything: a **stale-but-honest** read is a true cut from the past —
errors from it fall toward retention/delay and are closed forward by the temporal arms
(round-paced floor, exact-token delete, delete-site in-degree re-read, `Gc/CasBlobInDegree.cpp`).
A **fabricated cut** (omission under a seal) corresponds to no point in time; folding it and sealing
above the hidden records authorizes destruction and is unrecoverable. Delay-shaped errors are
acceptable; authorization-shaped errors are not.

## 4. Why a one-shot LIST-consistency probe cannot certify a backend {#probe-limits}

Established empirically: **three hammer runs, ~19M listed keys, ZERO holes** — then the violation
fired live within four minutes of an ordinary soak. The failure is rare, load- and timing-dependent,
and not reproducible on demand. Consequences (settled):

- A mount-time synthetic probe (PUT a few keys, LIST, compare) will pass on virtually every store,
  including RustFS. **Absence of probe evidence is not a passport.** Any "certification" design
  built on a one-shot probe is unsound.
- A passport can therefore only be one of:
  1. **Operator attestation per bucket** (config: "this bucket is AWS/GCS with documented strong
     LIST"), i.e. a human accepts the residual risk for that bucket — the trust mode is a runtime
     property of the *bucket*, never of a backend *type* string;
  2. **Continuous verification with auto-revocation**: keep trusting, but keep a background
     sample of exact-key probes (or `_ckpt`-vs-tail cross-checks) flowing; the first observed lie
     permanently demotes the bucket to probe-always and records an anomaly. Note honestly: sampling
     bounds *detection time*, not the *exposure* of unsampled namespaces inside the window — this
     is a risk-acceptance knob, not a proof;
  3. Both combined (attestation to enable, verification to revoke). Fail-close default is
     probe-always; any doubt — probe-always.
- RustFS **fails** any such passport today, by measurement.

## 5. What this means for optimization work {#optimization-scope}

`BACKLOG.md {#gc-frontier-one-list}` is the plan. Scope limits that follow from §2 and are NOT
tunable per backend: arithmetic middle intake everywhere; frontier probes survive for held
namespaces and namespaces with no listed logs; `tail < cursor` keeps feeding the store-quality
detector. The only backend-gradeable claim is tail-as-frontier-proof at `tail == cursor`, under a
§4-shaped passport. The parallel-walk lever is trust-neutral and needs none of this.
