########################################################################
# Setup apps
########################################################################

# the engine: sources, players, decoders - shared by every user interface (sdl_output.cpp is
# appended to this list by the top-level CMakeLists.txt, unless SDL is disabled)
list(APPEND ediscope_sources
    dabplus_decoder.cpp
    ensemble_source.cpp
    ensemble_player.cpp
    edi_source.cpp
    edi_player.cpp
    edi_network_source.cpp
    edi_pft.cpp
    eti_player.cpp
    dab_decoder.cpp
    fic_decoder.cpp
    pcm_output.cpp
    tools.cpp
    version.cpp
    wav_output.cpp
    fft.cpp
    audio_spectrum_analyzer.cpp
    mot_manager.cpp
    pad_decoder.cpp
    msc_packet_demux.cpp
    mot_directory_decoder.cpp
    spi_decoder.cpp
    xml_tree.cpp
    radiodns.cpp
    ecc_countries.cpp
    fig_analyzer.cpp
    edi_to_eti.cpp
    mnsc_decoder.cpp
    )

set(ediscope_qt_sources
    ediscope_qt_main.cpp
    ediscope_qt.cpp
    ediscope_qt_theme.cpp
    ediscope_qt_services_model.cpp
    ediscope_qt_cu_bar.cpp
    ediscope_qt_dl_plus.cpp
    ediscope_qt_sls.cpp
    ediscope_qt_meters.cpp
    ediscope_qt_links.cpp
    ediscope_qt_rdns.cpp
    ediscope_qt_spi.cpp
    ediscope_qt_fig.cpp
    ediscope_qt_announcements.cpp
    ediscope_qt_mnsc.cpp
    )

# resolv: res_query()/ns_*() used by radiodns.cpp for raw CNAME/SRV DNS record lookups - on
# Windows, DnsQuery() (dnsapi) instead, and Winsock (ws2_32) for every socket
# openssl: TLS transport for radiodns.cpp's HTTPS fetches (see top-level CMakeLists.txt)
if(WIN32)
    set(platform_link_list ws2_32 dnsapi)
else()
    set(platform_link_list resolv)
endif()
set(common_link_list
    fec
    ${ATOMIC_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${MPG123_LIBRARIES} ${SDL2_LIBRARIES} ${AAC_LIB} ${ICONV_LIBRARIES}
    ${platform_link_list} ${OPENSSL_LIBRARIES}
    )

include_directories(../fec)


########################################################################
# Build the applications and add install rules.
########################################################################

find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)

# ediscope (Qt)
add_executable(ediscope ${ediscope_sources} ${ediscope_qt_sources})
set_target_properties(ediscope PROPERTIES AUTOMOC ON)
target_link_libraries(ediscope ${common_link_list} Qt6::Widgets)
if(WIN32)
    # a GUI application: no console window
    set_target_properties(ediscope PROPERTIES WIN32_EXECUTABLE ON)
endif()
install(TARGETS ediscope DESTINATION bin)

# Also register a desktop launcher + icon for the current user right after every build (not
# just on "make install"), pointing straight at this build's own output - so the taskbar/dock
# icon keeps working without needing sudo, and self-corrects if the checkout is ever moved or
# renamed (its paths are only ever baked in fresh, at build time, never hand-maintained).
if(UNIX AND NOT APPLE AND NOT CYGWIN)
    add_custom_command(TARGET ediscope POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.local/share/applications"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.local/share/icons/hicolor/256x256/apps"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/res/ediscope-icon.png" "$ENV{HOME}/.local/share/icons/hicolor/256x256/apps/ediscope.png"
        COMMAND bash -c "printf '[Desktop Entry]\\nType=Application\\nName=EDIScope\\nComment=DAB+/EDI diagnostic player\\nExec=%s\\nIcon=%s\\nTerminal=false\\nStartupWMClass=ediscope\\nCategories=AudioVideo;Player;\\n' '$<TARGET_FILE:ediscope>' '${CMAKE_SOURCE_DIR}/res/ediscope-icon.png' > '$ENV{HOME}/.local/share/applications/ediscope.desktop'"
        COMMAND bash -c "[ -x '${UPDATE_DESKTOP_DATABASE}' ] && '${UPDATE_DESKTOP_DATABASE}' '$ENV{HOME}/.local/share/applications' || true"
        COMMAND bash -c "[ -x '${GTK_UPDATE_ICON_CACHE}' ] && '${GTK_UPDATE_ICON_CACHE}' -f '$ENV{HOME}/.local/share/icons/hicolor' 2>/dev/null || true"
        COMMENT "Registering EDIScope desktop launcher and icon for the current user"
        VERBATIM
        )
endif()
