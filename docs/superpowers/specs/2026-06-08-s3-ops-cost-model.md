---
description: 'Order-of-magnitude S3 request, tag, and throughput cost model used as the cost reference for the content-addressed MergeTree storage design.'
sidebar_label: 'S3 ops cost model'
sidebar_position: 3
slug: /superpowers/specs/s3-ops-cost-model
title: 'S3 Operations Cost Model (reference)'
doc_type: 'reference'
---

# S3 Operations Cost Model (reference) {#s3-ops-cost-model}

**Status:** reference cheat sheet for the content-addressed `MergeTree` storage design (see
[CA Merkle store requirements](/superpowers/specs/ca-merkle-store-requirements), goal `G9`). Numbers are for
**AWS S3 Standard, US East / common US pricing**, order of magnitude only; exact prices vary by region and storage
class. Use it to reason about which protocol operations are cheap, which are expensive, and which scale slowly.
This is the **default reference profile**; other S3-compatible backends may charge or scale differently (notably
`DELETE` may not be free, and `LIST`/throughput limits vary), so the design must adjust the request budget per
backend rather than assume these numbers universally.

## Request prices {#request-prices}

| Operation | What it means | Cost / 1k | Cost / 1M | Notes |
| --- | ---: | ---: | ---: | --- |
| `GET` | read object body | **$0.0004** | **$0.40** | Data transfer/retrieval can dominate. |
| `HEAD` | read object metadata only | **~$0.0004** | **~$0.40** | Same tier as `GET`; no body transfer. |
| `GetObjectTagging` | read object tags | **~$0.0004** | **~$0.40** | Reads tag subresource. |
| `PUT` | create/overwrite object | **$0.005** | **$5.00** | 12.5x `GET`. |
| `COPY` | copy object / self-copy metadata rewrite | **$0.005** | **$5.00** | Metadata changes usually require self-copy. |
| `POST` | form upload etc. | **$0.005** | **$5.00** | `PUT`-tier. |
| `LIST` | list keys, max 1k keys/page | **$0.005** | **$5.00** | Same tier as `PUT`/`COPY`/`POST`. |
| `PutObjectTagging` | replace full object tag set | **$0.005** | **$5.00** | Whole tag-set replacement. |
| `DELETE` object | delete object | **$0** | **$0** | AWS says `DELETE`/`CANCEL` are free. |
| `DeleteObjectTagging` | remove tags | preferable to empty `PUT` | preferable | Empty tag `PUT` is charged as Tier 1; `DELETE` avoids that. |

AWS groups `LIST` with S3 Standard `PUT`/`COPY`/`POST` pricing, says `DELETE` and `CANCEL` are free, and charges
retrieval fees separately for some storage classes such as Standard-IA, One Zone-IA, and Glacier Instant
Retrieval.

## Tag storage cost {#tag-storage-cost}

Object tags also carry a monthly per-tag charge (reduced to `$0.0065` per 10,000 tags per month in March 2025):

| Stored tags | Monthly cost |
| ---: | ---: |
| 10k tags | **$0.0065/month** |
| 1M tags | **$0.65/month** |
| 100M tags | **$65/month** |
| 1B tags | **$650/month** |

## Throughput and latency {#throughput-and-latency}

| Operation family | AWS baseline scaling per prefix | Practical implication |
| --- | ---: | --- |
| `GET`, `HEAD` | **>= 5,500 req/s per partitioned prefix** | Reads scale well; use parallelism and multiple prefixes for more. |
| `PUT`, `COPY`, `POST`, `DELETE` | **>= 3,500 req/s per partitioned prefix** | Writes/tag updates/metadata rewrites scale slower than reads. |
| `LIST` | paginated, **up to 1,000 keys/request** | Listing 1B keys means up to ~1M `LIST` calls if fully scanning. |
| High sudden rate | may return `503 Slow Down` while S3 scales | Use SDK retries / exponential backoff. |

There is no limit on the number of prefixes in a bucket, so write throughput scales by sharding the key namespace
across prefixes. For latency-sensitive paths, retrying the slowest ~1% of requests reduces tail latency.

## Cost of real workloads {#cost-of-real-workloads}

