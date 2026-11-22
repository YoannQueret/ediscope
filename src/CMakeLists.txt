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
    msc_packet_demux.cpp
    mot_directory_decoder.cpp
    spi_decoder.cpp
    xml_tree.cpp
    radiodns.cpp
    ediscope_gtk.cpp
    ediscope_gtk_dl_plus.cpp
    ediscope_gtk_sls.cpp
    ediscope_gtk_cu_bar.cpp
    ediscope_gtk_logs.cpp
    ediscope_gtk_spi.cpp
    ediscope_gtk_rdns.cpp
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
    # resolv: res_query()/ns_*() used by radiodns.cpp for raw CNAME/SRV DNS record lookups
    target_link_libraries(ediscope_gtk ${common_link_list} ${GTKMM_LIBRARIES} resolv)
    install(TARGETS ediscope_gtk DESTINATION bin)

    # Also register a desktop launcher + icon for the current user right after every build (not
    # just on "make install"), pointing straight at this build's own output - so the taskbar/dock
    # icon keeps working without needing sudo, and self-corrects if the checkout is ever moved or
    # renamed (its paths are only ever baked in fresh, at build time, never hand-maintained).
    if(UNIX AND NOT APPLE AND NOT CYGWIN)
        find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
        find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)

        add_custom_command(TARGET ediscope_gtk POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.local/share/applications"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.local/share/icons/hicolor/256x256/apps"
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/res/ediscope-icon.png" "$ENV{HOME}/.local/share/icons/hicolor/256x256/apps/ediscope.png"
            COMMAND bash -c "printf '[Desktop Entry]\\nType=Application\\nName=EDIScope\\nComment=DAB+/EDI diagnostic player\\nExec=%s\\nIcon=%s\\nTerminal=false\\nStartupWMClass=Ediscope_gtk\\nCategories=AudioVideo;Player;\\n' '$<TARGET_FILE:ediscope_gtk>' '${CMAKE_SOURCE_DIR}/res/ediscope-icon.png' > '$ENV{HOME}/.local/share/applications/ediscope-gtk.desktop'"
            COMMAND bash -c "[ -x '${UPDATE_DESKTOP_DATABASE}' ] && '${UPDATE_DESKTOP_DATABASE}' '$ENV{HOME}/.local/share/applications' || true"
            COMMAND bash -c "[ -x '${GTK_UPDATE_ICON_CACHE}' ] && '${GTK_UPDATE_ICON_CACHE}' -f '$ENV{HOME}/.local/share/icons/hicolor' 2>/dev/null || true"
            COMMENT "Registering EDIScope desktop launcher and icon for the current user"
            VERBATIM
            )
    endif()
endif()
