if(NOT FIPS_CLICKHOUSE OR NOT DEFINED AWSLC_SRC_DIR)
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(AWSLC_SSL_TEST_DIR "${AWSLC_SRC_DIR}/ssl/test")
set(AWSLC_LIBSSL "${CMAKE_BINARY_DIR}/awslc-build/output/libssl.a")
set(AWSLC_LIBCRYPTO "${CMAKE_BINARY_DIR}/awslc-build/output/libcrypto.a")

set(HSHAKER_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/awslc_handshaker_objs")

set(AWSLC_HSHAKER_CXX_SRCS
    ${AWSLC_SSL_TEST_DIR}/async_bio.cc
    ${AWSLC_SSL_TEST_DIR}/handshake_util.cc
    ${AWSLC_SSL_TEST_DIR}/handshaker.cc
    ${AWSLC_SSL_TEST_DIR}/mock_quic_transport.cc
    ${AWSLC_SSL_TEST_DIR}/packeted_bio.cc
    ${AWSLC_SSL_TEST_DIR}/settings_writer.cc
    ${AWSLC_SSL_TEST_DIR}/test_config.cc
    ${AWSLC_SSL_TEST_DIR}/test_state.cc
    ${AWSLC_SRC_DIR}/crypto/test/test_util.cc
)

set(AWSLC_HSHAKER_INCLUDE_FLAGS
    -I${AWSLC_SRC_DIR}/include
    -I${AWSLC_SRC_DIR}
    -I${AWSLC_SSL_TEST_DIR}
    -I${AWSLC_SRC_DIR}/third_party/googletest/include
)

set(AWSLC_HSHAKER_OBJS)
foreach(src ${AWSLC_HSHAKER_CXX_SRCS})
    get_filename_component(base "${src}" NAME_WE)
    get_filename_component(dir "${src}" DIRECTORY)
    string(MD5 dir_hash "${dir}")
    string(SUBSTRING "${dir_hash}" 0 8 dir_tag)
    set(obj "${HSHAKER_OBJ_DIR}/${base}_${dir_tag}.o")

    set(extra_defs "")
    if("${base}" STREQUAL "handshaker")
        set(extra_defs -Dmain=handshaker_main)
    endif()

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${HSHAKER_OBJ_DIR}"
        COMMAND /usr/bin/c++
            -std=c++17 -fPIC -g -O2
            ${AWSLC_HSHAKER_INCLUDE_FLAGS}
            ${extra_defs}
            -w
            -c "${src}"
            -o "${obj}"
        DEPENDS "${src}" build-awslc
        COMMENT "Building aws-lc handshaker object ${base}.cc (system C++ ABI)"
    )
    list(APPEND AWSLC_HSHAKER_OBJS "${obj}")
endforeach()

set(GLIBC_COMPAT_HS_OBJ "${HSHAKER_OBJ_DIR}/glibc_compat.o")
add_custom_command(
    OUTPUT "${GLIBC_COMPAT_HS_OBJ}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HSHAKER_OBJ_DIR}"
    COMMAND /usr/bin/cc -fPIC -O2 -w
        -c "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
        -o "${GLIBC_COMPAT_HS_OBJ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
    COMMENT "Building glibc C23 compat shim (handshaker)"
)
list(APPEND AWSLC_HSHAKER_OBJS "${GLIBC_COMPAT_HS_OBJ}")

set(AWSLC_HSHAKER_LIB "${HSHAKER_OBJ_DIR}/libawslc_handshaker.a")
add_custom_command(
    OUTPUT "${AWSLC_HSHAKER_LIB}"
    COMMAND ${CMAKE_AR} rcs "${AWSLC_HSHAKER_LIB}" ${AWSLC_HSHAKER_OBJS}
    DEPENDS ${AWSLC_HSHAKER_OBJS}
    COMMENT "Archiving libawslc_handshaker.a"
)
add_custom_target(awslc_handshaker_lib DEPENDS "${AWSLC_HSHAKER_LIB}")

set(CLICKHOUSE_SSL_HANDSHAKER_SOURCES SslHandshaker.cpp)
find_file(LIBSTDCXX_STATIC_HS libstdc++.a
    PATHS /usr/lib/gcc/x86_64-linux-gnu
    PATH_SUFFIXES 13 12 11
    NO_DEFAULT_PATH
)
if(NOT LIBSTDCXX_STATIC_HS)
    message(FATAL_ERROR "Could not find static libstdc++.a needed for ssl-handshaker")
endif()

set(CLICKHOUSE_SSL_HANDSHAKER_LINK
    PRIVATE
        "${AWSLC_HSHAKER_LIB}"
        "${AWSLC_LIBSSL}"
        "${AWSLC_LIBCRYPTO}"
        "${LIBSTDCXX_STATIC_HS}"
)
set(CLICKHOUSE_SSL_HANDSHAKER_INCLUDE PRIVATE ${AWSLC_SRC_DIR}/include)

clickhouse_program_add(ssl-handshaker)
add_dependencies(clickhouse-ssl-handshaker-lib awslc_handshaker_lib build-awslc)
target_compile_options(clickhouse-ssl-handshaker-lib PRIVATE -Wno-everything)
