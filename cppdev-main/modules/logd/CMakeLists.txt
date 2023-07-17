
cmake_minimum_required(VERSION 2.6)

###### logd CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS 
        src/proto_stub.cpp 
		src/main.cpp 
		src/log_server.cpp
		src/log_server_session.cpp
		src/log_manager.cpp
		src/log_file_instance.cpp
		src/log_proto_stubs.cpp
		src/log_protocol.cpp
    )
#可执行程序
add_executable(logd ${MODULE_SRCS})

#头文件目录
target_include_directories(logd
    PUBLIC
		inc
		protocol
)

#logd只能是多线程,受限于logmanager逻辑
set(SINGLE_THREAD FALSE)

#链接base,net,proto,timer,logger
if(SINGLE_THREAD)
	target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
else()
	target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
endif()


if(SINGLE_THREAD)
	#test为单线程,需要设置_SINGLE_THREAD_宏
	target_compile_definitions(logd PUBLIC _SINGLE_THREAD_)
else()
	#test为多线程,需要链接pthread
	target_link_libraries(logd PUBLIC pthread)
endif()