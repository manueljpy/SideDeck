# SideDeck Ogg stack: libogg + libopus + opusfile + libvorbis/vorbisfile
# (decode only, no HTTP / no vorbisenc).
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

# --- libvorbis + vorbisfile (decode only; no vorbisenc / books / modes) ---
set(VORBIS_ROOT "${OPUS_ROOT}/vorbis")
set(VORBIS_LIB "${VORBIS_ROOT}/lib")
add_library(vorbis STATIC
  "${VORBIS_LIB}/mdct.c"
  "${VORBIS_LIB}/smallft.c"
  "${VORBIS_LIB}/block.c"
  "${VORBIS_LIB}/envelope.c"
  "${VORBIS_LIB}/window.c"
  "${VORBIS_LIB}/lsp.c"
  "${VORBIS_LIB}/lpc.c"
  "${VORBIS_LIB}/analysis.c"
  "${VORBIS_LIB}/synthesis.c"
  "${VORBIS_LIB}/psy.c"
  "${VORBIS_LIB}/info.c"
  "${VORBIS_LIB}/floor1.c"
  "${VORBIS_LIB}/floor0.c"
  "${VORBIS_LIB}/res0.c"
  "${VORBIS_LIB}/mapping0.c"
  "${VORBIS_LIB}/registry.c"
  "${VORBIS_LIB}/codebook.c"
  "${VORBIS_LIB}/sharedbook.c"
  "${VORBIS_LIB}/lookup.c"
  "${VORBIS_LIB}/bitrate.c"
)
target_include_directories(vorbis
  PUBLIC "${VORBIS_ROOT}/include"
  PRIVATE "${VORBIS_LIB}"
)
target_link_libraries(vorbis PUBLIC ogg)
# libvorbis uses math; Android/NDK and some hosts need an explicit -lm.
find_library(SIDEDECK_LIBM m)
if(SIDEDECK_LIBM)
  target_link_libraries(vorbis PRIVATE ${SIDEDECK_LIBM})
endif()
target_compile_options(vorbis PRIVATE -Wno-shadow -Wno-sign-compare -Wno-unused-variable)

add_library(vorbisfile STATIC "${VORBIS_LIB}/vorbisfile.c")
target_include_directories(vorbisfile
  PUBLIC "${VORBIS_ROOT}/include"
  PRIVATE "${VORBIS_LIB}"
)
target_link_libraries(vorbisfile PUBLIC vorbis)
target_compile_options(vorbisfile PRIVATE -Wno-shadow -Wno-sign-compare)
