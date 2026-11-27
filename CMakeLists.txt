########################################################################
# Project setup
########################################################################

cmake_minimum_required(VERSION 3.10)
project(ediscope C CXX)

# Select the release build type by default to get optimization flags
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release")
   message(STATUS "Build type not specified: defaulting to release.")
endif(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} CACHE STRING "")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

include(CheckCXXSourceCompiles)

enable_testing()


########################################################################
# Build the FEC library before setting up the EDIScope application builds.
########################################################################

add_subdirectory(fec)


########################################################################
# Version information
########################################################################

# version derived from git (if possible)
execute_process(
	COMMAND git describe --dirty
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE VERSION_FROM_GIT
	ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(VERSION_FROM_GIT)
	add_definitions(-DEDISCOPE_VERSION="${VERSION_FROM_GIT}")
	set(VERSION_OUTPUT ${VERSION_FROM_GIT})
else()
	set(VERSION_OUTPUT "[see src/version.h]")
endif()

########################################################################
# Compiler specific setup
########################################################################

# C++17 (required by Qt 6) - set through CMAKE_CXX_STANDARD, so it only applies to the C++ sources
# and doesn't leak onto the bundled fec library's plain C sources
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_COMPILER_IS_CLANGXX 1)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANGXX)
    # warnings
    add_compile_options(-Wall)
    add_compile_options(-Wextra)
elseif(MSVC)
    # TODO
endif()
if(CMAKE_COMPILER_IS_GNUCXX)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        # strip symbols (TODO: add also for clang)
        SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -s")
    endif()
    
    # hardening (TODO: add also for clang/Cygwin) - ELF only, not for a Windows (MinGW) build
    if(NOT WIN32)
        add_compile_options(-fstack-protector-strong)
        add_compile_options(-fPIE)
        SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -pie")
        if(NOT CYGWIN)
            SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -z now")
        endif()
    endif()
endif()

# support 2GB+ files on 32bit systems
add_definitions(-D_FILE_OFFSET_BITS=64)

if(WIN32)
    # Windows (MinGW-w64, e.g. MSYS2): Windows 10 API (inet_pton, ...), no min/max macros from
    # windows.h (they break std::min/std::max), and EDIScope's own main() rather than SDL's
    add_definitions(-D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DSDL_MAIN_HANDLED)
else()
    # enable newer POSIX API
    add_definitions(-D_POSIX_C_SOURCE=200809L)
endif()


########################################################################
# Find build dependencies
########################################################################

find_package(PkgConfig)
find_package(Threads REQUIRED)

