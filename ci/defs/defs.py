from praktika import Artifact, Docker, Job, Secret
from praktika.utils import MetaClasses, Utils
from settings import altinity_overrides

# i.e. "ClickHouse/ci/tmp"
TEMP_DIR = f"{Utils.cwd()}/ci/tmp"  # == _Settings.TEMP_DIR != env_helper.TEMP_PATH

SYNC = "Altinity sync"

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
    ARM_LARGE = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
        "16c",
    ]
    AMD_MEDIUM = ["self-hosted", "altinity-on-demand", "altinity-func-tester", "16c"]
    ARM_MEDIUM = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
        "16c",
    ]
    AMD_MEDIUM_CPU = ["self-hosted", "altinity-on-demand", "altinity-builder", "16c"]
    ARM_MEDIUM_CPU = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
        "16c",
    ]
    AMD_MEDIUM_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester"]
    ARM_MEDIUM_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64"]
    AMD_SMALL = ["self-hosted", "altinity-on-demand", "altinity-func-tester"]
    ARM_SMALL = ["self-hosted", "altinity-on-demand", "altinity-func-tester-aarch64"]
    AMD_SMALL_MEM = ["self-hosted", "altinity-on-demand", "altinity-func-tester", "32g"]
    ARM_SMALL_MEM = [
        "self-hosted",
        "altinity-on-demand",
        "altinity-func-tester-aarch64",
        "32g",
    ]
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
    type=Secret.Type.AWS_SSM_VAR,
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
    # Secret.Config(
    #     name="woolenwolf_gh_app.clickhouse-app-id",
    #     type=Secret.Type.AWS_SSM_SECRET,
    # ),
    # Secret.Config(
    #     name="woolenwolf_gh_app.clickhouse-app-key",
    #     type=Secret.Type.AWS_SSM_SECRET,
    # ),
    Secret.Config(
        name="AWS_ACCESS_KEY_ID",
        type=Secret.Type.GH_SECRET,
    ),
    Secret.Config(
        name="AWS_SECRET_ACCESS_KEY",
        type=Secret.Type.GH_SECRET,
    ),
]

