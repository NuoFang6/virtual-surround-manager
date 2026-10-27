#!/bin/sh

set -eu

ARCH=$(uname -m)
VERSION="${VERSION:-$(grep -m1 '^project(' CMakeLists.txt | sed -n 's/.*VERSION \([0-9.]*\).*/\1/p')}"
VERSION="${VERSION:-unknown}"
SHARUN="https://raw.githubusercontent.com/pkgforge-dev/Anylinux-AppImages/refs/heads/main/useful-tools/quick-sharun.sh"

BUILD_DIR=$(realpath -m "${BUILD_DIR:-build-appimage}")
INSTALL_DIR=$(realpath -m "${INSTALL_DIR:-build_appimage/install}")
APPDIR=$(realpath -m "${APPDIR:-AppDir-appimage}")
OUTPATH=$(realpath -m "${OUTPATH:-build_appimage}")

if [ "$ARCH" = "x86_64" ]; then
    ARCH_FLAGS="-march=x86-64-v3"
    ARTIFACT_ARCH="x86_64-v3"
else
    ARCH_FLAGS=""
    ARTIFACT_ARCH="$ARCH"
fi

command -v python3 >/dev/null || {
    echo "python3 is required for pruning the AppDir" >&2
    exit 1
}

export APPDIR
export ARCH
export VERSION
export ICON="$INSTALL_DIR"/share/icons/hicolor/scalable/apps/de.berny23.virtual_surround_manager.svg
export DESKTOP="$INSTALL_DIR"/share/applications/de.berny23.virtual_surround_manager.desktop
export OUTPATH
export MAIN_BIN=virtual-surround-manager
export OUTNAME=virtual-surround-manager-"$VERSION"-"$ARTIFACT_ARCH".AppImage
export DEBLOAT_LOCALE=1

echo "Building Application"
echo "---------------------------------------------------------------"

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DIS_APPIMAGE=1 \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_C_FLAGS="$ARCH_FLAGS" \
    -DCMAKE_CXX_FLAGS="$ARCH_FLAGS"
cmake --build "$BUILD_DIR" --config Release
cmake --install "$BUILD_DIR" --config Release

echo "Bundling AppImage..."
echo "---------------------------------------------------------------"

rm -rf "$APPDIR"
wget --retry-connrefused --tries=30 "$SHARUN" -O ./quick-sharun.sh
chmod +x ./quick-sharun.sh

# Run the dependency scan without a real PipeWire session: the app must not
# touch the user's live audio streams while quick-sharun traces its dlopens.
export QT_QPA_PLATFORM=offscreen
export PIPEWIRE_RUNTIME_DIR="$BUILD_DIR/no-pipewire-session"
mkdir -p "$PIPEWIRE_RUNTIME_DIR"
./quick-sharun.sh "$INSTALL_DIR/bin/virtual-surround-manager"

# Data that quick-sharun does not associate with the binary name
rm -rf "$APPDIR/share/virtual-surround-manager"
cp -r "$INSTALL_DIR/share/virtual-surround-manager" "$APPDIR/share/"
cp -r "$INSTALL_DIR/share/locale" "$APPDIR/share/"

# Bundle the FreeSurround LADSPA upmix plugin (dlopened at runtime, not auto-detected)
mkdir -p "$APPDIR/usr/lib/ladspa"
cp "$INSTALL_DIR/lib/ladspa/freesurround_ladspa.so" "$APPDIR/usr/lib/ladspa/"

# Bundle the PipeWire filter-chain module and its SPA filter-graph plugin (both
# dlopened by the bundled libpipewire at runtime, not auto-detected by ldd).
# Derive the directories from pkg-config: on Arch/CachyOS they are not next to
# libpipewire-0.3.so itself.
PW_MODULE_DIR=$(pkg-config --variable=moduledir libpipewire-0.3 2>/dev/null || true)
SPA_PLUGIN_DIR=$(pkg-config --variable=plugindir libspa-0.2 2>/dev/null || true)
[ -n "$PW_MODULE_DIR" ] || PW_MODULE_DIR=/usr/lib/pipewire-0.3
[ -n "$SPA_PLUGIN_DIR" ] || SPA_PLUGIN_DIR=/usr/lib/spa-0.2
mkdir -p "$APPDIR/lib/pipewire-0.3" "$APPDIR/lib/spa-0.2"
cp "$PW_MODULE_DIR/libpipewire-module-filter-chain.so" "$APPDIR/lib/pipewire-0.3/"
rm -rf "$APPDIR/lib/spa-0.2/filter-graph"
cp -r "$SPA_PLUGIN_DIR/filter-graph" "$APPDIR/lib/spa-0.2/"

# Libraries loaded by QML plugins or the SPA filter-graph plugin that
# quick-sharun's ldd/strace scan does not reach.
for libdir in /usr/lib /usr/lib64; do
    [ -d "$libdir" ] || continue
    for lib in "$libdir"/libKirigamiFormsPrivate*.so* "$libdir"/libfftw3f.so*; do
        [ -e "$lib" ] || continue
        cp -a "$lib" "$APPDIR/lib/"
    done
done

# quick-sharun pulls in unrelated host payload: VS Code is matched from a
# "code" string, and the full Qt QML tree brings WebEngine, Plasma and media
# libraries. Trim the QML tree to what this app actually imports.
rm -rf "$APPDIR/share/code"

