
cmake_minimum_required(VERSION 2.6)

###### logd CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS 
		src/main.cpp 
		src/log_server.cpp
		src/log_server_session.cpp
		src/log_manager.cpp
		src/log_file_instance.cpp
    )
#可执行程序
add_executable(logd ${MODULE_SRCS})

#头文件目录
target_include_directories(logd
    PUBLIC
		inc
)

#logd只能是多线程,受限于logmanager逻辑
set(SINGLE_THREAD FALSE)

#链接base,net,proto,timer,logger
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(logd PUBLIC ${BASE_LIBS})
		target_compile_definitions(logd PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(logd PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(logd PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(logd PUBLIC pthread)
	endif()
endif()
