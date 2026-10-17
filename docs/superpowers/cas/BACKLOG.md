---
description: 'Pointer to the CAS MergeTree backlog, which lives in Backlog.md under docs/superpowers/cas/backlog.'
sidebar_label: 'CAS Backlog (live)'
sidebar_position: 9
slug: /superpowers/cas/backlog
title: 'CAS MergeTree — Live Backlog (pending issues)'
doc_type: 'guide'
---

# CAS MergeTree — Live Backlog {#cas-backlog}

The CAS backlog lives in Backlog.md at `docs/superpowers/cas/backlog` (task prefix `CAS-`). Use the `backlog` CLI:

- `backlog board` shows the Kanban board.
- `backlog task list -l area:<x> --plain` lists one area, for example `area:gc`; `-m <milestone>` and `--priority <p>` filter further.
- `backlog browser` opens the web UI.
- `backlog instructions overview` prints the workflow for agents.

Accepted decisions live in `backlog/decisions` (`backlog decision list`). New items are created with `backlog task create`
(or `--draft` for an undecided idea), never appended to this file.

The pre-migration topic files under `BACKLOG/` were deleted after their items were imported, in these commits:
`e024ed5c6c3` (operability-and-introspection), `004ff471a51` (mounts-and-lifecycle), `cb07efda888` (gcs),
`c11dbeedabb` (performance), `8ebc69079a3` (ref-protocol), `5fb09ee950e` (gc), `05c641ef970` (testing-and-ci),
`7d0957a75d5` (issue-2310), `128a94392ca` (docs-and-cleanup), `0f183528bcd` (formats-and-storage),
`1b43d2f4363` (replication). The last Inbox item of this file became `CAS-283`.

A product-level view of the same items, grouped by theme with one story per user-visible outcome, is kept in
`PM-BACKLOG.md` (slug `/superpowers/cas/pm-backlog`). It is a snapshot for planning; the live backlog wins when they differ.
