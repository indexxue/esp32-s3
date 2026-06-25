# Resolve PROJECT_VER from git semver tags when not passed via -DPROJECT_VER.
#
# Build: exact tag on HEAD, else nearest ancestor tag (git describe --tags --abbrev=0).
# Release (release_ext.py): TY_RELEASE_PROJECT_VER env var wins over git tag.

function(_git_parse_semver_tag raw out_var)
    string(STRIP "${raw}" tag)
    string(REGEX REPLACE "^[Vv]" "" tag "${tag}")
    if(tag MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(${out_var} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(git_project_version_from_git repo_root out_var)
    set(${out_var} "" PARENT_SCOPE)
    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
        return()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${repo_root}" describe --exact-match --tags HEAD
        RESULT_VARIABLE _exact_rc
        OUTPUT_VARIABLE _exact_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_exact_rc EQUAL 0)
        _git_parse_semver_tag("${_exact_out}" _ver)
        if(_ver)
            set(${out_var} "${_ver}" PARENT_SCOPE)
            return()
        endif()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${repo_root}" describe --tags --abbrev=0
        RESULT_VARIABLE _near_rc
        OUTPUT_VARIABLE _near_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_near_rc EQUAL 0)
        _git_parse_semver_tag("${_near_out}" _ver)
        if(_ver)
            set(${out_var} "${_ver}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Call before project() when PROJECT_VER is not set on the command line.
macro(apply_git_project_version repo_root)
    if(DEFINED ENV{TY_RELEASE_PROJECT_VER} AND NOT "$ENV{TY_RELEASE_PROJECT_VER}" STREQUAL "")
        set(PROJECT_VER "$ENV{TY_RELEASE_PROJECT_VER}" CACHE STRING "Project version" FORCE)
        message(STATUS "PROJECT_VER from release: ${PROJECT_VER}")
    else()
        git_project_version_from_git("${repo_root}" _git_project_ver)
        if(_git_project_ver)
            set(PROJECT_VER "${_git_project_ver}" CACHE STRING "Project version" FORCE)
            message(STATUS "PROJECT_VER from git tag: ${PROJECT_VER}")
        endif()
    endif()
endmacro()
