
cmake_minimum_required(VERSION 2.6)

###### ipcserver CMake??? ######

#???????г?????????????
set(MODULE_SRCS 
        src/main.cpp src/ipcserver.cpp 
    )
#????г???
add_executable(ipcserver ${MODULE_SRCS})

#??????
target_include_directories(ipcserver
    PUBLIC
		inc
		protocol
		${CMAKE_SOURCE_DIR}/modules/include
)

set(SINGLE_THREAD FALSE)

#????base,net,proto,timer,logger
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(ipcserver PUBLIC ${BASE_LIBS})
		target_compile_definitions(ipcserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcserver PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(ipcserver PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(ipcserver PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcserver PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(ipcserver PUBLIC pthread)
	endif()
endif()


