if(NOT DEFINED SAK_TOOLS_SOURCE)
    message(FATAL_ERROR "SAK_TOOLS_SOURCE is required")
endif()

if(NOT DEFINED SAK_TOOLS_DESTINATION)
    message(FATAL_ERROR "SAK_TOOLS_DESTINATION is required")
endif()

if(NOT EXISTS "${SAK_TOOLS_SOURCE}")
    message(FATAL_ERROR "Bundled tools source does not exist: ${SAK_TOOLS_SOURCE}")
endif()

# The destination is about to be deleted RECURSIVELY, so prove it is a contained directory
# path BEFORE touching it. An empty or relative value resolves against whatever directory the
# build happens to run in, a filesystem root would erase unrelated data, and a value equal to
# (or overlapping) the source would erase the very tools this script then copies -- leaving a
# successful-looking empty bundle. Refuse all of them instead.
if(SAK_TOOLS_DESTINATION STREQUAL "")
    message(FATAL_ERROR "SAK_TOOLS_DESTINATION is empty")
endif()

if(NOT IS_ABSOLUTE "${SAK_TOOLS_DESTINATION}")
    message(FATAL_ERROR "SAK_TOOLS_DESTINATION must be an absolute path: ${SAK_TOOLS_DESTINATION}")
endif()

get_filename_component(_sak_tools_source_abs "${SAK_TOOLS_SOURCE}" ABSOLUTE)
get_filename_component(_sak_tools_dest_abs "${SAK_TOOLS_DESTINATION}" ABSOLUTE)
get_filename_component(_sak_tools_dest_parent "${_sak_tools_dest_abs}" DIRECTORY)

if(_sak_tools_dest_parent STREQUAL "" OR _sak_tools_dest_parent STREQUAL _sak_tools_dest_abs)
    message(FATAL_ERROR
        "SAK_TOOLS_DESTINATION must not be a filesystem root: ${_sak_tools_dest_abs}")
endif()

if(_sak_tools_dest_abs STREQUAL _sak_tools_source_abs)
    message(FATAL_ERROR
        "SAK_TOOLS_DESTINATION must not be the tools source: ${_sak_tools_dest_abs}")
endif()

string(FIND "${_sak_tools_dest_abs}/" "${_sak_tools_source_abs}/" _sak_dest_inside_source)
string(FIND "${_sak_tools_source_abs}/" "${_sak_tools_dest_abs}/" _sak_source_inside_dest)
if(_sak_dest_inside_source EQUAL 0 OR _sak_source_inside_dest EQUAL 0)
    message(FATAL_ERROR
        "SAK_TOOLS_DESTINATION overlaps the tools source; refusing to delete it: "
        "${_sak_tools_dest_abs}")
endif()

# Existence is not enough: an empty or wrong source directory would wipe the destination and
# ship an app whose bundled tools are all missing.
file(GLOB _sak_tools_source_entries "${_sak_tools_source_abs}/*")
if(NOT _sak_tools_source_entries)
    message(FATAL_ERROR "Bundled tools source is empty: ${_sak_tools_source_abs}")
endif()

file(REMOVE_RECURSE "${SAK_TOOLS_DESTINATION}")
file(MAKE_DIRECTORY "${SAK_TOOLS_DESTINATION}")

file(COPY "${SAK_TOOLS_SOURCE}/"
    DESTINATION "${SAK_TOOLS_DESTINATION}"
    PATTERN "_build" EXCLUDE
)

# Prove the copy actually produced a bundle rather than reporting success over an empty tree.
file(GLOB _sak_tools_dest_entries "${_sak_tools_dest_abs}/*")
if(NOT _sak_tools_dest_entries)
    message(FATAL_ERROR
        "Bundled tools copy produced an empty destination: ${_sak_tools_dest_abs}")
endif()
