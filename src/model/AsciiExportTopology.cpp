#include <model/AsciiExportTopology.hpp>
#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <tuple>
#include <map>
#include <iterator>
#include <numeric>
#include <utility>

namespace neomdl::model::ascii_export {
namespace {

bool same(Vec3 a,Vec3 b) {
    return a.x==b.x && a.y==b.y && a.z==b.z;
}
std::uint32_t bits(float value) {
    if(value==0.0f) return 0u;
    std::uint32_t result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}

class CornerGroups {
public:
    explicit CornerGroups(std::size_t size):parents_(size) {
        std::iota(parents_.begin(),parents_.end(),std::size_t{});
    }
    std::size_t find(std::size_t corner) {
        while(parents_[corner]!=corner) {
            parents_[corner]=parents_[parents_[corner]];
            corner=parents_[corner];
        }
        return corner;
    }
    void join(std::size_t a,std::size_t b) {
        a=find(a);b=find(b);
        parents_[std::max(a,b)]=std::min(a,b);
    }
private:
    std::vector<std::size_t> parents_;
};

using Edge=std::array<std::uint32_t,2>;
Edge edge(std::uint32_t a,std::uint32_t b) {
    return {{std::min(a,b),std::max(a,b)}};
}

std::int16_t edgeNeighbor(const Mesh& mesh,const std::vector<std::size_t>& incident,
                          std::size_t face,std::size_t count) {
    std::int16_t result=-1;
    for(const auto candidate:incident) {
        if(candidate==face) continue;
        const auto narrowed=static_cast<std::int16_t>(candidate);
        if(mesh.animated?narrowed!=-1:narrowed>=0) return narrowed;
        if(candidate+1u==count || (candidate+2u==count && face+1u==count)) result=narrowed;
    }
    return result;
}

std::vector<std::array<std::int16_t,3>> reconstructedAdjacency(const Mesh& mesh,
                            const std::vector<std::array<std::uint32_t,3>>& faces,
                            const neoshared::bif::JobControl& job) {
    std::map<Edge,std::vector<std::size_t>> incident;
    for(std::size_t i=0;i<faces.size();++i) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner) {
            auto& list=incident[edge(faces[i][corner],faces[i][(corner+1u)%3u])];
            if(list.empty() || list.back()!=i) list.push_back(i);
        }
    }
    std::vector<std::array<std::int16_t,3>> result(faces.size());
    for(std::size_t i=0;i<faces.size();++i) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner) {
            result[i][corner]=edgeNeighbor(mesh,
                incident.at(edge(faces[i][corner],faces[i][(corner+1u)%3u])),i,faces.size());
        }
    }
    return result;
}

std::size_t changedAdjacency(const Mesh& mesh,
                            const std::vector<std::array<std::uint32_t,3>>& faces,
                            const neoshared::bif::JobControl& job) {
    const auto neighbors=reconstructedAdjacency(mesh,faces,job);
    std::size_t changed{};
    for(std::size_t face=0;face<faces.size();++face)
        for(std::size_t corner=0;corner<3u;++corner)
            changed+=neighbors[face][corner]!=mesh.faces[face].adjacentFaces[corner];
    return changed;
}

} // namespace

