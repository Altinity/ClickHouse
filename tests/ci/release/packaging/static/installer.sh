#!/bin/sh
# Altinity ClickHouse installer — curl builds.altinity.cloud | sh
set -eu

BASE_URL="${ALTINITY_BASE_URL:-https://builds.altinity.cloud}"
MIN_SUPPORTED_VERSION="23.8"

# ── output helpers ──────────────────────────────────────────────

die() {
  printf 'error: %s\n' "$1" >&2
  exit 1
}

info() {
  printf '%s\n' "$1"
}

# ── detection ────────────────────────────────────────────────────

detect_os() {
  [ -f /etc/os-release ] || die "cannot detect OS: /etc/os-release not found"
  . /etc/os-release
  DISTRO_ID="$ID"

  case " $ID $ID_LIKE " in
    *" debian "*|*" ubuntu "*)
      OS_FAMILY="debian"
      PKG_MGR="apt"
      ;;
    *" rhel "*|*" centos "*|*" fedora "*)
      OS_FAMILY="rhel"
      if command -v dnf >/dev/null 2>&1; then
        PKG_MGR="dnf"
      else
        PKG_MGR="yum"
      fi
      ;;
    *)
      die "unsupported OS: $ID (supported: Ubuntu, Debian, RHEL, CentOS, Rocky)"
      ;;
  esac
}

detect_arch() {
  raw_arch="$(uname -m)"
  case "$raw_arch" in
    x86_64|amd64)
      ARCH="x86_64"
      DEB_ARCH="amd64"
      ;;
    aarch64|arm64)
      ARCH="aarch64"
      DEB_ARCH="arm64"
      ;;
    *)
      die "unsupported architecture: $raw_arch (supported: x86_64, aarch64)"
      ;;
  esac
}

# ── argument parsing ─────────────────────────────────────────────

usage() {
  cat >&2 <<EOF
Usage: installer.sh [--build stable|fips|antalya] [--version VERSION] [--yes] [--allow-downgrades]

  --build TYPE        release type; skips the type menu
  --version VER       exact version (e.g. 25.8.22.20001.altinityantalya) or
                       shorthand major.minor (e.g. 25.8); skips the version menu
  --yes                skip the install confirmation prompt (implies --allow-downgrades)
  --allow-downgrades   allow installing an older version than what's installed
EOF
  exit 1
}

parse_args() {
  FLAG_BUILD=""
  FLAG_VERSION=""
  FLAG_YES=""
  FLAG_ALLOW_DOWNGRADES=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --build)
        [ $# -ge 2 ] || { printf 'error: --build requires a value\n' >&2; usage; }
        FLAG_BUILD="$2"
        shift 2
        ;;
      --version)
        [ $# -ge 2 ] || { printf 'error: --version requires a value\n' >&2; usage; }
        FLAG_VERSION="$2"
        shift 2
        ;;
      --yes)
        FLAG_YES="1"
        shift
        ;;
      --allow-downgrades)
        FLAG_ALLOW_DOWNGRADES="1"
        shift
        ;;
      -h|--help)
        usage
        ;;
      *)
        printf 'error: unknown argument: %s\n' "$1" >&2
        usage
        ;;
    esac
  done

  [ -z "$FLAG_VERSION" ] || [ -n "$FLAG_BUILD" ] || die "--version requires --build"

  # --yes implies consent for a downgrade too, since there's no prompt left
  # to warn through.
  [ -z "$FLAG_YES" ] || FLAG_ALLOW_DOWNGRADES="1"
}

validate_release_type() {
  case "$1" in
    stable|fips|antalya) ;;
    *) die "unknown release type: $1 (expected: stable, fips, antalya)" ;;
  esac
}

# display label for a release type - $RELEASE_TYPE itself stays lowercase
# since it's used verbatim in repo paths (repo_base_url) and --build values.
display_name() {
  case "$1" in
    stable)  echo "Stable" ;;
    fips)    echo "FIPS" ;;
    antalya) echo "Antalya" ;;
    *)       echo "$1" ;;
  esac
}

# ── menus ────────────────────────────────────────────────────────

