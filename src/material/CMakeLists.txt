add_library(neomdl_material
    Material.cpp
    Runtime.cpp
    ProceduralTexture.cpp
    ProcedureStorage.cpp
    DirtyMaterial.cpp
    ForceSight.cpp
)
add_library(neomdl::material ALIAS neomdl_material)
target_include_directories(neomdl_material PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(neomdl_material PUBLIC neoshared::texture neoshared::glu)
target_compile_features(neomdl_material PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neomdl_material PRIVATE /W4 /permissive- /utf-8 /fp:strict)
else()
    target_compile_options(neomdl_material PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()
neo_apply_minimal_release_to_directory_targets()
