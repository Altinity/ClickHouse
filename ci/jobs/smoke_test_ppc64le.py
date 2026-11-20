#!/usr/bin/env python3
"""
ppc64le smoke test.

Runs the cross-built binary with SELECT 1. The AMD func tester cannot exec a
powerpc64le ELF, so qemu-user is registered for the job and the binary runs in
a ppc64le Debian container. The self-extracting wrapper execs the unpacked
binary, which the registered binfmt handler picks up.
"""

from pathlib import Path

from ci.praktika.result import Result
from ci.praktika.utils import Shell, Utils

TEMP_DIR = Path(f"{Utils.cwd()}/ci/tmp")
BINARY_PATH = TEMP_DIR / "clickhouse"
DEBIAN_IMAGE = "debian:bookworm"
QEMU_IMAGE = "multiarch/qemu-user-static"


def main():
    stopwatch = Utils.Stopwatch()
    test_results = []

    def prepare_binary():
        if not BINARY_PATH.is_file():
            print(f"Missing binary {BINARY_PATH}")
            return False
        return Shell.check(f"chmod +x {BINARY_PATH}", verbose=True)

    test_results.append(
        Result.from_commands_run(
            name="Prepare binary",
            command=prepare_binary,
        )
    )

    if test_results[-1].is_ok():
        test_results.append(
            Result.from_commands_run(
                name="Register qemu-ppc64le",
                command=f"docker run --rm --privileged {QEMU_IMAGE} --reset -p yes",
            )
        )

    if test_results[-1].is_ok():
        test_results.append(
            Result.from_commands_run(
                name="Execute SELECT 1",
                command=(
                    "docker run --rm --platform linux/ppc64le "
                    f"--volume={TEMP_DIR}:/work "
                    f"{DEBIAN_IMAGE} "
                    "/work/clickhouse local --query 'SELECT 1'"
                ),
                with_info=True,
            )
        )

    Result.create_from(results=test_results, stopwatch=stopwatch).complete_job()


if __name__ == "__main__":
    main()
