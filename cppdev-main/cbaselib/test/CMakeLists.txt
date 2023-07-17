cmake_minimum_required(VERSION 2.6)


set(MODULE_SRCS 
        src/proto_stub.cpp src/main.cpp src/testclient.cpp src/testserver.cpp 
		src/test_timer.cpp
		src/test_net.cpp
		src/test_log.cpp
		src/test_common_bytes.cpp
    )

#可执行程序
add_executable(test ${MODULE_SRCS})

target_include_directories(test
    PUBLIC
		inc
		protocol
)

set(SINGLE_THREAD TRUE)

if(SINGLE_THREAD)
	#test为单线程，需要设置_SINGLE_THREAD_宏
	target_compile_definitions(test PUBLIC _SINGLE_THREAD_)
else()
	target_link_libraries(test PUBLIC pthread)
endif()

#test_m为多线程，需要链接pthread

#链接base和net
target_link_libraries(test PUBLIC ${BASE_LIBS})