std::vector<ReconstructedTopology> reconstructTopologies(
    const Mesh& mesh,
    const std::vector<Vertex>& compiledVertices,
    const std::vector<std::vector<std::uint32_t>>& sourceAttributes,
    const neoshared::bif::JobControl& job) {
    CornerGroups groups(mesh.faces.size()*3u);
    const auto compiled=[&](std::size_t face,std::size_t corner) {
        return static_cast<std::size_t>(mesh.faces.at(face).indices[corner]);
    };
    const auto compatible=[&](std::size_t a,std::size_t b) {
        return same(compiledVertices.at(a).position,compiledVertices.at(b).position) &&
               sourceAttributes.at(a)==sourceAttributes.at(b);
    };
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto neighbor=mesh.faces[i].adjacentFaces[corner];
            if(neighbor==-1) continue;
            const auto other=static_cast<std::size_t>(static_cast<std::uint16_t>(neighbor));
            const auto next=(corner+1u)%3u;
            const auto a=compiled(i,corner),b=compiled(i,next);
            std::vector<std::array<std::size_t,2>> matches;
            for(std::size_t x=0;x<3u;++x)
                for(std::size_t y=0;y<3u;++y) {
                    if(x==y || !compatible(a,compiled(other,x)) ||
                       !compatible(b,compiled(other,y))) continue;
                    const std::array<std::size_t,2> match{{
                        groups.find(other*3u+x),groups.find(other*3u+y)}};
                    if(std::find(matches.begin(),matches.end(),match)==matches.end())
                        matches.push_back(match);
                }
            // A compiled index can merge disconnected source vertices. Only
            // the retained edge and its identifiable endpoints establish a
            // source join; coincident positions alone do not establish one.
            if(matches.size()==1u) {
                groups.join(i*3u+corner,matches.front()[0]);
                groups.join(i*3u+next,matches.front()[1]);
            }
        }
    }

    std::vector<ReconstructedTopology> result;
    const auto appendCandidate=[&] {
        ReconstructedTopology candidate;
        candidate.sourceFaces.resize(mesh.faces.size());
        std::map<std::size_t,std::uint32_t> sourceIndex;
        std::vector<bool> referenced(compiledVertices.size());
        for(std::size_t i=0;i<mesh.faces.size();++i) {
            job.check();
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto index=compiled(i,corner);
                referenced.at(index)=true;
                const auto inserted=sourceIndex.emplace(groups.find(i*3u+corner),
                    static_cast<std::uint32_t>(candidate.sourceVertices.size()));
                if(inserted.second) candidate.sourceVertices.push_back(index);
                candidate.sourceFaces[i][corner]=inserted.first->second;
            }
        }
        // Unreferenced compiled rows have no adjacency evidence. Preserve
        // each row and its payload, including meshes with no faces.
        for(std::size_t i=0;i<compiledVertices.size();++i) {
            job.check();
            if(!referenced[i]) candidate.sourceVertices.push_back(i);
        }
        const auto repeated=std::any_of(result.begin(),result.end(),[&](const auto& previous) {
            return previous.sourceFaces==candidate.sourceFaces &&
                   previous.sourceVertices==candidate.sourceVertices;
        });
        if(!repeated) {
            candidate.changedAdjacency=changedAdjacency(mesh,candidate.sourceFaces,job);
            result.push_back(std::move(candidate));
        }
    };
    appendCandidate();
    auto neighbors=reconstructedAdjacency(mesh,result.front().sourceFaces,job);

    std::vector<std::array<std::uint32_t,3>> sourceFaces(mesh.faces.size());
    std::map<Edge,std::vector<std::size_t>> incident;
    for(std::size_t face=0;face<sourceFaces.size();++face) {
        for(std::size_t corner=0;corner<3u;++corner)
            sourceFaces[face][corner]=static_cast<std::uint32_t>(groups.find(face*3u+corner));
        for(std::size_t corner=0;corner<3u;++corner) {
            auto& list=incident[edge(sourceFaces[face][corner],sourceFaces[face][(corner+1u)%3u])];
            if(list.empty() || list.back()!=face) list.push_back(face);
        }
    }

    std::vector<std::vector<std::size_t>> members(mesh.faces.size()*3u);
    std::vector<ascii::NormalFaceCross> crosses;
    for(std::size_t face=0;face<mesh.faces.size();++face) {
        for(std::size_t corner=0;corner<3u;++corner) members[groups.find(face*3u+corner)].push_back(face*3u+corner);
        const auto x=compiledVertices[compiled(face,0u)].position;
        const auto y=compiledVertices[compiled(face,1u)].position;
        const auto z=compiledVertices[compiled(face,2u)].position;
        crosses.push_back(ascii::normalFaceCross(x,y,z));
    }
    const auto normalJoinImprovement=[&](std::size_t a,std::size_t b) {
        a=groups.find(a);b=groups.find(b);
        const auto target=compiledVertices[compiled(a/3u,a%3u)].normal;
        std::array<std::vector<std::size_t>,3> faces;
        std::size_t index{};
        for(const auto group:{a,b}) {
            for(const auto corner:members[group]) {
                if(!same(compiledVertices[compiled(corner/3u,corner%3u)].normal,target)) return 0.0;
                auto& list=faces[index];
                if(list.empty() || list.back()!=corner/3u) list.push_back(corner/3u);
            }
            ++index;
        }
        std::set_union(faces[0].begin(),faces[0].end(),faces[1].begin(),faces[1].end(),std::back_inserter(faces[2]));
        const auto error=[&](const std::vector<std::size_t>& faces) {
            Vec3 value{};
            for(const auto face:faces) value=crosses[face].accumulate(value);
            value=ascii::normalizeVertexNormal(value);
            const double x=double(value.x)-target.x,y=double(value.y)-target.y,z=double(value.z)-target.z;
            return (x*x+y*y)+z*z;
        };
        return members[a].size()*error(faces[0])+members[b].size()*error(faces[1])-
            (members[a].size()+members[b].size())*error(faces[2]);
    };

    const auto proposeJoins=[&](const std::vector<std::size_t>& corners,bool improveNormals=false) {
        job.check();
        std::vector<std::size_t> candidates;
        for(const auto corner:corners) {
            const auto group=groups.find(corner);
            if(std::find(candidates.begin(),candidates.end(),group)==candidates.end())
                candidates.push_back(group);
        }
        for(std::size_t i=0;i<candidates.size();++i)
            for(std::size_t j=i+1u;j<candidates.size();++j) {
                if(groups.find(candidates[i])==groups.find(candidates[j])) continue;
                if(improveNormals && !(normalJoinImprovement(candidates[i],candidates[j])>0.0)) continue;
                // Update only edges incident to the merged source. Retain
                // every recovered neighbor; unresolved source edges may move
                // while the remaining endpoint joins are reconstructed.
                const auto first=groups.find(candidates[i]),second=groups.find(candidates[j]);
                const auto kept=std::min(first,second),removed=std::max(first,second);
                std::map<std::size_t,std::array<std::uint32_t,3>> oldFaces;
                std::map<Edge,std::vector<std::size_t>> oldEdges;
                for(const auto corner:members[removed]) oldFaces.emplace(corner/3u,sourceFaces[corner/3u]);
                for(const auto& entry:oldFaces) {
                    auto& face=sourceFaces[entry.first];
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto key=edge(face[corner],face[(corner+1u)%3u]);
                        oldEdges.emplace(key,incident[key]);
                        auto& list=incident[key];
                        list.erase(std::remove(list.begin(),list.end(),entry.first),list.end());
                    }
                    for(auto& source:face) if(source==removed) source=static_cast<std::uint32_t>(kept);
                }
                for(const auto& entry:oldFaces) {
                    const auto& face=sourceFaces[entry.first];
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto key=edge(face[corner],face[(corner+1u)%3u]);
                        oldEdges.emplace(key,incident[key]);
                        auto& list=incident[key];
                        const auto at=std::lower_bound(list.begin(),list.end(),entry.first);
                        if(at==list.end() || *at!=entry.first) list.insert(at,entry.first);
                    }
                }
                bool retained=true;
                std::map<std::pair<std::size_t,std::size_t>,std::int16_t> changed;
                for(const auto& entry:oldEdges)
                    for(const auto face:incident.at(entry.first))
                        for(std::size_t corner=0;corner<3u;++corner) {
                            if(edge(sourceFaces[face][corner],sourceFaces[face][(corner+1u)%3u])!=entry.first) continue;
                            const auto neighbor=edgeNeighbor(mesh,incident.at(entry.first),face,sourceFaces.size());
                            if(neighbors[face][corner]==mesh.faces[face].adjacentFaces[corner] &&
                               neighbor!=neighbors[face][corner]) retained=false;
                            changed[{face,corner}]=neighbor;
                        }
                if(retained) {
                    auto& merged=members[kept];auto& discarded=members[removed];
                    merged.insert(merged.end(),discarded.begin(),discarded.end());
                    std::sort(merged.begin(),merged.end());discarded.clear();
                    groups.join(kept,removed);
                    for(const auto& entry:changed) neighbors[entry.first.first][entry.first.second]=entry.second;
                } else {
                    for(const auto& entry:oldFaces) sourceFaces[entry.first]=entry.second;
                    for(auto& entry:oldEdges) incident[entry.first]=std::move(entry.second);
                }
            }
    };
    std::vector<std::vector<std::size_t>> compiledCorners(compiledVertices.size());
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner)
            compiledCorners.at(compiled(i,corner)).push_back(i*3u+corner);
    }
    // Prioritize the missing smooth joins before ambiguous flat point joins
    // can consume their boundary adjacency.
    std::map<std::vector<std::uint32_t>,std::vector<std::size_t>> smoothCorners;
    for(std::size_t i=0;i<compiledVertices.size();++i) {
        auto key=sourceAttributes.at(i);
        const auto& v=compiledVertices[i];
        for(const auto value:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z})
            key.push_back(bits(value));
        for(const auto corner:compiledCorners[i]) smoothCorners[key].push_back(groups.find(corner));
    }
    std::vector<std::tuple<double,std::size_t,std::size_t>> joins;
    for(auto& entry:smoothCorners) {
        auto& corners=entry.second;
        std::sort(corners.begin(),corners.end());
        corners.erase(std::unique(corners.begin(),corners.end()),corners.end());
        for(std::size_t a=0;a<corners.size();++a)
            for(std::size_t b=a+1u;b<corners.size();++b) {
                const auto improvement=normalJoinImprovement(corners[a],corners[b]);
                if(improvement>0.0) joins.emplace_back(-improvement,corners[a],corners[b]);
            }
    }
    std::sort(joins.begin(),joins.end());
    for(const auto& join:joins) proposeJoins({std::get<1>(join),std::get<2>(join)},true);
    appendCandidate();

    // InternalGenVertices can merge source rows at a single point, outside
    // any shared edge. A shared compiled row retains equal source payloads.
    for(const auto& corners:compiledCorners) proposeJoins(corners);
    appendCandidate();

    for(const bool requireNormal:{true,false}) {
        if(requireNormal && mesh.enginePrelightInput && (mesh.mdxBitmap&0x20u)==0u) continue;
        // UV seams and different smoothing masks can split one source point
        // into distinct compiled rows. First retain equal normals, then allow
        // hard edges; the caller checks rebuilt normals and sphere together.
        std::map<std::vector<std::uint32_t>,std::vector<std::size_t>> matchingCorners;
        for(std::size_t i=0;i<compiledVertices.size();++i) {
            job.check();
            if(compiledCorners[i].empty()) continue;
            const auto& vertex=compiledVertices[i];
            auto key=sourceAttributes.at(i);
            for(const auto value:{vertex.position.x,vertex.position.y,vertex.position.z})
                key.push_back(bits(value));
            if(requireNormal)
                for(const auto value:{vertex.normal.x,vertex.normal.y,vertex.normal.z})
                    key.push_back(bits(value));
            auto& corners=matchingCorners[std::move(key)];
            corners.insert(corners.end(),compiledCorners[i].begin(),compiledCorners[i].end());
        }
        for(const auto& entry:matchingCorners) proposeJoins(entry.second);
        appendCandidate();
    }
    return result;
}

} // namespace neomdl::model::ascii_export
