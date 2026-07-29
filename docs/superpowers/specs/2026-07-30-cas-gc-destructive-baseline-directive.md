---
description: 'Authoritative user directive (2026-07-30): end-of-Stage-B sequence — delete probe A, enable catalog-proven destructive GC, sequential-baseline destructive soak + GC perf report.'
sidebar_label: 'GC destructive baseline directive'
sidebar_position: 65
slug: /superpowers/specs/2026-07-30-cas-gc-destructive-baseline-directive
title: 'GC destructive-baseline directive — probe-A removal, catalog-proven destruction, sequential soak'
doc_type: 'reference'
---

# GC destructive-baseline directive {#gc-destructive-baseline-directive}

Authoritative user directive, received 2026-07-30 (night), recorded verbatim (translated headers
added for anchors only). Placement: THE END of the current (amended) Stage B plan — this is the
next round's charter, GC-focused. The user's sequencing rationale is part of the directive: two
separate changes, probe-A removal FIRST, then catalog-proven destructive mode + soak — "так легче
отличить performance effect от возможной correctness-регрессии".

## The sequence {#sequence}

1. **Удалить probe A.** Удалить дополнительный LIST, setting, counters, phase, tests и
   устаревшие комментарии. Сохранить B1/B2 и mount-time capability probe.

2. **Включить destructive GC через catalog-authoritative universe.** В здоровом раунде должно
   получаться:

   ```
   frontier_complete = true
   suppress_destructive = false
   ```

   Но gate остаётся:

   ```
   suppress_destructive =
       anomalies ||
       carried_holds ||
       !frontier_complete
   ```

   То есть holds, budget exhaustion, incomplete frontier и ошибки по-прежнему запрещают удаления.

3. **Провести destructive soak на текущей последовательной реализации.** Пока не добавлять
   MultiDelete и parallel deletes: сначала получить честный baseline и понять реальные затраты:
   - pending_deletes;
   - owner-removed manifest deletion;
   - orphan-manifest sweep;
   - ref-object cleanup;
   - namespace cleanup;
   - generation pruning;
   - время и количество раундов до fixpoint.

## Критерии результата {#result-criteria}

- здоровые раунды действительно выполняют destructive work;
- `ca-fsck --detail` не находит dangling/stale-edge;
- backlog стабильно достигает нуля;
- holds/anomalies по-прежнему подавляют все irreversible paths;
- после удаления probe A нет второго полного ref LIST;
- phase timings и S3 operation counts дают baseline для MultiDelete и concurrency.

## Deliverable {#deliverable}

Провести исследование производительности GC на этом soak-е и написать новый документ вида
`docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md` (its successor for the destructive
baseline).
