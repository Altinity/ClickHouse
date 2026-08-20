# CAS final checks before release {#final-checks-todo}

Working checklist for the pre-release pass. Items link to their authority; this file is the
short-lived TODO, not the record.

## 1. Fix #2173 — cross-disk `ATTACH PARTITION FROM` local -> CAS {#fix-2173}

- Issue: https://github.com/Altinity/ClickHouse/issues/2173 (CONFIRMED; reproduced on HEAD first-try)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2173-freezeremote-gap}`
- Plan: mirror `clonePart`'s CAS single-transaction branch inside
  `DataPartStorageOnDiskBase::freezeRemote` + stateless test from the issue's 3-statement repro.
- Scheduled: tomorrow, before the release.

## 2. Fix #2212 — FREEZE shadow namespace must be under server_root_id {#fix-2212}

- Issue: https://github.com/Altinity/ClickHouse/issues/2212 (CONFIRMED; CAS-001, data loss on backups)
- Full adjudication + fix shape: `docs/superpowers/cas/BACKLOG.md` `{#issue-2212-shadow-namespace}`
- Plan: prefix `shadowNamespace` with `server_root_id` + the two `"shadow/"` enumeration scopes
  (`ContentAddressedMetadataStorage.cpp:1281`, `:1513`, `:1700`) + stateless two-root isolation test.
- Scheduled: tomorrow, before the release.
