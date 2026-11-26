cmake_minimum_required(VERSION 2.6)

###### ipcclient CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS
        src/main.cpp
        src/ipcclient.cpp
        src/test_db.cpp
        src/wake_bench.cpp
        src/test_board.cpp
        src/test_echo.cpp
        src/test_coro.cpp
        src/test_empty.cpp
        src/test_log.cpp
        src/test_redis.cpp
        src/test_hold.cpp
        src/test_timer.cpp
        src/test_nested_result.cpp
        src/test_task_start.cpp
        src/test_task_share.cpp
        src/test_blocking.cpp
        src/test_connect.cpp
        src/test_address.cpp
        src/test_isolation.cpp
        src/test_backpressure.cpp
        src/test_rpc_output.cpp
        src/test_rpc_await.cpp
        src/test_rpc_admission.cpp
        src/test_route.cpp
        src/test_registry.cpp
        src/test_rpc_service.cpp
        src/test_json.cpp
        src/test_binary.cpp
        src/test_encode.cpp
        src/test_http.cpp
        src/test_budget.cpp
        src/test_schedule.cpp
        src/test_rpc_pending.cpp
        src/test_udp.cpp
        src/test_multi.cpp
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