QML_DIR="$APPDIR/lib/qt6/qml"
if [ -d "$QML_DIR" ]; then
    find "$QML_DIR" -mindepth 1 -maxdepth 1 -type d \
        ! -name QtQuick ! -name QtQml ! -name QtCore ! -name Qt ! -name QML ! -name org \
        -exec rm -rf {} +
    find "$QML_DIR/QtQuick" -mindepth 1 -maxdepth 1 -type d \
        ! -name Controls ! -name Templates ! -name Layouts ! -name Window \
        ! -name Dialogs ! -name Effects ! -name Shapes \
        -exec rm -rf {} +
    if [ -d "$QML_DIR/org" ]; then
        find "$QML_DIR/org" -mindepth 1 -maxdepth 1 -type d ! -name kde -exec rm -rf {} +
    fi
    if [ -d "$QML_DIR/org/kde" ]; then
        find "$QML_DIR/org/kde" -mindepth 1 -maxdepth 1 -type d \
            ! -name kirigami ! -name kirigamiaddons ! -name desktop \
            ! -name qqc2desktopstyle ! -name coreaddons ! -name config \
            ! -name iconthemes ! -name kitemodels ! -name sonnet ! -name union \
            -exec rm -rf {} +
        find "$QML_DIR/org/kde/kirigamiaddons" -mindepth 1 -maxdepth 1 -type d \
            ! -name components ! -name delegates ! -name dateandtime \
            ! -name formcard ! -name treeview \
            -exec rm -rf {} +
        find "$QML_DIR/org/kde/kirigamiaddons" -maxdepth 1 -type f -delete
    fi
fi

# Optional plugins that need libraries which are not part of this app's graph
rm -f "$APPDIR/lib/qt6/plugins/platformthemes/libqgtk3.so"
rm -f "$APPDIR/lib/qt6/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so"
rm -rf "$APPDIR/lib/qt6/plugins/wayland-graphics-integration-server"
rm -f "$APPDIR"/lib/qt6/plugins/imageformats/kimg_*
rm -f "$APPDIR/lib/qt6/plugins/imageformats/libqpdf.so"
rm -f "$APPDIR"/lib/spa-0.2/filter-graph/libspa-filter-graph-plugin-ebur128.so
rm -f "$APPDIR"/lib/spa-0.2/filter-graph/libspa-filter-graph-plugin-lv2.so
rm -f "$APPDIR"/lib/spa-0.2/filter-graph/libspa-filter-graph-plugin-sofa.so

# Remove every library that no remaining executable, Qt plugin or QML plugin
# references. This also drops the GPU drivers, LLVM, WebEngine and Plasma
# leftovers instead of shipping them (and avoids them shadowing host drivers).
python3 - "$APPDIR" <<'PY'
import os
import subprocess
import sys

root = os.path.abspath(sys.argv[1])


def is_elf(path):
    try:
        return open(path, "rb").read(4) == b"\x7fELF"
    except OSError:
        return False


byname = {}
for dirpath, _, filenames in os.walk(root):
    for name in filenames:
        path = os.path.join(dirpath, name)
        if not is_elf(path):
            continue
        byname.setdefault(name, []).append(path)
        real = os.path.realpath(path)
        if real != path:
            byname.setdefault(os.path.basename(real), []).append(real)

roots = []
for sub in ("bin", "lib/ladspa", "lib/pipewire-0.3", "lib/spa-0.2",
            "lib/qt6/plugins", "lib/qt6/qml", "lib/sharun-preload"):
    directory = os.path.join(root, sub)
    if not os.path.isdir(directory):
        continue
    for dirpath, _, filenames in os.walk(directory):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if is_elf(path):
                roots.append(path)
for name in ("AppRun", "sharun", "lib/ld-linux-x86-64.so.2", "lib/vulkan.so.1"):
    path = os.path.join(root, name)
    if os.path.exists(path) and is_elf(path):
        roots.append(path)

reachable = set()
pending = list(roots)
needed_cache = {}
while pending:
    path = os.path.realpath(pending.pop())
    if path in reachable:
        continue
    reachable.add(path)
    if not is_elf(path):
        continue
    if path not in needed_cache:
        try:
            needed_cache[path] = subprocess.run(
                ["patchelf", "--print-needed", path],
                capture_output=True, text=True, timeout=5,
            ).stdout.split()
        except Exception:
            needed_cache[path] = []
    for lib in needed_cache[path]:
        for candidate in byname.get(lib, []):
            candidate = os.path.realpath(candidate)
            if candidate not in reachable:
                pending.append(candidate)

removed = 0
for dirpath, _, filenames in os.walk(os.path.join(root, "lib")):
    for name in list(filenames):
        path = os.path.join(dirpath, name)
        real = os.path.realpath(path)
        if not is_elf(path) and not (os.path.islink(path) and is_elf(real)):
            continue
        if real not in reachable:
            try:
                os.unlink(path)
                removed += 1
            except OSError:
                pass

missing = {}
for path in reachable:
    for lib in needed_cache.get(path, []):
        if lib not in byname:
            missing.setdefault(lib, []).append(os.path.relpath(path, root))
if missing:
    for lib, users in sorted(missing.items()):
        sys.stderr.write("missing library %s required by %s\n" % (lib, ", ".join(users[:3])))
    sys.exit(1)

print("Pruned %d unreachable files" % removed)
PY

./quick-sharun.sh --make-appimage
