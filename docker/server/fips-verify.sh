#!/usr/bin/env bash
#
# Verify a ClickHouse server image built from Dockerfile.ubuntu-fips.
#
#   ./docker/server/fips-verify.sh <image>
#
# Set PRO_TOKEN to also grep the image for a literal token leak.
#
# Exit status:
#   0  every check ran and passed
#   1  at least one FLAG — the image was evaluated and found wanting
#   2  the image could not be evaluated (wrong base, missing toolchain, bad args)
#
# Needs docker and trivy on PATH.
#
# Checks that depend on the host report INFO and never fail the run.

set -uo pipefail

IMAGE="${1:-}"
if [[ -z "$IMAGE" ]]; then
    echo "usage: $0 <image>" >&2
    exit 2
fi

FLAGS=0

pass() { printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
flag() { printf '  \033[31mFLAG\033[0m  %s\n' "$*"; FLAGS=$((FLAGS + 1)); }
info() { printf '  \033[33mINFO\033[0m  %s\n' "$*"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$*"; }

# Indent supporting output, including multi-line values.
detail() {
    local v=${1:-<no output>} pad='        '
    printf '%s%s\n' "$pad" "${v//$'\n'/$'\n'$pad}"
}

abort() {
    printf '  \033[31;1mERROR\033[0m %s\n' "$1"
    shift
    for line in "$@"; do printf '        %s\n' "$line"; done
    printf '\n\033[1mCannot evaluate this image. No findings reported.\033[0m\n'
    exit 2
}

# Run a command inside the image, bypassing the ClickHouse entrypoint. Each check redirects its
# own stderr.
in_image() { docker run --rm --entrypoint /bin/bash "$IMAGE" -c "$1"; }

printf '\033[1mFIPS verification: %s\033[0m\n' "$IMAGE"

# ---------------------------------------------------------------------------
# Every check below assumes an Ubuntu image with dpkg, apt and ClickHouse.
head2 "Preconditions"

if ! command -v trivy >/dev/null 2>&1; then
    abort "trivy not installed" "Needed for the vulnerability scan: https://trivy.dev"
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    abort "image not found locally: $IMAGE" "Pull or build it first."
fi

if ! in_image 'true' 2>/dev/null; then
    abort "cannot run /bin/bash in $IMAGE" "This script drives its checks through bash."
fi

OS_RELEASE=$(in_image 'cat /etc/os-release')
os_field() { grep -m1 "^$1=" <<<"$OS_RELEASE" | cut -d= -f2- | tr -d '"'; }

OS_ID=$(os_field ID)
VERSION_ID=$(os_field VERSION_ID)
PRETTY=$(os_field PRETTY_NAME)
PRETTY=${PRETTY:-unknown}

if [[ "$OS_ID" != "ubuntu" ]]; then
    abort "not an Ubuntu image: $PRETTY (ID=${OS_ID:-unknown})" \
          "These checks assert things about dpkg packages, apt sources and the Ubuntu Pro client."
fi

if ! in_image 'command -v dpkg-query >/dev/null'; then
    abort "dpkg-query not available in $PRETTY"
fi

if ! in_image 'command -v clickhouse-local >/dev/null'; then
    abort "clickhouse-local not available in $IMAGE"
fi

pass "$PRETTY with dpkg and clickhouse-local"

if [[ "$VERSION_ID" == "22.04" ]]; then
    pass "Ubuntu release: $VERSION_ID"
else
    flag "Ubuntu release: ${VERSION_ID:-unknown} (expected 22.04)"
fi

# ---------------------------------------------------------------------------
head2 "APT sources"

PUBLIC_MIRRORS=$(in_image 'grep -rhE "archive\.ubuntu\.com|security\.ubuntu\.com" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null')
info "$(grep -c . <<<"$PUBLIC_MIRRORS") public mirror entries found"

# ---------------------------------------------------------------------------
head2 "FIPS userspace packages"

# libgcrypt20-hmac holds the integrity checksums; openssl-fips-module-3
# ships ossl-modules-3/fips.so.
for pkg in openssl libssl3 libgcrypt20 libgcrypt20-hmac openssl-fips-module-3; do
    PKGVER=$(in_image "dpkg-query -W -f='\${Version}' $pkg 2>/dev/null")
    if [[ "$PKGVER" == *[Ff]ips* ]]; then
        pass "$pkg: $PKGVER"
    else
        flag "$pkg: $PKGVER (expected FIPS marker)"
    fi
done

# ---------------------------------------------------------------------------
head2 "Vulnerabilities"

TRIVY_TEMPLATE='{{ range . }}{{ range .Vulnerabilities }}{{ .VulnerabilityID }}  {{ .PkgName }}  {{ .InstalledVersion }} -> {{ .FixedVersion }}
{{ end }}{{ end }}'
TRIVY_OUT=$(trivy image --scanners vuln --severity HIGH,CRITICAL --ignore-unfixed --quiet \
                --exit-code 42 --format template --template "$TRIVY_TEMPLATE" "$IMAGE" 2>&1)
case $? in
    0)  pass "no HIGH or CRITICAL vulnerabilities with a fix available" ;;
    42) flag "HIGH/CRITICAL vulnerabilities with fixes available"
        detail "$TRIVY_OUT" ;;
    *)  abort "trivy failed to scan $IMAGE" "$TRIVY_OUT" ;;