show_type_menu() {
  info ""
  info "Select release type:"
  info "  1) Stable    - latest Altinity LTS builds"
  info "  2) FIPS      - FIPS 140-2 compatible builds"
  info "  3) Antalya   - Altinity Antalya builds"
  info ""
  printf '  > '
  read -r choice

  case "$choice" in
    1) RELEASE_TYPE="stable" ;;
    2) RELEASE_TYPE="fips" ;;
    3) RELEASE_TYPE="antalya" ;;
    *) die "invalid selection: $choice" ;;
  esac
}

show_version_menu() {
  info ""
  info "Available versions ($(display_name "$RELEASE_TYPE")):"

  i=0
  menu_versions=""
  for v in $SELECTED_LIST; do
    i=$((i + 1))
    if [ "$i" -eq 1 ]; then
      info "  $i) $v   [latest]"
    else
      info "  $i) $v"
    fi
    menu_versions="$menu_versions $v"
  done
  [ "$i" -gt 0 ] || die "no versions available for $(display_name "$RELEASE_TYPE")"

  info ""
  printf '  > '
  read -r choice

  case "$choice" in
    ''|*[!0-9]*) die "invalid selection: $choice" ;;
  esac
  [ "$choice" -ge 1 ] && [ "$choice" -le "$i" ] || die "invalid selection: $choice"

  # shellcheck disable=SC2086
  set -- $menu_versions
  eval "SELECTED_VERSION=\${$choice}"
}

# ── repo path mapping ────────────────────────────────────────────

repo_base_url() {
  type="$1"
  mgr="$2"
  case "$type" in
    stable)  prefix="" ;;
    fips)    prefix="fips-" ;;
    antalya) prefix="antalya-" ;;
    *) die "unknown release type: $type" ;;
  esac

  case "$mgr" in
    apt)      echo "$BASE_URL/${prefix}apt-repo" ;;
    yum|dnf)  echo "$BASE_URL/${prefix}yum-repo" ;;
    *) die "unknown package manager: $mgr" ;;
  esac
}

# ── version discovery ────────────────────────────────────────────

# Populates $RAW_VERSIONS: one "version" per line (apt) or
# "version rel" per line (yum/dnf), newest last (sort -V order).
fetch_raw_versions() {
  type="$1"

  if [ "$PKG_MGR" = "apt" ]; then
    url="$(repo_base_url "$type" apt)/dists/stable/main/binary-$DEB_ARCH/Packages"
    RAW_VERSIONS="$(curl -fsSL "$url" \
      | awk '/^Package: clickhouse-server$/{f=1} f&&/^Version:/{print $2; f=0}' \
      | sort -V)"
  else
    repo_url="$(repo_base_url "$type" "$PKG_MGR")"
    repomd="$(curl -fsSL "$repo_url/repodata/repomd.xml")" \
      || die "could not reach $repo_url"
    primary_href="$(printf '%s' "$repomd" \
      | grep -o 'repodata/[^"]*primary\.xml\.gz' | head -1)"
    [ -n "$primary_href" ] || die "could not find primary.xml.gz in repo metadata"

    RAW_VERSIONS="$(curl -fsSL "$repo_url/$primary_href" \
      | gunzip -c \
      | grep -A2 '<name>clickhouse-server</name>' \
      | grep -v '<name>clickhouse-server</name>' \
      | grep -v '^--$' \
      | awk -v arch="$ARCH" '
          NR % 2 == 1 {
            a = $0
            sub(/^.*<arch>/, "", a); sub(/<\/arch>.*$/, "", a)
            next
          }
          {
            v = $0
            sub(/^.*ver="/, "", v); sub(/".*$/, "", v)
            r = $0
            sub(/^.*rel="/, "", r); sub(/".*$/, "", r)
            if (a == arch || a == "noarch") print v, r
          }
        ' \
      | sort -k1,1V)"
  fi

  [ -n "$RAW_VERSIONS" ] || die "no clickhouse-server versions found for $type"
}

# major.minor of a version string, e.g. 25.3.8.10042.altinitystable -> 25.3
majmin() {
  printf '%s' "$1" | cut -d. -f1,2
}

