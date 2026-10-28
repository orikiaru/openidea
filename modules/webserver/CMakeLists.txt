cmake_minimum_required(VERSION 2.6)

###### webserver CMake文件 ######

set(MODULE_SRCS
	src/main.cpp
	src/web_server.cpp
)

add_executable(webserver ${MODULE_SRCS})

target_include_directories(webserver
	PUBLIC
		inc
)
target_include_directories(webserver SYSTEM
	PRIVATE
		${CMAKE_SOURCE_DIR}/cbaselib/3rd/cpp-httplib
)

# HTTP 服务自己带线程。这里不再开业务线程池。
set(SINGLE_THREAD TRUE)

if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(webserver PUBLIC ${BASE_LIBS})
		target_compile_definitions(webserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webserver PUBLIC ${BASE_M_LIBS})
	endif()
	target_link_libraries(webserver PUBLIC ws2_32)
	target_link_libraries(webserver PUBLIC jsoncpp_static)
else()
	if(SINGLE_THREAD)
		target_link_libraries(webserver PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(webserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webserver PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
	endif()
	target_link_libraries(webserver PUBLIC pthread)
	target_link_libraries(webserver PUBLIC jsoncpp_static)
endif()
