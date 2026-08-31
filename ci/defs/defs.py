from praktika import Artifact, Docker, Secret
from praktika.utils import MetaClasses, Utils
from settings import altinity_overrides

# i.e. "ClickHouse/ci/tmp"
TEMP_DIR = f"{Utils.cwd()}/ci/tmp"  # == _Settings.TEMP_DIR != env_helper.TEMP_PATH

SYNC = "Altinity sync"

GH_AUTH_TRUSTED_LAMBDA_NAME = "mint-token-trusted-lambda-terraform"

S3_BUCKET_NAME = altinity_overrides.S3_BUCKET_NAME
S3_REPORT_BUCKET_NAME = altinity_overrides.S3_REPORT_BUCKET_NAME
S3_BUCKET_HTTP_ENDPOINT = altinity_overrides.S3_BUCKET_HTTP_ENDPOINT
S3_REPORT_BUCKET_HTTP_ENDPOINT = altinity_overrides.S3_REPORT_BUCKET_HTTP_ENDPOINT


class RunnerLabels:
    CI_SERVICES = "ci_services"
    CI_SERVICES_EBS = "ci_services_ebs"
    BUILDER_AMD = ["self-hosted", "altinity-on-demand", "altinity-builder"]
    BUILDER_ARM = ["self-hosted", "altinity-on-demand", "altinity-builder"]
    FUNC_TESTER_AMD = ["self-hosted", "altinity-on-demand", "altinity-func-tester"]
    FUNC_TESTER_ARM = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
    ]
    AMD_LARGE = ["self-hosted", "altinity-on-demand", "altinity-builder", "64g"]
    ARM_LARGE = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64", "16c"]
    ARM_LARGE_STORAGE = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64"]
    AMD_MEDIUM = ["self-hosted", "altinity-on-demand", "altinity-func-tester", "16c"]
    ARM_MEDIUM = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64", "16c"]
    AMD_MEDIUM_CPU = ["self-hosted", "altinity-on-demand", "altinity-func-tester", "16c"]
    ARM_MEDIUM_CPU = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
        "16c",
    ]
    AMD_MEDIUM_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester"]
    ARM_MEDIUM_MEM = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
    ]
    AMD_SMALL = ["self-hosted", "altinity-on-demand", "altinity-func-tester"]
    ARM_SMALL = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64"]
    AMD_SMALL_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester", "32g"]
    ARM_SMALL_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64", "32g"]
    MACOS_ARM_SMALL = ["self-hosted", "macos_m2"]
    MACOS_AMD_SMALL = ["self-hosted", "amd_macos_m1"]
    STYLE_CHECK_AMD = ["self-hosted", "altinity-on-demand", "altinity-style-checker"]
    STYLE_CHECK_ARM = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-style-checker-aarch64",
    ]


class CIFiles:
    UNIT_TESTS_RESULTS = f"{TEMP_DIR}/unit_tests_result.json"
    UNIT_TESTS_BIN = f"{TEMP_DIR}/build/src/unit_tests_dbms"


BASE_BRANCH = altinity_overrides.MAIN_BRANCH

azure_secret = Secret.Config(
    name="azure_connection_string",
    type=Secret.Type.AWS_SSM_PARAMETER,
)

SECRETS = [
    Secret.Config(
        name=altinity_overrides.DOCKERHUB_SECRET,
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name=altinity_overrides.SECRET_CI_DB_URL,
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name=altinity_overrides.SECRET_CI_DB_USER,
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name=altinity_overrides.SECRET_CI_DB_PASSWORD,
        type=Secret.Type.GH_SECRET,
    ),
    # azure_secret,
    Secret.Config(
        name="AWS_ACCESS_KEY_ID",
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name="AWS_SECRET_ACCESS_KEY",
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name="GPG_BINARY_SIGNING_KEY",
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name="GPG_BINARY_SIGNING_PASSPHRASE",
        type=Secret.Type.GH_SECRET,
    ),
]

