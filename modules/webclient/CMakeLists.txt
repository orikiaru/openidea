cmake_minimum_required(VERSION 2.6)

###### webclient CMake文件 ######

set(MODULE_SRCS
	src/main.cpp
	src/web_client.cpp
)

add_executable(webclient ${MODULE_SRCS})

target_include_directories(webclient
	PUBLIC
		inc
)
target_include_directories(webclient SYSTEM
	PRIVATE
		${CMAKE_SOURCE_DIR}/cbaselib/3rd/cpp-httplib
		${CMAKE_SOURCE_DIR}/cbaselib/3rd/nlohmann
)

set(SINGLE_THREAD TRUE)

if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(webclient PUBLIC ${BASE_LIBS})
		target_compile_definitions(webclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webclient PUBLIC ${BASE_M_LIBS})
	endif()
	target_link_libraries(webclient PUBLIC ws2_32)
else()
	if(SINGLE_THREAD)
		target_link_libraries(webclient PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(webclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(webclient PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
	endif()
	target_link_libraries(webclient PUBLIC pthread)
endif()
