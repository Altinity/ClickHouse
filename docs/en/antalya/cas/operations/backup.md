---
description: 'How BACKUP and RESTORE work for a table on a content-addressed disk: what holds the data during a backup, when the copy runs inside S3, and what is not supported yet.'
sidebar_label: 'Backup'
sidebar_position: 5
slug: /antalya/cas/operations/backup
title: 'CAS Operations — Backup'
doc_type: 'guide'
---

# Operations — backup {#backup}

Ordinary `BACKUP` and `RESTORE` work for a table on a content-addressed (`CAS`) disk. This page
covers what happens during one, how it differs from a plain disk, and what the limits are.

The `CAS`-native backup model — `snapshot` / `mirror` / `fetch` / `restore` — is designed but not
implemented and is not wired into the SQL surface. See the [roadmap](/antalya/cas/roadmap#backups).

## What is supported {#supported}

```sql
BACKUP TABLE t TO S3('https://bucket.s3.amazonaws.com/backups/b1', 'key', 'secret');
RESTORE TABLE t AS t_restored FROM S3('https://bucket.s3.amazonaws.com/backups/b1', 'key', 'secret');
```

The destination can be anything: `S3`, `Disk`, `File`, or an archive. A backup can be restored onto a
disk of any type, because it holds the table's files rather than the pool's objects.

**An `Atomic` database is required.** That has been the default since 20.x. On the deprecated
`Ordinary` engine the backup fails with `SUPPORT_IS_DISABLED`: that path pins files with temporary
hard links, which object storage does not have.

## What holds the data during a backup {#holding}

On a plain disk a backup pins files against deletion with a hard link. `CAS` uses a different
mechanism — pointer holding:

- the backup holds a `shared_ptr` to the table and to each part;
- the outdated-part cleanup skips those parts;
- while a part is alive so is its [ref](/antalya/cas/architecture/manifests-and-refs#ref-table) — the
  name under which the part is registered in its namespace's ref table, and through which it points
  at its manifest;
- while the ref is alive, garbage collection sees the manifest and its blobs as reachable.

This mechanism lives in the process's memory and **does not survive a server restart**. An
interrupted backup leaves nothing behind in the pool, but it also stops protecting the data once the
process is gone. For durable pinning there is `FREEZE`, which publishes a real ref.

## How the bytes move {#copy-path}

Which path runs depends on the destination.

**A destination outside the pool** — the common case: another bucket, a local disk, an archive. Files
are read through the `CAS` read path and written to the destination. Pool deduplication is lost:
what was one blob shared by several replicas becomes ordinary files in the backup.

**An `S3(...)` destination on the same `S3` endpoint as the pool** — the copy of a blob then runs
inside the `S3` store itself: the ClickHouse server issues "copy these bytes" commands - one per
upload part, so a payload larger than one part takes several - and `S3` moves the bytes internally
without sending them through ClickHouse.

```sql
BACKUP TABLE t TO S3('http://s3.example.com/bucket/backups/b1', 'key', 'secret');
```

Here the pool is under `http://s3.example.com/bucket/pool/`.

The files of a part fall into two categories:

| Category | Example | How it is copied |
|---|---|---|
| Blob | `data.bin`, marks, `primary.idx` | an `UploadPartCopy` naming a byte range — only the payload moves, without the blob's internal header |
| Inside the manifest | `checksums.txt`, `count.txt`, `columns.txt` | read through the `CAS` read path and written through ClickHouse's buffers |

A blob object is `[header][payload]`, so a file never starts at the beginning of its object. Every
copy of a blob therefore names the payload range. A copy of the whole object would put the header
into the backup, and a later restore would read wrong data.

If a byte range cannot be copied inside `S3`, `CAS` does not fall back to copying the whole object —
the file is read and written through ClickHouse instead. That is slower, but correct. ClickHouse takes
that path on its own in three cases: the query sets `s3_allow_multipart_copy = 0`; the store is
recognized as `GCS`, whose current XML API has no `UploadPartCopy`; or `UploadPartCopy` is refused with
`AccessDenied`.

A store that refuses `UploadPartCopy` with any other error fails the `BACKUP` instead, because
ClickHouse cannot tell a missing capability from a transient failure. On such a store, set
`s3_allow_multipart_copy = 0` on the `BACKUP` query.

| Turn off the copy inside `S3` | Turn off the range copy |
|---|---|
| `SETTINGS allow_s3_native_copy = 0` in the `BACKUP` query | the query setting `s3_allow_multipart_copy = 0` |

```sql
BACKUP TABLE t TO S3(...) SETTINGS allow_s3_native_copy = 0;
```

**A `Disk(...)` destination** never copies inside `S3`, even when the disk is an `s3` or `s3_plain`
disk on the same endpoint as the pool:

```sql
BACKUP TABLE t TO Disk('backups_s3', 'b1');
```

Every file is read through the `CAS` read path and written through ClickHouse's buffers. The
`allow_s3_native_copy` and `s3_allow_multipart_copy` settings have no effect on this path.

## Restore {#restore}

Each part is materialized in **one disk transaction** and published as one manifest and one ref. A
partially restored part can never appear in the pool: either the whole part is published or nothing
is.

Restore onto a `CAS` disk never copies objects inside `S3`, even when the backup is on the same
endpoint as the pool. Each file of the part is read from the backup and written through the `CAS`
write path, because only that path can build the manifest and the blobs. A restore onto a plain disk
works as usual and can copy inside `S3`.

Restored data is packed afresh — on a `CAS` disk it gets new blobs and new refs. Deduplication
against data already in the pool works as usual: identical content hashes to the same blob and is
not written twice.

## `FREEZE` is not a backup {#freeze}

`ALTER TABLE ... FREEZE` works on `CAS` and publishes parts into a separate shadow namespace, which
is a garbage-collection root in its own right. `DROP PARTITION` removes the live refs and leaves the
snapshot alone; `SYSTEM UNFREEZE` removes only the shadow refs.

It is still not a snapshot of a table: there is no SQL metadata, no single commit marker, no
portable object with a listing and a restore API, and its lifetime is tied to a manual `UNFREEZE`.
It is a useful building block, not a replacement for `BACKUP`.

## Limitations {#limitations}

- The `CAS`-native backup model (`snapshot` / `mirror` / `fetch`) is not implemented.
- The `Ordinary` database engine is not supported.
- Pool deduplication is lost in the backup: its size follows the logical files, not the unique blobs.
- Pointer holding does not survive a server restart.
- The copy inside the store works only for an `S3(...)` destination on an `S3` or `S3`-compatible
  store, and only when the destination is on the same endpoint as the pool. Other destinations,
  including `Disk(...)`, get the copy through ClickHouse's buffers.
- A blob is copied inside `S3` only with multipart copy (`UploadPartCopy`), because only it can name a
  byte range. With `s3_allow_multipart_copy = 0`, on a store recognized as `GCS`, and when
  `UploadPartCopy` is refused with `AccessDenied`, every blob goes through ClickHouse's buffers
  instead. A store that refuses `UploadPartCopy` with another error fails the `BACKUP`; set
  `s3_allow_multipart_copy = 0` there.
- Restore onto a `CAS` disk always writes through ClickHouse, see [restore](#restore).
- A disk-level write of a part file onto a `CAS` disk outside of a part transaction is rejected with
  `NOT_IMPLEMENTED` and the message `Autocommit writes are not supported for content part files`: a
  `CAS` disk publishes a part's manifest and ref at commit, so it has nowhere to put a single
  autocommitted file.
- A `CAS` disk cannot be a backup destination: `BACKUP TABLE t TO Disk('<cas disk>', 'b1')` is rejected
  the same way. A backup's own layout mirrors the table's data directory, so its files sit under a part
  directory as well and count as part files.
