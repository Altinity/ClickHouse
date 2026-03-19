#!/bin/bash
# Build test harness static libraries for ClickHouse FIPS testing integration.
#
# Each library is partially linked with libstdc++ via `ld -r` and then all
# symbols are prefixed with __awslc_ using `objcopy --prefix-symbols`.
# Undefined symbols (libssl/libcrypto/libc refs) and the entry point are
# restored to their original names via `--redefine-syms`.
#
# This prevents libstdc++ symbols from colliding with ClickHouse's libc++
# at final link time, while keeping COMDAT groups intact (localizing COMDAT
# key symbols would cause lld to discard their sections).
#
# Usage: build_test_harness.sh <awslc-src-dir> <output-dir>
set -ex

SRC=${1:?usage: build_test_harness.sh <awslc-src-dir> <output-dir>}
OUTDIR=${2:?usage: build_test_harness.sh <awslc-src-dir> <output-dir>}
mkdir -p "$OUTDIR"

CXXFLAGS="-std=c++17 -fPIC -g -O2 -fno-exceptions -w"
CFLAGS="-fPIC -O2 -w"
INC="-I$SRC/include -I$SRC -I$SRC/ssl/test"
STDCXX=$(c++ -print-file-name=libstdc++.a)
PREFIX="__awslc_"

# build_target <name> <entry_grep> <obj_dir>
#   Partial-links .o files in <obj_dir> with libstdc++, prefixes all symbols,
#   then restores undefined refs and entry point to original names.
build_target() {
    local name=$1 entry_grep=$2 obj_dir=$3
    ld -r -o "$obj_dir/combined.o" "$obj_dir"/*.o "$STDCXX"

    # Collect undefined symbols — these reference external libraries
    # (libssl, libcrypto, libc, pthreads) and must keep their original names.
    nm -u "$obj_dir/combined.o" | awk 'NF>=2{print $NF}' | sort -u > "$obj_dir/undef.txt"

    # Find entry point symbol
    nm "$obj_dir/combined.o" | awk "/T.*${entry_grep}/{print \$3}" > "$obj_dir/entry.txt"

    # Build redefine map: after --prefix-symbols adds the prefix,
    # restore undefined symbols and entry point to original names.
    awk -v p="$PREFIX" '{print p $0 " " $0}' "$obj_dir/undef.txt"  > "$obj_dir/redefine.txt"
    awk -v p="$PREFIX" '{print p $0 " " $0}' "$obj_dir/entry.txt" >> "$obj_dir/redefine.txt"

    # Two passes: objcopy applies --redefine-syms BEFORE --prefix-symbols
    # in a single invocation, so we must split them.
    # Pass 1: prefix every symbol.
    objcopy --prefix-symbols="$PREFIX" "$obj_dir/combined.o"
    # Pass 2: restore undefined refs + entry point to original names,
    # and weaken all defined symbols so duplicates across archives
    # (same libstdc++ members pulled into shim, handshaker, acvp) don't clash.
    objcopy --redefine-syms="$obj_dir/redefine.txt" --weaken "$obj_dir/combined.o"

    ar rcs "$OUTDIR/lib${name}.a" "$obj_dir/combined.o"
    rm -rf "$obj_dir"
}

# --- ssl-shim ---
OBJ=$(mktemp -d)
for f in async_bio bssl_shim handshake_util mock_quic_transport \
         packeted_bio settings_writer ssl_transfer test_config test_state; do
    EXTRA="-Dposix_spawn=__ssl_posix_spawn"
    [ "$f" = "bssl_shim" ] && EXTRA="$EXTRA -Dmain=bssl_shim_main"
    c++ $CXXFLAGS $INC $EXTRA -c "$SRC/ssl/test/$f.cc" -o "$OBJ/$f.o"
done
c++ $CXXFLAGS $INC -Dposix_spawn=__ssl_posix_spawn \
    -c "$SRC/crypto/test/test_util.cc" -o "$OBJ/test_util.o"
cc $CFLAGS -c /tmp/posix_spawn_2.c -o "$OBJ/posix_spawn_2.o"
cc $CFLAGS -c /tmp/glibc_compat.c  -o "$OBJ/glibc_compat.o"
build_target awslc_shim bssl_shim_main "$OBJ"

# --- ssl-handshaker ---
OBJ=$(mktemp -d)
for f in async_bio handshake_util handshaker mock_quic_transport \
         packeted_bio settings_writer test_config test_state; do
    EXTRA="-Dposix_spawn=__ssl_posix_spawn"
    [ "$f" = "handshaker" ] && EXTRA="$EXTRA -Dmain=handshaker_main"
    c++ $CXXFLAGS $INC $EXTRA -c "$SRC/ssl/test/$f.cc" -o "$OBJ/$f.o"
done
c++ $CXXFLAGS $INC -Dposix_spawn=__ssl_posix_spawn \
    -c "$SRC/crypto/test/test_util.cc" -o "$OBJ/test_util.o"
cc $CFLAGS -c /tmp/posix_spawn_2.c -o "$OBJ/posix_spawn_2.o"
cc $CFLAGS -c /tmp/glibc_compat.c  -o "$OBJ/glibc_compat.o"
build_target awslc_handshaker handshaker_main "$OBJ"

# --- acvp-server ---
OBJ=$(mktemp -d)
ACVP_DIR="$SRC/util/fipstools/acvp/modulewrapper"
c++ $CXXFLAGS -I"$ACVP_DIR" -I"$SRC/include" -I"$SRC" \
    -Dmain=acvp_modulewrapper_main \
    -c "$ACVP_DIR/main.cc" -o "$OBJ/main.o"
c++ $CXXFLAGS -I"$ACVP_DIR" -I"$SRC/include" -I"$SRC" \
    -c "$ACVP_DIR/modulewrapper.cc" -o "$OBJ/modulewrapper.o"
cc $CFLAGS -c /tmp/glibc_compat.c -o "$OBJ/glibc_compat.o"
build_target awslc_acvp_server acvp_modulewrapper_main "$OBJ"
