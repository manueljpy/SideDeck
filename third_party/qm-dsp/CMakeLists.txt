cmake_minimum_required(VERSION 3.5)
project(qm_dsp_beats LANGUAGES C CXX)

add_library(qm_dsp STATIC
  maths/MathUtilities.cpp
  base/Pitch.cpp
  dsp/onsets/DetectionFunction.cpp
  dsp/phasevocoder/PhaseVocoder.cpp
  dsp/transforms/FFT.cpp
  dsp/tempotracking/TempoTrackV2.cpp
  dsp/chromagram/ConstantQ.cpp
  dsp/chromagram/Chromagram.cpp
  dsp/rateconversion/Decimator.cpp
  dsp/keydetection/GetKeyMode.cpp
  ext/kissfft/kiss_fft.c
  ext/kissfft/tools/kiss_fftr.c
)

target_include_directories(qm_dsp PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/ext/kissfft"
  "${CMAKE_CURRENT_SOURCE_DIR}/ext/kissfft/tools"
)

target_compile_definitions(qm_dsp PRIVATE _USE_MATH_DEFINES)
target_compile_definitions(qm_dsp PUBLIC kiss_fft_scalar=double)
target_compile_options(qm_dsp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
set_target_properties(qm_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(qm_dsp PUBLIC cxx_std_17)