# is $1 (major.minor) an LTS line? (x.3 or x.8)
is_lts() {
  case "$1" in
    *.3|*.8) return 0 ;;
    *) return 1 ;;
  esac
}

# version comparison: 0 if $1 >= $2 (major.minor only)
majmin_ge() {
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

# full-version comparison: 0 if $1 > $2
version_gt() {
  [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

# currently-installed clickhouse-server version, or empty if not installed
detect_installed_version() {
  if [ "$PKG_MGR" = "apt" ]; then
    dpkg-query -W -f='${Version}' clickhouse-server 2>/dev/null || true
  else
    rpm -q --queryformat '%{VERSION}' clickhouse-server 2>/dev/null || true
  fi
}

# Groups $RAW_VERSIONS by major.minor, keeping the newest entry per
# group (RAW_VERSIONS is already ascending, so last write wins).
# Populates $GROUPED_LIST: one full line (same shape as RAW_VERSIONS)
# per group, ascending by major.minor.
group_by_release_line() {
  input="$1"
  awk_prog='
    { key = $1; sub(/\.[0-9]+\.[0-9]+\.[^.]*$/, "", key); last[key] = $0 }
    END { for (k in last) print last[k] }
  '
  GROUPED_LIST="$(printf '%s\n' "$input" | awk "$awk_prog" | sort -k1,1V)"
}

# Applies the release-selection rules for $RELEASE_TYPE to $RAW_VERSIONS.
# Populates $SELECTED_LIST: display versions, newest first.
select_versions() {
  group_by_release_line "$RAW_VERSIONS"

  if [ "$RELEASE_TYPE" != "antalya" ]; then
    kept=""
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      v="$(printf '%s\n' "$line" | awk '{print $1}')"
      mm="$(majmin "$v")"
      if majmin_ge "$mm" "$MIN_SUPPORTED_VERSION"; then
        kept="$kept
$line"
      fi
    done <<EOF
$GROUPED_LIST
EOF
    SELECTED_LIST="$(printf '%s\n' "$kept" | grep -v '^$' | sort -k1,1Vr | awk '{print $1}')"
    return
  fi

  # antalya: latest line + LTS-aware second line
  latest_line="$(printf '%s\n' "$GROUPED_LIST" | grep -v '^$' | sort -k1,1V | tail -1)"
  latest_version="$(printf '%s\n' "$latest_line" | awk '{print $1}')"
  latest_mm="$(majmin "$latest_version")"

  second_line=""
  if is_lts "$latest_mm"; then
    # highest LTS group strictly below latest
    second_line="$(printf '%s\n' "$GROUPED_LIST" | grep -v '^$' | while IFS= read -r line; do
      v="$(printf '%s\n' "$line" | awk '{print $1}')"
      mm="$(majmin "$v")"
      [ "$mm" = "$latest_mm" ] && continue
      is_lts "$mm" && printf '%s\n' "$line"
    done | sort -k1,1V | tail -1)"
  else
    # highest LTS group overall (guaranteed below latest, since latest isn't LTS)
    second_line="$(printf '%s\n' "$GROUPED_LIST" | grep -v '^$' | while IFS= read -r line; do
      v="$(printf '%s\n' "$line" | awk '{print $1}')"
      is_lts "$(majmin "$v")" && printf '%s\n' "$line"
    done | sort -k1,1V | tail -1)"
  fi

  if [ -z "$second_line" ]; then
    # fallback: no LTS line exists at all - use highest non-LTS group below latest
    second_line="$(printf '%s\n' "$GROUPED_LIST" | grep -v '^$' | while IFS= read -r line; do
      v="$(printf '%s\n' "$line" | awk '{print $1}')"
      mm="$(majmin "$v")"
      [ "$mm" = "$latest_mm" ] && continue
      printf '%s\n' "$line"
    done | sort -k1,1V | tail -1)"
  fi

  SELECTED_LIST="$latest_version"
  if [ -n "$second_line" ]; then
    SELECTED_LIST="$SELECTED_LIST
$(printf '%s\n' "$second_line" | awk '{print $1}')"
  fi
}