# In-region AWS Ubuntu mirror. Canonical's archive.ubuntu.com (amd64) /
# ports.ubuntu.com (arm64) are frequently unreachable over IPv4 from the runners
# and have no IPv6 route; the in-region mirror is reachable and fast. Passed as
# build args to the Ubuntu-based images, whose Dockerfiles keep canonical
# defaults so local builds are unchanged.
#APT_MIRROR_BUILD_ARGS = {
#    "apt_archive": "http://us-east-1.ec2.archive.ubuntu.com",
#    "apt_ports_archive": "http://us-east-1.ec2.ports.ubuntu.com",
# }
APT_MIRROR_BUILD_ARGS = {} # NOTE (strtgbb): Our runners are not primarily on aws

DOCKERS = [
    Docker.Config(
        name="altinityinfra/style-test",
        path="./ci/docker/style-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/fasttest",
        path="./ci/docker/fasttest",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/binary-builder",
        path="./ci/docker/binary-builder",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/fasttest"],
    ),
    Docker.Config(
        name="altinityinfra/wasm-builder",
        path="./ci/docker/wasm-builder",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/stateless-test",
        path="./ci/docker/stateless-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/cctools",
        path="./ci/docker/cctools",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/fasttest"],
    ),
    Docker.Config(
        name="altinityinfra/utils",
        path="./ci/docker/utils",
        platforms=[Docker.Platforms.AMD],
        depends_on=["altinityinfra/fasttest"],
    ),
    Docker.Config(
        name="altinityinfra/test-base",
        path="./ci/docker/test-base",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/stress-test",
        path="./ci/docker/stress-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/stateless-test"],
    ),
    Docker.Config(
        name="altinityinfra/fuzzer",
        path="./ci/docker/fuzzer",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/performance-comparison",
        path="./ci/docker/performance-comparison",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/keeper-jepsen-test",
        path="./ci/docker/keeper-jepsen-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/server-jepsen-test",
        path="./ci/docker/server-jepsen-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/integration-test",
        path="./ci/docker/integration/base",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/integration-tests-runner",
        path="./ci/docker/integration/runner",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/test-base"],
    ),
    Docker.Config(
        name="altinityinfra/integration-test-with-unity-catalog",
        path="./ci/docker/integration/clickhouse_with_unity_catalog",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/integration-test-with-hms",
        path="./ci/docker/integration/clickhouse_with_hms_catalog",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/integration-helper",
        path="./ci/docker/integration/helper_container",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/kerberos-kdc",
        path="./ci/docker/integration/kerberos_kdc",
        platforms=[Docker.Platforms.AMD],
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/test-mysql80",
        path="./ci/docker/integration/mysql80",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/test-mysql57",
        path="./ci/docker/integration/mysql57",
        platforms=Docker.Platforms.AMD,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/mysql-golang-client",
        path="./ci/docker/integration/mysql_golang_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/mysql-java-client",
        path="./ci/docker/integration/mysql_java_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/mysql-js-client",
        path="./ci/docker/integration/mysql_js_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/arrowflight-server-test",
        path="./ci/docker/integration/arrowflight",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/dotnet-client",
        path="./ci/docker/integration/dotnet_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/mysql-php-client",
        path="./ci/docker/integration/mysql_php_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/nginx-dav",
        path="./ci/docker/integration/nginx_dav",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/postgresql-java-client",
        path="./ci/docker/integration/postgresql_java_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/python-bottle",
        path="./ci/docker/integration/resolver",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/s3-proxy",
        path="./ci/docker/integration/s3_proxy",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/docs-builder",
        path="./ci/docker/docs-builder",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/install-deb-test",
        path="./ci/docker/install/deb",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/install-rpm-test",
        path="./ci/docker/install/rpm",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/sqlancer-test",
        path="./ci/docker/sqlancer-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
    Docker.Config(
        name="altinityinfra/mysql_dotnet_client",
        path="./ci/docker/integration/mysql_dotnet_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
        #build_args=APT_MIRROR_BUILD_ARGS,
    ),
]