esac

# ---------------------------------------------------------------------------
head2 "OpenSSL FIPS provider"

if PROVIDER_OUT=$(in_image 'openssl list -providers -provider fips 2>&1'); then
    pass "fips provider installed and loadable"
else
    flag "fips provider failed to load (module missing or broken)"
fi
detail "$PROVIDER_OUT"

HOST_FIPS=$(in_image 'cat /proc/sys/crypto/fips_enabled 2>/dev/null || echo "absent"')
BARE_PROVIDERS=$(in_image 'openssl list -providers 2>/dev/null | grep -c "^  fips"')
info "host /proc/sys/crypto/fips_enabled: $HOST_FIPS"
if [[ "${BARE_PROVIDERS:-0}" -gt 0 ]]; then
    pass "bare 'openssl list -providers' shows fips (host is FIPS-enabled)"
else
    info "bare 'openssl list -providers' has no fips (reflects the host)"
fi

# ---------------------------------------------------------------------------
head2 "Credentials"

if in_image 'command -v pro >/dev/null 2>&1'; then
    pass "ubuntu-pro-client present (needed for runtime 'pro attach')"
else
    flag "ubuntu-pro-client missing (the container cannot attach a subscription)"
fi

PRIVATE=$(in_image 'ls -A /var/lib/ubuntu-advantage/private 2>/dev/null')
if [[ -z "$PRIVATE" ]]; then
    pass "/var/lib/ubuntu-advantage/private is empty"
else
    flag "/var/lib/ubuntu-advantage/private holds files (possible credential material)"
    detail "$PRIVATE"
fi

AUTH=$(in_image 'ls -A /etc/apt/auth.conf.d 2>/dev/null')
if [[ -z "$AUTH" ]]; then
    pass "/etc/apt/auth.conf.d is empty"
else
    flag "/etc/apt/auth.conf.d holds files (apt credentials may have shipped)"
    detail "$AUTH"
fi

if [[ -n "${PRO_TOKEN:-}" ]]; then
    TOKEN_HITS=$(docker run --rm -e TOKEN="$PRO_TOKEN" --entrypoint /bin/bash "$IMAGE" -c \
        'grep -rl --exclude-dir=proc --exclude-dir=sys --exclude-dir=dev -- "$TOKEN" / 2>/dev/null')
    if [[ -n "$TOKEN_HITS" ]]; then
        flag "PRO_TOKEN found in the image filesystem"
        detail "$TOKEN_HITS"
    else
        pass "PRO_TOKEN not in the image filesystem"
    fi

    HISTORY_HITS=$(docker history --no-trunc "$IMAGE" 2>/dev/null | grep -c -- "$PRO_TOKEN")
    if [[ "${HISTORY_HITS:-0}" -gt 0 ]]; then
        flag "PRO_TOKEN found in $HISTORY_HITS image history entries"
    else
        pass "PRO_TOKEN not in the image history"
    fi
else
    info "PRO_TOKEN not set; skipping token grep"
fi

# ---------------------------------------------------------------------------
head2 "ClickHouse FIPS build"

BUILD_OPTS=$(in_image "clickhouse-local -q \"SELECT name || '=' || value FROM system.build_options WHERE name IN ('OPENSSL_VERSION','FIPS_CLICKHOUSE','USE_SSL') FORMAT TSV\" 2>/dev/null")
if grep -q 'OPENSSL_VERSION=AWS-LC-FIPS' <<<"$BUILD_OPTS"; then
    pass "build options report an AWS-LC-FIPS build"
else
    flag "build options do not report AWS-LC-FIPS"
fi
detail "$BUILD_OPTS"

SERVER_LOG=/var/log/clickhouse-server/clickhouse-server.log
CID=$(docker run -d --rm "$IMAGE" 2>/dev/null)
if [[ -z "$CID" ]]; then
    abort "could not start a container from $IMAGE" \
          "The FIPS banner can only be read from a running server."
fi

BANNER=""
LOG_SEEN=0
for _ in $(seq 30); do
    if docker exec "$CID" test -s "$SERVER_LOG" 2>/dev/null; then
        LOG_SEEN=1
        BANNER=$(docker exec "$CID" grep -m1 -o 'Starting in FIPS mode.*' "$SERVER_LOG" 2>/dev/null)
        if [[ -n "$BANNER" ]] || docker exec "$CID" grep -q 'Starting ClickHouse' "$SERVER_LOG" 2>/dev/null; then
            break
        fi
    fi
    sleep 1
done
docker rm -f "$CID" >/dev/null 2>&1

if [[ "$LOG_SEEN" -eq 0 ]]; then
    abort "server log never appeared at $SERVER_LOG" \
          "Cannot tell whether FIPS mode was entered, so this is not reported as a finding."
elif [[ -z "$BANNER" ]]; then
    flag "server started but logged no FIPS banner — FIPS_mode() was false"
else
    pass "$BANNER"
    # BORINGSSL_self_test() and BORINGSSL_integrity_test() both return 1 on success.
    if grep -qE 'KAT test result: 1, Integrity check: 1' <<<"$BANNER"; then
        pass "KAT and integrity self-tests passed"
    else
        flag "FIPS self-tests did not both return 1"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n\033[1mFlags: %d\033[0m\n' "$FLAGS"
exit $(( FLAGS > 0 ? 1 : 0 ))
