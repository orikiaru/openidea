cmake_minimum_required(VERSION 3.10)

#开启编译详细信息
set(CMAKE_VERBOSE_MAKEFILE ON)

project(CPP_DEV)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(OI_IPC_SINGLE_THREAD "Build ipcclient and ipcserver with single-thread scheduling" OFF)
option(OI_ENABLE_TEST_HOOKS "Enable isolated regression fault injection (never enable in production)" OFF)

# 给 clangd 用。Ninja 直接写 compile_commands.json；其它生成器编译时刷 build-clangd-win 或 build-clangd-linux。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Write compile_commands.json for clangd" FORCE)

include("${CMAKE_CURRENT_SOURCE_DIR}/framework.cmake")

set(MODULES_PATH ${CMAKE_SOURCE_DIR}/modules)
set(DEPOLY_PATH ${CMAKE_SOURCE_DIR}/deploy)
include_directories(${MODULES_PATH}/include)

set(OI_PROTO_CONFIG ${MODULES_PATH}/protoconfig)
# 协议正文可以有多份，按这个列表的顺序读入。名字和协议号在全部文件里不能重复。
set(OI_PROTO_FILES
    ${OI_PROTO_CONFIG}/db.proto
    ${OI_PROTO_CONFIG}/json.proto
    ${OI_PROTO_CONFIG}/rpc.proto
)
set(PROTO_INPUTS
    ${OI_PROTO_FILES}
    ${OI_PROTO_CONFIG}/module.proto
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROTO_INPUTS})

set(OI_MODULE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/oi_modules.cmake")
execute_process(
    COMMAND "${PYTHON_BIN}" "${PROTO_TOOL_DIR}/main.py"
        "${OI_PROTO_CONFIG}/module.proto"
        "${MODULES_PATH}/include" "${MODULES_PATH}" "${DEPOLY_PATH}" "${OI_MODULE_MANIFEST}"
        -- ${OI_PROTO_FILES}
    RESULT_VARIABLE PROTO_GEN_RESULT
)
if(NOT "${PROTO_GEN_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Protocol and module generation failed: ${PROTO_GEN_RESULT}")
endif()
include("${OI_MODULE_MANIFEST}")

#设置可执行程序和库文件生成目录到 deploy/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${DEPOLY_PATH}/ar)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${DEPOLY_PATH}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${DEPOLY_PATH}/bin)
set(LIBRARY_OUTPUT_PATH ${DEPOLY_PATH}/path)
oi_add_framework_programs("${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# 模块名单由 module.proto 生成。
foreach(_mod IN LISTS OI_MODULES)
    add_subdirectory("${MODULES_PATH}/${_mod}")
    if(TARGET oi_clangd_db AND TARGET ${_mod})
        add_dependencies(${_mod} oi_clangd_db)
    endif()
endforeach()

if(OI_ENABLE_TEST_HOOKS AND TARGET logd)
    target_compile_definitions(logd PRIVATE OI_ENABLE_TEST_HOOKS)
endif()
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/oi_test_hooks.txt" CONTENT "${OI_ENABLE_TEST_HOOKS}\n")
