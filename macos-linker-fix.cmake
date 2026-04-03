# macOS Linker Fix for Xenia
# macOS needs particular code to make sure we can properly link for our environment

# macOS Frameworks (for discord-rpc and Foundation APIs)
set(MACOS_FRAMEWORKS
    "-framework Foundation"
    "-framework AppKit" 
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Cocoa"
)

# Find SDL2
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
    
    if(NOT SDL2_FOUND)
        find_path(SDL2_INCLUDE_DIR SDL.h
            PATHS
            /usr/local/x86brew/include/SDL2
            /usr/local/include/SDL2
            /opt/homebrew/include/SDL2
        )
        find_library(SDL2_LIBRARY SDL2
            PATHS
            /usr/local/x86brew/lib
            /usr/local/lib
            /opt/homebrew/lib
        )
        if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
            set(SDL2_FOUND TRUE)
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
            set(SDL2_LIBRARIES ${SDL2_LIBRARY})
        endif()
    endif()
endif()

# Apply fixes to all targets
macro(apply_macos_linker_fix target)
    target_link_libraries(${target} PRIVATE ${MACOS_FRAMEWORKS})
    
    if(SDL2_FOUND)
        target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})
        message(STATUS "Linked SDL2 to ${target}")
    else()
        message(WARNING "SDL2 not found for ${target}")
    endif()
    
    target_link_libraries(${target} PRIVATE
        -ldl
        -lpthread
    )
    
    # Set macOS deployment target
    set_target_properties(${target} PROPERTIES
        OSX_DEPLOYMENT_TARGET "11.0"
    )
endmacro()
