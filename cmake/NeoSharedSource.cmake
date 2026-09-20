# Resolve only explicit or sibling shared sources. Never fetch mutable main during configure.
get_filename_component(_neo_tool_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
function(_neo_shared_source output tool_source)
    # Respect an explicit CMake/build-script override. This is important for CI,
    # packaged workspaces, and offline builds; silently replacing it with an
    # automatic checkout makes --neoshared-root ineffective.
    if(DEFINED NEOSHARED_ROOT AND NOT "${NEOSHARED_ROOT}" STREQUAL "")
        get_filename_component(_explicit "${NEOSHARED_ROOT}" ABSOLUTE BASE_DIR "${tool_source}")
        if(NOT EXISTS "${_explicit}/CMakeLists.txt")
            message(FATAL_ERROR "NEOSHARED_ROOT does not contain CMakeLists.txt: ${_explicit}")
        endif()
        set(${output} "${_explicit}" PARENT_SCOPE)
        return()
    endif()
    if(TARGET neoshared::wx)
        message(FATAL_ERROR "A workspace supplied neoshared::wx without NEOSHARED_ROOT.")
    endif()
    foreach(_name IN ITEMS neoshared NeoShared)
        get_filename_component(_candidate "${tool_source}/../${_name}" ABSOLUTE)
        if(EXISTS "${_candidate}/CMakeLists.txt")
            set(${output} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "NeoMDL requires a matched neoshared checkout. Place it beside NeoMDL "
        "or set NEOSHARED_ROOT; configure does not download or update sources.")
endfunction()
_neo_shared_source(_neo_resolved_shared "${_neo_tool_source}")
# Publish the resolved source path to the cache for the rest of this configure.
set(NEOSHARED_ROOT "${_neo_resolved_shared}" CACHE PATH "Matched NeoShared sources" FORCE)
if(CMAKE_SCRIPT_MODE_FILE)
    # Shell/PowerShell build entry points use the same resolver before packaging.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${NEOSHARED_ROOT}")
endif()
