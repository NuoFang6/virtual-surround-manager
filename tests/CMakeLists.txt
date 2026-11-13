add_executable(hrir_wav_test hrir_wav_test.cpp ../src/hrir_wav.cpp)
target_include_directories(hrir_wav_test PRIVATE ../src)
target_link_libraries(hrir_wav_test PRIVATE Qt6::Core)
add_test(NAME hrir_wav COMMAND hrir_wav_test)

add_executable(output_equalizer_test output_equalizer_test.cpp ../src/output_equalizer.cpp)
target_include_directories(output_equalizer_test PRIVATE ../src)
target_link_libraries(output_equalizer_test PRIVATE Qt6::Core)
add_test(NAME output_equalizer COMMAND output_equalizer_test)

add_executable(hrir_analysis_test hrir_analysis_test.cpp ../src/hrir_wav.cpp ../src/output_equalizer.cpp ../src/pipewire_manager.cpp ../src/pipewire_manager.h)
target_include_directories(hrir_analysis_test PRIVATE ../src ${PIPEWIRE_INCLUDE_DIRS})
target_compile_definitions(hrir_analysis_test PRIVATE VSM_LADSPA_PLUGIN_PATH="$<TARGET_FILE:freesurround_ladspa>")
target_link_libraries(hrir_analysis_test PRIVATE Qt6::Core ${PIPEWIRE_LIBRARIES} ${CMAKE_DL_LIBS})
add_test(NAME hrir_analysis COMMAND hrir_analysis_test ${CMAKE_SOURCE_DIR}/src/assets/hrir_wav)

add_executable(plugin_regression_test plugin_regression_test.cpp)
target_include_directories(plugin_regression_test PRIVATE ../third_party/freesurround_ladspa)
target_link_libraries(plugin_regression_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(plugin_regression_test freesurround_ladspa)
add_test(NAME plugin_regression COMMAND plugin_regression_test $<TARGET_FILE:freesurround_ladspa>)
if(TARGET freesurround_ladspa_test)
    add_test(NAME plugin_smoke COMMAND freesurround_ladspa_test $<TARGET_FILE:freesurround_ladspa>)
endif()

find_program(VSM_PIPEWIRE_EXECUTABLE pipewire)
find_program(VSM_WIREPLUMBER_EXECUTABLE wireplumber)
find_program(VSM_WPCTL_EXECUTABLE wpctl)
find_program(VSM_PW_CLI_EXECUTABLE pw-cli)
find_program(VSM_PW_DUMP_EXECUTABLE pw-dump)
find_program(VSM_PW_METADATA_EXECUTABLE pw-metadata)
find_program(VSM_JQ_EXECUTABLE jq)
if(VSM_PIPEWIRE_EXECUTABLE AND VSM_WIREPLUMBER_EXECUTABLE AND VSM_WPCTL_EXECUTABLE AND
   VSM_PW_CLI_EXECUTABLE AND VSM_PW_DUMP_EXECUTABLE AND VSM_PW_METADATA_EXECUTABLE AND VSM_JQ_EXECUTABLE)
    add_test(NAME virtual_sink_mute
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/virtual_sink_mute_test.sh
                $<TARGET_FILE:virtual-surround-manager>
                ${CMAKE_SOURCE_DIR}/src/assets/hrir_wav/27_95_142.wav)
    set_tests_properties(virtual_sink_mute PROPERTIES TIMEOUT 30)
endif()
