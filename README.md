<div align=center>

[![Website](https://img.shields.io/website?up_message=AVAILABLE&down_message=DOWN&url=https://docs.altinity.com/altinitystablebuilds&style=for-the-badge)](https://docs.altinity.com/altinitystablebuilds/)
[![Apache 2.0 License](https://img.shields.io/badge/license-Apache%202.0-blueviolet?style=for-the-badge)](https://www.apache.org/licenses/LICENSE-2.0)

<picture align=center>
    <source media="(prefers-color-scheme: dark)" srcset="/docs/logo_horizontal_blue_white.png">
    <source media="(prefers-color-scheme: light)" srcset="/docs/logo_horizontal_blue_black.png">
    <img alt="Altinity company logo" src="/docs/logo_horizontal_blue_black.png">
</picture>

</div>

<h1>Altinity Antalya</h1>

- [github releases](https://github.com/Altinity/ClickHouse/releases?q=altinityantalya&expanded=false)
- [docker images](https://hub.docker.com/r/altinity/clickhouse-server/tags?page=1&name=altinityantalya)
- [packages](https://builds.altinity.cloud/#altinityantalya)
<details><summary><h3>Feature comparison matrix</h2></summary>

| Feature | Category | Altinity PR | First Altinity Release | ClickHouse PR | First ClickHouse Release |
| ------- | -------- | :---: | :---: | :---: | :---: |
| Setting object_storage_max_nodes | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/677 | 25.2.2 |  |  |
| Rendezvous hashing filesystem cache | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/709 | 25.2.2 | https://github.com/ClickHouse/ClickHouse/pull/77326 |  |
| Better S3 URL parsing for Hive partitioning | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/700 | 25.2.2 | https://github.com/ClickHouse/ClickHouse/pull/78185 | 25.5.1 |
| s3Cluster hive partitioning for old analyzer | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/703 | 25.2.2 |  |  |
| Convert functions with object_storage_cluster setting to cluster functions | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/712 | 25.2.2 |  |  |
| General engine definition for Iceberg tables | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/675 | 25.2.2 | https://github.com/ClickHouse/ClickHouse/pull/77125 |  |
| Iceberg metadata files cache | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/733 | 25.2.2 | https://github.com/ClickHouse/ClickHouse/pull/77156  | 25.5.1 |
| RBAC for S3 | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/688 | 25.2.2 |  |  |
| Parquet file metadata caching: clear cache | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/713 | 25.2.2 |  |  |
| Auxiliary autodiscovery | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/531 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/71911 | 24.11.1 |
| Fix remote call of s3Cluster function | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/583 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/72625 |  |
| s3Cluster hive partitioning | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/584 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/73910 |  |
| Cluster auto discovery | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/629 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/76001 | 25.3.1 |
| Alternative syntax for object storage cluster functions | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/592 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/70659 | 25.3.1 |
| Limit parsing threads for distibuted case | CLUSTER QUERIES | https://github.com/Altinity/ClickHouse/pull/648 | 24.12.2 |  |  |
| Iceberg REST Catalog integration | DATABASE CATALOGS | https://github.com/Altinity/ClickHouse/pull/575 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/71542 | 24.12.1 |
| Parquet: bloom filters support | READ PERFORMANCE | same as upstream => | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/62966 | 24.10.1 |
| Parquet: page header v2 support on native reader | READ PERFORMANCE | same as upstream => | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/70807 | 24.10.1 |
| Parquet: boolean support on native reader | READ PERFORMANCE | same as upstream => | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/71055 | 24.11.1 |
| Parquet: merge bloom filter and min/max evaluation | READ PERFORMANCE | https://github.com/Altinity/ClickHouse/pull/590 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/71383 | 25.2.1 |
| Parquet: Int logical type support on native reader | READ PERFORMANCE | https://github.com/Altinity/ClickHouse/pull/589 | 24.12.2 | https://github.com/ClickHouse/ClickHouse/pull/72105 | 25.1.1 |
| Parquet file metadata caching | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/586 | 24.12.2 |  |  |
| Distributed request to tables with Object Storage Engines | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/615 | 24.12.2 |  |  |
| Parquet file metadata caching: use cache for parquetmetadata format | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/636 | 24.12.2 |  |  |
| Parquet file metadata caching: turn cache on by default | TABLE ENGINES | https://github.com/Altinity/ClickHouse/pull/669, https://github.com/Altinity/ClickHouse/pull/674 | 24.12.2 |  |  |
</details></details>

**Altinity Antalya** is an experimental build of ClickHouse® incorporating features and fixes from the future Altinity Stable Build releases. It is **Not for production use**.

<h1>Altinity Stable Builds®</h1>

- [github releases](https://github.com/Altinity/ClickHouse/releases?q=altinitystable&expanded=false)
- [docker images](https://hub.docker.com/r/altinity/clickhouse-server/tags?page=1&name=altinitystable)
- [packages](https://builds.altinity.cloud/#altinitystable)

**Altinity Stable Builds** are releases of ClickHouse® that undergo rigorous testing to verify they are secure and ready for production use. Among other things, they are: 

* Supported for three years
* Validated against client libraries and visualization tools
* Tested for cloud use, including Kubernetes
* 100% open source and 100% compatible with ClickHouse upstream builds
* Available in FIPS-compatible versions

**We encourage you to use Altinity Stable Builds whether you're an Altinity Support customer or not.**

## Acknowledgement

We at Altinity, Inc. are thoroughly grateful to the worldwide ClickHouse community, including the core committers who make ClickHouse the world's best analytics database. 

## What should I do if I find a bug in an Altinity Stable Build?

ClickHouse’s thousands of core features are all well-tested and stable. To maintain that stability, Altinity Stable Builds are built with a CI system that runs tens of thousands of tests against every commit. But of course, things can always go wrong. If that happens, let us know! **We stand behind our work.**

### If you're an Altinity customer:

1. [Contact Altinity support](https://docs.altinity.com/support/) to file an issue.

### If you're not an Altinity customer:

1. Try to upgrade to the latest bugfix release. If you’re using v23.8.8 but you know that v23.8.11.29 exists, start by upgrading to the bugfix. Upgrades to the latest maintenance releases are smooth and safe.
2. Look for similar issues in the [Altinity/ClickHouse](https://github.com/Altinity/ClickHouse/issues) or [ClickHouse/ClickHouse](https://github.com/ClickHouse/ClickHouse/issues) repos; it's possible the problem has been logged and a fix is on the way.
3. If you can reproduce the bug, try to isolate it. For example, remove pieces of a failing query one by one, creating the simplest scenario where the error still occurs. Creating [a minimal reproducible example](https://stackoverflow.com/help/minimal-reproducible-example) is a huge step towards a solution.
4. [File an issue](https://github.com/Altinity/ClickHouse/issues/new/choose) in the Altinity/ClickHouse repo.

## Useful Links

* [Release notes](https://docs.altinity.com/releasenotes/altinity-stable-release-notes/) - Complete details on the changes and fixes in each Altinity Stable Build release
* [Builds page](https://builds.altinity.cloud/) - Download and installation instructions for Altinity Stable Builds
* [Dockerhub page](https://hub.docker.com/r/altinity/clickhouse-server) - Home of the Altinity Stable Build container images
* [Knowledge base](https://kb.altinity.com/) - Insight, knowledge, and advice from the Altinity Engineering and Support teams
* [Documentation](https://docs.altinity.com/altinitystablebuilds/) - A list of current releases and their lifecycles
* [Altinity support for ClickHouse](https://altinity.com/clickhouse-support/) - The best ClickHouse support in the industry, delivered by the most knowledgeable ClickHouse team in the industry
* [Altinity administrator training for ClickHouse](https://altinity.com/clickhouse-training/) - Understand how ClickHouse works, not just how to use it
* [Altinity blog](https://altinity.com/blog/) - The latest news on Altinity's ClickHouse-related projects, release notes, tutorials, and more
* [Altinity YouTube channel](https://www.youtube.com/@AltinityB) - ClickHouse tutorials, webinars, conference presentations, and other useful things
* [Altinity Slack channel](https://altinitydbworkspace.slack.com/join/shared_invite/zt-1togw9b4g-N0ZOXQyEyPCBh_7IEHUjdw#/shared-invite/email) - The Altinity Slack channel

<hr>

*©2025 Altinity Inc. All rights reserved. Altinity®, Altinity.Cloud®, and Altinity Stable Builds® are registered trademarks of Altinity, Inc. ClickHouse® is a registered trademark of ClickHouse, Inc.*
