if(NOT FIPS_CLICKHOUSE OR NOT DEFINED AWSLC_SRC_DIR)
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(AWSLC_ACVP_MODULEWRAPPER_DIR "${AWSLC_SRC_DIR}/util/fipstools/acvp/modulewrapper")
set(AWSLC_LIBCRYPTO "${CMAKE_BINARY_DIR}/awslc-build/output/libcrypto.a")

set(ACVP_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/awslc_acvp_server_objs")

set(AWSLC_ACVP_SERVER_CXX_SRCS
    ${AWSLC_ACVP_MODULEWRAPPER_DIR}/main.cc
    ${AWSLC_ACVP_MODULEWRAPPER_DIR}/modulewrapper.cc
)

set(AWSLC_ACVP_SERVER_INCLUDE_FLAGS
    -I${AWSLC_ACVP_MODULEWRAPPER_DIR}
    -I${AWSLC_SRC_DIR}/include
    -I${AWSLC_SRC_DIR}
)

set(AWSLC_ACVP_SERVER_OBJS)
foreach(src ${AWSLC_ACVP_SERVER_CXX_SRCS})
    get_filename_component(base "${src}" NAME_WE)
    get_filename_component(dir "${src}" DIRECTORY)
    string(MD5 dir_hash "${dir}")
    string(SUBSTRING "${dir_hash}" 0 8 dir_tag)
    set(obj "${ACVP_OBJ_DIR}/${base}_${dir_tag}.o")

    set(extra_defs "")
    if("${base}" STREQUAL "main")
        set(extra_defs -Dmain=acvp_modulewrapper_main)
    endif()

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ACVP_OBJ_DIR}"
        COMMAND /usr/bin/c++
            -std=c++17 -fPIC -g -O2
            ${AWSLC_ACVP_SERVER_INCLUDE_FLAGS}
            ${extra_defs}
            -w
            -c "${src}"
            -o "${obj}"
        DEPENDS "${src}" build-awslc
        COMMENT "Building aws-lc acvp-server object ${base}.cc (system C++ ABI)"
    )
    list(APPEND AWSLC_ACVP_SERVER_OBJS "${obj}")
endforeach()

set(GLIBC_COMPAT_ACVP_OBJ "${ACVP_OBJ_DIR}/glibc_compat.o")
add_custom_command(
    OUTPUT "${GLIBC_COMPAT_ACVP_OBJ}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ACVP_OBJ_DIR}"
    COMMAND /usr/bin/cc -fPIC -O2 -w
        -c "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
        -o "${GLIBC_COMPAT_ACVP_OBJ}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ssl-common/glibc_compat.c"
    COMMENT "Building glibc C23 compat shim (acvp-server)"
)
list(APPEND AWSLC_ACVP_SERVER_OBJS "${GLIBC_COMPAT_ACVP_OBJ}")

set(AWSLC_ACVP_SERVER_LIB "${ACVP_OBJ_DIR}/libawslc_acvp_server.a")
add_custom_command(
    OUTPUT "${AWSLC_ACVP_SERVER_LIB}"
    COMMAND ${CMAKE_AR} rcs "${AWSLC_ACVP_SERVER_LIB}" ${AWSLC_ACVP_SERVER_OBJS}
    DEPENDS ${AWSLC_ACVP_SERVER_OBJS}
    COMMENT "Archiving libawslc_acvp_server.a"
)
add_custom_target(awslc_acvp_server_lib DEPENDS "${AWSLC_ACVP_SERVER_LIB}")

set(CLICKHOUSE_ACVP_SERVER_SOURCES AcvpServer.cpp)
find_file(LIBSTDCXX_STATIC_ACVP libstdc++.a
    PATHS /usr/lib/gcc/x86_64-linux-gnu
    PATH_SUFFIXES 13 12 11
    NO_DEFAULT_PATH
)
if(NOT LIBSTDCXX_STATIC_ACVP)
    message(FATAL_ERROR "Could not find static libstdc++.a needed for acvp-server")
endif()

set(CLICKHOUSE_ACVP_SERVER_LINK
    PRIVATE
        "${AWSLC_ACVP_SERVER_LIB}"
        "${AWSLC_LIBCRYPTO}"
        "${LIBSTDCXX_STATIC_ACVP}"
)
set(CLICKHOUSE_ACVP_SERVER_INCLUDE PRIVATE ${AWSLC_SRC_DIR}/include)

clickhouse_program_add(acvp-server)
add_dependencies(clickhouse-acvp-server-lib awslc_acvp_server_lib build-awslc)
target_compile_options(clickhouse-acvp-server-lib PRIVATE -Wno-everything)
target_link_options(clickhouse-acvp-server-lib INTERFACE "LINKER:--allow-multiple-definition")