class BuildTypes(metaclass=MetaClasses.WithIter):
    AMD_DEBUG = "amd_debug"
    AMD_RELEASE = "amd_release"
    # sccache-warmup variants of the release builds (MasterCI only): PR-style
    # cmake flags (no official-build flag, debug symbols stripped, no PGO/BOLT),
    # but built on master so the shared sccache is populated read-write for
    # read-only PR builds to reuse. See build_clickhouse.py and
    # PR_CACHE_WARMUP_BUILD_TYPES.
    AMD_RELEASE_PR_CACHE_WARMUP = "amd_release_pr_cache_warmup"
    AMD_BINARY = "amd_binary"
    AMD_ASAN_UBSAN = "amd_asan_ubsan"
    AMD_TSAN = "amd_tsan"
    AMD_MSAN = "amd_msan"
    ARM_RELEASE = "arm_release"
    ARM_RELEASE_PR_CACHE_WARMUP = "arm_release_pr_cache_warmup"
    ARM_DEBUG = "arm_debug"
    ARM_ASAN_UBSAN = "arm_asan_ubsan"
    ARM_TSAN = "arm_tsan"
    ARM_MSAN = "arm_msan"
    LLVM_COVERAGE_BUILD = "llvm_coverage_build"
    PER_TEST_COVERAGE = "amd_llvm_coverage_per_test"
    AMD_COVERAGE = "amd_coverage"
    ARM_BINARY = "arm_binary"
    AMD_TIDY = "amd_tidy"
    ARM_TIDY = "arm_tidy"
    AMD_DARWIN = "amd_darwin"
    ARM_DARWIN = "arm_darwin"
    ARM_V80COMPAT = "arm_v80compat"
    AMD_FREEBSD = "amd_freebsd"
    PPC64LE = "ppc64le"
    AMD_COMPAT = "amd_compat"
    AMD_MUSL = "amd_musl"
    RISCV64 = "riscv64"
    S390X = "s390x"
    LOONGARCH64 = "loongarch64"
    # WebAssembly (wasm64, through Emscripten). Experimental: the multicall `clickhouse`
    # binary builds, and `clickhouse local` runs under Node.js >= 24 and in browsers.
    # The CI job pins the binary target (see build_clickhouse.py).
    WASM64 = "wasm64"
    # The standalone WebAssembly build of just the SQL parser (`utils/wasm-parser`), for a
    # browser. A CMake project of its own rather than a target of this tree, with its own
    # toolchain and its own job script - see `build_wasm_parser.py`.
    WASM_PARSER = "wasm_parser"
    ARM_FUZZERS = "arm_fuzzers"
    AMD_CFI = "amd_cfi"