DOCKERS = [
    Docker.Config(
        name="altinityinfra/style-test",
        path="./ci/docker/style-test",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/fasttest",
        path="./ci/docker/fasttest",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/binary-builder",
        path="./ci/docker/binary-builder",
        platforms=Docker.Platforms.arm_amd,
        depends_on=["altinityinfra/fasttest"],
    ),
    Docker.Config(
        name="altinityinfra/test-old-centos",
        path="./ci/docker/compatibility/centos",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
    Docker.Config(
        name="altinityinfra/test-old-ubuntu",
        path="./ci/docker/compatibility/ubuntu",
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
        name="altinityinfra/test-base",
        path="./ci/docker/test-base",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
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
    # Docker.Config(
    #    name="clickhouse/docs-builder",
    #     path="./ci/docker/docs-builder",
    #     platforms=Docker.Platforms.arm_amd,
    #     depends_on=[],
    # ),
    Docker.Config(
        name="altinityinfra/install-deb-test",
        path="./ci/docker/install/deb",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
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
    ),
    Docker.Config(
        name="altinityinfra/mysql_dotnet_client",
        path="./ci/docker/integration/mysql_dotnet_client",
        platforms=Docker.Platforms.arm_amd,
        depends_on=[],
    ),
]


class BuildTypes(metaclass=MetaClasses.WithIter):
    AMD_DEBUG = "amd_debug"
    AMD_RELEASE = "amd_release"
    AMD_BINARY = "amd_binary"
    AMD_ASAN = "amd_asan"
    AMD_TSAN = "amd_tsan"
    AMD_MSAN = "amd_msan"
    AMD_UBSAN = "amd_ubsan"
    ARM_RELEASE = "arm_release"
    ARM_ASAN = "arm_asan"

    ARM_COVERAGE = "arm_coverage"
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
    FUZZERS = "fuzzers"


class JobNames:
    DOCKER_BUILDS_ARM = "Dockers build (arm)"
    DOCKER_BUILDS_AMD = "Dockers build (amd)"
    STYLE_CHECK = "Style check"
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
    Docs = "Docs check"
    CLICKBENCH = "ClickBench"
    DOCKER_SERVER = "Docker server image"
    DOCKER_KEEPER = "Docker keeper image"
    SQL_TEST = "SQLTest"
    SQLANCER = "SQLancer"
    INSTALL_TEST = "Install packages"
    ASTFUZZER = "AST fuzzer"
    BUZZHOUSE = "BuzzHouse"
    BUILDOCKER = "BuildDockers"
    BUGFIX_VALIDATE = "Bugfix validation"
    BUGFIX_VALIDATE_IT = "Bugfix validation (integration tests)"
    BUGFIX_VALIDATE_FT = "Bugfix validation (functional tests)"
    JEPSEN_KEEPER = "ClickHouse Keeper Jepsen"
    JEPSEN_SERVER = "ClickHouse Server Jepsen"
    LIBFUZZER_TEST = "libFuzzer tests"


class ToolSet:
    COMPILER_C = "clang-19"
    COMPILER_CPP = "clang++-19"


class ArtifactNames:
    CH_AMD_DEBUG = "CH_AMD_DEBUG"
    CH_AMD_RELEASE = "CH_AMD_RELEASE"
    CH_AMD_RELEASE_STRIPPED = "CH_AMD_RELEASE_STRIPPED"
    CH_AMD_ASAN = "CH_AMD_ASAN"
    CH_AMD_TSAN = "CH_AMD_TSAN"
    CH_AMD_MSAN = "CH_AMD_MSAN"
    CH_AMD_UBSAN = "CH_AMD_UBSAN"
    CH_AMD_BINARY = "CH_AMD_BINARY"
    CH_ARM_RELEASE = "CH_ARM_RELEASE"
    CH_ARM_RELEASE_STRIPPED = "CH_ARM_RELEASE_STRIPPED"
    CH_ARM_ASAN = "CH_ARM_ASAN"

    CH_COV_BIN = "CH_COV_BIN"
    CH_ARM_BINARY = "CH_ARM_BIN"
    CH_TIDY_BIN = "CH_TIDY_BIN"
    CH_AMD_DARWIN_BIN = "CH_AMD_DARWIN_BIN"
    CH_ARM_DARWIN_BIN = "CH_ARM_DARWIN_BIN"
    CH_ARM_V80COMPAT = "CH_ARMV80C_DARWIN_BIN"
    CH_AMD_FREEBSD = "CH_ARM_FREEBSD_BIN"
    CH_PPC64LE = "CH_PPC64LE_BIN"
    CH_AMD_COMPAT = "CH_AMD_COMPAT_BIN"
    CH_AMD_MUSL = "CH_AMD_MUSL_BIN"
    CH_RISCV64 = "CH_RISCV64_BIN"
    CH_S390X = "CH_S390X_BIN"
    CH_LOONGARCH64 = "CH_LOONGARCH64_BIN"

    FAST_TEST = "FAST_TEST"
    UNITTEST_AMD_ASAN = "UNITTEST_AMD_ASAN"
    UNITTEST_AMD_TSAN = "UNITTEST_AMD_TSAN"
    UNITTEST_AMD_MSAN = "UNITTEST_AMD_MSAN"
    UNITTEST_AMD_UBSAN = "UNITTEST_AMD_UBSAN"

    DEB_AMD_DEBUG = "DEB_AMD_DEBUG"
    DEB_AMD_RELEASE = "DEB_AMD_RELEASE"
    DEB_COV = "DEB_COV"
    DEB_AMD_ASAN = "DEB_AMD_ASAN"
    DEB_AMD_TSAN = "DEB_AMD_TSAN"
    DEB_AMD_MSAM = "DEB_AMD_MSAM"
    DEB_AMD_UBSAN = "DEB_AMD_UBSAN"
    DEB_ARM_RELEASE = "DEB_ARM_RELEASE"
    DEB_ARM_ASAN = "DEB_ARM_ASAN"

    RPM_AMD_RELEASE = "RPM_AMD_RELEASE"
    RPM_ARM_RELEASE = "RPM_ARM_RELEASE"

    TGZ_AMD_RELEASE = "TGZ_AMD_RELEASE"
    TGZ_ARM_RELEASE = "TGZ_ARM_RELEASE"

    FUZZERS = "FUZZERS"
    FUZZERS_CORPUS = "FUZZERS_CORPUS"


class ArtifactConfigs:
    clickhouse_binaries = Artifact.Config(
        name="...",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/build/programs/self-extracting/clickhouse",
    ).parametrize(
        names=[
            ArtifactNames.CH_AMD_DEBUG,
            ArtifactNames.CH_AMD_RELEASE,
            ArtifactNames.CH_AMD_ASAN,
            ArtifactNames.CH_AMD_TSAN,
            ArtifactNames.CH_AMD_MSAN,
            ArtifactNames.CH_AMD_UBSAN,
            ArtifactNames.CH_AMD_BINARY,
            ArtifactNames.CH_ARM_RELEASE,
            ArtifactNames.CH_ARM_ASAN,
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
    clickhouse_debians = Artifact.Config(
        name="*",
        type=Artifact.Type.S3,
        path=f"{TEMP_DIR}/*.deb",
    ).parametrize(
        names=[
            ArtifactNames.DEB_AMD_RELEASE,
            ArtifactNames.DEB_AMD_DEBUG,
            ArtifactNames.DEB_AMD_ASAN,
            ArtifactNames.DEB_AMD_TSAN,
            ArtifactNames.DEB_AMD_MSAM,
            ArtifactNames.DEB_AMD_UBSAN,
            ArtifactNames.DEB_COV,
            ArtifactNames.DEB_ARM_RELEASE,
            ArtifactNames.DEB_ARM_ASAN,
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
            ArtifactNames.UNITTEST_AMD_ASAN,
            ArtifactNames.UNITTEST_AMD_TSAN,
            ArtifactNames.UNITTEST_AMD_MSAN,
            ArtifactNames.UNITTEST_AMD_UBSAN,
        ]
    )
    fuzzers = Artifact.Config(
        name=ArtifactNames.FUZZERS,
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
