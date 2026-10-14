#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_BIN="${CMAKE:-cmake}"
BUILD_DIR="$ROOT_DIR/build"
BUILD_TYPE="Release"
BUILD_WX="ON"
REQUIRE_WX="OFF"
BUILD_CLI="ON"
MINIMAL_RELEASE="ON"
JOBS="${JOBS:-}"
GENERATOR=""
TARGET=""
NEOSHARED_ROOT_VALUE=""
INSTALL_PREFIX=""
CLEAN=0
EXTRA=()

usage() {
  cat <<'USAGE'
usage: ./scripts/build.sh [options] [-- extra-cmake-args...]

Options:
  --build-dir DIR          Build directory [default: ./build]
  --build-type TYPE        Debug, Release, RelWithDebInfo, or MinSizeRel
  --wx ON|OFF              Build the wxWidgets renderer [default: ON]
  --require-wx ON|OFF      Fail if wxWidgets/OpenGL are unavailable [default: OFF]
  --cli ON|OFF             Build neomdl-cli [default: ON]
  --minimal-release ON|OFF Enable minimal release flags [default: ON]
  --jobs N                 Parallel build jobs
  --target NAME            Build only a named target
  --generator NAME         CMake generator
  --neoshared-root DIR     Path to the separate neoshared repository
  --install-prefix DIR     Configure and install to this prefix
  --clean                  Remove the repository-local build directory first
  -h, --help               Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --wx) BUILD_WX="$2"; shift 2;;
    --require-wx) REQUIRE_WX="$2"; shift 2;;
    --cli) BUILD_CLI="$2"; shift 2;;
    --minimal-release) MINIMAL_RELEASE="$2"; shift 2;;
    --jobs|--parallel) JOBS="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --generator) GENERATOR="$2"; shift 2;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --install-prefix) INSTALL_PREFIX="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; EXTRA+=("$@"); break;;
    *) EXTRA+=("$1"); shift;;
  esac
done

case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR";; esac
if [[ "$CLEAN" == 1 && -e "$BUILD_DIR" ]]; then
  case "$(cd "$(dirname "$BUILD_DIR")" && pwd)/$(basename "$BUILD_DIR")" in
    "$ROOT_DIR"/*) rm -rf "$BUILD_DIR";;
    *) echo "Refusing to clean a build directory outside the repository: $BUILD_DIR" >&2; exit 2;;
  esac
fi

if [[ -z "$NEOSHARED_ROOT_VALUE" ]]; then
  NEOSHARED_ROOT_VALUE="$($CMAKE_BIN -P "$ROOT_DIR/cmake/NeoSharedSource.cmake" | tail -n 1)"
fi
case "$NEOSHARED_ROOT_VALUE" in /*|[A-Za-z]:/*) ;; *) NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE";; esac
[[ -f "$NEOSHARED_ROOT_VALUE/CMakeLists.txt" ]] || {
  echo "neoshared CMakeLists.txt not found under: $NEOSHARED_ROOT_VALUE" >&2
  exit 2
}

cmake_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
  "-DNEOSHARED_ROOT=$NEOSHARED_ROOT_VALUE"
  "-DNEOMDL_BUILD_WX_GUI=$BUILD_WX"
  "-DNEOMDL_REQUIRE_WX_GUI=$REQUIRE_WX"
  "-DNEOMDL_BUILD_CLI=$BUILD_CLI"
  "-DNEO_MINIMAL_RELEASE=$MINIMAL_RELEASE"
)
if [[ -n "$GENERATOR" ]]; then
  cmake_args+=(-G "$GENERATOR")
elif command -v ninja >/dev/null 2>&1; then
  cmake_args+=(-G Ninja)
fi
if [[ -n "$INSTALL_PREFIX" ]]; then
  cmake_args+=("-DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX")
fi
cmake_args+=("${EXTRA[@]}")

"$CMAKE_BIN" "${cmake_args[@]}"
build_args=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
[[ -z "$JOBS" ]] || build_args+=(--parallel "$JOBS")
[[ -z "$TARGET" ]] || build_args+=(--target "$TARGET")
"$CMAKE_BIN" "${build_args[@]}"

if [[ -n "$INSTALL_PREFIX" && -z "$TARGET" ]]; then
  "$CMAKE_BIN" --install "$BUILD_DIR" --config "$BUILD_TYPE"
fi
