---
description: 'CAS scenario-suite stabilization project status: dev + prod-scale campaign results, artifact classes, landed fixes, open items, and newly commissioned scenarios (MOVE PART/PARTITION, multi-disk)'
sidebar_label: 'Scenarios Stabilization Status'
sidebar_position: 20260715
slug: /superpowers/reports/scenarios-stabilization-status-2026-07-13
title: 'CAS Scenarios Stabilization — Project Status (2026-07-13)'
doc_type: 'reference'
---

# CAS Scenarios Stabilization — Project Status (2026-07-13) {#scenarios-stabilization-status}

Branch `cas-gc-rebuild`, binary at `2174a893f33` (+ harness `a75dd75ab6d`/`7a22bc5b700`).
Sources: `.superpowers/sdd/task4-scenarios-report.md`, `task4-rerun-report.md`,
`task5-scenarios-report.md` (partial), `tmp/task4_progress.log`, `tmp/task5_progress.log`,
`utils/ca-soak/scenarios/RUN_HISTORY.md`.

## Campaign results {#campaigns}

**Dev campaign (task 4): 35/35 run, zero skips.** Final state after fixes and re-runs: 22
effectively clean (20 first-pass + S06/S07 after fixes), S13 product regression FOUND AND FIXED
(verified: `precommit_reclaim=18`, 0 orphans), S31 known dryrun-tool gap, 11 dev-scale
inconclusives (5 of them resolved at prod scale next day, see below).

**Prod-scale campaign (task 5): PAUSED BY USER after 7 scenarios** (priority-first order achieved
its purpose early — all 5 formerly-dev-inconclusive priority verdicts got definitive resolutions):

| № | Масштаб | Результат | Найденные артефакты | Планируемый фикс |
|---|---|---|---|---|
| S01 | full, блоб 8→2 GiB | **PASS 13/13**; RSS-вердикт разрешён | RUSTFS-MULTIPART-TIMEOUT на 8 GiB (5× tmp-чурн) | инфра-класс записан; rustfs-потолок ≈2 GiB |
| S02 | full, 4 GiB | **PASS** (второй инсерт — только метаданные) | — | — |
| S03 | full | FAIL-INFRA ×2 (попытка-2 прервана паузой) | RUSTFS-PUT-DEGRADATION при ~6.4 GB устойчивой записи; `stageManifest` UNCERTAIN после 90 s конверта (availfix работал как задуман) | re-run на реальном S3 или пониженный прифилл |
| S07 | full, 20 000 колонок | INCONCL 8/9; cap-trip разрешён-как-недостижимый (1 048 576 vs ~40k) | вердикту нужен cap-lowering hook | конфиг-хук (харнесс) |
| S08 | ci, 20 000 частей | INCONCL 13/14; **ref_objects=4 на 20k вставок** | HARNESS-STALE-COUNTER: `CasRootCas` мёртв на insert-пути | обновить счётчики карты после §0 |
| S21 | ci, ~14.7 GB | INCONCL; root-decode-амортизация PASS (122 GET « 1800) | HARNESS-NEEDS-COLD-READ: вердикту нужен холодный читатель, не объём | карта: добавить cold-read фазу |
| S29 | full, 20M строк | RSS ограничен (630 MB / 2.8 GB part) | HARNESS-PREMISE-GAP: spill-to-blob делает пол атрибуции недостижимым | карта: пересмотреть floor |

**Product findings across both campaigns: ONE (S13, fixed same day).** Every other failure was
harness, infra, or scale-of-verdict.

## Artifact classes and their state {#artifact-classes}

| Класс | Сценарии | Состояние |
|---|---|---|
| rustfs fd-exhaustion (`nofile=1024`) | S06, S07, фон S12 | **FIXED** `a75dd75ab6d` (ulimits 262144 во всех 6 compose), verified in-container |
| Oracle race (`assert_replicas_agree` без ожидания) | S06, S07 | **FIXED** `7a22bc5b700` (конвергентный ре-полл; настоящая дивергенция падает) |
| GC-log flush window | S03-S05, S11 (dev) | **FIXED** `a75dd75ab6d` (`SYSTEM FLUSH LOGS` + bounded retry в общем хелпере) |
| Stale host log dirs | S01 (dev, boot) | **FIXED** `a75dd75ab6d` (archive-then-recreate) |
| S13 dangling-precommit regression | S13 | **FIXED** `2174a893f33` (retry-until-clean sweep + reclaim events), re-run 12/12 |
| RUSTFS-MULTIPART-TIMEOUT | S01@8GiB | OPEN (infra): rustfs `.rustfs.sys/tmp` чурн ~5× на multipart; потолок ≈2 GiB блоба |
| RUSTFS-PUT-DEGRADATION | S03@full | OPEN (infra): деградация PUT при ~6.4 GB устойчивой записи; full-scale S03/S04 требуют реального S3 |
| Cap-lowering hook | S07 | OPEN (harness): конфиг-хук для понижения `kMaxManifestEntries` в тестах |
| Variable-node compose | S23 | OPEN (harness): 1/10-node бейзлайны |
| S31 dryrun-shard0 | S31 | OPEN (known, tool-only): `previewDeletes` смотрит только шард 0 |
| S08 contention counters | S08 | OPEN (harness): пере-подключить на счётчики §0 |

