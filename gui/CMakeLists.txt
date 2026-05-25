add_executable(ccbot_gui WIN32
  src/main.cpp
  src/app.cpp

  # ImGui sources
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(ccbot_gui PRIVATE
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends"
  src
)

target_link_libraries(ccbot_gui PRIVATE ccbot_common)
target_link_libraries(ccbot_gui PRIVATE d3d11 d3dcompiler dxgi)
target_compile_definitions(ccbot_gui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(ccbot_gui PRIVATE "/MANIFESTUAC:level='requireAdministrator'" "/MAP" "/DEBUG")

set_target_properties(ccbot_gui PROPERTIES
  OUTPUT_NAME "CQGraphicsConfig"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
