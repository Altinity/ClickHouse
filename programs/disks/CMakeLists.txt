set (CLICKHOUSE_DISKS_SOURCES
    DisksApp.cpp
    DisksClient.cpp
    ICommand.cpp
    CommandChangeDirectory.cpp
    CommandCopy.cpp
    CommandLink.cpp
    CommandList.cpp
    CommandListDisks.cpp
    CommandMkDir.cpp
    CommandMove.cpp
    CommandRead.cpp
    CommandReadBitmap.cpp
    CommandReadChecksums.cpp
    CommandRemove.cpp
    CommandSwitchDisk.cpp
    CommandWrite.cpp
    CommandSed.cpp
    CommandHelp.cpp
    CommandTouch.cpp
    CommandFsck.cpp
    CommandCaGcDryRun.cpp
    CommandCaGcRebuild.cpp
    CommandCaInspect.cpp
    CommandCaDropMember.cpp
    CommandCaMigratePool.cpp
    CommandGetCurrentDiskAndPath.cpp
    CommandPackedIO.cpp
    CommandDiskUsage.cpp
    CommandWordCount.cpp
    Utils.cpp)

set (CLICKHOUSE_DISKS_LINK
    PRIVATE
        boost::program_options
        clickhouse_aggregate_functions
        clickhouse_common_config
        clickhouse_common_io
        clickhouse_functions
        clickhouse_parsers
        clickhouse_table_functions
        )

clickhouse_program_add(disks)

# The CAS gtests drive `DisksClient` and the `cas-*` commands in-process, as the binary does. These headers include one
# another by bare name, so they are linked into one directory that is the test's only include path into this one.
if (TARGET unit_tests_dbms)
    set (DISKS_HEADERS_FOR_TESTS_DIR ${CMAKE_CURRENT_BINARY_DIR}/headers_for_tests)
    file (MAKE_DIRECTORY ${DISKS_HEADERS_FOR_TESTS_DIR})
    foreach (header DisksApp.h DisksClient.h ICommand.h ICommand_fwd.h Utils.h)
        file (CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${header} ${DISKS_HEADERS_FOR_TESTS_DIR}/${header} SYMBOLIC)
    endforeach ()
    target_link_libraries(unit_tests_dbms PRIVATE clickhouse-disks-lib)
    target_include_directories(unit_tests_dbms PRIVATE ${DISKS_HEADERS_FOR_TESTS_DIR})
endif()