if(WIN32)
    # Windows: one self-contained ediscope.exe - every library is linked statically. MSYS2 provides
    # them as .a, and Qt as its qt6-static package, installed in <MSYS2 prefix>/qt6-static
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    get_filename_component(MINGW_PREFIX "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(MINGW_PREFIX "${MINGW_PREFIX}" DIRECTORY)
    list(PREPEND CMAKE_PREFIX_PATH "${MINGW_PREFIX}/qt6-static")
endif()

# a pkg-config package's libraries, as listed for static linking (Libs.private too) on Windows
macro(use_static_pkg prefix)
    if(WIN32)
        set(${prefix}_LIBRARIES ${${prefix}_STATIC_LIBRARIES})
        set(${prefix}_LIBRARY_DIRS ${${prefix}_STATIC_LIBRARY_DIRS})
    endif()
endmacro()

# SDL2
if(NOT DISABLE_SDL)
    pkg_check_modules(SDL2 sdl2 REQUIRED)
    use_static_pkg(SDL2)
	include_directories(${SDL2_INCLUDE_DIRS})
	link_directories(${SDL2_LIBRARY_DIRS})
	if(WIN32)
		# SDL2main brings its own WinMain() - EDIScope has its own main() (SDL_MAIN_HANDLED), and Qt its WinMain()
		list(REMOVE_ITEM SDL2_LIBRARIES SDL2main)
	endif()
	list(APPEND ediscope_sources sdl_output.cpp)
else()
    add_definitions(-DEDISCOPE_DISABLE_SDL)
endif()

# libmpg123
pkg_check_modules(MPG123 libmpg123 REQUIRED)
use_static_pkg(MPG123)
include_directories(${MPG123_INCLUDE_DIRS})
link_directories(${MPG123_LIBRARY_DIRS})

# Qt 6 Widgets - the ediscope application itself
find_package(Qt6 REQUIRED COMPONENTS Widgets)
if(WIN32)
    get_target_property(QT_CORE_TYPE Qt6::Core TYPE)
    if(NOT QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Windows: a static Qt 6 is required (MSYS2 package qt6-static), found ${Qt6_DIR}")
    endif()

    # static Qt links freetype as a plain library: add what freetype itself needs (brotli, bzip2,
    # ...), as listed by pkg-config for static linking
    pkg_check_modules(FREETYPE_STATIC freetype2)
    foreach(freetype_target WrapFreetype::WrapFreetype WrapSystemFreetype::WrapSystemFreetype)
        if(TARGET ${freetype_target} AND FREETYPE_STATIC_FOUND)
            set_property(TARGET ${freetype_target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${FREETYPE_STATIC_STATIC_LIBRARIES})
            message(STATUS "Static freetype: ${freetype_target} also links ${FREETYPE_STATIC_STATIC_LIBRARIES}")
        endif()
    endforeach()
endif()

# openssl - TLS transport only (for radiodns.cpp's HTTPS logo/SI.xml fetches); all actual
# DAB/EDI/RadioDNS protocol parsing stays hand-rolled
pkg_check_modules(OPENSSL openssl REQUIRED)
use_static_pkg(OPENSSL)
include_directories(${OPENSSL_INCLUDE_DIRS})
link_directories(${OPENSSL_LIBRARY_DIRS})

if(USE_FDK-AAC)
    # fdk-aac / fdk-aac-dabplus
    pkg_check_modules(FDKAAC fdk-aac)
    pkg_check_modules(FDKAAC-DABPLUS fdk-aac-dabplus)
    use_static_pkg(FDKAAC)
    use_static_pkg(FDKAAC-DABPLUS)
    if(NOT(FDKAAC_FOUND OR FDKAAC-DABPLUS_FOUND))
        message(FATAL_ERROR "fdk-aac or fdk-aac-dabplus required to compile EDIScope with USE_FDK-AAC\n")
    endif()
    if(FDKAAC-DABPLUS_FOUND)
        include_directories(${FDKAAC-DABPLUS_INCLUDE_DIRS})
        link_directories(${FDKAAC-DABPLUS_LIBRARY_DIRS})
        list(APPEND AAC_LIB ${FDKAAC-DABPLUS_LIBRARIES})
    else()
        include_directories(${FDKAAC_INCLUDE_DIRS})
        link_directories(${FDKAAC_LIBRARY_DIRS})
        list(APPEND AAC_LIB ${FDKAAC_LIBRARIES})
    endif()
    add_definitions(-DEDISCOPE_AAC_FDKAAC)
else()
    # libfaad2
    find_package(FAAD REQUIRED)
    include_directories(${FAAD_INCLUDE_DIRS})
    link_directories(${FAAD_LIBRARY_DIRS})
    list(APPEND AAC_LIB ${FAAD_LIBRARIES})
    add_definitions(-DEDISCOPE_AAC_FAAD2)
endif()

# iconv
find_package(ICONV REQUIRED)
include_directories(${ICONV_INCLUDE_DIRS})
link_directories(${ICONV_LIBRARY_DIRS})

# atomic
list(APPEND CMAKE_REQUIRED_FLAGS -std=c++0x)
check_cxx_source_compiles("
    #include <atomic>
    std::atomic<double> x(1.0);
    int main() {return x;}"
    NATIVE_ATOMICS_SUPPORT
    )
if(NOT NATIVE_ATOMICS_SUPPORT)
    find_package(ATOMIC REQUIRED)
    link_directories(${ATOMIC_LIBRARY_DIRS})
endif()

########################################################################
# Setup EDIScope application build
########################################################################

add_subdirectory(src)
add_subdirectory(doc)

if(NOT WIN32)
install(FILES res/ediscope-icon.png DESTINATION share/icons/hicolor/256x256/apps RENAME ediscope.png)
install(FILES res/ediscope.desktop DESTINATION share/applications)
endif()

# refresh the icon/desktop caches as part of "make install" itself, so a newly installed or
# changed icon shows up right away, without a separate manual step after sudo make install
find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)	# the desktop's hicolor icon cache tool, despite its name
if(UPDATE_DESKTOP_DATABASE)
    install(CODE "execute_process(COMMAND \"${UPDATE_DESKTOP_DATABASE}\" \"${CMAKE_INSTALL_PREFIX}/share/applications\")")
endif()
if(GTK_UPDATE_ICON_CACHE)
    install(CODE "execute_process(COMMAND \"${GTK_UPDATE_ICON_CACHE}\" -f \"${CMAKE_INSTALL_PREFIX}/share/icons/hicolor\")")
endif()

########################################################################
# Print Summary
########################################################################
message(STATUS "")
message(STATUS "##########################################################")
message(STATUS "## Building version: ${VERSION_OUTPUT}")
message(STATUS "## Using install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "##########################################################")
message(STATUS "")


########################################################################
# Create uninstall target
########################################################################

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

