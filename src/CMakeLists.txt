add_executable(virtual-surround-manager)

# Kirigami libraries, settings and links
ecm_add_qml_module(virtual-surround-manager
    URI de.berny23.virtual_surround_manager
)

target_sources(virtual-surround-manager
    PRIVATE
    frontend_manager.h
    frontend_manager.cpp
    main.cpp
    pipewire_manager.h
    pipewire_manager.cpp
    tray_icon.h
    tray_icon.cpp
)

target_link_libraries(virtual-surround-manager
    PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::Widgets
    Qt6::QuickControls2
    KF6::I18n
    KF6::I18nQml
    KF6::CoreAddons
    KF6::IconThemes
    KF6::ConfigCore
    ${CMAKE_DL_LIBS}
)

# PipeWire libraries
target_include_directories(virtual-surround-manager PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_compile_options(virtual-surround-manager PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
target_link_libraries(virtual-surround-manager PRIVATE ${PIPEWIRE_LIBRARIES})

# Expose the project version and the installation path of the bundled
# FreeSurround LADSPA plugin to the code (the plugin path is not stable in
# AppImages, which resolve it via $APPDIR at runtime instead)
target_compile_definitions(virtual-surround-manager PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
)
if(NOT IS_APPIMAGE)
    target_compile_definitions(virtual-surround-manager PRIVATE
        VSM_LADSPA_PLUGIN_PATH="${KDE_INSTALL_FULL_LIBDIR}/ladspa/freesurround_ladspa.so"
    )
endif()

# Flatpak stuff
if(IS_FLATPAK)
    message(STATUS "Flatpak build")
    target_compile_definitions(virtual-surround-manager PRIVATE IS_FLATPAK=1)
    target_link_libraries(virtual-surround-manager
        PRIVATE
        PkgConfig::LIBPORTAL
        PkgConfig::LIBPORTALQT
    )
endif(IS_FLATPAK)

# AppImage stuff
if(IS_APPIMAGE)
    message(STATUS "AppImage build")
    target_compile_definitions(virtual-surround-manager PRIVATE IS_APPIMAGE=1)
endif(IS_APPIMAGE)

# Install default wav files
install(
    DIRECTORY assets/hrir_wav/
    DESTINATION ${KDE_INSTALL_DATADIR}/virtual-surround-manager/hrir_wav
    FILES_MATCHING
    PATTERN "*.wav"
    PATTERN "*.txt" # The license text for HeSuVi
)

# Install icon
install(
    FILES assets/icons/${APPLICATION_ID}.svg
    DESTINATION ${KDE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

# QML sources are registered here and not from qml/ because
# qt6_target_qml_sources must run in the same directory scope as the target
# definition, otherwise configuring with the default Unix Makefiles generator
# fails (only Ninja tolerates the cross-scope call)
ecm_target_qml_sources(virtual-surround-manager
    SOURCES
    qml/Main.qml
    qml/Settings.qml
)

# Install binary to default system path
install(TARGETS virtual-surround-manager ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
