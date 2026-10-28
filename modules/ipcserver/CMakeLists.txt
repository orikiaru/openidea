cmake_minimum_required(VERSION 2.6)

###### ipcserver CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS
        src/main.cpp src/ipcserver.cpp
    )
#可执行程序
add_executable(ipcserver ${MODULE_SRCS})

#头文件目录
target_include_directories(ipcserver
    PUBLIC
		inc
		${CMAKE_SOURCE_DIR}/modules/include
)

set(SINGLE_THREAD FALSE)

#链接base,net,proto,timer,logger,http
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(ipcserver PUBLIC http_static ${BASE_LIBS})
		target_compile_definitions(ipcserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcserver PUBLIC http_m_static ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(ipcserver PUBLIC -Wl,--start-group http_static ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(ipcserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcserver PUBLIC -Wl,--start-group http_m_static ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(ipcserver PUBLIC pthread)
	endif()
endif()
