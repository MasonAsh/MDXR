if(WIN32)
    find_path(WinPixEventRuntime_INCLUDE_DIR
        NAMES pix3.h
        PATHS
          "$ENV{WINPIXEVENTRUNTIME_SDK}/include/WinPixEventRuntime"
    )

    find_library(WinPixEventRuntime_LIBRARY
        NAMES WinPixEventRuntime
        PATHS
            "$ENV{WINPIXEVENTRUNTIME_SDK}/bin/x64"
            NO_SYSTEM_ENVIRONMENT_PATH
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WinPixEventRuntime
  DEFAULT_MSG
  WinPixEventRuntime_LIBRARY WinPixEventRuntime_INCLUDE_DIR)

if(WinPixEventRuntime_FOUND AND NOT TARGET WinPixEventRuntime::WinPixEventRuntime)
  add_library(WinPixEventRuntime::WinPixEventRuntime UNKNOWN IMPORTED)
  set_target_properties(WinPixEventRuntime::WinPixEventRuntime PROPERTIES
    IMPORTED_LOCATION "${WinPixEventRuntime_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WinPixEventRuntime_INCLUDE_DIRS}")
endif()