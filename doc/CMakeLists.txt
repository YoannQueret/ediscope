########################################################################
# Add docs install rules.
########################################################################

if(NOT MAN_INSTALL_DIR)
    set(MAN_INSTALL_DIR "share/man")
endif()


# ediscope
install(FILES ediscope.1 DESTINATION ${MAN_INSTALL_DIR}/man1/)