# resolves rel for a chosen yum/dnf version out of $RAW_VERSIONS
lookup_rel() {
  printf '%s\n' "$RAW_VERSIONS" | awk -v v="$1" '$1==v{print $2; exit}'
}

# Resolves $1 (an exact version, or a "major.minor" shorthand) against the
# full $RAW_VERSIONS for $RELEASE_TYPE - not the curated $SELECTED_LIST, so
# --version can reach any release line the repo has, including ones the
# interactive menu wouldn't show (e.g. a non-LTS antalya line, or something
# below MIN_SUPPORTED_VERSION). Populates $SELECTED_VERSION.
resolve_shorthand_version() {
  requested="$1"

  if printf '%s\n' "$RAW_VERSIONS" | awk '{print $1}' | grep -qx -- "$requested"; then
    SELECTED_VERSION="$requested"
    return
  fi

  group_by_release_line "$RAW_VERSIONS"
  # trailing `true` matters under set -e: without it, this assignment's exit
  # status is the last line's match test, and a non-matching final group
  # (the common case) would kill the script even after a match was found.
  match_line="$(printf '%s\n' "$GROUPED_LIST" | grep -v '^$' | {
    while IFS= read -r line; do
      v="$(printf '%s\n' "$line" | awk '{print $1}')"
      [ "$(majmin "$v")" = "$requested" ] && printf '%s\n' "$line"
    done
    true
  })"

  [ -n "$match_line" ] || die "no $RELEASE_TYPE version matching '$requested' found"
  SELECTED_VERSION="$(printf '%s\n' "$match_line" | awk '{print $1}')"
}

# ── confirmation ─────────────────────────────────────────────────

confirm_install() {
  installed_version="$(detect_installed_version)"
  is_downgrade=0
  if [ -n "$installed_version" ] && version_gt "$installed_version" "$SELECTED_VERSION"; then
    is_downgrade=1
  fi

  info ""
  info "The following packages will be installed:"
  if [ "$PKG_MGR" = "apt" ]; then
    info "  clickhouse-server=$SELECTED_VERSION"
    info "  clickhouse-client=$SELECTED_VERSION"
    info "  clickhouse-common-static=$SELECTED_VERSION (dependency)"
  else
    rel="$(lookup_rel "$SELECTED_VERSION")"
    [ -n "$rel" ] || die "could not resolve package release for $SELECTED_VERSION"
    info "  clickhouse-server-$SELECTED_VERSION-$rel"
    info "  clickhouse-client-$SELECTED_VERSION-$rel"
    info "  clickhouse-common-static-$SELECTED_VERSION-$rel (dependency)"
  fi
  info "  release type: $(display_name "$RELEASE_TYPE")"
  info "  source: $BASE_URL"

  # Suppressed when --yes: that path auto-applies allow-downgrades silently
  # (parse_args already set it) instead of warning through a prompt no one
  # will see.
  if [ "$is_downgrade" -eq 1 ] && [ -z "$FLAG_YES" ]; then
    info ""
    info "WARNING: this will downgrade clickhouse-server from $installed_version to $SELECTED_VERSION"
  fi
  info ""

  [ -z "$FLAG_YES" ] || return 0

  printf 'Proceed with installation? [Y/n] '
  read -r reply
  case "$reply" in
    ''|y|Y|yes|Yes|YES) ;;
    *) info "Aborted."; exit 0 ;;
  esac

  [ "$is_downgrade" -eq 0 ] || FLAG_ALLOW_DOWNGRADES="1"
}

# ── repo setup ───────────────────────────────────────────────────

add_repo_apt() {
  repo_url="$(repo_base_url "$RELEASE_TYPE" apt)"
  mkdir -p /usr/share/keyrings
  curl -fsSL "$repo_url/pubkey.gpg" | gpg --yes --dearmor -o /usr/share/keyrings/altinity-archive-keyring.gpg
  echo "deb [signed-by=/usr/share/keyrings/altinity-archive-keyring.gpg] $repo_url stable main" \
    > /etc/apt/sources.list.d/altinity.list
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
}

