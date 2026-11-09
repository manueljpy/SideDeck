# SideDeck native engine for iOS (Android still builds via android/app/src/main/cpp).
cmake_minimum_required(VERSION 3.22.1)
project(dj_engine VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(SIDEDECK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ENGINE_DIR "${SIDEDECK_ROOT}/native/engine")
set(DR_LIBS "${SIDEDECK_ROOT}/third_party/dr_libs")
set(STRETCH_DIR "${SIDEDECK_ROOT}/third_party/signalsmith-stretch")
set(LINEAR_DIR "${SIDEDECK_ROOT}/third_party/signalsmith-linear")
set(QM_DSP_DIR "${SIDEDECK_ROOT}/third_party/qm-dsp")

add_subdirectory("${QM_DSP_DIR}" qm-dsp-build)
include("${SIDEDECK_ROOT}/third_party/opus_stack.cmake")

add_library(dj_engine STATIC
  "${ENGINE_DIR}/dj_engine.cpp"
  "${ENGINE_DIR}/last_error.cpp"
)
target_compile_definitions(dj_engine PRIVATE _USE_MATH_DEFINES)
# Hide internals; DJ_FFI_EXPORT marks the C API as default+used for Dart dlsym.
target_compile_options(dj_engine PRIVATE -fexceptions -fvisibility=hidden)
if(APPLE)
  target_compile_options(dj_engine PRIVATE -fvisibility-inlines-hidden)
endif()

if(APPLE)
  target_sources(dj_engine PRIVATE "${ENGINE_DIR}/ios_audio.mm")
  set_source_files_properties("${ENGINE_DIR}/ios_audio.mm" PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
endif()

target_include_directories(dj_engine PRIVATE
  "${ENGINE_DIR}"
  "${DR_LIBS}"
  "${STRETCH_DIR}"
  "${LINEAR_DIR}/include"
  "${QM_DSP_DIR}"
)

target_link_libraries(dj_engine PRIVATE qm_dsp opusfile)

if(APPLE)
  target_link_libraries(dj_engine PRIVATE
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework Accelerate"
  )
  # One archive so Xcode can -force_load the whole engine (FFI symbols).
  add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/libdj_engine_bundle.a"
    COMMAND /usr/bin/libtool -static -o "${CMAKE_BINARY_DIR}/libdj_engine_bundle.a"
      $<TARGET_FILE:dj_engine>
      $<TARGET_FILE:qm_dsp>
      $<TARGET_FILE:opusfile>
      $<TARGET_FILE:opus>
      $<TARGET_FILE:ogg>
    DEPENDS dj_engine qm_dsp opusfile opus ogg
    VERBATIM
  )
  add_custom_target(dj_engine_bundle ALL DEPENDS "${CMAKE_BINARY_DIR}/libdj_engine_bundle.a")
endif()
