# Standalone CAS loading regression

Run only in an isolated Linux Docker environment with synthetic data. Docker
Compose creates private test storage, Keeper, writer, reader and fault proxies.
No production credentials or endpoints are used. This is a standalone reproduction,
not yet wired into the upstream pytest integration runner.

```sh
cd tests/integration/test_cas_load_recovery
export CLICKHOUSE_IMAGE=your-locally-built-clickhouse-image
python3 prepare.py
docker compose up -d store keeper writer-proxy reader-proxy
python3 lab.py init
python3 lab.py rls --role writer
python3 lab.py rls --role reader
python3 lab.py loading --role writer --expect-recovery
python3 lab.py loading --role reader --expect-recovery
python3 lab.py loading --role writer --expect-recovery
python3 lab.py shutdown --role writer
docker compose down -v
```

Always run `docker compose down -v` after saving `.lab/results.jsonl`, including
on test failure. For the unmodified control, start a fresh environment and run only
`python3 lab.py loading --role writer` after `init`. It requires the cached
`ASYNC_LOAD_WAIT_FAILED`/`NETWORK_ERROR` after storage recovery; destroy that
environment before running another control. The failed writer cannot be reused.
To check terminal refusal, run `python3 lab.py terminal --role writer` after a
successful recovery, then destroy the environment (do not reuse it for shutdown).
The standalone scenario was validated on ARM64 Linux; it is not automatically run by the upstream pytest runner.
