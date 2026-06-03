#pragma once
#include <model/Model.hpp>

namespace neomdl::model {
// Reads exactly the MDX channels used by LightPartTriMesh, including a color
// offset whose corresponding interleaved-array bitmap bit is disabled.
EnginePrelightSource decodeEnginePrelightSource(const Mesh&);
}