| Task | Request count | Money | Time bottleneck |
| --- | ---: | ---: | --- |
| Read metadata for 1M objects | 1M `HEAD` | ~$0.40 | round trips / concurrency |
| Read object bodies for 1M objects | 1M `GET` | ~$0.40 + bytes/egress/retrieval | network throughput + object size |
| Upload 1M objects | 1M `PUT` | ~$5 | write concurrency / small-object overhead |
| Change metadata for 1M objects | 1M `COPY` | ~$5 | server-side copy + write rate |
| Set tags on 1M objects | 1M `PutObjectTagging` | ~$5 + tag storage | write-rate API fanout |
| Read tags on 1M objects | 1M `GetObjectTagging` | ~$0.40 | round trips / concurrency |
| List 1M objects | ~1,000 `LIST` calls | ~$0.005 | sequential pagination unless sharded prefixes |
| List 1B objects | ~1M `LIST` calls | ~$5 | pagination wall-clock time |

## Hidden costs {#hidden-costs}

- **Data transfer can dwarf request price.** A million `GET`s cost ~`$0.40` in request fees, but at 1 MiB each
  that is ~1 TiB read/egressed. Standard-IA / Glacier classes add retrieval fees.
- **Small objects amplify request costs.** For tiny objects, `PUT`/`GET`/`LIST`/`HEAD` fees become visible; for
  large objects, storage, retrieval, and egress dominate.
- **`LIST` is cheap per discovered object but slow for full scans.** One page gives 1,000 keys for `$0.000005`,
  but pagination is serial per prefix unless the namespace is sharded.
- **Tags are cheap to store, expensive to churn.** Storing 100M tags is ~`$65/month`, but rewriting tags on 100M
  objects is ~`$500` in request fees plus the time to issue 100M write calls.

## Mental model {#mental-model}

```text
GET / HEAD / read tags:         ~$0.40 per 1M
PUT / COPY / LIST / write tags: ~$5.00 per 1M
DELETE:                         free
Tag storage:                    ~$0.65 per 1M tags-month

Read scale:  >= 5.5k req/s/prefix
Write scale: >= 3.5k req/s/prefix
LIST page:   <= 1,000 keys/request
```

## Design implications for the CA store {#design-implications}

- **`DELETE` is free** - the cost of GC is in *discovering* what to delete (`LIST` / catalog reads) and in the
  *writes* it issues (retire markers, manifest fences, snapshots), not in the deletions themselves.
- **Write-tier requests are ~12.5x reads.** Minimize `PUT`/`COPY`/`LIST`/tagging on the writer hot path and in
  regular GC. A part costs its `F` blob `PUT`s (unavoidable bytes) plus a small constant of metadata `PUT`s; keep
  that constant small and avoid per-object `COPY`/tagging churn.
- **Avoid per-object `HEAD`/tag/`COPY` for bulk scans.** Use the manifest / delta log / snapshot (a catalog) for
  reachability, not per-object metadata calls. `HEAD` is cheap per call but a per-object fan-out over `10^11`
  objects is not.
- **Full `LIST` is the expensive, slow operation.** A full scan of `10^11` objects is ~`10^8` `LIST` calls
  (~`$500`) and is pagination-bound in wall-clock unless prefixes are sharded - which is exactly why regular GC
  must be delta-based and full `LIST` confined to rare full GC.
- **Prefer plain marker objects over object tags** for retire barriers: writing a marker is one `PUT` and removing
  it is free, whereas tags add `PutObjectTagging` (`PUT`-tier) plus monthly tag storage.
- **Manifest write-amplification is a throughput cost, not a per-request cost.** Rewriting a large manifest on
  every commit is one `PUT` request (price flat) but pays egress/throughput in bytes - bound shard size to keep
  it cheap in time.
- **Shard prefixes** (`<H[:2]>` style) both spread write throughput across partitions and parallelize `LIST`.

## Sources {#sources}

- S3 pricing: <https://aws.amazon.com/s3/pricing/>
- Object tagging price reduction (2025-03): <https://aws.amazon.com/about-aws/whats-new/2025/03/amazon-s3-reduces-pricing-object-tagging/>
- Performance best practices: <https://docs.aws.amazon.com/AmazonS3/latest/userguide/optimizing-performance.html>
- Performance design patterns: <https://docs.aws.amazon.com/AmazonS3/latest/userguide/optimizing-performance-design-patterns.html>