class JobNames:
    DOCKER_BUILDS_ARM = "Dockers build (arm)"
    DOCKER_BUILDS_AMD = "Dockers build (amd)"
    STYLE_CHECK = "Style check"
    CODE_REVIEW = "Code Review"
    FAST_TEST = "Fast test"
    BUILD = "Build"
    UNITTEST = "Unit tests"
    STATELESS = "Stateless tests"
    STATEFUL = "Stateful tests"
    INTEGRATION = "Integration tests"
    STRESS = "Stress test"
    UPGRADE = "Upgrade check"
    PERFORMANCE = "Performance Comparison"
    COMPATIBILITY = "Compatibility check"
    SIGN_MACOS = "Sign macOS binary"
    DOCS_MINTLIFY = "Docs check (Mintlify)"
    CLICKBENCH = "ClickBench"
    DOCKER_SERVER = "Docker server image"
    DOCKER_KEEPER = "Docker keeper image"
    SQL_TEST = "SQLTest"
    SQL_LOGIC_TEST = "SQLLogic test"
    SQL_STORM_TEST = "SQLStorm test"
    SQLANCER = "SQLancer"
    # No "++": the job name becomes the GitHub Actions job id via
    # Utils.normalize_string, and '+' is not a valid id character.
    SQLANCER_PP = "SQLancerPP"
    LLVM_COVERAGE = "LLVM Coverage"
    PROMQL_COMPLIANCE = "PromQL Compliance"
    BUILD_PROFILE_DIFF = "Build profile diff"
    INSTALL_TEST = "Install packages"
    ASTFUZZER = "AST fuzzer"
    BUZZHOUSE = "BuzzHouse"
    BUILDOCKER = "BuildDockers"
    BUGFIX_VALIDATE = "Bugfix validation"
    # Per-arch bugfix validation jobs. Each runs the new/modified test on
    # master HEAD and on the PR, and reports one of three top-level statuses:
    #   * `OK`     : bug reproduced on master HEAD AND fixed on PR (validated)
    #   * `SKIPPED`: bug did not reproduce on master HEAD on this arch
    #                (no-repro: another arch can still validate)
    #   * `ERROR`  : infrastructure error / inconclusive run (no signal)
    # The runners (`ci/jobs/functional_tests.py`,
    # `ci/jobs/integration_test_job.py`) propagate `SKIPPED` to the top-level
    # `R` directly so the post-hook does not treat the no-repro case as
    # validated; see `invert_bugfix_validation_status`.
    # Per-arch jobs are configured with `allow_failure=True` so a genuine
    # `ERROR` (sanitizer assert, OOM, runner termination) does not block PR
    # merge on its own. The merge-blocking decision is made by the
    # `new_tests_check.py` post-hook, which uses strict `is_success` (`OK` or
    # `XFAIL`); `SKIPPED`/`ERROR`/`FAIL` per-arch jobs do NOT count as a
    # validation. The bug is considered validated as long as AT LEAST ONE
    # per-arch job is strict-success.
    BUGFIX_VALIDATE_FT_AMD = "Bugfix validation (functional tests, amd64)"
    BUGFIX_VALIDATE_FT_ARM = "Bugfix validation (functional tests, aarch64)"
    BUGFIX_VALIDATE_IT_AMD = "Bugfix validation (integration tests, amd64)"
    BUGFIX_VALIDATE_IT_ARM = "Bugfix validation (integration tests, aarch64)"
    # Unit-test (gtest) bugfix validation. Unlike the functional/integration
    # validators above, this is a single AMD-only job: it builds a merge-base
    # "before" `unit_tests_dbms` (AMD ASan+UBSan) in-job and reports
    # `OK`/`XFAIL`/`FAIL` directly, so it is not part of the per-arch
    # aggregation in `new_tests_check.py`.
    BUGFIX_VALIDATE_UT = "Bugfix validation (unit tests)"
    JEPSEN_KEEPER = "ClickHouse Keeper Jepsen"
    JEPSEN_SERVER = "ClickHouse Server Jepsen"
    LIBFUZZER_TEST = "libFuzzer tests"
    PARSER_MEMORY_CHECK = "Parser memory check"
    BUILD_TOOLCHAIN = "Build Toolchain (PGO, BOLT)"
    UPDATE_TOOLCHAIN_DOCKERFILE = "Update Toolchain Dockerfile"
    COLLECT_CLICKHOUSE_PROFILES = "Collect ClickHouse Profiles (PGO, BOLT)"
    CI_TESTS = "CI Tests"


class ToolSet:
    COMPILER_C = "clang-22"
    COMPILER_CPP = "clang++-22"

    COMPILER_CACHE = "sccache"
    COMPILER_CACHE_LEGACY = "sccache"


