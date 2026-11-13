########################################################################
# Setup apps
########################################################################

list(APPEND ediscope_sources
    dabplus_decoder.cpp
    ensemble_source.cpp
    ensemble_player.cpp
    edi_source.cpp
    edi_player.cpp
    eti_player.cpp
    dab_decoder.cpp
    fic_decoder.cpp
    pcm_output.cpp
    tools.cpp
    version.cpp
    wav_output.cpp
    fft.cpp
    audio_spectrum_analyzer.cpp
    )

set(ediscope_cli_sources
    ediscope.cpp
    )

set(ediscope_gtk_sources
    mot_manager.cpp
    pad_decoder.cpp
    ediscope_gtk.cpp
    ediscope_gtk_dl_plus.cpp
    ediscope_gtk_sls.cpp
    ediscope_gtk_cu_bar.cpp
    ediscope_gtk_logs.cpp
    ediscope_gtk_level_meter.cpp
    ediscope_gtk_spectrum.cpp
    ediscope_gtk_stereometer.cpp
    ediscope_gtk_correlation.cpp
    ecc_countries.cpp
    edi_network_source.cpp
    edi_pft.cpp
    )

set(common_link_list
    fec
    ${ATOMIC_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${MPG123_LIBRARIES} ${SDL2_LIBRARIES} ${AAC_LIB} ${ICONV_LIBRARIES}
    )

include_directories(../fec)


########################################################################
# Build the applications and add install rules.
########################################################################

# ediscope
add_executable(ediscope ${ediscope_sources} ${ediscope_cli_sources})
target_link_libraries(ediscope ${common_link_list})
install(TARGETS ediscope DESTINATION bin)

# ediscope_gtk
if(GTKMM_FOUND)
    add_executable(ediscope_gtk ${ediscope_sources} ${ediscope_gtk_sources})
    target_link_libraries(ediscope_gtk ${common_link_list} ${GTKMM_LIBRARIES})
    install(TARGETS ediscope_gtk DESTINATION bin)
endif()
