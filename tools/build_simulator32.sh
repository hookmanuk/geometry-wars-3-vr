#!/bin/bash
# Builds a 32-bit copy of the OpenXR Simulator into deps/simulator32, for testing this 32-bit game.
# The simulator's own folder is not touched: its CMake file writes to its bin folder, so the
# sources are copied first.
set -e
SRC=/f/rpsc3/source/OpenXR-Simulator
DST=/h/gw3-vr/deps/simulator32
CM="/c/Program Files/Microsoft Visual Studio/18/Insiders/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
mkdir -p "$DST"
cp -r "$SRC/src" "$SRC/include" "$SRC/tests" "$SRC/CMakeLists.txt" "$DST/"
# stdcall decorates the 32-bit export; the OpenXR loader looks it up by its plain name.
cat >> "$DST/src/runtime.cpp" <<'EOF'

#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:xrNegotiateLoaderRuntimeInterface=_xrNegotiateLoaderRuntimeInterface@8")
#endif
EOF
VULKAN_SDK='C:\VulkanSDK\1.4.341.1' "$CM" -S "$DST" -B "$DST/build" -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF
"$CM" --build "$DST/build" --config Release
ls -la "$DST/bin"
