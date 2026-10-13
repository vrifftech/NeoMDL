#include <model/AsciiExportSmoothing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace neomdl::model::ascii_export {
namespace {

constexpr auto ownFace=std::numeric_limits<std::size_t>::max();
struct Contribution { std::size_t face,relation; };
struct Corner { Vec3 target; std::size_t source; std::vector<Contribution> contributions; };
struct Relation { std::size_t first,second; std::vector<std::size_t> corners; };

Vec3 subtract(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 cross(Vec3 a,Vec3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
Vec3 normalizeSmoothing(Vec3 value) {
    // ProcessVerts: K1 0x5c3950, K2 0x6e1a50. Exact zero stays zero;
    // nonzero vectors take Vector::Normalize's separate 1e-9 branch.
    const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
    if(length!=0.0f) {
        if(static_cast<double>(length)<1e-9) return {1.0f,0.0f,0.0f};
        const float scale=1.0f/length;
        value={value.x*scale,value.y*scale,value.z*scale};
    }
    return value;
}
double difference(Vec3 a,Vec3 b) {
    const double x=static_cast<double>(a.x)-b.x;
    const double y=static_cast<double>(a.y)-b.y;
    const double z=static_cast<double>(a.z)-b.z;
    return (x*x+y*y)+z*z;
}

struct Smoothing {
    std::vector<Vec3> crosses;
    std::vector<Corner> corners;
    std::vector<Relation> relations;
    std::vector<std::vector<std::size_t>> faceRelations;
    const neoshared::bif::JobControl& job;

    Vec3 normal(std::size_t corner,const std::vector<std::uint8_t>& joined) const {
        Vec3 sum{};
        // Contributions remain in source face order. Equal positive masks
        // reuse a prior normal in ProcessVerts; their contribution sets and
        // accumulation order are identical here. Mask zero includes only self.
        for(const auto& entry:corners[corner].contributions)
            if(entry.relation==ownFace || joined[entry.relation]) {
                const auto weighted=crosses[entry.face];
                sum={sum.x+weighted.x,sum.y+weighted.y,weighted.z+sum.z};
            }
        return normalizeSmoothing(sum);
    }
    double error(std::size_t corner,const std::vector<std::uint8_t>& joined) const {
        return difference(normal(corner,joined),corners[corner].target);
    }
    std::vector<double> errors(const std::vector<std::uint8_t>& joined) const {
        std::vector<double> result;result.reserve(corners.size());
        for(std::size_t i=0;i<corners.size();++i) result.push_back(error(i,joined));
        return result;
    }
    double score(const std::vector<std::uint8_t>& joined) const {
        double result{};
        for(std::size_t i=0;i<corners.size();++i) result+=error(i,joined);
        return result;
    }
    std::vector<std::uint8_t> fit(std::vector<std::uint8_t> joined,bool remove) const {
        auto current=errors(joined);
        bool changed;
        do {
            changed=false;
            for(std::size_t i=0;i<relations.size();++i) {
                job.check();
                // Each relation changes once in the selected direction.
                // No pass count or angular threshold is needed.
                if(joined[i]!=(remove?1u:0u)) continue;
                double before{},after{};
                for(const auto corner:relations[i].corners) before+=current[corner];
                joined[i]=remove?0u:1u;
                for(const auto corner:relations[i].corners) after+=error(corner,joined);
                if(after<before) {
                    changed=true;
                    for(const auto corner:relations[i].corners) current[corner]=error(corner,joined);
                } else joined[i]=remove?1u:0u;
            }
        } while(changed);
        return joined;
    }

    std::vector<std::uint8_t> fitPairs(std::vector<std::uint8_t> joined) const {
        std::vector<std::pair<std::size_t,std::size_t>> pairs;
        for(const auto& corner:corners)
            for(std::size_t a=0;a<corner.contributions.size();++a) {
                const auto first=corner.contributions[a].relation;
                if(first==ownFace) continue;
                for(std::size_t b=a+1u;b<corner.contributions.size();++b) {
                    const auto second=corner.contributions[b].relation;
                    if(second!=ownFace) pairs.push_back(std::minmax(first,second));
                }
            }
        std::sort(pairs.begin(),pairs.end());
        pairs.erase(std::unique(pairs.begin(),pairs.end()),pairs.end());
        auto current=errors(joined);
        for(const auto& pair:pairs) {
            job.check();
            auto affected=relations[pair.first].corners;
            const auto& other=relations[pair.second].corners;
            affected.insert(affected.end(),other.begin(),other.end());
            std::sort(affected.begin(),affected.end());
            affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            double before{},after{};
            for(const auto corner:affected) before+=current[corner];
            joined[pair.first]^=1u;joined[pair.second]^=1u;
            for(const auto corner:affected) after+=error(corner,joined);
            if(after<before) {
                for(const auto corner:affected) current[corner]=error(corner,joined);
            } else {
                joined[pair.first]^=1u;joined[pair.second]^=1u;
            }
        }
        return joined;
    }

    std::pair<std::vector<std::uint8_t>,std::vector<std::uint8_t>> normalGroups() const {
        std::vector<std::uint8_t> any(relations.size()),all(relations.size(),1u);
        for(std::size_t relation=0;relation<relations.size();++relation) {
            const auto& affected=relations[relation].corners;
            for(std::size_t i=0;i<affected.size();++i)
                for(std::size_t j=i+1u;j<affected.size();++j) {
                    const auto& a=corners[affected[i]];const auto& b=corners[affected[j]];
                    if(affected[i]/3u==affected[j]/3u || a.source!=b.source) continue;
                    const bool same=a.target.x==b.target.x && a.target.y==b.target.y && a.target.z==b.target.z;
                    any[relation]|=same;all[relation]&=same;
                }
        }
        return {std::move(any),std::move(all)};
    }

    std::vector<std::uint32_t> encode(const std::vector<std::uint8_t>& wanted) const {
        std::vector<std::uint32_t> masks(crosses.size());
        std::vector<std::uint8_t> joined(relations.size());
        auto current=errors(joined);
        std::uint32_t usedBits{};
        struct Choice {
            std::uint32_t bit{};
            bool compatible{};
            double improvement{};
            std::vector<std::size_t> relations,corners;
        };
        bool changed;
        do {
            changed=false;
            for(std::size_t i=0;i<relations.size();++i) {
                job.check();
                if(!wanted[i] || joined[i]) continue;
                const auto& pair=relations[i];
                Choice best;
                bool unusedBitVisited=false;
                for(unsigned index=0;index<32u;++index) {
                    const auto bit=std::uint32_t{1}<<index;
                    // Unused bits induce the same single new intersection.
                    if(!(usedBits&bit)) {
                        if(unusedBitVisited) continue;
                        unusedBitVisited=true;
                    }
                    Choice choice;choice.bit=bit;choice.compatible=true;
                    for(const auto face:{pair.first,pair.second})
                        for(const auto relation:faceRelations[face]) {
                            if(joined[relation]) continue;
                            const auto& adjacent=relations[relation];
                            const auto first=masks[adjacent.first] |
                                ((adjacent.first==pair.first || adjacent.first==pair.second)?bit:0u);
                            const auto second=masks[adjacent.second] |
                                ((adjacent.second==pair.first || adjacent.second==pair.second)?bit:0u);
                            if(first&second) choice.relations.push_back(relation);
                        }
                    std::sort(choice.relations.begin(),choice.relations.end());
                    choice.relations.erase(std::unique(choice.relations.begin(),choice.relations.end()),choice.relations.end());
                    for(const auto relation:choice.relations) {
                        choice.compatible=choice.compatible && wanted[relation];
                        choice.corners.insert(choice.corners.end(),relations[relation].corners.begin(),relations[relation].corners.end());
                        joined[relation]=1u;
                    }
                    std::sort(choice.corners.begin(),choice.corners.end());
                    choice.corners.erase(std::unique(choice.corners.begin(),choice.corners.end()),choice.corners.end());
                    for(const auto corner:choice.corners)
                        choice.improvement+=current[corner]-error(corner,joined);
                    for(const auto relation:choice.relations) joined[relation]=0u;
                    if(!best.bit || (choice.compatible && !best.compatible) ||
                       (choice.compatible==best.compatible && choice.improvement>best.improvement))
                        best=std::move(choice);
                }
                // Prefer an exact encoding of the fitted intersections.
                // At the format's 32-bit limit, retain only an improvement
                // measured against the actual stored normals.
                if(!best.compatible && !(best.improvement>0.0)) continue;
                masks[pair.first]|=best.bit;masks[pair.second]|=best.bit;
                usedBits|=best.bit;
                for(const auto relation:best.relations) joined[relation]=1u;
                for(const auto corner:best.corners) current[corner]=error(corner,joined);
                changed=true;
            }
        } while(changed);
        return masks;
    }
    std::vector<std::uint8_t> intersections(const std::vector<std::uint32_t>& masks) const {
        std::vector<std::uint8_t> joined;joined.reserve(relations.size());
        for(const auto& pair:relations) joined.push_back((masks[pair.first]&masks[pair.second])!=0u);
        return joined;
    }
};

} // namespace

ReconstructedSmoothing reconstructSmoothing(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const neoshared::bif::JobControl& job,
    const std::vector<Vec3>* cornerTargets,bool refine) {
    Smoothing work{{},{},{},{},job};
    work.faceRelations.resize(sourceFaces.size());
    std::vector<std::vector<std::size_t>> incident(sourceVertices.size());
    for(std::size_t face=0;face<sourceFaces.size();++face) {
        job.check();
        const auto& indices=sourceFaces[face];
        const auto a=vertices[sourceVertices[indices[0]]].position;
        const auto b=vertices[sourceVertices[indices[1]]].position;
        const auto c=vertices[sourceVertices[indices[2]]].position;
        work.crosses.push_back(cross(subtract(b,a),subtract(c,b)));
        for(const auto source:indices) {
            auto& faces=incident[source];
            if(faces.empty() || faces.back()!=face) faces.push_back(face);
        }
    }
    std::map<std::pair<std::size_t,std::size_t>,std::size_t> relationIndices;
    for(const auto& faces:incident) {
        job.check();
        for(std::size_t a=0;a<faces.size();++a)
            for(std::size_t b=a+1u;b<faces.size();++b)
                relationIndices.emplace(std::make_pair(faces[a],faces[b]),0u);
    }
    for(auto& entry:relationIndices) {
        entry.second=work.relations.size();
        const auto first=entry.first.first,second=entry.first.second;
        work.faceRelations[first].push_back(entry.second);
        work.faceRelations[second].push_back(entry.second);
        work.relations.push_back({first,second,{}});
    }
    for(std::size_t face=0;face<sourceFaces.size();++face)
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto index=work.corners.size();
            const auto target=cornerTargets?(*cornerTargets)[face*3u+corner]:
                vertices[static_cast<std::size_t>(mesh.faces[face].indices[corner])].normal;
            Corner value{target,sourceFaces[face][corner],{}};
            for(const auto candidate:incident[sourceFaces[face][corner]]) {
                const auto relation=candidate==face?ownFace:
                    relationIndices.at(std::minmax(face,candidate));
                value.contributions.push_back({candidate,relation});
                if(relation!=ownFace) work.relations[relation].corners.push_back(index);
            }
            work.corners.push_back(std::move(value));
        }
    ReconstructedSmoothing result;
    result.masks.assign(sourceFaces.size(),0u);
    std::vector<std::uint8_t> joined(work.relations.size());
    double best=work.score(joined);
    const auto consider=[&](const std::vector<std::uint8_t>& relations) {
        auto masks=work.encode(relations);
        auto candidate=work.intersections(masks);
        const auto score=work.score(candidate);
        if(score<best) {
            best=score;joined=std::move(candidate);result.masks=std::move(masks);
        }
    };
    // Equal stored corner normals retain the old source-split contribution
    // sets while shared source IDs preserve connectivity. Face masks act at
    // every shared vertex, so conflicting equality evidence supplies both
    // candidate relations; the actual constructor error selects between them.
    const auto groups=work.normalGroups();
    const auto considerGroup=[&](const std::vector<std::uint8_t>& group) {
        consider(group);
        const auto added=work.fit(group,false),removed=work.fit(group,true);
        if(added!=group) {
            consider(added);
            consider(work.fit(added,true));
        }
        if(removed!=group) {
            consider(removed);
            consider(work.fit(removed,false));
        }
    };
    considerGroup(groups.first);
    if(groups.second!=groups.first) considerGroup(groups.second);
    const auto added=work.fit(std::vector<std::uint8_t>(work.relations.size()),false);
    consider(added);
    consider(work.fit(added,true));
    const auto removed=work.fit(std::vector<std::uint8_t>(work.relations.size(),1u),true);
    consider(removed);
    consider(work.fit(removed,false));
    for(const auto mask:{0u,1u}) {
        std::vector<std::uint8_t> candidate(work.relations.size(),mask);
        const auto score=work.score(candidate);
        if(score<best) {
            best=score;joined=std::move(candidate);
            result.masks.assign(sourceFaces.size(),mask);
        }
    }
    // Two intersecting relations can improve together even when either
    // change alone increases error. Refit the encoded intersections until
    // the actual representable result stops improving.
    while(refine) {
        const auto previous=best;
        consider(work.fit(joined,false));
        consider(work.fit(joined,true));
        consider(work.fitPairs(joined));
        if(!(best<previous)) break;
    }
    result.squaredNormalError=best;
    for(std::size_t corner=0;corner<work.corners.size();++corner) {
        const auto actual=work.normal(corner,joined),target=work.corners[corner].target;
        const float delta=std::max({std::abs(actual.x-target.x),std::abs(actual.y-target.y),std::abs(actual.z-target.z)});
        result.changedCornerNormals+=delta!=0.0f;
        result.maxNormalComponentDifference=std::max(result.maxNormalComponentDifference,delta);
    }
    return result;
}

} // namespace neomdl::model::ascii_export
