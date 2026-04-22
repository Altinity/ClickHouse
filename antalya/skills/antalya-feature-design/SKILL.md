---
name: antalya-feature-design
description: Scaffold or review a feature design document for ClickHouse / Antalya. Use this whenever a developer wants to design or implement a new feature, add a SQL function, add a setting, add a new engine or format, or change server behavior in a non-trivial way — even if they don't explicitly ask for a "design doc". Also use when reviewing an existing design before implementation starts.
---

# Antalya Feature Design

Help the developer produce a feature design for ClickHouse / Antalya before code is
written. A good design shortens review, prevents abandoned branches, and surfaces compatibility
landmines before they become migrations.

## When to create vs. review

- **Create** when the developer is starting new work and has no design yet, or only a rough idea.
- **Review** when the developer hands you an existing design (file path, paste, or PR link) and
  asks for feedback, a critique, or to "check" it.

If unclear, ask one question: "Are we drafting a new design, or reviewing an existing one?"

---

## Creating a design

### 1. Ask a few questions first (don't guess)

The template has four sections. Before filling them in, confirm the basics — a wrong premise wastes
more time than a short interview:

- **What problem is being solved?** Push for a concrete workload or user report, not "users want X".
- **What's the rough shape?** New SQL syntax? A new setting? A new engine/format? A behavior change?
- **Any hard constraints?** Upstream ClickHouse parity, on-disk compatibility, a specific release
  train, a customer deadline.
- **Where should the design file live?** Default to `docs/design/<feature-slug>.md` relative to the
  repo root, but let the developer override.

If the developer has already answered these in conversation, skip ahead — don't re-interview them.

### 2. Copy the template, then fill it in

The template lives at `assets/design-template.md` next to this `SKILL.md`. Copy it to the chosen
path, then populate each section based on the conversation. Leave a section marked `TBD` with a
pointed question rather than inventing content — a visible gap is more useful than a plausible
fabrication.

### 3. Conventions to apply while writing

These match the project `CLAUDE.md` and make the design consistent with the rest of the codebase:

- Wrap SQL keywords, function names, class names, setting names, engine names, and literal log
  message excerpts in inline code blocks: `MergeTree`, `SELECT`, `max_threads`, `system.errors`.
- Refer to functions as `f` (not `f()`) when naming the function itself rather than a call.
- Say "throws an exception" rather than "crashes" for logical errors — release builds don't crash
  on `LOGICAL_ERROR`.
- Cite files as `path/to/file.cpp:line` so reviewers can jump directly.
- Prefer one-line statements of intent over prose padding.

### 4. Push back on weak spots as you write

Drafting is also a review — don't wait for section 4 to think. In particular:

- If the **Motivation** reduces to "users want X" with no concrete workload, ask for one.
- If **Goals** are not measurable, say so and suggest a measurable form.
- If there are no **Non-goals**, propose a few. The absence of non-goals is the single biggest
  source of scope creep.
- If **Alternatives considered** is empty, press for at least one rejected approach. A design with
  no alternatives considered usually means the author has a solution looking for a problem.

---

## Reviewing a design

Read the design end-to-end first, then walk the checklist below. Report findings grouped by
severity: **blocking** (must fix before implementation), **should-address** (fix before merge),
**nit** (optional). Quote the exact text you're critiquing so the author can find it fast.

### Requirements
- Motivation cites a concrete workload, incident, or user report — not a generic assertion.
- Requirements are measurable. "Faster" is not a requirement; "query `Q` drops from 5s to <500ms on dataset `D`"
  is.
- Non-requirements are listed. Missing non-requirement almost always produce scope creep in review.

### Functional specification
- Every new/changed SQL syntax has at least one concrete example.
- Every new setting has scope (server / user / query), default, and valid range.
- Default-value changes that alter behavior for existing workloads are called out explicitly.
- Error behavior specifies the error code, not just "throws an error".
- Backward compatibility covers: older client → newer server, newer client → older server,
  mixed-version cluster, on-disk format. If any of these is genuinely N/A, the design should say
  so — silence is not the same as "not applicable".
- Formatting convention: inline code blocks around SQL identifiers, engine names, settings.

### Implementation
- Architecture section names the subsystems touched (parser / analyzer / planner / executor /
  storage / replication / keeper). If it touches many, that's a design smell worth flagging.
- New abstractions pull their weight. If an interface has one implementation and no foreseeable
  second, flag it — per project `CLAUDE.md`, three similar lines beat a premature abstraction.
- Storage format changes include a migration path and version-bump strategy.
- Performance section identifies the benchmark that will cover the hot path. Hand-wavy "shouldn't
  be a regression" is not enough for hot-path code.
- Alternatives considered contains at least one rejected approach with a reason. A design with no
  rejected alternatives is under-explored.
- Error handling lives at boundaries (user input, external systems), not scattered through
  internal code that already trusts its callers.

### Test plan
- Golden path, edge cases, and error cases are all enumerated — not just "add tests".
- Tests use `tests/queries/0_stateless` for functional coverage and `tests/integration` for
  anything touching replication, keeper, distributed queries, S3, auth, or Kafka.
- No `no-*` tags (especially `no-parallel`) unless there's a stated reason they're necessary.
- New tests are proposed as new files, not extensions of existing ones.
- Integration-test invocation is specified:
  `python -m ci.praktika run "integration" --test <selectors>`.
- Performance tests exist if the feature is in the hot path.
- Rollout section names the specific risk and whether a feature flag (and default) is warranted.

### Cross-cutting red flags
- Feature flags or backwards-compatibility shims added "just in case" — per project `CLAUDE.md`,
  prefer changing the code directly when you can.
- Comments in the proposed implementation that restate what the code does, or name the current
  task/PR/issue — those belong in commit messages and PR descriptions, not in source.
- Unfinished sections left as `TBD` with no owner or question — either resolve or annotate with
  the specific decision needed and who owns it.

---

## Output etiquette

- When creating, write the file and then tell the developer which sections need their input (the
  `TBD` items) rather than showing the whole template in chat.
- When reviewing, lead with a one-line verdict ("ready to implement", "needs revisions in
  sections 2 and 4", "fundamental rethink needed"), then the grouped findings.
- Keep the review terse. Block quotes from the design plus one-sentence critiques beat long prose.
