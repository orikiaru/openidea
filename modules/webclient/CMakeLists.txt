cmake_minimum_required(VERSION 2.6)

###### webclient CMake文件 ######

set(MODULE_SRCS
	src/main.cpp
	src/web_client.cpp
	src/test_http_bodyless.cpp
)

add_executable(webclient ${MODULE_SRCS})

target_include_directories(webclient
	PUBLIC
		inc
	PRIVATE
		../webtest/inc
)

# 客户端挂在 poll 的 libuv 上，单线程就能 co_await。多线程模块改链 http_m_static。
option(OI_WEBCLIENT_SINGLE_THREAD "Run webclient on the single-thread scheduler" ON)
set(SINGLE_THREAD ${OI_WEBCLIENT_SINGLE_THREAD})

if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(webclient PUBLIC http_static ${BASE_LIBS})
		target_compile_definitions(webclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webclient PUBLIC http_m_static ${BASE_M_LIBS})
	endif()
	target_link_libraries(webclient PUBLIC ws2_32)
	target_link_libraries(webclient PUBLIC jsoncpp_static)
else()
	if(SINGLE_THREAD)
		target_link_libraries(webclient PUBLIC -Wl,--start-group http_static ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(webclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webclient PUBLIC -Wl,--start-group http_m_static ${BASE_M_LIBS} -Wl,--end-group)
	endif()
	target_link_libraries(webclient PUBLIC pthread)
	target_link_libraries(webclient PUBLIC jsoncpp_static)
endif()
