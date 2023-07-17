
cmake_minimum_required(VERSION 2.6)

###### test CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS 
        src/proto_stub.cpp src/main.cpp src/testclient.cpp src/testserver.cpp 
		src/test_timer.cpp
		src/test_net.cpp
		src/test_log.cpp
		src/test_common_bytes.cpp
    )
#可执行程序
add_executable(test ${MODULE_SRCS})

#头文件目录
target_include_directories(test
    PUBLIC
		inc
		protocol
)

set(SINGLE_THREAD FALSE)

#链接base,net,proto,timer,logger
if(SINGLE_THREAD)
	target_link_libraries(test PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
else()
	target_link_libraries(test PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
endif()


if(SINGLE_THREAD)
	#test为单线程,需要设置_SINGLE_THREAD_宏
	target_compile_definitions(test PUBLIC _SINGLE_THREAD_)
else()
	#test为多线程,需要链接pthread
	target_link_libraries(test PUBLIC pthread)
endif()

