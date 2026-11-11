#!/bin/sh
# Builds Atelier (AU, VST3, Standalone), runs the offline checks, and installs the
# plugins into ~/Library/Audio/Plug-Ins. Needs cmake + ninja (pip3 install --user cmake ninja).
#
# The build directory lives outside ~/Documents on purpose: a cloud-synced Documents
# folder makes the thousands of small build files slow and occasionally time out.
set -e
cd "$(dirname "$0")"
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
BUILD="$HOME/Library/Caches/Atelier-build"

# JUCE is not stored in this repository: fetch the version the plugin is built against.
if [ ! -f JUCE/CMakeLists.txt ]; then
    git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git JUCE
fi

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DATELIER_INSTALL_PLUGINS=ON
cmake --build "$BUILD" --target AtelierTests
"$BUILD/AtelierTests_artefacts/Release/AtelierTests" "$BUILD/shots"
cmake --build "$BUILD"
echo "Standalone app: $BUILD/Atelier_artefacts/Release/Standalone/Atelier.app"
