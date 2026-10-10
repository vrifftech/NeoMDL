#include "model/Geometry.hpp"
#include <cmath>
#include <limits>

namespace neomdl::model {
namespace {
double dot(Vec3 a, Vec3 b) {
    return static_cast<double>(a.x)*b.x + static_cast<double>(a.y)*b.y + static_cast<double>(a.z)*b.z;
}
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double determinant(const Mat4& m) {
    const auto& a = m.values;
    return static_cast<double>(a[0])*(static_cast<double>(a[5])*a[10]-static_cast<double>(a[9])*a[6])
         - static_cast<double>(a[4])*(static_cast<double>(a[1])*a[10]-static_cast<double>(a[9])*a[2])
         + static_cast<double>(a[8])*(static_cast<double>(a[1])*a[6]-static_cast<double>(a[5])*a[2]);
}
}

RenderGeometry prepareRenderGeometry(const Mesh& mesh, const DeformedMesh* deformed, bool engineAnimMeshLocal) {
    if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max())
        throw ModelError("Too many vertices for a 32-bit indexed render buffer");
    const bool posed = deformed && deformed->valid;
    if (posed && (deformed->positions.size() != mesh.vertices.size() ||
                   deformed->normals.size() != mesh.vertices.size()))
        throw ModelError("Deformed vertex pools disagree with the mesh vertex count");
    if (mesh.animated && (!posed || !deformed->animmesh ||
        !deformed->animmesh->positionsDefined ||
        (!deformed->animmesh->texcoordsDefined && static_cast<std::int32_t>(mesh.mdxTexcoordOffsets[0])>=0) ||
        deformed->animmesh->localPositions.size()!=mesh.vertices.size() ||
        deformed->animmesh->texcoords.size()!=mesh.vertices.size()))
        throw ModelError("Engine animmesh draw requires its defined current position and UV pools");
    const bool localAnimMesh = mesh.animated && engineAnimMeshLocal;
    const bool preservedBasis = posed && deformed->preserveTangentBasis;
    if (preservedBasis && (deformed->tangents.size() != mesh.vertices.size() ||
        deformed->bitangents.size() != mesh.vertices.size() ||
        deformed->basisNormals.size() != mesh.vertices.size() ||
        deformed->exactTangentBasis.size() != mesh.vertices.size()))
        throw ModelError("Deformed tangent pools disagree with the mesh vertex count");
    RenderGeometry result;
    result.vertices.resize(mesh.vertices.size());
    for (std::size_t i=0; i<mesh.vertices.size(); ++i) {
        const auto& source = mesh.vertices[i];
        auto& v = result.vertices[i];
        v.position = localAnimMesh ? deformed->animmesh->localPositions[i] :
            posed ? deformed->positions[i] : source.position;
        v.normal = posed && !localAnimMesh ? deformedNormalAt(mesh,*deformed,i) : mesh.normalAt(i);
        v.rawNormal = localAnimMesh ? mesh.normalAt(i) : posed && mesh.skinned && deformed->rawSkinNormals.size()==mesh.vertices.size()
            ? deformed->rawSkinNormals[i] : v.normal;
        v.texcoord0 = mesh.animated ? deformed->animmesh->texcoords[i] : source.texcoord0;
        v.texcoord1 = source.texcoord1;
        for (std::size_t c=0; c<4; ++c) v.color[c] = static_cast<float>(source.color[c])/255.0f;
        if (preservedBasis && deformed->exactTangentBasis[i] != 0u) {
            v.tangent=deformed->tangents[i];
            v.bitangent=deformed->bitangents[i];
            v.basisNormal=deformed->basisNormals[i];
        } else if ((!posed || mesh.animated) && ((mesh.mdxBitmap & 0x80u) != 0u || source.tangentBasis.valid)) {
            v.tangent=source.tangentBasis.tangent;
            v.bitangent=source.tangentBasis.bitangent;
            v.basisNormal=source.tangentBasis.normal;
        } else {
            continue;
        }
        v.tangentHandedness=dot(cross(v.basisNormal,v.tangent),v.bitangent)<0.0 ? -1.0f : 1.0f;
        v.tangentBasisMode=1.0f;
        ++result.authoredTangents;
    }
    for (const auto index:mesh.indices)
        if (index>=mesh.vertices.size()) throw ModelError("Draw index exceeds its vertex pool");
    result.indices=mesh.indices;
    return result;
}

std::array<float,9> makeNormalMatrix(const Mat4& m) {
    const auto& a=m.values;
    const double d=determinant(m);
    const double x=a[0], y=a[4], z=a[8], p=a[1], q=a[5], r=a[9], s=a[2], t=a[6], u=a[10];
    const double values[9]={(q*u-r*t)/d,(z*t-y*u)/d,(y*r-z*q)/d,
                            (r*s-p*u)/d,(x*u-z*s)/d,(z*p-x*r)/d,
                            (p*t-q*s)/d,(y*s-x*t)/d,(x*q-y*p)/d};
    std::array<float,9> out;
    for (std::size_t i=0;i<out.size();++i)
        out[i]=static_cast<float>(values[i]);
    return out;
}
float transformHandedness(const Mat4& m) noexcept {
    const auto d=determinant(m); return std::isfinite(d) && d<0.0 ? -1.0f : 1.0f;
}
} // namespace neomdl::model