## Fixes landed during stabilization (chronological) {#fixes}

`2174a893f33` stale-precommit retry-until-clean + reclaim events · `a75dd75ab6d` harness batch
(ulimits/flush-window/log-dirs) · `7a22bc5b700` oracle convergence · plus the campaign-adjacent
product work the runs validated: snappatch `3c7003ce190`, checker `fb0a7697890`, driver tolerance
`ff8a8e4b0d7`+`7fb4c952a2a`, stagefix `c3d9aa9d8d6`, availfix `dcbbc34ec1a`+`324584be4cb`+`4f4f93c6bc6`.

## Newly commissioned scenarios (user, 2026-07-13) {#new-scenarios}

**S36: MOVE PART / MOVE PARTITION between disks — BOTH directions.**
Purpose: prove `ALTER TABLE ... MOVE PART|PARTITION TO DISK/VOLUME` works local→CA and CA→local,
with correct CAS lifecycle on each side: moving TO the CA disk publishes the part through the
normal build path (blobs/manifest/refs; dedup applies); moving OFF the CA disk drops the CAS refs
(deferred physical reclaim by GC — no orphans, no dangling) while the local copy serves reads;
concurrent SELECTs during the move never fail; `fsck` clean after each direction; GC reclaims the
vacated side's objects within bounded rounds. Both PART and PARTITION variants; include a
move-back-and-forth cycle (round trip must dedup — the second move TO CA re-adopts, near-zero body
uploads).
NOTE: today `MOVE PARTITION`-class tests are gated `no-content-addressed-storage` (B21
whole-part-clone contract) — this scenario is ALSO the acceptance test for closing that gate.

**S37: multi-disk storage configurations (2+ disks including CA).**
Purpose: prove a storage policy with ≥2 disks (local + CA; and a 3-disk local+local+CA variant)
behaves correctly: parts land per policy (`max_data_part_size_bytes`, `move_factor`, TTL MOVE
expressions targeting the CA volume and back), background policy moves take the same both-direction
lifecycle as S36, `system.parts.disk_name` truthful, per-disk space accounting sane, restart with a
multi-disk policy re-attaches every part to the right disk, and mixed-disk merges (sources on both
disks) write the result to the policy-selected disk with correct CAS publish/skip. Chaos leg:
node restart mid-policy-move — the move either completes or rolls back atomically (no half-moved
part, fsck clean).

Both scenarios need compose/config additions (a policy with local+CA disks in the scenario
framework's storage config) — harness work, to be planned with the card implementations.

## Product-adjacent finding (backlog) {#merge-retry-cost}

**Merge upload-failure handling** (S01@8GiB; investigation OPEN and quality-flagged — see the
`merge-upload-retry-investigation` memory note; three early analyses were wrong, systematic
debugging required before any fix). Log-verified: repeated MPU aborts (26/8min) while the merge
progress reset in loops; the INSERT path survived the same store stall by re-streaming from its
live scratch file within the query. **Required behavior (user): a merge whose upload fails must
retry the UPLOAD from the staged part — never rerun the whole merge.** To verify first: which layer
discards the staged result today, which S3 client uploads blob-body parts, whether the controller
budget truly starves gigabyte streams. Circuit-breaker remains the outer safety net only.

## Where this leaves the suite {#next}

1. Re-run S03/S04 full-scale against real S3 (or accept the rustfs ceiling and record ci-scale as
   authoritative for those cards).
2. Implement S36/S37 (cards + multi-disk configs) — the user's new coverage priorities.
3. Harness follow-ups: cap-lowering hook (S07), §0-counter repoint (S08), variable-node compose
   (S23).
4. The remaining 28 prod-scale scenarios resume whenever the campaign unpauses (progress log and
   scale plan are durable; priority verdicts are already banked).
