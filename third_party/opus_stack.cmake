# SideDeck Opus stack: libogg + libopus + opusfile (decode only, no HTTP).
cmake_minimum_required(VERSION 3.16)

set(OPUS_ROOT "${CMAKE_CURRENT_LIST_DIR}")

# Keep the codec build decode-only: no shared libs, tests, demos, or install.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# --- libogg ---
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPUS_ROOT}/ogg" ogg-build EXCLUDE_FROM_ALL)

# --- libopus ---
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPUS_ROOT}/opus" opus-build EXCLUDE_FROM_ALL)

# --- opusfile (local files only; skip http.c / OpenSSL) ---
add_library(opusfile STATIC
  "${OPUS_ROOT}/opusfile/src/info.c"
  "${OPUS_ROOT}/opusfile/src/internal.c"
  "${OPUS_ROOT}/opusfile/src/opusfile.c"
  "${OPUS_ROOT}/opusfile/src/stream.c"
)
target_include_directories(opusfile PUBLIC
  "${OPUS_ROOT}/opusfile/include"
)
target_include_directories(opusfile PRIVATE
  "${OPUS_ROOT}/opusfile/src"
)
target_link_libraries(opusfile PUBLIC ogg opus)
target_compile_options(opusfile PRIVATE -Wno-shadow -Wno-sign-compare)
