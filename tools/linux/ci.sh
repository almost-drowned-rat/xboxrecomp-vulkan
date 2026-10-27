#!/usr/bin/env bash
# ci.sh - headless Linux build and tests of the runtime (A10: Linux from now on,
# not after a port). Used by .github/workflows/linux.yml and by hand (WSL):
#
#   SDL3_DIR=<prefix with lib/cmake/SDL3> bash tools/linux/ci.sh [build dir]
#
# Builds the runtime libraries Vulkan-only (XBOXRECOMP_D3D11=OFF), then each
# portable native test under tests/ and runs it, then the Python tool tests.
# No window, no GPU: tests that need one are not in the list.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build-linux}"
JOBS="${JOBS:-2}"
PREFIX="${SDL3_DIR:-}"
CMAKE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="${CC:-clang}"
            -DCMAKE_CXX_COMPILER="${CXX:-clang++}")
[ -n "$PREFIX" ] && CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$PREFIX")

echo "== runtime libraries"
cmake -S "$ROOT" -B "$OUT/runtime" "${CMAKE_ARGS[@]}" \
      -DXBOXRECOMP_D3D11=OFF -DXBOXRECOMP_VULKAN="${VULKAN:-ON}"
cmake --build "$OUT/runtime" -j "$JOBS"

# Native tests that run without a window or GPU. Each is its own CMake
# project that pulls the runtime in with add_subdirectory(../..). The other
# tests under tests/ still use Win32 file APIs directly (GetTempPathA,
# CreateDirectoryA, _wfopen ...) and are Windows-only until ported.
TESTS="${TESTS:-kernel_bridge kernel_memory_posix memory_layout_posix memory_regressions nv2a_combiner nv2a_vsh adpcm_decode fp_precision mmx_integer mmx_rounding fist}"
# Known Linux failures, excluded by name so a new one shows. None at the moment
# (2026-10-15: the memory_regressions ext-vma hang and the memory_layout_posix
# crash were fixed -- reserved 4 GB guest window, VirtualQuery sizes, joined
# NV2A threads).
EXCLUDE="${EXCLUDE:-^$}"
fail=0
for t in $TESTS; do
    echo "== test $t"
    if cmake -S "$ROOT/tests/$t" -B "$OUT/tests/$t" "${CMAKE_ARGS[@]}" -DXBOXRECOMP_D3D11=OFF >"$OUT/$t.cfg.log" 2>&1 \
       && cmake --build "$OUT/tests/$t" -j "$JOBS" >"$OUT/$t.build.log" 2>&1; then
        (cd "$OUT/tests/$t" && ctest --output-on-failure --timeout 120 -E "$EXCLUDE") || fail=1
    else
        echo "   build failed:"; grep -hE "error|Error" "$OUT/$t.cfg.log" "$OUT/$t.build.log" 2>/dev/null | head -20 || true; fail=1
    fi
done

echo "== python tool tests"
if [ "${PYTEST:-1}" = 1 ]; then (cd "$ROOT" && python3 -m pytest -q tools/recomp) || fail=1; fi

exit $fail
