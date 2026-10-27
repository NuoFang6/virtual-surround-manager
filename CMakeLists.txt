cmake_minimum_required(VERSION 3.28)

# Hard fork of virtual-surround-manager (upstream: Berny23), versioning starts fresh
project("Virtual Surround Manager" VERSION 0.0.1 LANGUAGES C CXX)

set(APPLICATION_ID "de.berny23.virtual_surround_manager")
set(APPLICATION_NAME "Virtual Surround")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IS_FLATPAK "Some things need to be handled differently in C++ if running in a Flatpak" OFF)

# Default to an optimized Release build so a plain `cmake -B build && cmake --build build`
# never produces an unoptimized binary
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optional: optimize for the build machine's CPU. Great for a local install,
# but the resulting binaries must NOT be redistributed (they can crash on other CPUs).
option(VSM_NATIVE_ARCH "Optimize for the build machine's CPU (-march=native); binaries are not redistributable" OFF)
if(VSM_NATIVE_ARCH)
    add_compile_options(-march=native)
endif()

# Link-time optimization for optimized builds (the LADSPA plugin enables its own LTO)
include(CheckIPOSupported)
check_ipo_supported(RESULT VSM_IPO_SUPPORTED)
if(VSM_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

# Kirigami dependencies
find_package(ECM 6.0.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

set(CMAKE_AUTOMOC ON)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMFindQmlModule)
include(ECMQmlModule)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Quick
    Gui
    QuickControls2
    Widgets
)
find_package(KF6 REQUIRED COMPONENTS
    Kirigami
    I18n
    CoreAddons
    QQC2DesktopStyle
    IconThemes
    Config
)

ecm_find_qmlmodule(org.kde.kirigami REQUIRED)

find_package(PkgConfig REQUIRED)

# PipeWire dependencies
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

# Flatpak stuff
if(IS_FLATPAK)
    pkg_check_modules(LIBPORTAL IMPORTED_TARGET REQUIRED libportal)
    pkg_check_modules(LIBPORTALQT IMPORTED_TARGET REQUIRED libportal-qt6)
endif(IS_FLATPAK)

# Translations
ki18n_install(po)

# Bundled FreeSurround (Real3D) LADSPA upmix plugin, installed next to the app
add_subdirectory(third_party/freesurround_ladspa)

# Include CMakeLists.txt in src directory
add_subdirectory(src)

# Prepare and install shortcut to default system application path (e.g. /usr/share/applications/)
configure_file(dist/${APPLICATION_ID}.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_ID}.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_ID}.desktop
    DESTINATION ${KDE_INSTALL_APPDIR})

# Prepare and install MetaInfo file for app stores
configure_file(dist/${APPLICATION_ID}.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_ID}.metainfo.xml)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_ID}.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

# From kirigami docs
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
