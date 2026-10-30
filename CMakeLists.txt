cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")
endif()

project(NeoMDL VERSION 0.1.11
    DESCRIPTION "C++17/wxWidgets Odyssey MDL/MDX model renderer"
    LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/NeoSharedSource.cmake")
include(GNUInstallDirs)
message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoVersion.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
set(NEOSHARED_ENABLE_ERF ON CACHE BOOL
    "Build the NeoShared ERF/MOD/RIM core required by installation resource lookup" FORCE)
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET neoshared::texture)
    message(FATAL_ERROR "NeoMDL requires the shared texture/TXI codec library.")
endif()
if(NOT TARGET neoshared::material)
    message(FATAL_ERROR "NeoMDL requires the shared material/TXI library.")
endif()

add_subdirectory(src/model)
add_subdirectory(src/material)

if(NOT TARGET neoshared::bif)
    message(FATAL_ERROR "NeoMDL requires the matched neoshared::bif reader library.")
endif()

add_library(neomdl_core
    src/core/AsciiMaterials.cpp
    src/core/ModelFilenames.cpp
    src/core/Conversion.cpp
    src/core/CollisionAssets.cpp
    src/core/ActivePauseClock.cpp
    src/core/Engine2DA.cpp
    src/core/CreatureAppearance.cpp
    src/core/CreatureLipSync.cpp
    src/core/DialogueAnimation.cpp
    src/core/DialogueObjectAnimation.cpp
    src/core/DialogueRuntime.cpp
    src/core/DialogueEffects.cpp
    src/core/DialogueSpeech.cpp
    src/core/DialogueParticipants.cpp
    src/core/LookAtAnimation.cpp
    src/core/WeatherScene.cpp
    src/core/SceneAudio.cpp
    src/core/SceneAudioVoices.cpp
    src/core/ResourceContext.cpp
    src/core/ModelDocument.cpp
    src/core/MeshReplacement.cpp
    src/core/MeshConversion.cpp
    src/core/MeshNormals.cpp
    src/core/BinaryEditing.cpp
    src/core/BinaryGameConversion.cpp
    src/core/BinaryAnalysis.cpp
    src/core/SceneEffects.cpp
    src/core/VideoEffects.cpp
    src/core/ObjectVisualState.cpp
    src/core/CameraScene.cpp
    src/core/ReferenceScene.cpp
    src/core/ParticleScene.cpp
    src/core/EmitterBucket.cpp
    src/core/LightsaberScene.cpp
    src/core/ShadowScene.cpp
    src/core/StaticPrelighting.cpp
    src/core/SceneOwners.cpp
    src/core/MotionBlurScene.cpp
    src/core/LightScene.cpp
    src/core/GrassScene.cpp
    src/core/GrassOrdering.cpp
    src/core/AreaLoad.cpp
    src/core/AreaEnvironment.cpp
    src/core/WorldAnimation.cpp
    src/core/TrackScene.cpp
    src/core/DirtyArea.cpp
    src/core/AreaObject.cpp
    src/core/AreaScene.cpp
    src/core/RoomConnections.cpp
    src/core/RoomEditing.cpp
    src/core/ModelTransform.cpp
    src/core/TriggerScene.cpp
    src/core/TriggerRuntime.cpp
    src/core/TrapObjectEvents.cpp
    src/core/SceneQueries.cpp
    src/core/RoomSurfaceMesh.cpp
    src/core/LensFlares.cpp)
