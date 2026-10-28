cmake_minimum_required(VERSION 3.10)

# JsonCpp 1.9.6。协议 JSON 和 HTTP JSON 都用这一套。
add_library(jsoncpp_static STATIC
	src/lib_json/json_reader.cpp
	src/lib_json/json_value.cpp
	src/lib_json/json_writer.cpp
)

target_include_directories(jsoncpp_static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_json)
target_include_directories(jsoncpp_static SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
	target_compile_definitions(jsoncpp_static PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_options(jsoncpp_static PRIVATE /W0)
else()
	target_compile_options(jsoncpp_static PRIVATE -w -Wno-error)
endif()
