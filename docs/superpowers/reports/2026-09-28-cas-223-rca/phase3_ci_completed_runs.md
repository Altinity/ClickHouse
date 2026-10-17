# CAS-223: evidence from completed (not cancelled) sanitizer CAS runs

Paths relative to `tmp/cas223/`.

## Which runs have system-table dumps

- Run 36347426173 (PR #2440): the stateless jobs upload system-table dumps only when they FAIL. The successful ASan CAS shards
  and the TSan S3 shards uploaded only server logs and `job.log`; the cancelled TSan/MSan CAS shards uploaded nothing
  (`artifacts_listing_pr2440.tsv`; the only dump set there is `amd_msan, WasmEdge, parallel, 3/4`, a failure).
- In the last 200 workflow runs since 2026-09-01 (`runs_sep.tsv`, `cas_sanitizer_jobs.tsv`) no TSan or MSan CAS stateless job
  completed: they are skipped everywhere except run 36347426173, where they were cancelled. The completed sanitizer CAS jobs are
  ASan only. The slowest two, both FAILED and therefore with full dumps: run 34105324590 (PR #2320, sha `9091be4a672`,
  2026-09-07), `amd_asan_ubsan, cas s3 storage, parallel 1/2` (303 min, job 101730936890) and `2/2` (266 min).
  Files per job (`artifacts_listing_run34105324590.tsv`): `metric_log`, `query_log`, `query_metric_log`, `part_log`, `trace_log`,
  `zookeeper_log`, `error_log`, `transactions_info_log`, server log, `rustfs.log`, `minio.log`, `job.log`. No `cas_log` or `cas_gc_log`.
- Analysed: shard 1/2, downloaded to `ci0907/`. Its code is three weeks older than the PR head (no `CASRequestAttempt` counter,
  no drain-starvation message), so it is a specimen of the ramp, not of the exact PR-head mechanism.

## Timeline (`ci0907/timeline_15min.tsv`, from `metric_log`)

Queries per 15 min fall from 20,393-24,546 to 6,498-12,961; the server uses 2.6-7.2 of 16 cores throughout (not CPU-bound);
`GlobalThread` 1465 -> 1688; `MemoryTracking` 15-19 GB flat; catalog GETs (`CASOtherGet`) ~19-33 k per 15 min, flat;
`CASRefQueueWaitMicroseconds` 2,515 s -> 16,173 s per 15 min (about 3 -> 18 threads waiting in per-table ref-log queues on average).

## INSERT decomposition (`ci0907/insert_decomposition.txt`, `query_log`, INSERTs with `CASRootPut > 0`)

Mean INSERT 830 ms -> 3.4-4.3 s. Request counts per INSERT constant (~11 PUT, ~9 GET, 2.6-3.5 catalog GETs). S3 time per write
6.7 -> ~10 ms, per read 1.6 -> ~3.5 ms. Ref-queue wait per INSERT 159 ms -> 1,000-1,442 ms: the dominant growing term.

## GC (`ci0907/gc_rounds.txt`, server log)

Default pool: round length grows from ~1 min to 15 min (round 33), 30 min (34), 55 min (35), 2 h 4 min (36);
`deleted` plateaus at 4,920-4,953 per round while `candidates` reach 42,512. No catalog-erase failure message in this older build.

## trace_log: top stacks, early (14:00-14:45) vs late (17:00-18:15)

Symbolized against the matching CI binary `build_amd_asan_ubsan/clickhouse` of sha `9091be4a672` (47,754 distinct addresses,
47,118 resolved; `ci0907/syms.tsv`). Stack key = innermost 4 frames after dropping libc/std/sanitizer/thread-pool frames
(`ci0907/agg.sql`, output `ci0907/agg_out.tsv`). Samples: CPU 14,536 early / 27,206 late; Real 2,230,482 / 4,300,449.

### Inclusive share of CPU samples

| function | early % | late % |
|---|---|---|
| `DB::Cas::CasRefCatalog::casUpdate` | 0.15 | 0.23 |
| `DB::Cas::CasRefCatalog::read` | 7.29 | 20.29 |
| `DB::Cas::CasRefLedger::appendRefOpsOnRuntime` | 10.08 | 22.73 |
| `DB::Cas::CasRefLedger::commitRefChunk` | 9.03 | 21.31 |
| `DB::Cas::CasRefLedger::flushRefBatch` | 10.06 | 22.68 |
| `DB::Cas::CatalogLifeIndex::CatalogLifeIndex` | 0.42 | 1.78 |
| `DB::Cas::Gc::runRegularRound` | 7.75 | 7.36 |
| `DB::Cas::decodeRefCatalog` | 3.14 | 13.26 |
| `DB::DatabaseCatalog::dropTableFinally` | 3.65 | 5.19 |
| `DB::MergeTreeData::clearPartsFromFilesystemImpl` | 6.45 | 14.31 |
| `DB::ReadBufferFromS3::nextImpl` | 8.39 | 10.08 |
| `DB::WriteBufferFromS3::finalizeImpl` | 1.79 | 2 |

### Top 15 CPU stacks by late-window share (early %, late %, delta)

| early % | late % | delta | stack (innermost first) |
|---|---|---|---|
| 0.63 | 2.98 | 2.35 | `DB::Cas::readLine <- DB::Cas::decodeRefCatalog <- DB::Cas::CasRefCatalog::read <- DB::Cas::CasRefLedger::commitRefChunk` |
| 0.41 | 2.56 | 2.15 | `DB::Cas::JsonObjectReader::nextKey <- DB::Cas::decodeRefCatalog <- DB::Cas::CasRefCatalog::read <- DB::Cas::CasRefLedger::commitRefChunk` |
| 0.3 | 1.58 | 1.28 | `DB::Cas::decodeRefCatalog <- DB::Cas::CasRefCatalog::read <- DB::Cas::CasRefLedger::commitRefChunk <- DB::Cas::CasRefLedger::flushRefBatch` |
| 1.02 | 1.43 | 0.41 | `Aws::Client::AWSAuthV4Signer::SignRequestWithCreds <- Aws::Client::AWSAuthV4Signer::SignRequest <- bool smithy::components::tracing::TracingUtils::MakeCallWithT` |
| 0.23 | 1.28 | 1.05 | `DB::Cas::CatalogLifeIndex::CatalogLifeIndex <- DB::Cas::CasRefCatalog::read <- DB::Cas::CasRefLedger::commitRefChunk <- DB::Cas::CasRefLedger::flushRefBatch` |
| 1.03 | 1.04 | 0.02 | `DB::KeeperRequestDispatcher::dispatchThread <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std` |
| 1.02 | 1.02 | 0 | `DB::KeeperRequestDispatcher::responseThread <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std` |
| 0.21 | 0.94 | 0.73 | `void DB::readJSONStringInto<std::__1::basic_string<char, std::__1::char_traits, std::__1::allocator>, void> <- DB::Cas::JsonObjectReader::readString <- DB::Cas:` |
| 0.14 | 0.8 | 0.66 | `DB::Cas::CasRefCatalog::Snapshot::~Snapshot <- DB::Cas::CasRefLedger::commitRefChunk <- DB::Cas::CasRefLedger::flushRefBatch <- DB::Cas::CasRefLedger::runRefQue` |
| 0.69 | 0.73 | 0.04 | `Allocator::free <- DB::Cas::CaInlineWriteBuffer::~CaInlineWriteBuffer <- DB::Cas::CaInlineWriteBuffer::~CaInlineWriteBuffer <- DB::WriteBufferFromFileDecorator:` |
| 0.19 | 0.69 | 0.5 | `void DB::readJSONStringInto<std::__1::basic_string<char, std::__1::char_traits, std::__1::allocator>, void> <- DB::Cas::JsonObjectReader::readString <- DB::Cas:` |
| 0.18 | 0.65 | 0.47 | `DB::Cas::CatalogEntry* std::__1::construct_at[abi:fqe220101] <- DB::Cas::CatalogEntry* std::__1::__uninitialized_allocator_copy_impl[abi:fqe220101]<std::__1::al` |
| 0.64 | 0.64 | 0 | `Allocator::free <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std::__1::default_delete>, void` |
| 0.01 | 0.58 | 0.58 | `DB::PODArray<unsigned long, 4096ul, Allocator, 63ul, 64ul>::operator[] <- COW::immutable_ptr DB::ColumnVector::indexImpl <- DB::ColumnVector::permute <- COW::im` |
| 0.59 | 0.58 | -0.01 | `DB::AsynchronousInsertQueue::processBatchDeadlines <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_stru` |

### Top 15 CPU stacks by early-window share

| early % | late % | delta | stack (innermost first) |
|---|---|---|---|
| 2.17 | 0.13 | -2.05 | `DB::PODArray<unsigned long, 4096ul, Allocator, 63ul, 64ul>::operator[] <- DB::ComparatorHelperImpl<DB::ColumnString::ComparatorBase,  <- unsigned long ipnsort::` |
| 1.46 | 0.25 | -1.21 | `DB::PODArray<unsigned short, 4096ul, Allocator, 63ul, 64ul>::operator[] <- COW::immutable_ptr DB::ColumnVector::indexImpl <- COW::immutable_ptr DB::selectIndexI` |
| 1.15 | 0 | -1.15 | `DB::PODArray<unsigned long, 4096ul, Allocator, 63ul, 64ul>::operator[] <- DB::ColumnString::getDataAt <- DB::UniqVariadicHash::apply <- DB::IAggregateFunctionHe` |
| 1.03 | 0.12 | -0.9 | `DB::PODArray<unsigned short, 4096ul, Allocator, 63ul, 64ul>::operator[] <- DB:: <- DB::ColumnLowCardinality::Dictionary::compact <- DB::ColumnLowCardinality::cu` |
| 1.03 | 1.04 | 0.02 | `DB::KeeperRequestDispatcher::dispatchThread <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std` |
| 1.02 | 1.02 | 0 | `DB::KeeperRequestDispatcher::responseThread <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std` |
| 1.02 | 1.43 | 0.41 | `Aws::Client::AWSAuthV4Signer::SignRequestWithCreds <- Aws::Client::AWSAuthV4Signer::SignRequest <- bool smithy::components::tracing::TracingUtils::MakeCallWithT` |
| 0.89 | 0.46 | -0.43 | `DB::PODArray<char8_t, 4096ul, Allocator, 63ul, 64ul>::operator[] <- COW::immutable_ptr DB::ColumnVector::indexImpl <- COW::immutable_ptr DB::selectIndexImpl<DB:` |
| 0.73 | 0.58 | -0.15 | `DB::ColumnDescription::operator= <- DB::ColumnDescription::ColumnDescription <- void boost::multi_index::detail::copy_map<boost::multi_index::detail::sequenced_` |
| 0.69 | 0.73 | 0.04 | `Allocator::free <- DB::Cas::CaInlineWriteBuffer::~CaInlineWriteBuffer <- DB::Cas::CaInlineWriteBuffer::~CaInlineWriteBuffer <- DB::WriteBufferFromFileDecorator:` |
| 0.67 | 0 | -0.67 | `DB::IColumn::assertTypeEquality <- DB::IColumnHelper<DB::ColumnVector, DB::ColumnFixedSizeHelper>::scatter <- DB::FunctionArrayFold::executeImpl <- DB::Function` |
| 0.67 | 0 | -0.67 | `DB::PODArray<unsigned long, 4096ul, Allocator, 63ul, 64ul>::operator[] <- DB::ColumnVector::doInsertFrom <- DB::IColumnHelper<DB::ColumnVector, DB::ColumnFixedS` |
| 0.67 | 0 | -0.67 | `DB::PODArray<unsigned long, 4096ul, Allocator, 63ul, 64ul>::operator[] <- DB::IColumnHelper<DB::ColumnVector, DB::ColumnFixedSizeHelper>::scatter <- DB::Functio` |
| 0.64 | 0.64 | 0 | `Allocator::free <- void* std::__1::__thread_proxy[abi:fqe220101]<std::__1::tuple<std::__1::unique_ptr<std::__1::__thread_struct, std::__1::default_delete>, void` |
| 0.63 | 2.98 | 2.35 | `DB::Cas::readLine <- DB::Cas::decodeRefCatalog <- DB::Cas::CasRefCatalog::read <- DB::Cas::CasRefLedger::commitRefChunk` |

### Real (wall-clock, all threads)

`Real` samples are dominated by idle pool threads (`BackgroundSchedulePool::threadFunction` 65.1 % -> 51.5 %, filesystem-cache
download threads 10.7 % -> 13.3 %); CAS frames are a small share of all threads' wall time. Inclusive: `appendRefOpsOnRuntime`
0.33 % -> 1.46 %, `CasRefCatalog::read` 0.07 % -> 0.60 %, `clearPartsFromFilesystemImpl` 0.15 % -> 0.70 %,
`WriteBufferFromS3::finalizeImpl` 0.26 % -> 0.57 %. Full top-15 lists in `ci0907/agg_out.tsv`.

## Reading

On CI data, the CPU the server spends grows toward the whole-catalog read under the ref-log queue leader:
`CasRefCatalog::read` 7.3 % -> 20.3 % of CPU samples, `decodeRefCatalog` 3.1 % -> 13.3 %, `CatalogLifeIndex` 0.4 % -> 1.8 %;
the three largest late CPU stacks are all catalog decode under `commitRefChunk` (each +1.3 to +2.4 points), while query-execution
stacks shrink. The leader holds the per-table queue while it decodes, which is how this CPU becomes the ref-queue wait that grows
7-9x per INSERT. This is RC2 of `RCA.md` observed in CI. The server is not CPU-saturated (3-5 of 16 cores), so the wait, not CPU
exhaustion, is what slows the tests. The catalog size itself is not in the dumps (no `cas_log`/`cas_gc_log`), so the link from
GC lag to catalog growth rests on the GC round lengths above and on the local reproduction.