add_library(neomdl::core ALIAS neomdl_core)
target_include_directories(neomdl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomdl_core PUBLIC neomdl::model neomdl::material neoshared::path neoshared::gff neoshared::twoda neoshared::lip neoshared::bwm neoshared::bif neoshared::dlg)
target_compile_features(neomdl_core PUBLIC cxx_std_17)
target_compile_definitions(neomdl_core PUBLIC NEOMDL_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(neomdl_core PRIVATE /W4 /permissive- /utf-8 /fp:strict)
else()
    # Engine room and triangle queries retain the scalar rounding.
    target_compile_options(neomdl_core PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()

add_library(neomdl_game_resources STATIC src/core/GameResourceSession.cpp)
add_library(neomdl::game_resources ALIAS neomdl_game_resources)
target_include_directories(neomdl_game_resources PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomdl_game_resources
    PUBLIC neomdl_core neoshared::game
    PRIVATE neoshared::bif)
target_compile_features(neomdl_game_resources PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neomdl_game_resources PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(neomdl_game_resources PRIVATE -Wall -Wextra -Wpedantic)
endif()

# SDK-independent OpenGL dispatch; the wx frontend supplies its context.
add_library(neomdl_render STATIC
    src/render/ShaderMeshRenderer.cpp
    src/render/GrassPass.cpp
    src/render/MaterialPass.cpp
    src/render/DirtyMaterialPass.cpp
    src/render/MotionBlurPass.cpp
    src/render/SceneStageState.cpp
    src/render/ScenePostprocess.cpp
    src/render/SkinMaterialPass.cpp
    src/render/DesktopMaterialPrograms.cpp
    src/render/K1MaterialPass.cpp
    src/render/SoftShadowPass.cpp
    src/render/TextureCache.cpp
    src/render/LightSubmission.cpp
    src/render/OwnerEffects.cpp)
add_library(neomdl::render ALIAS neomdl_render)
target_include_directories(neomdl_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomdl_render PUBLIC neomdl::core)
target_compile_features(neomdl_render PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neomdl_render PRIVATE /W4 /permissive- /utf-8 /fp:strict)
else()
    # Engine material and framebuffer helpers use separate scalar operations.
    target_compile_options(neomdl_render PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()

option(NEOMDL_BUILD_CLI "Build the NeoMDL command-line converter" ON)
if(NEOMDL_BUILD_CLI)
    add_executable(neomdl-cli src/cli/main.cpp)
    target_link_libraries(neomdl-cli PRIVATE neomdl_core neoshared::game neoshared::texture)
    target_compile_features(neomdl-cli PRIVATE cxx_std_17)
    neo_add_windows_version_resource(neomdl-cli
        VERSION "${PROJECT_VERSION}"
        PRODUCT_NAME "NeoMDL"
        FILE_DESCRIPTION "NeoMDL command-line model converter"
        INTERNAL_NAME "neomdl-cli"
        ORIGINAL_FILENAME "neomdl-cli.exe")
endif()

option(NEOMDL_BUILD_AREA_CLI "Build the developer area diagnostic utility" OFF)
if(NEOMDL_BUILD_AREA_CLI)
    add_executable(neomdl-area-info src/cli/area.cpp)
    target_link_libraries(neomdl-area-info PRIVATE neomdl::core neomdl::game_resources)
    target_compile_features(neomdl-area-info PRIVATE cxx_std_17)
    neo_add_windows_version_resource(neomdl-area-info
        VERSION "${PROJECT_VERSION}" PRODUCT_NAME "NeoMDL"
        FILE_DESCRIPTION "NeoMDL module-scoped area load utility"
        INTERNAL_NAME "neomdl-area-info" ORIGINAL_FILENAME "neomdl-area-info.exe")
    install(TARGETS neomdl-area-info RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(NEOMDL_BUILD_WX_GUI "Build the wxWidgets NeoMDL desktop renderer when wxWidgets is available" ON)
option(NEOMDL_BUILD_WX_MODULE "Build the reusable wxWidgets NeoMDL editor panel" OFF)
option(NEOMDL_REQUIRE_WX_GUI "Fail configure if wxWidgets/OpenGL are unavailable" OFF)

function(neomdl_link_wx target_name)
    target_compile_definitions(${target_name} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    set(_wx_targets)
    if(TARGET wx::core)
        foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wx::aui wx::gl wx::stc)
            if(TARGET ${_wx_target})
                list(APPEND _wx_targets ${_wx_target})
            endif()
        endforeach()
    elseif(TARGET wxWidgets::wxWidgets)
        list(APPEND _wx_targets wxWidgets::wxWidgets)
    endif()
    if(_wx_targets)
        target_link_libraries(${target_name} PUBLIC ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PUBLIC ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PUBLIC ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PUBLIC ${wxWidgets_LIBRARIES})
    endif()
endfunction()

if(NEOMDL_BUILD_WX_GUI OR NEOMDL_BUILD_WX_MODULE)
    if(UNIX AND NOT CMAKE_CROSSCOMPILING AND NOT wxWidgets_CONFIG_EXECUTABLE)
        find_program(NEOMDL_SYSTEM_WX_CONFIG NAMES wx-config NO_CMAKE_FIND_ROOT_PATH)
        if(NEOMDL_SYSTEM_WX_CONFIG)
            set(wxWidgets_CONFIG_EXECUTABLE "${NEOMDL_SYSTEM_WX_CONFIG}"
                CACHE FILEPATH "Path to wx-config" FORCE)
        endif()
    endif()
    set(NEOMDL_WX_COMPONENTS core base adv aui gl stc)
    if(WIN32)
        find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOMDL_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOMDL_WX_COMPONENTS})
        endif()
    else()
        find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOMDL_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOMDL_WX_COMPONENTS})
        endif()
    endif()
    find_package(OpenGL QUIET)
    set(NEOMDL_OPENGL_LINK)
    if(TARGET OpenGL::GL)
        set(NEOMDL_OPENGL_LINK OpenGL::GL)
    elseif(TARGET OpenGL::OpenGL)
        set(NEOMDL_OPENGL_LINK OpenGL::OpenGL)
    elseif(OPENGL_LIBRARIES)
        set(NEOMDL_OPENGL_LINK ${OPENGL_LIBRARIES})
    endif()
    set(NEOMDL_WX_READY OFF)
    if((wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets) AND
       OpenGL_FOUND AND NEOMDL_OPENGL_LINK)
        set(NEOMDL_WX_READY ON)
    endif()
    if(NEOMDL_WX_READY)
        include("${NEOSHARED_ROOT}/cmake/NeoAudio.cmake")
        add_library(neomdl_wx STATIC
            src/wx/AsciiEditor.cpp
            src/wx/AsciiProperties.cpp
            src/wx/AsciiDataProperties.cpp
            src/wx/CollisionEditor.cpp
            src/wx/MeshReplacementEditor.cpp
            src/wx/BatchConversion.cpp
            src/wx/BinaryTools.cpp
            src/wx/ModelCanvas.cpp
            src/wx/WalkmeshOverlay.cpp
            src/wx/SceneAudioPlayback.cpp
            src/wx/SceneAudioDevice.cpp
            src/wx/ModelEditorPanel.cpp)
        add_library(neomdl::wx ALIAS neomdl_wx)
        target_include_directories(neomdl_wx PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/wx)
        target_link_libraries(neomdl_wx PUBLIC
            neomdl_core neomdl_game_resources neomdl_render neoshared::wx neoshared::audio neoshared::json
            ${NEOMDL_OPENGL_LINK} ${CMAKE_DL_LIBS})
        neomdl_link_wx(neomdl_wx)
        target_compile_features(neomdl_wx PUBLIC cxx_std_17)
        if(MSVC)
            target_compile_options(neomdl_wx PRIVATE /W4 /permissive- /utf-8 /fp:strict)
        else()
            target_compile_options(neomdl_wx PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
        endif()

        if(NEOMDL_BUILD_WX_GUI)
            if(WIN32)
                add_executable(NeoMDL WIN32 src/wx/main.cpp)
            elseif(APPLE)
                add_executable(NeoMDL MACOSX_BUNDLE src/wx/main.cpp)
            else()
                add_executable(NeoMDL src/wx/main.cpp)
            endif()
            target_include_directories(NeoMDL PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wx)
            target_link_libraries(NeoMDL PRIVATE neomdl_wx)
            target_compile_features(NeoMDL PRIVATE cxx_std_17)
            neo_add_windows_version_resource(NeoMDL
                VERSION "${PROJECT_VERSION}"
                PRODUCT_NAME "NeoMDL"
                FILE_DESCRIPTION "Odyssey MDL/MDX model renderer"
                INTERNAL_NAME "NeoMDL"
                ORIGINAL_FILENAME "NeoMDL.exe")
            if(APPLE)
                neo_configure_macos_bundle(NeoMDL
                    NAME "NeoMDL"
                    IDENTIFIER "com.vrifftech.neomdl"
                    VERSION "${PROJECT_VERSION}")
            endif()
            install(TARGETS NeoMDL
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                BUNDLE DESTINATION .)
        endif()
    elseif(NEOMDL_REQUIRE_WX_GUI)
        message(FATAL_ERROR "NeoMDL wx targets require wxWidgets core/base/adv/aui/gl/stc and OpenGL.")
    else()
        message(STATUS "wxWidgets/OpenGL not found; NeoMDL GUI/module targets are omitted.")
    endif()
endif()

if(TARGET neomdl-cli)
    if(APPLE AND TARGET NeoMDL)
        install(TARGETS neomdl-cli RUNTIME DESTINATION "NeoMDL.app/Contents/MacOS")
    else()
        install(TARGETS neomdl-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

set(NEOMDL_DOC_DESTINATION "${CMAKE_INSTALL_DOCDIR}")
if(APPLE AND TARGET NeoMDL)
    set(NEOMDL_DOC_DESTINATION "NeoMDL.app/Contents/Resources/docs")
endif()
install(FILES "${NEOSHARED_ROOT}/licenses/miniaudio.txt"
    DESTINATION "${NEOMDL_DOC_DESTINATION}/licenses")

neo_apply_minimal_release_to_directory_targets()
