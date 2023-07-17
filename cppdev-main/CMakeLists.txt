cmake_minimum_required(VERSION 2.6)

#开启编译详细信息
set(CMAKE_VERBOSE_MAKEFILE ON)

project(CPP_DEV)

#通用编译选项
set(_CXX_OPTION_ -ggdb -g -Wall -std=gnu++11 
    -Werror -Wno-unused-function 
    -Wno-unused-but-set-variable 
    -Wno-literal-suffix 
    -Wno-format-security
)
add_compile_options(${_CXX_OPTION_})

function (CheckLibExist lib_name)
    message(STATUS "CheckLibExist lib_name=" ${lib_name})
    find_library(LIBVAR ${lib_name})
    if(${LIBVAR} STREQUAL "LIBVAR-NOTFOUND")
        message(FATAL_ERROR "CheckLibExist not find lib_name=" ${lib_name})
    else()
        message(STATUS "CheckLibExist find lib_name=" ${LIBVAR})
    endif()
endfunction()

#是否使用openssl
set(_USE_SSL_ false)
if (_USE_SSL_)
    CheckLibExist(ssl)
    # debian系可以通过  sudo apt update && sudo apt-get install openssl && sudo apt-get install libssl-dev 来获取库环境
else()
    message(STATUS "AddDefine  __UNUSE_OPENSSL__")
    add_compile_definitions(__UNUSE_OPENSSL__)
endif()

#一些通用路径变量
set(BASE_LIB_PATH ${CMAKE_SOURCE_DIR}/cbaselib)
set(MODULES_PATH ${CMAKE_SOURCE_DIR}/modules)

set(BASE_M_LIBS timer_m_static logger_m_static net_m_static proto_m_static base_m_static )
set(BASE_LIBS timer_static logger_static net_static proto_static base_static)
set(LOGD_M_LIBS timer_m_static logd_m_static net_m_static proto_m_static base_m_static )


set(DEPOLY_PATH ${CMAKE_SOURCE_DIR}/deploy)

#通用头文件目录
include_directories(
    #基础库头文件
	${BASE_LIB_PATH}/include
    ${BASE_LIB_PATH}/base/inc
    ${BASE_LIB_PATH}/net/inc
    ${BASE_LIB_PATH}/timer/inc
    ${BASE_LIB_PATH}/logger/inc
    ${BASE_LIB_PATH}/proto/inc

    #模块开发通用依赖头文件
    ${MODULES_PATH}/include

    #一些三方库
	${BASE_LIB_PATH}/3rd/cpptoml

)

#标准库依赖目录
add_subdirectory(${BASE_LIB_PATH}/base EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/net  EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/proto EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/timer EXCLUDE_FROM_ALL)
add_subdirectory(${BASE_LIB_PATH}/logger EXCLUDE_FROM_ALL)


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

execute_process(COMMAND  echo "---------------ProtoAndModuleGen Start---------------")
execute_process(COMMAND  python3 ${PROTO_TOOL_PATH} ${PROTO_GEN_PARAM})
execute_process(COMMAND  echo "---------------ProtoAndModuleGen End---------------")

#设置可执行程序和库文件生成目录到 deploy/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${DEPOLY_PATH}/ar)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${DEPOLY_PATH}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${DEPOLY_PATH}/bin)
set(LIBRARY_OUTPUT_PATH ${DEPOLY_PATH}/path)

#增加编译模块
add_subdirectory(${MODULES_PATH}/test)
add_subdirectory(${MODULES_PATH}/logd)




