add_library(ccbot_common STATIC
  src/tcp_server.cpp
  src/tcp_client.cpp
)

target_include_directories(ccbot_common PUBLIC include)
target_link_libraries(ccbot_common PUBLIC asio nlohmann_json::nlohmann_json)
target_link_libraries(ccbot_common PUBLIC ws2_32)
