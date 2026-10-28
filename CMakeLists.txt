cmake_minimum_required(VERSION 3.10)

#开启编译详细信息
set(CMAKE_VERBOSE_MAKEFILE ON)

project(CPP_DEV)

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

set(BASE_M_LIBS timer_m_static logger_m_static redis_m_static net_m_static proto_m_static base_m_static hiredis_static)
set(BASE_LIBS timer_static logger_static redis_static net_static proto_static base_static hiredis_static)
set(LOGD_M_LIBS timer_m_static logd_m_static net_m_static proto_m_static base_m_static )


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


set(PROTO_TOOL_PATH ${BASE_LIB_PATH}/proto/proto_tool/main.py)
set(PROTO_XML_PATH ${MODULES_PATH}/xmlconfig)
set(PROTO_GEN_PARAM 
    ${PROTO_XML_PATH}/proto.xml  
    ${PROTO_XML_PATH}/module.xml 
    ${PROTO_XML_PATH}/module_component.xml 
    ${MODULES_PATH}/include 
    ${MODULES_PATH} 
    ${DEPOLY_PATH}
)

find_program(PYTHON_BIN NAMES python3 python)
execute_process(COMMAND  echo "---------------ProtoAndModuleGen Start---------------")
execute_process(COMMAND  ${PYTHON_BIN} ${PROTO_TOOL_PATH} ${PROTO_GEN_PARAM})
execute_process(COMMAND  echo "---------------ProtoAndModuleGen End---------------")

#设置可执行程序和库文件生成目录到 deploy/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${DEPOLY_PATH}/ar)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${DEPOLY_PATH}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${DEPOLY_PATH}/bin)
set(LIBRARY_OUTPUT_PATH ${DEPOLY_PATH}/path)

#增加编译模块。新模块由 xml 生成到 modules/<name>/CMakeLists.txt
foreach(_mod logd ipcclient ipcserver redisserver webserver webclient)
    if(EXISTS ${MODULES_PATH}/${_mod}/CMakeLists.txt)
        add_subdirectory(${MODULES_PATH}/${_mod})
    endif()
endforeach()




