# CAS GC request cuts: measurements and adjudications, 2026-09-30

Reports of the stand runs and code adjudications made for the branch `cas/gc-request-cuts`
(base `antalya-26.6`). Spec: `docs/superpowers/specs/2026-09-29-cas-gc-request-cuts-design.md`.
They describe the trees and binaries named inside each file; paths under `tmp/` and `.superpowers/`
mentioned in them are scratch and may be gone.

| File | Content |
|---|---|
| `01-before-after-10s-rounds.md` | ca-soak S11 / S30 / S46, base vs branch, GC round every 10 s |
| `02-confirmation-final-binary.md` | the same on a quiet machine with a later tree of the branch |
| `03-long-rounds-and-latency.md` | S11 with one round per 300 s, with and without 20 ms per request |
| `04-leak-verdict-rca-and-head-window.md` | the failed leak verdict is a harness race; HEAD requests of the head-window change |
| `05-memo-vs-deposed-leader-adjudication.md` | manifest memo against a deposed leader: blob deletes safe, `.meta` delete jobs reachable |
| `06-list-requests-per-page.md` | LIST requests per logical listing page and their attribution |
| `07-sentinel-probe-flake-rca.md` | boundary mismatch in `probeSentinel` (flaky gtest) |
| `08-janitor-batches-on-long-streams-and-memo-reuse-distance.md` | janitor batch deletes on a dropped table with 7k and 20k ref-stream keys; reuse distance of manifest bodies and a last-K cache simulation |

Related: `docs/superpowers/reports/2026-09-30-cas-30-stale-meta-delete-adjudication.md`,
`docs/superpowers/reports/2026-09-30-soak-s11-dryrun-subset-red-rca.md`.
