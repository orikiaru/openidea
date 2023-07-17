cmake_minimum_required(VERSION 2.6)

#开启编译详细信息
set(CMAKE_VERBOSE_MAKEFILE ON)

project(C_BASELIB_TEST)

#通用编译选项
set(_CXX_OPTION_ -ggdb -g -Wall -std=gnu++11 
    -Werror -Wno-unused-function 
    -Wno-unused-but-set-variable 
    -Wno-literal-suffix 
    -Wno-format-security
)

add_compile_options(${_CXX_OPTION_})

set(BASE_M_LIBS timer_m_static proto_m_static logger_m_static net_m_static base_m_static )
set(BASE_LIBS timer_static proto_static logger_static net_static base_static )

set(LOGD_M_LIBS timer_m_static proto_m_static logd_m_static net_m_static base_m_static )

set(BASE_LIB_PATH ${CMAKE_SOURCE_DIR}/)

#通用头文件目录
include_directories(
	include/
	base/inc/
	net/inc/
	proto/inc/
	timer/inc/
	logger/inc/
	test/include/
    3rd/cpptoml/
)

#标准库依赖目录
add_subdirectory(${BASE_LIB_PATH}/base)
add_subdirectory(${BASE_LIB_PATH}/net)
add_subdirectory(${BASE_LIB_PATH}/proto)
add_subdirectory(${BASE_LIB_PATH}/timer)
add_subdirectory(${BASE_LIB_PATH}/logger)

#模块生成
set(PROTO_TOOL_PATH ${BASE_LIB_PATH}/proto/proto_tool/main.py)
set(PROTO_XML_PATH ${BASE_LIB_PATH}/proto/proto_tool/config)
set(PROTO_GEN_PARAM ${PROTO_XML_PATH}/proto.xml  ${PROTO_XML_PATH}/module.xml ${PROTO_XML_PATH}/module_component.xml ${PROTO_XML_PATH}/module_gen_test/include ${PROTO_XML_PATH}/module_gen_test)

execute_process(COMMAND  echo "---------------ProtoAndModuleGen Start---------------")
execute_process(COMMAND  python3 ${PROTO_TOOL_PATH} ${PROTO_GEN_PARAM})
execute_process(COMMAND  echo "---------------ProtoAndModuleGen End---------------")

set(DEPOLY_PATH ${CMAKE_SOURCE_DIR}/test/deploy)
#设置可执行程序和库文件生成目录到 test/deploy/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${DEPOLY_PATH}/ar)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${DEPOLY_PATH}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${DEPOLY_PATH}/bin)
set(LIBRARY_OUTPUT_PATH ${DEPOLY_PATH}/path)


add_subdirectory(./test)




