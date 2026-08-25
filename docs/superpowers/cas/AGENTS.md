---
description: 'Working guide for any agent (any model, any session) touching the CAS branch: hard invariants and user vetoes, build/test/gate recipes, delegation and git policy, known traps, and where everything lives. Absorbs INTENT.md and the agent-relevant parts of 02-methodology.md.'
sidebar_label: 'CAS agent guide'
sidebar_position: 1
slug: /superpowers/cas/agents
title: 'CAS — Agent Working Guide'
doc_type: 'reference'
---

# CAS — agent working guide {#cas-agents}

Read this before touching the branch. It is deliberately short; every entry is one rule plus where to verify it. Names below were verified against HEAD on 2026-08-04 — when in doubt, re-verify against code, never against older docs.

## 1. Orientation {#orientation}

| What | Where |
|---|---|
| Implementation (almost all of it) | `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/{Primitives,Formats,Backend,Pool,Parts,Gc,Tools}` |
| Live pending work (single source of truth) | `docs/superpowers/cas/BACKLOG.md` is the index; the items live in `docs/superpowers/cas/BACKLOG/*.md` by topic, plus an `## Inbox` in `BACKLOG.md` for un-triaged quick adds — issue IDs are never renumbered |
| User/ops documentation | `docs/en/antalya/cas/` (architecture, runbooks, roadmap) |
| Audit trail of the 2026-08 docs consolidation | `docs/superpowers/cas/consolidation-2026-08/COVERAGE-MATRIX.md` |
| System tables | `system.cas_log`, `system.cas_gc_log`, `system.cas_mounts` |
| Disk registration | `metadata_type = cas` (`MetadataStorageFactory.cpp`, literal `"cas"`) |
| SQL surface | `SYSTEM CAS GC RUN/STOP/START/REBUILD`, `SYSTEM CAS FSCK`, `SYSTEM CAS FORGET`, `SYSTEM CAS DROP POOL MEMBER '<id>' FROM DISK '<disk>'` |
| System-table naming caveat | table names are `cas_log`/`cas_gc_log` (via the `SystemLog.h` registration macro + `<cas_log>` config blocks), while the C++ classes remain `ContentAddressedLog`/`ContentAddressedGarbageCollectionLog` — grep the macro, not `attachSystemTables.cpp`, when verifying log tables |
| CLI tools | `clickhouse-disks`: `cas-fsck`, `cas-gc-dryrun`, `cas-inspect`, `cas-gc-rebuild`, `cas-drop-member` |
| Soak/chaos harness | `utils/ca-soak/` (compose variants per backend/shard-count), scenario suite in `utils/ca-soak/scenarios/` |

Naming note: C++ suites/events use `CAS` (`CASBlobInDegree`, `CASGC*` ProfileEvents); classes are `Cas::Pool`, `Cas::PartWriteTxn` (old `Store`/`Build` names are dead). Prefixed setting spellings (`cas_*`, `content_addressed_*`) are dead — settings are unprefixed inside the disk config block.

## 2. Hard invariants and user vetoes {#invariants}

Violating any of these is not a judgment call; it needs the user's explicit sign-off.

1. **No silent data loss, ever.** No path may lose acknowledged data; no path may delete an object a committed reference still names. A gap gets named, modelled, tested, and tracked — never softened into a caveat.
2. **Revival = fresh re-upload only.** Never `GET` a condemned object to revive it.
3. **GC never throws on a 404 during fold** — record and continue; an escaped exception can wedge reclamation permanently.
4. **Fail closed under ambiguity.** An operation that MAY have landed is not one that did not. No fallback paths that hide errors; doubt about the SOURCE aborts, only doubt about the MECHANISM may reroute.
5. **Protocol steps are untouchable as "cheap optimizations"** (e.g. `HEAD`-before-`PUT`). Suspected costs get instrumented first, decided on data, and approved by the user.
6. **No CAS concepts in generic MergeTree/Replicated/Keeper code or formats.** Accepted exceptions are recorded with reasoning.
7. **No compat scaffolding pre-release.** The pool format has no persisted production data; a format bump is cheap, a dual protocol is not.
8. **S3 LIST trust is SETTLED — do not re-argue.** Middle omission below a correct tail is legal on compliant S3; tail under-reporting of a completed write is not; a one-shot LIST probe cannot certify a backend. Trust chain: catalog cut → `_ckpt` → exact `GET`s → conditional writes; LIST is a hint whose lies may only delay reclamation, never authorize destruction.
9. **Hash equality needs the adversary model** — no skip-read shortcuts; re-hashing is the identity primitive.
10. **A plan is a hypothesis, not an authority.** When a plan step contradicts this document, this document wins and the plan is amended. Reviewer remedies are checked before being applied.
11. **A green that cannot go red is not evidence.** Every proof must be able to fail; no vacuous asserts, no whitelist that silently drops a class.

## 3. Build, test, gates {#build-test}

