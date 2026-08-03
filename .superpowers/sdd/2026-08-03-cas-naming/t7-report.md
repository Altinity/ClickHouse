# Task 7 report — documentation

## Step 1 — page renames and identifier sweep
`git mv` of the three system-table pages to `cas_gc_log.md`, `cas_log.md`, `cas_mounts.md`, then the
identifier sweep across `docs/en/`. Frontmatter survived intact and consistent: `slug`
(`/operations/system-tables/cas_*`), `title` (`system.cas_*`), `sidebar_label`, `sidebar_position`,
`description`, `doc_type` — each checked by reading all three blocks, not assumed from the sed.

## Step 2 — SQL text, headings, anchors
Headings are now `### SYSTEM CAS GC RUN {#system-cas-gc-run}`, `{#system-cas-gc-rebuild}`,
`{#system-cas-drop-pool-member}`; the first also fixes the previously inconsistent
`#content-addressed-garbage-collection` anchor. Syntax blocks updated. Every heading in the touched files
carries an explicit `{#anchor}`, per the repo doc rule.

**The plan named three inbound links to fix; there were seven.** Beyond `storing-data.md:461-463`, the
old anchors were also linked from `cas_gc_log.md` (x2) and `cas_mounts.md` (x2). Found by resolving every
`(/sql-reference/statements/system#…)` link in `docs/en/` against the set of anchors actually defined in
`system.md`, rather than by grepping the three lines the plan listed. All seven now resolve; the checker
reports `broken links into system.md: none`.

## Step 3 — prose
- 14 `CA`-as-feature occurrences -> `CAS`, first mention per page still spelled out
  ("content-addressed (CAS) MergeTree …"). Checked first that no `CA` in scope meant *certificate
  authority* before running the rewrite.
- The four `CAS` occurrences meaning compare-and-swap in `cas_gc_log.md` (lines 72, 73, and two on 84)
  reworded to "compare-and-swap". `grep -rn '`CAS`' docs/en/` is now empty.
- `cas_gc_log.md` "`Cas*` counters" -> "`CAS*` counters". Worth noting: this one is invisible to a
  `Cas[A-Z]` grep, because the next character is `*`, not an uppercase letter — the plan was right to call
  it out as a separate item rather than leave it to the pattern.

### The `storing-data.md` config block — claim verified before rewriting
The plan states the documented nested `<content_addressed>` settings block does not exist in code. Confirmed
by reading the code rather than trusting the plan: `ContentAddressedSettings::loadFromConfig` calls
`config.keys(config_prefix, …)` and then `config.getString(config_prefix + "." + key)`, where
`config_prefix` is the **disk element** — so the keys are direct children of the disk, flat. The `<cas>`
elements that do exist in real configs (`tests/config/config.d/`, `utils/ca-soak/configs/`) are disk and
policy *names*, not settings blocks.

Rewritten to flat keys under the disk element, and the surrounding prose rewritten with it — the old text
justified the missing prefixes by "the `<cas>` block already scopes every key", which was the fictional
part. The line-496 sentence about unprefixed per-disk keys is kept, as instructed, now resting on the disk
element doing the scoping.

The example is also now *checkable*, and was checked: every key it shows is a registered CAS setting in
`LIST_OF_CONTENT_ADDRESSED_SETTINGS` (`example keys not registered as CAS settings: none`, against 30
registered settings).

The `#required-parameters-content-addressed` / `#optional-parameters-content-addressed` anchors and the
"### Using Content-Addressed Storage" heading keep their spelled-out form: nothing links to them, so no
link breaks, and the plan says not to invent `#…-cas` anchors in that case.

## Step 4 — verification
- `grep -rn 'content_addressed\|CONTENT ADDRESSED\|Cas[A-Z]' docs/en/` (minus the false-positive
  exclusions): **no output**.
- `grep -rnE '\bCA\b'` over the five CAS docs, minus `certificate`: **no output**.
- All links into `system.md` resolve; all CAS headings carry anchors.

Two headings my anchor checker flagged in `system.md` are **pre-existing and untouched by me**: the `#`
H1 title (which takes no anchor) and `### SYSTEM UNFREEZE {#query_language-system-unfreeze}`, whose anchor
uses an underscore my `[a-z0-9-]+` pattern rejected. Confirmed neither appears in this task's diff.

## Carry-forward status
The Task-3 item (`gtest_cas_settings.cpp:65` citing the old config filename) was already fixed in Task 5's
commit `c4f0ba4184f`; nothing to fold in here. Verified by reading the line at HEAD.

## Scope
`docs/en` only, 5 files, 79 insertions / 81 deletions. No docs build was run — none is wired up in this
worktree — so this task claims correct link/anchor resolution and code-checked content, not a rendered page.