add_repo_rpm() {
  repo_url="$(repo_base_url "$RELEASE_TYPE" "$PKG_MGR")"
  curl -fsSL "$repo_url/altinity.repo" -o /etc/yum.repos.d/altinity.repo
}

# ── install ──────────────────────────────────────────────────────

install_package() {
  info ""
  info "Installing clickhouse-server $SELECTED_VERSION..."

  if [ "$PKG_MGR" = "apt" ]; then
    # noninteractive: clickhouse-server's postinst otherwise prompts to set
    # a default-user password, which would hang a piped/scripted install.
    #
    # clickhouse-common-static must be pinned explicitly too: server/client
    # depend on it at the exact same version, but apt's solver defaults
    # unpinned dependencies to the newest candidate and only then checks the
    # constraint - it does not backtrack to the matching older version on
    # its own, so installing anything but the newest release without this
    # pin fails with an unmet-dependency error.
    #
    # --allow-downgrades: apt-get refuses a downgrade under -y without this
    # ("Packages were downgraded and -y was used without --allow-downgrades")
    # even though the exact target version was explicitly requested.
    allow_downgrades_flag=""
    [ -z "$FLAG_ALLOW_DOWNGRADES" ] || allow_downgrades_flag="--allow-downgrades"
    DEBIAN_FRONTEND=noninteractive apt-get install -y $allow_downgrades_flag \
      "clickhouse-server=$SELECTED_VERSION" \
      "clickhouse-client=$SELECTED_VERSION" \
      "clickhouse-common-static=$SELECTED_VERSION"
  else
    # dnf/yum resolve a downgrade correctly on their own via plain install -
    # no equivalent flag needed.
    rel="$(lookup_rel "$SELECTED_VERSION")"
    [ -n "$rel" ] || die "could not resolve package release for $SELECTED_VERSION"
    "$PKG_MGR" install -y \
      "clickhouse-server-$SELECTED_VERSION-$rel" \
      "clickhouse-client-$SELECTED_VERSION-$rel" \
      "clickhouse-common-static-$SELECTED_VERSION-$rel"
  fi
}

# ── main ─────────────────────────────────────────────────────────

main() {
  parse_args "$@"

  [ "$(id -u)" = "0" ] || die "this script must be run as root (try: sudo sh -c \"\$(curl -fsSL $BASE_URL)\")"

  # Only steal a controlling tty if something below will actually read from
  # stdin. A fully-specified shortcut (--build + --version + --yes) never
  # prompts, so it must not touch /dev/tty - it may not exist in CI.
  need_tty=1
  if [ -n "$FLAG_BUILD" ] && [ -n "$FLAG_VERSION" ] && [ -n "$FLAG_YES" ]; then
    need_tty=0
  fi
  if [ "$need_tty" -eq 1 ] && [ ! -t 0 ]; then
    exec < /dev/tty
  fi

  info "Checking system..."
  detect_os
  detect_arch
  info "Detected: $DISTRO_ID · $ARCH · $PKG_MGR"

  if [ -n "$FLAG_BUILD" ]; then
    validate_release_type "$FLAG_BUILD"
    RELEASE_TYPE="$FLAG_BUILD"
  else
    show_type_menu
  fi

  info ""
  info "Fetching $(display_name "$RELEASE_TYPE") versions from $BASE_URL..."
  fetch_raw_versions "$RELEASE_TYPE"

  if [ -n "$FLAG_VERSION" ]; then
    resolve_shorthand_version "$FLAG_VERSION"
  else
    select_versions
    show_version_menu
  fi

  confirm_install

  info ""
  info "Adding Altinity repository..."
  if [ "$PKG_MGR" = "apt" ]; then
    add_repo_apt
  else
    add_repo_rpm
  fi

  install_package

  info ""
  info "clickhouse-server $SELECTED_VERSION installed."

  # Defensive reset: several installers in this chain (gpg, dpkg/rpm
  # scriptlets) briefly touch terminal modes. Only meaningful if we're
  # actually attached to one.
  [ ! -t 0 ] || stty sane 2>/dev/null || true
}

main "$@"
