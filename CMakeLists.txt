cmake_minimum_required(VERSION 3.10)

#开启编译详细信息
set(CMAKE_VERBOSE_MAKEFILE ON)

project(CPP_DEV)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(OI_IPC_SINGLE_THREAD "Build ipcclient and ipcserver with single-thread scheduling" OFF)
option(OI_ENABLE_TEST_HOOKS "Enable isolated regression fault injection (never enable in production)" OFF)

# 给 clangd 用。Ninja 配置后会在构建目录写出 compile_commands.json。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Write compile_commands.json for clangd" FORCE)

if(MSVC)
    add_compile_options(/W3)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++20>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
else()
    set(_CXX_OPTION_ -ggdb -g -Wall -std=gnu++20
        -Werror -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-literal-suffix
        -Wno-format-security
        -Wno-overloaded-virtual
    )
    # GCC 10 的协程还要单独打开。GCC 11 起 -std=gnu++20 已默认带上。
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        list(APPEND _CXX_OPTION_ -fcoroutines)
    endif()
    # 只加到 C++。这些选项进了 C 文件后，gcc 会直接拒绝编译 libuv。
    foreach(_opt IN LISTS _CXX_OPTION_)
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${_opt}>")
    endforeach()
endif()

#一些通用路径变量
set(BASE_LIB_PATH ${CMAKE_SOURCE_DIR}/cbaselib)
set(MODULES_PATH ${CMAKE_SOURCE_DIR}/modules)

set(BASE_M_LIBS timer_m_static logger_m_static redis_m_static net_m_static proto_m_static base_m_static net_m_static timer_m_static hiredis_static)
set(BASE_LIBS timer_static logger_static redis_static net_static proto_static base_static net_static timer_static hiredis_static)

set(DEPOLY_PATH ${CMAKE_SOURCE_DIR}/deploy)

# libuv 只编静态库，跟工程一起编译
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${BASE_LIB_PATH}/3rd/libuv EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/3rd/hiredis EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/3rd/jsoncpp EXCLUDE_FROM_ALL)
if(MSVC AND TARGET uv_a)
	target_compile_options(uv_a PRIVATE /W0)
endif()

#通用头文件目录。放在 libuv 后面，避免 proto/inc/rpc.h 盖住 Windows 的 rpc.h。
include_directories(
	${BASE_LIB_PATH}/include
    ${BASE_LIB_PATH}/base/inc
    ${BASE_LIB_PATH}/net/inc
    ${BASE_LIB_PATH}/timer/inc
    ${BASE_LIB_PATH}/logger/inc
    ${BASE_LIB_PATH}/redis/inc
    ${BASE_LIB_PATH}/http/inc
    ${BASE_LIB_PATH}/proto/inc
	${MODULES_PATH}/include
	${BASE_LIB_PATH}/port
)
include_directories(SYSTEM
	${BASE_LIB_PATH}/3rd/cpptoml
	${BASE_LIB_PATH}/3rd/libuv/include
	${BASE_LIB_PATH}/3rd/hiredis
	${BASE_LIB_PATH}/3rd/jsoncpp/include
)

if(MSVC)
    include_directories(${BASE_LIB_PATH}/port/win)
    add_compile_options("/FI${BASE_LIB_PATH}/port/win/msvc_prefix.h")
    # 三方头走 SYSTEM 目录，按外部头处理，不再报它们自己的警告。
    add_compile_options(/external:W0)
endif()

#标准库依赖目录
add_subdirectory(${BASE_LIB_PATH}/base EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/net  EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/proto EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/timer EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/logger EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/redis EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/http EXCLUDE_FROM_ALL)


set(PROTO_TOOL_DIR "${BASE_LIB_PATH}/proto/proto_tool")
set(PROTO_XML_PATH ${MODULES_PATH}/xmlconfig)
set(PROTO_INPUTS
    ${PROTO_XML_PATH}/proto.xml
    ${PROTO_XML_PATH}/module.xml
    ${PROTO_XML_PATH}/module_component.xml
)
file(GLOB PROTO_TOOL_SOURCES "${PROTO_TOOL_DIR}/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROTO_INPUTS} ${PROTO_TOOL_SOURCES})

find_program(PYTHON_BIN NAMES python3 python)
if(NOT PYTHON_BIN)
    message(FATAL_ERROR "Python 3 is required to generate protocols and modules")
endif()
set(OI_MODULE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/oi_modules.cmake")
set(OI_WORK_PROTOCOL "${BASE_LIB_PATH}/net/protocol/proto.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OI_WORK_PROTOCOL}")
execute_process(
    COMMAND "${PYTHON_BIN}" "${PROTO_TOOL_DIR}/main.py" --data-only
        "${OI_WORK_PROTOCOL}" "${BASE_LIB_PATH}/net/inc/work_protocol"
    RESULT_VARIABLE WORK_PROTO_GEN_RESULT
)
if(NOT "${WORK_PROTO_GEN_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Internal work protocol generation failed: ${WORK_PROTO_GEN_RESULT}")
endif()
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
endforeach()

if(OI_ENABLE_TEST_HOOKS AND TARGET logd)
    target_compile_definitions(logd PRIVATE OI_ENABLE_TEST_HOOKS)
endif()
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/oi_test_hooks.txt" CONTENT "${OI_ENABLE_TEST_HOOKS}\n")




