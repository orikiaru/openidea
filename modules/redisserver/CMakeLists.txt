cmake_minimum_required(VERSION 2.6)

###### redisserver CMake文件 ######

set(MODULE_SRCS
	src/main.cpp
	src/redis_server.cpp
	src/redis_server_session.cpp
)

add_executable(redisserver ${MODULE_SRCS})

target_include_directories(redisserver
	PUBLIC
		inc
)

# 单线程够用。不够就多起进程，不在这里加线程。
set(SINGLE_THREAD TRUE)

if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(redisserver PUBLIC ${BASE_LIBS})
		target_compile_definitions(redisserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(redisserver PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(redisserver PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(redisserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(redisserver PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(redisserver PUBLIC pthread)
	endif()
endif()
