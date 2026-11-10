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

set(PROTO_XML_PATH ${MODULES_PATH}/xmlconfig)
set(PROTO_INPUTS
    ${PROTO_XML_PATH}/proto.xml
    ${PROTO_XML_PATH}/module.xml
    ${PROTO_XML_PATH}/module_component.xml
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROTO_INPUTS})

set(OI_MODULE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/oi_modules.cmake")
execute_process(
    COMMAND "${PYTHON_BIN}" "${PROTO_TOOL_DIR}/main.py" ${PROTO_INPUTS}
        "${MODULES_PATH}/include" "${MODULES_PATH}" "${DEPOLY_PATH}" "${OI_MODULE_MANIFEST}"
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

# 模块名单由 module.xml 生成。
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
