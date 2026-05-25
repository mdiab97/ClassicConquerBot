add_library(ccbot_dll SHARED
  src/dllmain.cpp
  src/hwid_spoof.cpp
)

target_link_libraries(ccbot_dll PRIVATE ccbot_common minhook nlohmann_json::nlohmann_json)
target_link_libraries(ccbot_dll PRIVATE ws2_32 iphlpapi dbghelp)
target_compile_definitions(ccbot_dll PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(ccbot_dll PRIVATE
  /NODEFAULTLIB:libboost_thread-vc143-mt-gd-x64-1_79
  /NODEFAULTLIB:libboost_thread-vc143-mt-x64-1_79
  /NODEFAULTLIB:libboost_chrono-vc143-mt-gd-x64-1_79
  /NODEFAULTLIB:libboost_date_time-vc143-mt-gd-x64-1_79
)

set_target_properties(ccbot_dll PROPERTIES
  OUTPUT_NAME "d3d11_config"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
