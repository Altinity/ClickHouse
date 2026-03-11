if(NOT FIPS_CLICKHOUSE OR NOT DEFINED AWSLC_SRC_DIR)
    return()
endif()

set(AWSLC_SSL_TEST_DIR "${AWSLC_SRC_DIR}/ssl/test")
set(AWSLC_LIBSSL "${CMAKE_BINARY_DIR}/awslc-build/output/libssl.a")
set(AWSLC_LIBCRYPTO "${CMAKE_BINARY_DIR}/awslc-build/output/libcrypto.a")

set(SHIM_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/awslc_shim_objs")

set(AWSLC_SHIM_CXX_SRCS
    ${AWSLC_SSL_TEST_DIR}/async_bio.cc
    ${AWSLC_SSL_TEST_DIR}/bssl_shim.cc
    ${AWSLC_SSL_TEST_DIR}/handshake_util.cc
    ${AWSLC_SSL_TEST_DIR}/mock_quic_transport.cc
    ${AWSLC_SSL_TEST_DIR}/packeted_bio.cc
    ${AWSLC_SSL_TEST_DIR}/settings_writer.cc
    ${AWSLC_SSL_TEST_DIR}/ssl_transfer.cc
    ${AWSLC_SSL_TEST_DIR}/test_config.cc
    ${AWSLC_SSL_TEST_DIR}/test_state.cc
    ${AWSLC_SRC_DIR}/crypto/test/test_util.cc
)

set(AWSLC_SHIM_INCLUDE_FLAGS
    -I${AWSLC_SRC_DIR}/include
    -I${AWSLC_SRC_DIR}
    -I${AWSLC_SSL_TEST_DIR}
    -I${AWSLC_SRC_DIR}/third_party/googletest/include
)

# Build each aws-lc shim source file with the system compiler (matching
# the libstdc++ ABI of the Docker-built libssl.a / libcrypto.a).
# This avoids the libc++ ↔ libstdc++ C++ ABI mismatch for internal
# types like bssl::UniquePtr<SSL_SESSION>.
set(AWSLC_SHIM_OBJS)
foreach(src ${AWSLC_SHIM_CXX_SRCS})
    get_filename_component(base "${src}" NAME_WE)
    get_filename_component(dir "${src}" DIRECTORY)
    # Disambiguate files with same basename from different directories
    string(MD5 dir_hash "${dir}")
    string(SUBSTRING "${dir_hash}" 0 8 dir_tag)
    set(obj "${SHIM_OBJ_DIR}/${base}_${dir_tag}.o")

    set(extra_defs "")
    if("${base}" STREQUAL "bssl_shim")
        set(extra_defs -Dmain=bssl_shim_main)
    endif()

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHIM_OBJ_DIR}"
        COMMAND /usr/bin/c++
            -std=c++17 -fPIC -g -O2
            ${AWSLC_SHIM_INCLUDE_FLAGS}
            ${extra_defs}
            -w
            -c "${src}"
            -o "${obj}"
        DEPENDS "${src}" build-awslc
        COMMENT "Building aws-lc shim object ${base}.cc (system C++ ABI)"
    )
    list(APPEND AWSLC_SHIM_OBJS "${obj}")
endforeach()

# Compile the glibc C23 compatibility shim (provides __isoc23_strtoll etc.
# which newer GCC/glibc emit but the ClickHouse sysroot's libc lacks).
set(GLIBC_COMPAT_OBJ "${SHIM_OBJ_DIR}/glibc_compat.o")
add_custom_command(
    OUTPUT "${GLIBC_COMPAT_OBJ}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHIM_OBJ_DIR}"
    COMMAND /usr/bin/cc -fPIC -O2 -w
        -c "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
        -o "${GLIBC_COMPAT_OBJ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
    COMMENT "Building glibc C23 compat shim"
)
list(APPEND AWSLC_SHIM_OBJS "${GLIBC_COMPAT_OBJ}")

# Bundle all system-compiled objects into a static library
set(AWSLC_SHIM_LIB "${SHIM_OBJ_DIR}/libawslc_shim.a")
add_custom_command(
    OUTPUT "${AWSLC_SHIM_LIB}"
    COMMAND ${CMAKE_AR} rcs "${AWSLC_SHIM_LIB}" ${AWSLC_SHIM_OBJS}
    DEPENDS ${AWSLC_SHIM_OBJS}
    COMMENT "Archiving libawslc_shim.a"
)
add_custom_target(awslc_shim_lib DEPENDS "${AWSLC_SHIM_LIB}")

# The CH-side entry point: only this file uses the ClickHouse toolchain.
# It contains a plain C-linkage function so there is no C++ ABI concern.
set(CLICKHOUSE_SSL_SHIM_SOURCES SslShim.cpp)
# The system-compiled shim objects use libstdc++ (matching the Docker-built
# libssl.a ABI). Link the full static libstdc++ to satisfy those symbols.
# The --allow-multiple-definition flag handles the few symbols that overlap
# between libstdc++ and ClickHouse's libc++/libcxxabi (e.g. std::exception).
find_file(LIBSTDCXX_STATIC libstdc++.a
    PATHS /usr/lib/gcc/x86_64-linux-gnu
    PATH_SUFFIXES 13 12 11
    NO_DEFAULT_PATH
)
if(NOT LIBSTDCXX_STATIC)
    message(FATAL_ERROR "Could not find static libstdc++.a needed for ssl-shim")
endif()

set(CLICKHOUSE_SSL_SHIM_LINK
    PRIVATE
        "${AWSLC_SHIM_LIB}"
        "${AWSLC_LIBSSL}"
        "${AWSLC_LIBCRYPTO}"
        "${LIBSTDCXX_STATIC}"
)
set(CLICKHOUSE_SSL_SHIM_INCLUDE PRIVATE ${AWSLC_SRC_DIR}/include)

clickhouse_program_add(ssl-shim)
add_dependencies(clickhouse-ssl-shim-lib awslc_shim_lib build-awslc)
target_compile_options(clickhouse-ssl-shim-lib PRIVATE -Wno-everything)
