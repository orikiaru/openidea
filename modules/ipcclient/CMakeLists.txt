cmake_minimum_required(VERSION 2.6)

###### ipcclient CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS
        src/main.cpp
        src/ipcclient.cpp
        src/db_mgr.cpp
        src/test_board.cpp
        src/echo_mgr.cpp
        src/log_mgr.cpp
        src/redis_mgr.cpp
        src/hold_mgr.cpp
        src/timer_mgr.cpp
        src/nested_result_mgr.cpp
        src/task_start_mgr.cpp
        src/task_share_mgr.cpp
        src/blocking_mgr.cpp
        src/connect_mgr.cpp
        src/address_mgr.cpp
        src/isolation_mgr.cpp
        src/backpressure_mgr.cpp
        src/rpc_output_mgr.cpp
        src/rpc_await_mgr.cpp
        src/rpc_admission_mgr.cpp
        src/route_mgr.cpp
        src/registry_mgr.cpp
        src/rpc_service_mgr.cpp
        src/json_mgr.cpp
        src/binary_mgr.cpp
        src/encode_mgr.cpp
        src/http_mgr.cpp
        src/budget_mgr.cpp
        src/schedule_mgr.cpp
        src/rpc_pending_mgr.cpp
        src/udp_mgr.cpp
        src/multi_mgr.cpp
    )
#可执行程序
add_executable(ipcclient ${MODULE_SRCS})

#头文件目录
target_include_directories(ipcclient
    PUBLIC
		inc
		${CMAKE_SOURCE_DIR}/modules/include
)

set(SINGLE_THREAD ${OI_IPC_SINGLE_THREAD})

#链接base,net,proto,timer,logger
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(ipcclient PUBLIC ${BASE_LIBS})
		target_compile_definitions(ipcclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcclient PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(ipcclient PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(ipcclient PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(ipcclient PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(ipcclient PUBLIC pthread)
	endif()
endif()

# Hold regressions cover generated JSON decoding and its error guarantees.
target_link_libraries(ipcclient PUBLIC jsoncpp_static)

if(SINGLE_THREAD)
    target_link_libraries(ipcclient PUBLIC http_static)
else()
    target_link_libraries(ipcclient PUBLIC http_m_static)
endif()
