# FreeSurround (Real3D) stereo-to-7.1 upmixer, packaged as a LADSPA plugin.
#
# This is vendored from the Real3D-VST repository (see README.md for provenance).
# The application's virtual surround filter graph loads this plugin by absolute
# path whenever stereo upmixing is enabled, so it is built and installed together
# with the application instead of relying on a system package.

add_library(freesurround_ladspa MODULE
    freesurround_ladspa.cpp
    freesurround_decoder.cpp
    channelmaps.cpp
    kissfft/kiss_fft.c
    kissfft/kiss_fftr.c
)

target_include_directories(freesurround_ladspa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/kissfft
)

# The decoder works on std::complex<double>, so kissfft must use the same scalar
# type (freesurround_decoder.cpp asserts this for its own translation unit).
target_compile_definitions(freesurround_ladspa PRIVATE kiss_fft_scalar=double)

# LADSPA hosts dlopen "<name>.so", so no "lib" prefix
set_target_properties(freesurround_ladspa PROPERTIES
    PREFIX ""
    C_STANDARD 99
)

# The KDE compiler settings default to hidden visibility, which would hide
# ladspa_descriptor() from dlsym; the plugin API must stay exported
set_target_properties(freesurround_ladspa PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# This is vendored third-party code - keep its warnings out of our build log
target_compile_options(freesurround_ladspa PRIVATE -w)

# The FFT math never touches errno, and dropping that assumption lets the
# compiler vectorize the inner loops more aggressively. Deliberately NO
# -ffast-math/-Ofast here: they change floating-point semantics (audio quality).
target_compile_options(freesurround_ladspa PRIVATE -fno-math-errno)

# Link-time optimization across the wrapper/decoder/kissfft boundary
include(CheckIPOSupported)
check_ipo_supported(RESULT freesurround_ipo_supported)
if(freesurround_ipo_supported)
    set_property(TARGET freesurround_ladspa PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Optional: optimize for the build machine. Great for a local install, but the
# resulting binary must NOT be redistributed (it can crash on other CPUs).
# Enable with: cmake -DVSM_PLUGIN_NATIVE_ARCH=ON ..
option(VSM_PLUGIN_NATIVE_ARCH "Build the LADSPA plugin with -march=native (local installs only, not redistributable)" OFF)
if(VSM_PLUGIN_NATIVE_ARCH)
    target_compile_options(freesurround_ladspa PRIVATE -march=native)
endif()

# Optional host-side smoke test that dlopens the plugin without PipeWire:
#   cmake -DBUILD_LADSPA_TEST_HOST=ON .. && cmake --build . --target freesurround_ladspa_test
if(BUILD_LADSPA_TEST_HOST)
    add_executable(freesurround_ladspa_test test_host.cpp)
    target_include_directories(freesurround_ladspa_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(freesurround_ladspa_test PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(freesurround_ladspa_test freesurround_ladspa)
endif()

# Install into the standard LADSPA directory next to the application, e.g.
# /usr/lib/ladspa, /usr/lib64/ladspa or /app/lib/ladspa (Flatpak)
install(TARGETS freesurround_ladspa DESTINATION ${KDE_INSTALL_LIBDIR}/ladspa)
