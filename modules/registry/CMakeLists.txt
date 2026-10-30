cmake_minimum_required(VERSION 2.6)

set(MODULE_SRCS
	src/main.cpp
)

add_executable(registry ${MODULE_SRCS})

target_include_directories(registry PUBLIC inc)

# 注册表很小，单线程够用。
set(SINGLE_THREAD TRUE)

if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(registry PUBLIC ${BASE_LIBS})
		target_compile_definitions(registry PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(registry PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(registry PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(registry PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(registry PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(registry PUBLIC pthread)
	endif()
endif()
