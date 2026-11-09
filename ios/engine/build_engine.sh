#!/bin/sh
# Build libdj_engine.a for the current Xcode SDK / architecture.
# Xcode provides: PROJECT_DIR, SDKROOT, ARCHS, PLATFORM_NAME, CONFIGURATION
set -e

ENGINE_DIR="${PROJECT_DIR}/engine"
ROOT="${PROJECT_DIR}/.."
OUT="${ENGINE_DIR}/.built/${PLATFORM_NAME}"
BUILD="${ENGINE_DIR}/.built/cmake-${PLATFORM_NAME}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake not found. Install CMake (brew install cmake) to build SideDeck for iOS." >&2
  exit 1
fi

mkdir -p "${OUT}" "${BUILD}"

ARCH_LIST=$(echo "${ARCHS}" | tr ' ' ';')
BUILD_TYPE=Release
if [ "${CONFIGURATION}" = "Debug" ]; then
  BUILD_TYPE=RelWithDebInfo
fi

# Homebrew CMake 4+ rejects cmake_minimum_required < 3.5 in vendored deps
# (libogg). Policy floor keeps configure working if a dep is still old.
cmake -S "${ROOT}/native" -B "${BUILD}" \
  -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="${SDKROOT}" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCH_LIST}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

cmake --build "${BUILD}" --target dj_engine_bundle --parallel
cp -f "${BUILD}/libdj_engine_bundle.a" "${OUT}/libdj_engine.a"

# Fail the Xcode build early if FFI entry points never made it into the archive.
if ! nm "${OUT}/libdj_engine.a" 2>/dev/null | grep -E ' T _?dj_create$' >/dev/null; then
  echo "error: dj_create missing from ${OUT}/libdj_engine.a (FFI will fail at runtime)" >&2
  nm "${OUT}/libdj_engine.a" 2>/dev/null | grep dj_ | head -50 >&2 || true
  exit 1
fi

echo "SideDeck engine -> ${OUT}/libdj_engine.a (dj_create OK)"
