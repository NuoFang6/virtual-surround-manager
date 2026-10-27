#!/bin/sh

set -eu

ARCH=$(uname -m)
VERSION="${VERSION:-unknown}"
SHARUN="https://raw.githubusercontent.com/pkgforge-dev/Anylinux-AppImages/refs/heads/main/useful-tools/quick-sharun.sh"

export ARCH
export VERSION
export ICON=./build_install/share/icons/hicolor/scalable/apps/de.berny23.virtual_surround_manager.svg
export DESKTOP=./build_install/share/applications/de.berny23.virtual_surround_manager.desktop
export OUTPATH=./build_appimage
export MAIN_BIN=virtual-surround-manager
export OUTNAME=virtual-surround-manager-"$VERSION"-"$ARCH".AppImage
export DEBLOAT_LOCALE=1

echo "Building Application"
echo "---------------------------------------------------------------"

cmake -B build -G Ninja -DIS_APPIMAGE=1 --install-prefix "$(pwd)/build_install"
cmake --build build --config Release
cmake --install build --config Release

echo "Bundling AppImage..."
echo "---------------------------------------------------------------"
wget --retry-connrefused --tries=30 "$SHARUN" -O ./quick-sharun.sh
chmod +x ./quick-sharun.sh
./quick-sharun.sh ./build_install/bin/virtual-surround-manager

# Copy files that are not auto-detected
cp -r ./build_install/share/virtual-surround-manager ./AppDir/share/
cp -r ./build_install/share/locale                   ./AppDir/share/

# Bundle the FreeSurround LADSPA upmix plugin (dlopened at runtime, not auto-detected)
mkdir -p ./AppDir/usr/lib/ladspa
cp ./build_install/lib*/ladspa/freesurround_ladspa.so ./AppDir/usr/lib/ladspa/

# Bundle the PipeWire filter-chain module and its SPA filter-graph plugin (both
# dlopened by the bundled libpipewire at runtime, not auto-detected by ldd).
# The bundled libpipewire resolves these relative to its own location, so they
# must sit next to it inside the AppDir.
PW_LIB_DIR="$(dirname "$(ldd ./build_install/bin/virtual-surround-manager | awk '/libpipewire-0.3\.so/{print $3; exit}')")"
cp "$PW_LIB_DIR"/libpipewire-module-filter-chain.so ./AppDir/lib/pipewire-0.3/
cp -r "$PW_LIB_DIR"/../spa-0.2/filter-graph ./AppDir/lib/spa-0.2/

./quick-sharun.sh --make-appimage
