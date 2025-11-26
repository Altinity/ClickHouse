# Local Testing

Upstream provides a utility `praktika`, which is used to run tests both locally and in CI.

## Stateless

### Suite

Fast Test is self-contained and can be run with no extra setup.
```sh
python -m ci.praktika run "Fast test"
```

The stateless suites expect an executable `clickhouse` binary in `ci/tmp`
I recommend not running suites with the `s3 storage` option as that requires extra setup.
```sh
python -m ci.praktika run "Stateless tests (amd_debug, sequential)"
```

Logs can be found in `ci/tmp` and `ci/tmp/var/log/clickhouse-server`.

### One Test

Running a single test can be done with bare `clickhouse-test` or with the `praktika` utility.

#### clickhouse-test

https://clickhouse.com/docs/development/tests#running-a-test-locally

Start Clickhouse server,

```sh
cd ci/tmp
./clickhouse server
```

then, in another window:

```sh
tests/clickhouse-test -b ci/tmp/clickhouse 03222_pr_asan_index_granularity
```

Logs will get written into the `tests/queries/0_stateless` folder.

#### praktika

```sh
python -m ci.praktika run "Stateless tests (amd_debug, sequential)" --test 01107_atomic_db_detach_attach
```

You must specify the name of a job from the PR workflow to instruct praktika on the necessary configuration.

## Integration

### Suite

https://github.com/ClickHouse/ClickHouse/blob/master/tests/integration/README.md

> The runner searches in this order and uses the first found:
> ./ci/tmp/clickhouse
> ./build/programs/clickhouse
> ./clickhouse

```sh
python -m ci.praktika run "Integration tests (amd_binary, 4/5)"
```

Logs can be found in `ci/tmp` and `ci/tmp/var/log/clickhouse-server`.

### One Test

```sh
cd tests/integration
./runner --docker-image-version 72c567235d2f7658765b --docker-compose-images-tags=altinityinfra/integration-test:9c789665fa1f2c3b60a8 --binary ../../ci/tmp/clickhouse 'test_storage_hudi/test.py::test_types'
```

Docker images may or may not need to be specified. Get the tags by examining ci logs.
Search logs for `integration-test`, look for `"altinityinfra/integration-tests-runner": "72c567235d2f7658765b"`
and `"altinityinfra/integration-test": "9c789665fa1f2c3b60a8"`

Logs are written to the local dir, which is `tests/integration` in this example.

Note: `runner` has been removed from 25.9 onwards.
