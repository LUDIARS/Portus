cmake_minimum_required(VERSION 3.24)
project(portus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PORTUS_BUILD_TESTS "Build the test runner" ON)
option(PORTUS_BUILD_EXAMPLES "Build the example server" ON)

# ビルド時には取得せず、セットアップ済みのパッケージを使う。
find_package(Lorica 0.1 CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_library(portus STATIC
	src/paging/db.cpp
	src/paging/keyset.cpp
	src/core/socket.cpp
	src/core/event_loop.cpp
	src/core/thread_pool.cpp
	src/core/tcp_conn.cpp
	src/core/util.cpp
	src/core/url.cpp
	src/core/json.cpp
	src/core/encoding.cpp
	src/core/crypto.cpp
	src/core/id.cpp
	src/core/env.cpp
	src/core/file_follow.cpp
	src/core/follow_file.cpp
	src/log/record.cpp
	src/log/format.cpp
	src/log/sink.cpp
	src/log/async_sink.cpp
	src/log/logger.cpp
	src/http/message.cpp
	src/http/reply.cpp
	src/http/body.cpp
	src/http/router.cpp
	src/http/server.cpp
	src/http/websocket_upgrade.cpp
	src/ws/frame.cpp
	src/ws/handshake.cpp
	src/ws/socket.cpp
	src/ws/server.cpp
	src/ws/client.cpp
	src/redis/resp.cpp
	src/redis/client.cpp
	src/mqtt/packet.cpp
	src/mqtt/client.cpp
	src/net/channel.cpp
	src/net/fetch.cpp
	src/push/encrypt.cpp
	src/push/vapid.cpp
	src/push/endpoint.cpp
	src/push/send.cpp
	src/aws/sigv4.cpp
	src/aws/credentials.cpp
	src/aws/s3.cpp
	src/ai/openai.cpp
	src/ai/anthropic.cpp
	src/ai/gemini.cpp
	src/ai/notion.cpp
)
add_library(portus::portus ALIAS portus)
target_include_directories(portus PUBLIC include PRIVATE src/paging)
target_compile_features(portus PRIVATE cxx_std_23)
target_link_libraries(portus PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads PRIVATE Lorica::boundary)
lorica_harden_target(portus)
if(WIN32)
	target_link_libraries(portus PUBLIC ws2_32)
endif()

if(MSVC)
	# Public headers use UTF-8 and are parsed by the consuming target.
	target_compile_options(portus PRIVATE /W4 PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
else()
	target_compile_options(portus PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PORTUS_BUILD_EXAMPLES)
	add_executable(portus_hello examples/hello_server.cpp)
	target_link_libraries(portus_hello PRIVATE portus)
	lorica_harden_target(portus_hello)
endif()

if(PORTUS_BUILD_TESTS)
	enable_testing()
	add_executable(portus_tests
		tests/main.cpp
		tests/keyset_test.cpp
		tests/db_test.cpp
		tests/core_test.cpp
		tests/file_follow_test.cpp
		tests/log_test.cpp
		tests/http_test.cpp
		tests/server_test.cpp
		tests/ws_test.cpp
		tests/http_websocket_test.cpp
		tests/redis_test.cpp
		tests/mqtt_test.cpp
		tests/net_test.cpp
		tests/push_test.cpp
		tests/aws_test.cpp
	)
	target_link_libraries(portus_tests PRIVATE portus)
	lorica_harden_target(portus_tests)
	add_test(NAME portus_tests COMMAND portus_tests)
endif()
