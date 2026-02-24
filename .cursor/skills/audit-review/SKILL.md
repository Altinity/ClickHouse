---
name: audit-review
description: Perform deep feature audits with transition-matrix and logical fault-injection validation. Use when reviewing complex changes, regressions, state-machine behavior, config interactions, API/protocol flows, and concurrency-sensitive logic.
---

# Audit Review

## Purpose

Run a repeatable deep audit for any feature and report confirmed defects with severity.
Default mode is static reasoning unless runtime execution is explicitly performed.

## Workflow

1. If PR scope is large, partition by functionality/workstream first:
   - define partitions and boundaries,
   - review each partition independently with the full workflow below,
   - track per-partition findings and coverage,
   - deduplicate cross-partition findings by root cause,
   - finish with cross-partition interaction risks.
2. Build call graph first:
   - user/system entrypoints (API, RPC, CLI, worker, scheduler)
   - dispatch and validation layers
   - state/storage/cache interactions
   - downstream integrations (network, filesystem, service calls)
   - exception and error-propagation paths
3. Build transition matrix:
   - request/event entry -> processing stages -> state changes -> outputs/side effects
   - define key invariants and annotate where each transition must preserve them
4. Perform logical testing of all code paths:
   - enumerate all reachable branches in changed logic,
   - record expected branch outcomes (success, handled failure, fail-open/fail-closed, exception),
   - include happy path, malformed input, integration timeout/failure, and concurrency/timing branches.
5. Define logical fault categories from the code under review:
   - derive categories from actual components, transitions, and dependencies in scope,
   - document category boundary and affected states/transitions,
   - prioritize categories by risk and blast radius.
6. Run logical fault injection category-by-category:
   - execute one category at a time,
   - for each category cover success/failure/edge/concurrency paths as applicable,
   - record pass/fail-open/fail-closed/exception behavior per injected fault.
   - maintain a category completion matrix with status:
     - Executed / Not Applicable / Deferred,
     - outcome,
     - defects found,
     - justification for Not Applicable or Deferred.
7. Confirm each finding with code-path evidence.
8. Produce coverage accounting:
   - reviewed vs unreviewed call-graph nodes,
   - reviewed vs unreviewed transitions,
   - executed vs skipped fault categories (with reasons).
   - mark coverage complete only when every in-scope node/transition/category is reviewed or explicitly skipped with justification.
9. For multithreaded/shared-state paths, perform interleaving analysis:
   - write several plausible thread interleavings per critical transition,
   - identify race/deadlock/lifetime hazards per interleaving.
10. For mutation-heavy paths, perform rollback/partial-update analysis:
   - reason about exception/cancellation at intermediate points,
   - verify state invariants still hold.

## C++ Bug-Type Coverage (Required for C++ audits)

- memory lifetime defects (use-after-free/use-after-move/dangling refs)
- iterator/reference invalidation
- data races and lock-order/deadlock risks
- exception-safety and partial-update rollback hazards
- integer overflow/underflow and signedness conversion bugs
- ownership/resource leaks (RAII violations)
- undefined behavior from invalid casts/aliasing/lifetime misuse

## Multithreaded Database Emphasis

For ClickHouse-style multithreaded systems, prioritize these checks before lower-risk issues:

1. Shared mutable state touched by multiple threads without clear synchronization.
2. Lock hierarchy consistency and potential lock-order inversion/deadlock cycles.
3. Cross-thread lifetime safety (dangling references/pointers after erase/reload/shutdown).
4. Concurrent container mutation + iterator/reference use.
5. Exception/cancellation paths that can leave locks/state inconsistent.

## Output Contract

- Start with confirmed defects only.
- Group by severity: High, Medium, Low.
- For each defect include:
  - title,
  - impact,
  - file/function anchor,
  - fault-injection trigger,
  - transition mapping,
  - why it is a defect (not a design preference),
  - smallest logical repro steps,
  - likely fix direction (one line),
  - regression test direction (one line),
  - affected subsystem and blast radius,
  - at least one code snippet proving the defect.
- Separate “not confirmed” or “needs runtime proof” from confirmed defects.
- Include an **Assumptions & Limits** section for static reasoning.
- Include an overall **confidence rating** and what additional evidence would raise confidence.
- If no defects are found, include residual risks and untested paths.
- For large PRs, include per-partition findings/coverage and final cross-partition risk summary.
- Include a fault-category completion matrix for every deep audit.

### Canonical report order

1. Scope and partitions (if large PR)
2. Call graph
3. Transition matrix
4. Logical code-path testing summary
5. Fault categories and category-by-category injection results
6. Confirmed defects (High/Medium/Low)
7. Coverage accounting + stop-condition status
8. Assumptions & Limits
9. Confidence rating and confidence-raising evidence
10. Residual risks and untested paths

## Severity Rubric

- High: realistic trigger can cause crash/UB/data corruption/auth bypass/deadlock.
- Medium: correctness/reliability issue with narrower trigger conditions.
- Low: diagnostics/consistency issues without direct correctness break.

## Checklist

- Verify call graph is explicitly documented before defect analysis.
- Verify invariants are explicitly listed and checked against transitions.
- Verify fail-open vs fail-closed behavior where security-sensitive.
- Verify logical branch coverage for all changed code paths.
- Verify fault categories are explicitly defined from the reviewed code before injection starts.
- Verify category-by-category execution and reporting completeness.
- Verify full fault-category completion matrix is present and complete.
- Verify concurrency and cache/state transition paths.
- Verify multithreaded interleavings are explicitly analyzed for critical shared-state paths.
- Verify rollback/partial-update safety under exception/cancellation points.
- Verify major C++ bug classes are explicitly covered (or marked not applicable).
- Verify race/deadlock/crash class defects are prioritized and explicitly reported.
- Verify error-contract consistency across equivalent fault paths.
- Verify performance/resource failure classes were considered.
- Verify findings are deduplicated by root cause.
- Verify coverage accounting is present (covered vs skipped with reason).
- Verify stop-condition criteria for coverage completion are explicitly satisfied.
- Verify every confirmed defect includes code evidence snippets.
- Verify parser/config/runtime consistency.
- Verify protocol/API parity across entrypoints.
- Verify no sensitive-data leakage in logs/errors.
