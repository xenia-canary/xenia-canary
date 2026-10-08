function(generate_version_h)
    message(STATUS "Generating version.h...")
    set(XE_BUILD_BRANCH "detached")
    set(XE_BUILD_COMMIT "unknown")
    set(XE_BUILD_COMMIT_SHORT "unknown")

    find_package(Git)
    if(${Git_FOUND})
        execute_process(
            COMMAND ${GIT_EXECUTABLE} symbolic-ref --short -q HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE XE_BUILD_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        if(NOT XE_BUILD_BRANCH)
            set(XE_BUILD_BRANCH "detached")
        endif()
        message(STATUS "  Build branch: ${XE_BUILD_BRANCH}")

        execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE XE_BUILD_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        message(STATUS "  Git hash: ${XE_BUILD_COMMIT}")

        execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE XE_BUILD_COMMIT_SHORT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        message(STATUS "  Git hash (short): ${XE_BUILD_COMMIT_SHORT}")
    else()
        message(WARNING "Failed to find git; cannot determine branch and commit!")
    endif()

    if($ENV{GITHUB_REF} MATCHES "refs\/pull\/.*")
        set(XE_BUILD_IS_PR TRUE)
        string(REGEX REPLACE "refs\/pull\/([0-9]+)\/.*" "\\1" XE_BUILD_PR_NUMBER $ENV{GITHUB_REF})
        message(STATUS "  PR number: ${XE_BUILD_PR_NUMBER}")
    endif()

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h @ONLY)
endfunction()
