# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The program-file manifest (security brief 4c): the relative path and SHA-256 of every .exe and
# .dll this build ships whose bytes a release does not change -- the Qt runtime and plugins, the
# MSVC runtime, the vcpkg DLLs and the bundled tools. sak::program_integrity accepts a program
# file before an elevation when its path and hash are listed here, or when it is signed by the
# same signer as sak_utility.exe (the files a release signs). The list is compiled into the
# program, so a file planted beside it on a stick is not in it.
#
# sak_write_program_manifest(<output.inc> QT_BIN <dir> QT_PLUGINS <dir> VC_RUNTIME <dir>
#                            VCPKG_BIN <dir> TOOLS <dir>
#                            [DEBUG_VCPKG_BIN <dir>] [DEBUG_FILES <file>...])
# Every hashed file is a configure dependency: replacing one re-runs configure and the list.
#
# A Debug build (_DEBUG, the Debug CRT) compiles a list of its own: vcpkg's Debug DLLs
# (DEBUG_VCPKG_BIN: zd.dll, bz2d.dll, and a liblzma.dll of other bytes) in place of the Release
# ones, and DEBUG_FILES beside the program (the AddressSanitizer runtime when ENABLE_ASAN is on).
# Those are what a Debug build puts beside its programs; a Release build's list is unchanged.

function(_sak_manifest_add out_var relative file)
    file(SHA256 "${file}" _sak_hash)
    string(TOLOWER "${relative}" _sak_relative)
    string(REPLACE "/" "\\\\" _sak_relative "${_sak_relative}")
    set(${out_var} "${${out_var}}    {L\"${_sak_relative}\", \"${_sak_hash}\"},\n" PARENT_SCOPE)
endfunction()

function(sak_write_program_manifest output)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
                          "QT_BIN;QT_PLUGINS;VC_RUNTIME;VCPKG_BIN;TOOLS;DEBUG_VCPKG_BIN"
                          "DEBUG_FILES")
    foreach(_sak_required QT_BIN QT_PLUGINS VC_RUNTIME TOOLS)
        if(NOT ARG_${_sak_required} OR NOT IS_DIRECTORY "${ARG_${_sak_required}}")
            message(FATAL_ERROR
                "Program manifest: ${_sak_required} folder is missing ('${ARG_${_sak_required}}'). "
                "Without it the elevation check would refuse the files it holds.")
        endif()
    endforeach()

    set(_sak_entries "")
    set(_sak_depends "")
    # Qt's runtime DLLs and the DLLs windeployqt copies from Qt's bin (d3dcompiler, opengl32sw).
    file(GLOB _sak_files LIST_DIRECTORIES false "${ARG_QT_BIN}/*.dll")
    foreach(_sak_file IN LISTS _sak_files)
        get_filename_component(_sak_name "${_sak_file}" NAME)
        _sak_manifest_add(_sak_entries "${_sak_name}" "${_sak_file}")
        list(APPEND _sak_depends "${_sak_file}")
    endforeach()
    # Qt plugins land in <type>\<name>.dll beside the program.
    file(GLOB _sak_files LIST_DIRECTORIES false "${ARG_QT_PLUGINS}/*/*.dll")
    foreach(_sak_file IN LISTS _sak_files)
        file(RELATIVE_PATH _sak_relative "${ARG_QT_PLUGINS}" "${_sak_file}")
        _sak_manifest_add(_sak_entries "${_sak_relative}" "${_sak_file}")
        list(APPEND _sak_depends "${_sak_file}")
    endforeach()
    # The MSVC runtime the build copies beside the program.
    file(GLOB _sak_files LIST_DIRECTORIES false "${ARG_VC_RUNTIME}/*.dll")
    foreach(_sak_file IN LISTS _sak_files)
        get_filename_component(_sak_name "${_sak_file}" NAME)
        _sak_manifest_add(_sak_entries "${_sak_name}" "${_sak_file}")
        list(APPEND _sak_depends "${_sak_file}")
    endforeach()
    # zlib, bzip2 and liblzma from the build's own vcpkg tree, Release and Debug each their own.
    set(_sak_release_entries "")
    set(_sak_debug_entries "")
    foreach(_sak_kind release debug)
        if(_sak_kind STREQUAL "release")
            set(_sak_vcpkg_dir "${ARG_VCPKG_BIN}")
        else()
            set(_sak_vcpkg_dir "${ARG_DEBUG_VCPKG_BIN}")
        endif()
        if(_sak_vcpkg_dir AND IS_DIRECTORY "${_sak_vcpkg_dir}")
            file(GLOB _sak_files LIST_DIRECTORIES false "${_sak_vcpkg_dir}/*.dll")
            foreach(_sak_file IN LISTS _sak_files)
                get_filename_component(_sak_name "${_sak_file}" NAME)
                _sak_manifest_add(_sak_${_sak_kind}_entries "${_sak_name}" "${_sak_file}")
                list(APPEND _sak_depends "${_sak_file}")
            endforeach()
        endif()
    endforeach()
    # The bundled tools, at tools\<path> as the package carries them.
    set(_sak_tool_entries "")
    file(GLOB_RECURSE _sak_files LIST_DIRECTORIES false "${ARG_TOOLS}/*.exe" "${ARG_TOOLS}/*.dll")
    foreach(_sak_file IN LISTS _sak_files)
        file(RELATIVE_PATH _sak_relative "${ARG_TOOLS}" "${_sak_file}")
        _sak_manifest_add(_sak_tool_entries "tools/${_sak_relative}" "${_sak_file}")
        list(APPEND _sak_depends "${_sak_file}")
    endforeach()
    # What only a Debug build puts beside its programs, by name.
    foreach(_sak_file IN LISTS ARG_DEBUG_FILES)
        if(NOT EXISTS "${_sak_file}")
            message(FATAL_ERROR "Program manifest: '${_sak_file}' (DEBUG_FILES) does not exist.")
        endif()
        get_filename_component(_sak_name "${_sak_file}" NAME)
        _sak_manifest_add(_sak_debug_entries "${_sak_name}" "${_sak_file}")
        list(APPEND _sak_depends "${_sak_file}")
    endforeach()

    set(_sak_content "// Generated by cmake/program_manifest.cmake at configure time. Do not edit.\n")
    if(ARG_DEBUG_VCPKG_BIN OR ARG_DEBUG_FILES)
        string(APPEND _sak_content "#ifdef _DEBUG\n"
               "constexpr PinnedFile kPinnedFiles[] = {\n"
               "${_sak_entries}${_sak_debug_entries}${_sak_tool_entries}};\n"
               "#else\n"
               "constexpr PinnedFile kPinnedFiles[] = {\n"
               "${_sak_entries}${_sak_release_entries}${_sak_tool_entries}};\n"
               "#endif\n")
    else()
        string(APPEND _sak_content "constexpr PinnedFile kPinnedFiles[] = {\n"
               "${_sak_entries}${_sak_release_entries}${_sak_tool_entries}};\n")
    endif()
    # Rewrite only when it changed, so a configure does not rebuild the program for nothing.
    set(_sak_staging "${output}.new")
    file(WRITE "${_sak_staging}" "${_sak_content}")
    configure_file("${_sak_staging}" "${output}" COPYONLY)
    file(REMOVE "${_sak_staging}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sak_depends})
endfunction()
