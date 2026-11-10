# First-party product code is compiled ONCE into the static libraries declared in the root
# CMakeLists.txt, and the application, the helper executables and every test link those
# libraries instead of recompiling the same sources per target.
#
# A static library only keeps that promise if a consumer pulls in just the objects it references.
# AUTOMOC's default output defeats that: every header moc result of a target lands in ONE
# mocs_compilation translation unit, so a consumer that touches a single Q_OBJECT class pulls the
# meta-objects of every class in the library, and through their slots the implementation of all of
# them -- turning each test link into a link of the whole library, with every dependency it has.
#
# sak_add_product_library() therefore gives each moc'd header its own translation unit: a
# generated one-line source that includes the header's AUTOMOC output by name. AUTOMOC still runs
# moc exactly as it does for any target (same definitions, include paths and dependency
# tracking); only the place the output is compiled changes.
#
# Usage:
#   sak_add_product_library(<name> <root-relative sources and headers>...)
#
# Headers are given as include/<dir>/<name>.h. Only headers that contain a moc macro get a unit;
# a header that gains one later without a reconfigure still works (AUTOMOC falls back to
# mocs_compilation), it is just not split until the next configure.

function(sak_add_product_library name)
    set(moc_units)
    foreach(source IN LISTS ARGN)
        if(NOT source MATCHES "^include/(.+)/([A-Za-z0-9_]+)\\.h$")
            continue()
        endif()
        set(header_dir "${CMAKE_MATCH_1}")
        set(header_base "${CMAKE_MATCH_2}")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" moc_macros
            REGEX "^[ \t]*(Q_OBJECT|Q_GADGET|Q_NAMESPACE)([ \t]|$)")
        if(NOT moc_macros)
            continue()
        endif()
        set(unit "${CMAKE_CURRENT_BINARY_DIR}/sak_moc_units/${name}/${header_dir}/moc_unit_${header_base}.cpp")
        # AUTOMOC resolves "<dir>/moc_<base>.cpp" against the target's include directories, finds
        # include/<dir>/<base>.h, and writes the result to its own per-config include directory.
        file(CONFIGURE OUTPUT "${unit}"
            CONTENT "#include \"${header_dir}/moc_${header_base}.cpp\"\n" @ONLY)
        set_source_files_properties("${unit}" PROPERTIES SKIP_LINTING ON)
        list(APPEND moc_units "${unit}")
    endforeach()

    add_library(${name} STATIC ${ARGN} ${moc_units})
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    # The application's own compile definitions. Every library object used to be compiled as part
    # of sak_utility with exactly these.
    target_compile_definitions(${name} PRIVATE
        QT_NO_KEYWORDS
        QT_DEPRECATED_WARNINGS
        QT_DISABLE_DEPRECATED_BEFORE=0x060000
    )
    target_include_directories(${name} PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_BINARY_DIR}/include
    )
endfunction()