class ArtifactNames:
    CH_AMD_DEBUG = "CH_AMD_DEBUG"
    CH_AMD_LLVM_COVERAGE_BUILD = (
        "CH_AMD_LLVM_COVERAGE_BUILD"  # build with LLVM coverage enabled
    )
    CH_AMD_PER_TEST_COVERAGE_BUILD = (
        "CH_AMD_PER_TEST_COVERAGE_BUILD"  # build with LLVM coverage + per-test depth instrumentation
    )
    LLVM_COVERAGE_FILE = "LLVM_COVERAGE_FILE"  # .profdata file
    LLVM_COVERAGE_INFO_FILE = "LLVM_COVERAGE_INFO_FILE"  # .info file generated from .profdata, used for debugging coverage results
    CH_AMD_RELEASE = "CH_AMD_RELEASE"
    CH_AMD_RELEASE_STRIPPED = "CH_AMD_RELEASE_STRIPPED"
    CH_AMD_ASAN_UBSAN = "CH_AMD_ASAN_UBSAN"
    CH_AMD_TSAN = "CH_AMD_TSAN"
    CH_AMD_MSAN = "CH_AMD_MSAN"
    CH_AMD_BINARY = "CH_AMD_BINARY"
    CH_ARM_RELEASE = "CH_ARM_RELEASE"
    CH_ARM_RELEASE_STRIPPED = "CH_ARM_RELEASE_STRIPPED"
    CH_ARM_DEBUG = "CH_ARM_DEBUG"
    CH_ARM_ASAN_UBSAN = "CH_ARM_ASAN_UBSAN"
    CH_ARM_TSAN = "CH_ARM_TSAN"
    CH_ARM_MSAN = "CH_ARM_MSAN"

    CH_COV_BIN = "CH_COV_BIN"
    CH_ARM_BINARY = "CH_ARM_BIN"
    CH_TIDY_BIN = "CH_TIDY_BIN"
    CH_AMD_DARWIN_BIN = "CH_AMD_DARWIN_BIN"
    CH_ARM_DARWIN_BIN = "CH_ARM_DARWIN_BIN"
    CH_AMD_DARWIN_PLAIN = "CH_AMD_DARWIN_PLAIN"
    CH_ARM_DARWIN_PLAIN = "CH_ARM_DARWIN_PLAIN"
    CH_AMD_DARWIN_SIGNED = "CH_AMD_DARWIN_SIGNED"
    CH_ARM_DARWIN_SIGNED = "CH_ARM_DARWIN_SIGNED"
    CH_ARM_V80COMPAT = "CH_ARMV80C_DARWIN_BIN"
    CH_AMD_FREEBSD = "CH_ARM_FREEBSD_BIN"
    CH_PPC64LE = "CH_PPC64LE_BIN"
    CH_AMD_COMPAT = "CH_AMD_COMPAT_BIN"
    CH_AMD_MUSL = "CH_AMD_MUSL_BIN"
    CH_RISCV64 = "CH_RISCV64_BIN"
    CH_S390X = "CH_S390X_BIN"
    CH_LOONGARCH64 = "CH_LOONGARCH64_BIN"
    CH_WASM64 = "CH_WASM64_BIN"
    CH_WASM_PARSER = "CH_WASM_PARSER_BIN"

    # GitHub Actions copies of the self-extracting binary. The runner strips a
    # trailing `_GH` to find the matching S3 artifact on cache hit / expired
    # retention, so `CH_ARM_BINARY_GH` must be `CH_ARM_BIN_GH`.
    CH_AMD_DEBUG_GH = "CH_AMD_DEBUG_GH"
    CH_ARM_DEBUG_GH = "CH_ARM_DEBUG_GH"
    CH_AMD_BINARY_GH = "CH_AMD_BINARY_GH"
    CH_ARM_BINARY_GH = "CH_ARM_BIN_GH"
    CH_AMD_TSAN_GH = "CH_AMD_TSAN_GH"
    CH_AMD_MSAN_GH = "CH_AMD_MSAN_GH"
    CH_AMD_ASAN_UBSAN_GH = "CH_AMD_ASAN_UBSAN_GH"
    CH_ARM_TSAN_GH = "CH_ARM_TSAN_GH"
    CH_ARM_ASAN_UBSAN_GH = "CH_ARM_ASAN_UBSAN_GH"
    CH_ARM_MSAN_GH = "CH_ARM_MSAN_GH"

    FAST_TEST = "FAST_TEST"

    UNITTEST_AMD_ASAN_UBSAN = "UNITTEST_AMD_ASAN_UBSAN"
    UNITTEST_AMD_TSAN = "UNITTEST_AMD_TSAN"
    UNITTEST_AMD_MSAN = "UNITTEST_AMD_MSAN"
    UNITTEST_LLVM_COVERAGE = "UNITTEST_LLVM_COVERAGE"

    # Packages are built for the release builds only - they are what gets published, and
    # everything else in CI runs from the `CH_*` binary.
    DEB_AMD_RELEASE = "DEB_AMD_RELEASE"
    DEB_ARM_RELEASE = "DEB_ARM_RELEASE"

    RPM_AMD_RELEASE = "RPM_AMD_RELEASE"
    RPM_ARM_RELEASE = "RPM_ARM_RELEASE"

    TGZ_AMD_RELEASE = "TGZ_AMD_RELEASE"
    TGZ_ARM_RELEASE = "TGZ_ARM_RELEASE"

    ARM_FUZZERS = "ARM_FUZZERS"
    FUZZERS_CORPUS = "FUZZERS_CORPUS"
    CLICKHOUSE_EXAMPLES = "CLICKHOUSE_EXAMPLES"

    TOOLCHAIN_PGO_BOLT_AMD = "TOOLCHAIN_PGO_BOLT_AMD"
    TOOLCHAIN_PGO_BOLT_ARM = "TOOLCHAIN_PGO_BOLT_ARM"
    CH_AMD_CFI = "CH_AMD_CFI"

    CLICKHOUSE_PGO_PROFILE_AMD = "CLICKHOUSE_PGO_PROFILE_AMD"
    CLICKHOUSE_PGO_PROFILE_ARM = "CLICKHOUSE_PGO_PROFILE_ARM"
    CLICKHOUSE_BOLT_PROFILE_AMD = "CLICKHOUSE_BOLT_PROFILE_AMD"
    CLICKHOUSE_BOLT_PROFILE_ARM = "CLICKHOUSE_BOLT_PROFILE_ARM"


