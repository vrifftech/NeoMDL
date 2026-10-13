add_library(neomdl_model
    Model.cpp
    AabbBuild.cpp
    ModelInstance.cpp
    AsciiModel.cpp
    AsciiMesh.cpp
    AsciiControllers.cpp
    AsciiNodeFields.cpp
    AsciiTree.cpp
    AsciiSkin.cpp
    AsciiLightmap.cpp
    AsciiSkinSeams.cpp
    AsciiExport.cpp
    AsciiExportControllers.cpp
    AsciiExportFields.cpp
    AsciiExportMesh.cpp
    AsciiExportTopology.cpp
    AsciiExportSmoothing.cpp
    AsciiExportSkin.cpp
    PackedExport.cpp
    PackedExportAnimation.cpp
    PackedExportNodes.cpp
    PackedExportMesh.cpp
    PackedExportSpecialized.cpp
    AnimMesh.cpp
    UvAnimation.cpp
    Geometry.cpp
    EngineCpuSkin.cpp
    Shadow.cpp
    Lighting.cpp
    Emitter.cpp
    LightningControl.cpp
    EmitterOrdering.cpp
    Trigger.cpp
    Wind.cpp
    SceneOrdering.cpp
)
add_library(neomdl::model ALIAS neomdl_model)
target_include_directories(neomdl_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(neomdl_model PUBLIC neoshared::path neoshared::bwm)
target_compile_features(neomdl_model PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neomdl_model PRIVATE /W4 /permissive- /utf-8 /fp:strict)
else()
    # controller/particle operations retain their float32 rounding
    # boundaries; fused multiply-add and reassociation change those sequences.
    target_compile_options(neomdl_model PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()
neo_apply_minimal_release_to_directory_targets()
