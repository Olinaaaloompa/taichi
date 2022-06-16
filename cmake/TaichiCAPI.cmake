cmake_minimum_required(VERSION 3.0)




set(TAICHI_C_API_NAME taichi_c_api)

file(GLOB_RECURSE C_API_SOURCE "c_api/src/*.cpp")
add_library(${TAICHI_C_API_NAME} SHARED ${C_API_SOURCE})
target_link_libraries(${TAICHI_C_API_NAME} PRIVATE taichi_isolated_core)
set_target_properties(${TAICHI_C_API_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")

target_include_directories(${TAICHI_C_API_NAME}
    PUBLIC
        # Used when building the library:
        $<BUILD_INTERFACE:${taichi_c_api_BINARY_DIR}/c_api/include>
        $<BUILD_INTERFACE:${taichi_c_api_SOURCE_DIR}/c_api/include>
        # Used when installing the library:
        $<INSTALL_INTERFACE:/c_api/include>
    PRIVATE
        # Used only when building the library:
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/c_api/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Vulkan-Headers/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/VulkanMemoryAllocator/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/SPIRV-Tools/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/volk
    )

# This helper provides us standard locations across Linux/Windows/MacOS
include(GNUInstallDirs)

install(TARGETS ${TAICHI_C_API_NAME} EXPORT ${TAICHI_C_API_NAME}Targets
    LIBRARY DESTINATION c_api/lib
    ARCHIVE DESTINATION c_api/lib
    RUNTIME DESTINATION c_api/bin
    INCLUDES DESTINATION c_api/include
    )

message( --------------------- ${CMAKE_INSTALL_LIBDIR})
# Install the export set, which contains the meta data of the target
install(EXPORT ${TAICHI_C_API_NAME}Targets
    FILE ${TAICHI_C_API_NAME}Targets.cmake
    NAMESPACE ${TAICHI_C_API_NAME}::
    DESTINATION c_api/${CMAKE_INSTALL_LIBDIR}/cmake/${TAICHI_C_API_NAME}
    )

include(CMakePackageConfigHelpers)

# Generate the config file
configure_package_config_file(
      "${PROJECT_SOURCE_DIR}/cmake/${TAICHI_C_API_NAME}Config.cmake.in"
      "${PROJECT_BINARY_DIR}/${TAICHI_C_API_NAME}Config.cmake"
    INSTALL_DESTINATION
       c_api/${CMAKE_INSTALL_LIBDIR}/cmake/${TAICHI_C_API_NAME}
    )

# Generate the config version file
set(${TAICHI_C_API_NAME}_VERSION "${TI_VERSION_MAJOR}.${TI_VERSION_MINOR}.${TI_VERSION_PATCH}")
write_basic_package_version_file(
    "${TAICHI_C_API_NAME}ConfigVersion.cmake"
    VERSION ${${TAICHI_C_API_NAME}_VERSION}
    COMPATIBILITY SameMajorVersion
    )

# Install the config files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${TAICHI_C_API_NAME}Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/${TAICHI_C_API_NAME}ConfigVersion.cmake"
    DESTINATION
    c_api/${CMAKE_INSTALL_LIBDIR}/cmake/${TAICHI_C_API_NAME}
    )

# Install public headers for this target
# TODO: Replace files here with public headers when ready.
install(DIRECTORY
      ${PROJECT_SOURCE_DIR}/c_api/include
    DESTINATION c_api
    FILES_MATCHING
    PATTERN *.h
    PATTERN *.hpp
    )