LLVM_FT_NUM_BATCHES = 3
LLVM_IT_NUM_BATCHES = 8
# The old-analyzer + s3 + DBReplicated + WasmEdge parallel variant runs the
# whole stateless suite un-batched and is the slowest job in CI (main run alone
# ~1h40m-2h10m under coverage instrumentation). It is split into batches so each
# shard finishes well inside the runner lease and is not torn down mid-job.
LLVM_FT_OLD_S3_DB_REPL_WASM_NUM_BATCHES = 3
# The sequential counterpart is lighter than the parallel variant but still slow
# enough to benefit from being split, so it gets its own (smaller) batch count.
LLVM_FT_OLD_S3_DB_REPL_WASM_SEQUENTIAL_NUM_BATCHES = 2
LLVM_FT_ARTIFACTS_LIST = [
    # default.profdata files for 3 batches from Stateless(Functional) tests
    ArtifactNames.LLVM_COVERAGE_FILE + f"_ft_{batch}"
    for total_batches in (LLVM_FT_NUM_BATCHES,)
    for batch in range(1, total_batches + 1)
]

LLVM_FT_ARTIFACTS_LIST += [
    # default.profdata files for batches from Functional tests with Old Analyzer + S3 + DBReplicated + WasmEdge, parallel execution
    ArtifactNames.LLVM_COVERAGE_FILE + f"_ft_old_s3_db_repl_wasm_parallel_{batch}"
    for total_batches in (LLVM_FT_OLD_S3_DB_REPL_WASM_NUM_BATCHES,)
    for batch in range(1, total_batches + 1)
]

LLVM_FT_ARTIFACTS_LIST += [
    # default.profdata files for batches from Functional tests with Old Analyzer + S3 + DBReplicated + WasmEdge, sequential execution
    ArtifactNames.LLVM_COVERAGE_FILE + f"_ft_old_s3_db_repl_wasm_sequential_{batch}"
    for total_batches in (LLVM_FT_OLD_S3_DB_REPL_WASM_SEQUENTIAL_NUM_BATCHES,)
    for batch in range(1, total_batches + 1)
]

LLVM_FT_ARTIFACTS_LIST += [
    # default.profdata files for jobs from Functional tests with Old Analyzer + S3 + AsyncInsert + parallel/sequential execution
    ArtifactNames.LLVM_COVERAGE_FILE + "_ft_s3_parallel",
    ArtifactNames.LLVM_COVERAGE_FILE + "_ft_s3_sequential",
    ArtifactNames.LLVM_COVERAGE_FILE + "_ft_s3_async_parallel",
    ArtifactNames.LLVM_COVERAGE_FILE + "_ft_s3_async_sequential",
]

LLVM_IT_ARTIFACTS_LIST = [
    # default.profdata files for the batches from Integration tests
    ArtifactNames.LLVM_COVERAGE_FILE + f"_it_{batch}"
    for total_batches in (LLVM_IT_NUM_BATCHES,)
    for batch in range(1, total_batches + 1)
]

LLVM_ARTIFACTS_LIST = (
    LLVM_FT_ARTIFACTS_LIST + LLVM_IT_ARTIFACTS_LIST + [ArtifactNames.LLVM_COVERAGE_FILE]
)

BINARIES_WITH_LONG_RETENTION = [
    ArtifactNames.CH_AMD_DEBUG,
    ArtifactNames.CH_AMD_RELEASE,
    ArtifactNames.CH_AMD_RELEASE_STRIPPED,
    ArtifactNames.CH_AMD_ASAN_UBSAN,
    ArtifactNames.CH_AMD_TSAN,
    ArtifactNames.CH_AMD_MSAN,
    ArtifactNames.CH_AMD_BINARY,
    ArtifactNames.CH_ARM_RELEASE,
    ArtifactNames.CH_ARM_RELEASE_STRIPPED,
    ArtifactNames.CH_ARM_DEBUG,
    ArtifactNames.CH_ARM_ASAN_UBSAN,
    ArtifactNames.CH_ARM_TSAN,
    ArtifactNames.CH_ARM_MSAN,
]


