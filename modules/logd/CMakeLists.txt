
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

#logd 只能多线程：写文件在 LogManager 自己的写线程上
#链接base,net,proto,timer,logger
if(MSVC)
	target_link_libraries(logd PUBLIC ${BASE_M_LIBS})
else()
	target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
	target_link_libraries(logd PUBLIC pthread)
endif()
