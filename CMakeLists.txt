cmake_minimum_required(VERSION 3.24)
project(ClassicConquerBot VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ── CPM package manager ──────────────────────────────────────────────
include(cmake/CPM.cmake)

# ── Dependencies ─────────────────────────────────────────────────────
CPMAddPackage(
  NAME asio
  VERSION 1.30.2
  GITHUB_REPOSITORY chriskohlhoff/asio
  GIT_TAG asio-1-30-2
  DOWNLOAD_ONLY YES
)
if(asio_ADDED)
  add_library(asio INTERFACE)
  target_include_directories(asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
  target_compile_definitions(asio INTERFACE
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
    BOOST_ALL_NO_LIB
    _WIN32_WINNT=0x0A00
  )
endif()

CPMAddPackage(
  NAME imgui
  VERSION 1.91.6
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.91.6
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME minhook
  GITHUB_REPOSITORY TsudaKageyu/minhook
  GIT_TAG v1.3.3
  DOWNLOAD_ONLY YES
)
if(minhook_ADDED)
  add_library(minhook STATIC
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde32.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
  )
  target_include_directories(minhook PUBLIC "${minhook_SOURCE_DIR}/include")
endif()

CPMAddPackage("gh:nlohmann/json@3.11.3")

# ── Subprojects ──────────────────────────────────────────────────────
add_subdirectory(common)
add_subdirectory(gui)
add_subdirectory(dll)