class ArtifactConfigs:
    clickhouse_binaries = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/programs/self-extracting/clickhouse",
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_DEBUG,
            ArtifactNames.CH_AMD_LLVM_COVERAGE_BUILD,
            ArtifactNames.CH_AMD_PER_TEST_COVERAGE_BUILD,
            ArtifactNames.CH_AMD_RELEASE,
            ArtifactNames.CH_AMD_ASAN_UBSAN,
            ArtifactNames.CH_AMD_TSAN,
            ArtifactNames.CH_AMD_MSAN,
            ArtifactNames.CH_AMD_BINARY,
            ArtifactNames.CH_ARM_RELEASE,
            ArtifactNames.CH_ARM_DEBUG,
            ArtifactNames.CH_ARM_ASAN_UBSAN,
            ArtifactNames.CH_ARM_TSAN,
            ArtifactNames.CH_ARM_MSAN,
            ArtifactNames.CH_COV_BIN,
            ArtifactNames.CH_ARM_BINARY,
            ArtifactNames.CH_TIDY_BIN,
            ArtifactNames.CH_AMD_DARWIN_BIN,
            ArtifactNames.CH_ARM_DARWIN_BIN,
            ArtifactNames.CH_ARM_V80COMPAT,
            ArtifactNames.CH_AMD_FREEBSD,
            ArtifactNames.CH_PPC64LE,
            ArtifactNames.CH_AMD_COMPAT,
            ArtifactNames.CH_AMD_MUSL,
            ArtifactNames.CH_RISCV64,
            ArtifactNames.CH_S390X,
            ArtifactNames.CH_LOONGARCH64,
            ArtifactNames.CH_AMD_CFI,
        ]
    )
    clickhouse_stripped_binaries = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/programs/self-extracting/clickhouse-stripped",
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_RELEASE_STRIPPED,
            ArtifactNames.CH_ARM_RELEASE_STRIPPED,
        ]
    )
    clickhouse_binaries_gh = Artifact.Config(
        name="...",
        type=Artifact.Type.GH,
        path=f"{TEMP_DIR}/build/programs/self-extracting/clickhouse",
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_DEBUG_GH,
            ArtifactNames.CH_AMD_BINARY_GH,
            ArtifactNames.CH_ARM_BINARY_GH,
            ArtifactNames.CH_AMD_TSAN_GH,
            ArtifactNames.CH_AMD_MSAN_GH,
            ArtifactNames.CH_AMD_ASAN_UBSAN_GH,
            ArtifactNames.CH_ARM_ASAN_UBSAN_GH,
            ArtifactNames.CH_ARM_MSAN_GH,
            ArtifactNames.CH_ARM_DEBUG_GH,
            ArtifactNames.CH_ARM_TSAN_GH,
        ]
    )
    clickhouse_darwin_plain_binaries = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/programs/clickhouse",
        compress_zst=True,
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_DARWIN_PLAIN,
            ArtifactNames.CH_ARM_DARWIN_PLAIN,
        ]
    )
    clickhouse_darwin_signed_zips = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clickhouse-macos.zip",
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_DARWIN_SIGNED,
            ArtifactNames.CH_ARM_DARWIN_SIGNED,
        ]
    )
    llvm_profdata_file = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=[
            "./*.profdata",
        ],
        # The coverage merge (llvm-profdata) runs non-blocking and can produce no
        # .profdata (e.g. it crashes on a corrupt .profraw). A missing batch is
        # tolerated by the downstream LLVM Coverage aggregation, which globs
        # whatever .profdata files exist, so a missing file must not redden a
        # coverage job whose tests all passed. Marking the artifact optional lets
        # the runner skip a missing file with a warning instead of erroring.
        optional=True,
    ).parametrize(names=LLVM_ARTIFACTS_LIST)

    llvm_coverage_info_file = Artifact.Config(
        name=ArtifactNames.LLVM_COVERAGE_INFO_FILE,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/llvm_coverage.info",
    )
    clickhouse_debians = Artifact.Config(
        name="*",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/*.deb",
    ).parametrize(
        names=[
            ArtifactNames.DEB_AMD_RELEASE,
            ArtifactNames.DEB_ARM_RELEASE,
        ]
    )
    clickhouse_rpms = Artifact.Config(
        name="*",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/*.rpm",
    ).parametrize(
        names=[
            ArtifactNames.RPM_AMD_RELEASE,
            ArtifactNames.RPM_ARM_RELEASE,
        ]
    )
    clickhouse_tgzs = Artifact.Config(
        name="*",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/*64.tgz*",
    ).parametrize(
        names=[
            ArtifactNames.TGZ_AMD_RELEASE,
            ArtifactNames.TGZ_ARM_RELEASE,
        ]
    )
    unittests_binaries = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/src/unit_tests_dbms",
        compress_zst=True,
    ).parametrize(
        names=[
            ArtifactNames.UNITTEST_AMD_ASAN_UBSAN,
            ArtifactNames.UNITTEST_AMD_TSAN,
            ArtifactNames.UNITTEST_AMD_MSAN,
            ArtifactNames.UNITTEST_LLVM_COVERAGE,
        ]
    )
    # `emcc` emits a pair: the WebAssembly module and the JavaScript that instantiates it
    # (memory setup, syscalls, the Web Workers backing pthreads).
    clickhouse_wasm = Artifact.Config(
        name=ArtifactNames.CH_WASM64,
        type=Artifact.Type.S3,
        path=[
            f"{TEMP_DIR}/build/programs/clickhouse.js",
            f"{TEMP_DIR}/build/programs/clickhouse.wasm",
        ],
    )
    # The two configurations of the standalone SQL parser that `Build (wasm_parser)` publishes:
    # everything, and the smallest build the project offers (no formatting, no access management).
    # No JavaScript sidecar, unlike the Emscripten build above - the module is a WASI reactor, and
    # the consumer supplies the preview1 imports. See utils/wasm-parser/README.md.
    wasm_parser = Artifact.Config(
        name=ArtifactNames.CH_WASM_PARSER,
        type=Artifact.Type.S3,
        path=[
            f"{TEMP_DIR}/build/parser.wasm",
            f"{TEMP_DIR}/build/parser-no-formatting-no-dcl.wasm",
        ],
    )
    fuzzers = Artifact.Config(
        name=ArtifactNames.ARM_FUZZERS,
        type=Artifact.Type.S3,
        path=[
            f"{TEMP_DIR}/build/programs/*_fuzzer",
            f"{TEMP_DIR}/build/programs/*_fuzzer.options",
            f"{TEMP_DIR}/build/programs/all.dict",
        ],
    )
    fuzzers_corpus = Artifact.Config(
        name=ArtifactNames.FUZZERS_CORPUS,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/programs/*_seed_corpus.zip",
    )
    clickhouse_examples = Artifact.Config(
        name=ArtifactNames.CLICKHOUSE_EXAMPLES,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/src/Examples/clickhouse-examples",
    )
    toolchain_pgo_bolt_amd = Artifact.Config(
        name=ArtifactNames.TOOLCHAIN_PGO_BOLT_AMD,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clang-pgo-bolt.tar.zst",
    )
    toolchain_pgo_bolt_arm = Artifact.Config(
        name=ArtifactNames.TOOLCHAIN_PGO_BOLT_ARM,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clang-pgo-bolt.tar.zst",
    )
    clickhouse_pgo_profile_amd = Artifact.Config(
        name=ArtifactNames.CLICKHOUSE_PGO_PROFILE_AMD,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clickhouse-pgo.profdata.zst",
    )
    clickhouse_pgo_profile_arm = Artifact.Config(
        name=ArtifactNames.CLICKHOUSE_PGO_PROFILE_ARM,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clickhouse-pgo.profdata.zst",
    )
    clickhouse_bolt_profile_amd = Artifact.Config(
        name=ArtifactNames.CLICKHOUSE_BOLT_PROFILE_AMD,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clickhouse-bolt.fdata.zst",
    )
    clickhouse_bolt_profile_arm = Artifact.Config(
        name=ArtifactNames.CLICKHOUSE_BOLT_PROFILE_ARM,
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/clickhouse-bolt.fdata.zst",
    )
