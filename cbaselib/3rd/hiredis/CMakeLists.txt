cmake_minimum_required(VERSION 3.10)

# hiredis 1.5.0，只编库本身。协议和异步连接用它，不把它的安装和测试带进来。
add_library(hiredis_static STATIC
	alloc.c
	async.c
	hiredis.c
	net.c
	read.c
	sds.c
	sockcompat.c
)

target_include_directories(hiredis_static SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
	target_compile_definitions(hiredis_static PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
	target_compile_options(hiredis_static PRIVATE /W0)
	target_link_libraries(hiredis_static PUBLIC ws2_32)
endif()