| Task | Recipe |
|---|---|
| Build | `ninja` in a `build*/` dir, output redirected to a log file in the build dir; no `-j`, no `nproc`; analyze the log with a subagent |
| CAS unit-test gate | run the gtest binary with `--gtest_filter='CAS*'` — the naming policy is STRICT: every CAS suite (death-test variants included) MUST start with `CAS`, and nothing non-CAS may. Never widen the filter to `Ca*`/`Cas*` "just in case": the widened form only drags in foreign suites (`CascadeWriteBuffer`) and masks a misnamed suite instead of catching it. A suite that escapes `CAS*` escapes the gate (happened twice) — the fix is renaming the suite, never widening the filter |
| `LOGICAL_ERROR` in tests | NEVER `EXPECT_THROW`/`EXPECT_ANY_THROW` a `LOGICAL_ERROR` site directly: under sanitizer builds `LOGICAL_ERROR` ABORTS the process and hides every test after it (5+ recurrences). Before asserting a throw, READ the thrown error code at the site. If the condition is reachable via bad input, the code is wrong too — reclassify (`CORRUPTED_DATA`/`BAD_ARGUMENTS`), which also fixes the test. Only a genuinely unreachable-state assert stays `LOGICAL_ERROR`, and its test goes into a separate `*DeathTest` suite |
| `LOGICAL_ERROR` tests | never plain `EXPECT_THROW` on a `LOGICAL_ERROR` site — sanitizer lanes abort. Use the death-test split (`EXPECT_DEATH` under `abort_on_logical_error`) |
| Stateless/integration (praktika) | `python3 -m ci.praktika run "stateless" --test <names>` / `... run "integration" --test <selectors>` from repo root; binary symlinked at `ci/tmp/clickhouse`; `--test` takes ONE space-separated list (repeats collapse) |
| Local praktika caveat | local runs prune docker containers/volumes — never overlap with a live ca-soak |
| Real soak | phase 3 with `--duration Nm` (phase 1 `--ops` finishes ~10× faster and is NOT a soak); clean restart = `docker compose down -v` |
| Test debris | never `git add -A`; tests must not leave files at repo root |

After any cross-cutting sweep: `git status` the WHOLE tree and verify the committed HEAD builds. A green suite after a failed build is evidence about a different binary.

## 4. Delegation, subagents, git {#delegation}

- **Mechanical work → codex**: `codex exec -m gpt-5.6-luna -s workspace-write - < prompt-file`. Prompt always via file (inline strands stdin). WITHOUT `-s workspace-write` codex can hit a read-only sandbox, exit 0 and write NOTHING — never trust its exit code; diff expected-vs-actual outputs. Judgment and review stay with Claude.
- **Background execution**: use the harness-native background mechanism; bare `nohup ... &` children get killed by the harness (known issue). Poll on-disk artifacts; Monitor is unreliable for waits >10 min.
- **Subagent results**: every subagent stages results to a FILE and names the path in its final message — message-only results have been lost repeatedly.
- **Git in a shared worktree**: concurrent agents commit here. Commit explicit paths only, then verify `git show --stat HEAD` contains exactly your files. Never rebase/amend; never commit to `master`; NEVER push unless the user asks — a past push request is not standing authorization.
- **Reviews**: verify claims against code, not against the report; evidence expires when the tree moves — re-run, re-read, never relay.

## 5. Known traps {#traps}

| Trap | Rule |
|---|---|
| tmpfs inode exhaustion under concurrent test gates | check `df -i`, not `df -h`; TMPDIR fixes builds but not gtest `CaptureStdout` |
| NUL bytes in logs | plain `grep` misattributes matches; use `grep -a` |
| mermaid diagrams | an unquoted `;` in sequence text kills rendering; validate programmatically before publishing |
| jemalloc profiling | `SYSTEM JEMALLOC ENABLE PROFILE` lies; use `jemalloc_enable_global_profiler` + restart |
| `LogSeriesLimiter` | "accepted series X/N" counts ALL rate-limited messages on the logger, not your message |
| `--time-style` sort | lexicographic `sort` on `HH:MM` breaks across midnight; sort by mtime (`ls -t`) |
| "impossible" claims | name the enumeration or happens-before that makes it impossible; absence-of-evidence needs the discriminator (what exactly was searched) |
| exhaustiveness claims | never from a `head`-truncated grep; use untruncated output |

## 6. Reporting conventions {#reporting}

- Scenario/campaign results: a table — № / description / result / artifacts / planned fix.
- "No known reds": any red gets an RCA or a tracked return-item with a named owner-task; tactical tolerance is never silent.
- Non-code (prose/comment) findings during reviews: batch into one deferred-docs pass, never a per-finding fix round — but CLASSIFY first; prose-looking findings are sometimes code.
- Comments in code state constraints, never provenance: no references to plans/reviews/BACKLOG/task files (they get deleted); keep the reason, drop the citation.
