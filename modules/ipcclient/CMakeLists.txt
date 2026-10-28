cmake_minimum_required(VERSION 2.6)

###### ipcclient CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS
        src/main.cpp
        src/ipcclient.cpp
        src/test_board.cpp
        src/echo_mgr.cpp
        src/log_mgr.cpp
        src/redis_mgr.cpp
        src/hold_mgr.cpp
    )
#可执行程序
add_executable(ipcclient ${MODULE_SRCS})

#头文件目录
target_include_directories(ipcclient
    PUBLIC
		inc
		protocol
		${CMAKE_SOURCE_DIR}/modules/include
)

set(SINGLE_THREAD FALSE)

#链接base,net,proto,timer,logger
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(ipcclient PUBLIC ${BASE_LIBS})
		target_compile_definitions(ipcclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcclient PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(ipcclient PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(ipcclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcclient PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(ipcclient PUBLIC pthread)
	endif()
endif()
