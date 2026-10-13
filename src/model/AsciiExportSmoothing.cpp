#include <model/AsciiExportSmoothing.hpp>
#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <tuple>

namespace neomdl::model::ascii_export {
namespace {

constexpr auto ownFace=std::numeric_limits<std::size_t>::max();
struct Contribution { std::size_t face,relation; };
struct Corner {
    Vec3 target;
    std::size_t source,compiledVertex;
    std::vector<Contribution> contributions;
    bool scored{true};
    TangentBasis basisTarget;
    std::vector<Contribution> basisContributions;
    std::size_t basisWriter{};
};
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
Vec3 normalizeTangent(Vec3 value) {
    const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
    if(static_cast<double>(length)<1e-9) return {1.0f,0.0f,0.0f};
    const float scale=1.0f/length;
    return {value.x*scale,value.y*scale,value.z*scale};
}

struct Smoothing {
    std::vector<Vec3> crosses;
    std::vector<Corner> corners;
    std::vector<Relation> relations;
    std::vector<std::vector<std::size_t>> faceRelations;
    const neoshared::bif::JobControl& job;
    std::vector<TangentBasis> faceBases;
    mutable std::map<std::pair<std::vector<std::uint8_t>,bool>,std::vector<std::uint8_t>> fitted;

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
        if(!faceBases.empty()) {
            // CalculateTangentSpaceBasis starts with self, then accumulates
            // each matching position corner in face order (K2 0x6e3f40).
            const auto writer=corners[corner].basisWriter;
            auto basis=faceBases[writer/3u];
            const auto add=[](Vec3 a,Vec3 b) { return Vec3{a.x+b.x,a.y+b.y,a.z+b.z}; };
            for(const auto& entry:corners[writer].basisContributions)
                if(entry.relation!=ownFace && joined[entry.relation]) {
                    const auto& other=faceBases[entry.face];
                    basis.bitangent=add(basis.bitangent,other.bitangent);
                    basis.tangent=add(basis.tangent,other.tangent);
                    basis.normal=add(basis.normal,other.normal);
                }
            const auto& target=corners[corner].basisTarget;
            return difference(normalizeTangent(basis.bitangent),target.bitangent)+
                difference(normalizeTangent(basis.tangent),target.tangent)+
                difference(normalizeTangent(basis.normal),target.normal);
        }
        return corners[corner].scored?difference(normal(corner,joined),corners[corner].target):0.0;
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
        auto key=std::make_pair(joined,remove);
        const auto found=fitted.find(key);
        if(found!=fitted.end()) return found->second;
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
        fitted.emplace(std::move(key),joined);
        return joined;
    }

    std::vector<std::uint8_t> fitPairs(std::vector<std::uint8_t> joined) const {
        std::vector<std::pair<std::size_t,std::size_t>> pairs;
        for(const auto& corner:corners) if(corner.scored) {
            const auto& contributions=faceBases.empty()?corner.contributions:
                corners[corner.basisWriter].basisContributions;
            for(std::size_t a=0;a<contributions.size();++a) {
                const auto first=contributions[a].relation;
                if(first==ownFace) continue;
                for(std::size_t b=a+1u;b<contributions.size();++b) {
                    const auto second=contributions[b].relation;
                    if(second!=ownFace && second!=first) pairs.push_back(std::minmax(first,second));
                }
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

    std::vector<std::uint8_t> fitSharedVertices(std::vector<std::uint8_t> joined,bool useTargetNormals=false,
                                             bool deferScoring=false,
                                             std::vector<std::vector<std::uint8_t>>* relationCandidates=nullptr) const {
        // A retained vertex can require several joins together. Share the
        // current contribution sets of its corners, then score the whole move.
        std::map<std::pair<std::size_t,std::size_t>,std::vector<std::size_t>> groups;
        std::map<std::size_t,std::vector<std::size_t>> representatives;
        for(std::size_t i=0;i<corners.size();++i) if(corners[i].scored) {
            auto identity=corners[i].compiledVertex;
            if(useTargetNormals) {
                auto& source=representatives[corners[i].source];
                const auto found=std::find_if(source.begin(),source.end(),[&](std::size_t index) {
                    const auto a=corners[i].target,b=corners[index].target;
                    return a.x==b.x && a.y==b.y && a.z==b.z;
                });
                identity=found==source.end()?i:*found;
                if(found==source.end()) source.push_back(i);
            }
            groups[{corners[i].source,identity}].push_back(i);
        }
        auto current=errors(joined);
        for(const auto& group:groups) {
            job.check();
            std::vector<std::size_t> faces;
            for(const auto corner:group.second)
                for(const auto& entry:corners[corner].contributions)
                    if(entry.relation==ownFace || joined[entry.relation]) faces.push_back(entry.face);
            std::sort(faces.begin(),faces.end());
            faces.erase(std::unique(faces.begin(),faces.end()),faces.end());
            std::vector<std::size_t> added;
            for(const auto corner:group.second)
                for(const auto& entry:corners[corner].contributions)
                    if(entry.relation!=ownFace && !joined[entry.relation] &&
                       std::binary_search(faces.begin(),faces.end(),entry.face)) added.push_back(entry.relation);
            std::sort(added.begin(),added.end());
            added.erase(std::unique(added.begin(),added.end()),added.end());
            if(relationCandidates) {
                if(!added.empty()) {
                    auto candidate=joined;
                    for(const auto relation:added) candidate[relation]=1u;
                    relationCandidates->push_back(std::move(candidate));
                }
                continue;
            }
            if(deferScoring) {
                // Score the combined move after mask encoding.
                for(const auto relation:added) joined[relation]=1u;
                continue;
            }
            std::vector<std::size_t> affected;
            for(const auto relation:added)
                affected.insert(affected.end(),relations[relation].corners.begin(),relations[relation].corners.end());
            std::sort(affected.begin(),affected.end());
            affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            double before{},after{};
            for(const auto corner:affected) before+=current[corner];
            for(const auto relation:added) joined[relation]=1u;
            for(const auto corner:affected) after+=error(corner,joined);
            if(after<before) {
                for(const auto corner:affected) current[corner]=error(corner,joined);
            } else {
                for(const auto relation:added) joined[relation]=0u;
            }
        }
        return joined;
    }

    std::vector<std::uint8_t> fitSharedContributions(std::vector<std::uint8_t> joined) const {
        // Corners of a retained row share the final result. Intersect their
        // current contribution sets to propose coupled removals together.
        std::map<std::pair<std::size_t,std::size_t>,std::vector<std::size_t>> groups;
        for(std::size_t i=0;i<corners.size();++i)
            groups[{corners[i].source,corners[i].compiledVertex}].push_back(i);
        auto current=errors(joined);
        for(const auto& group:groups) {
            job.check();
            std::vector<std::size_t> common;
            bool first=true;
            for(const auto corner:group.second) {
                std::vector<std::size_t> faces;
                for(const auto& entry:(faceBases.empty()?corners[corner].contributions:corners[corner].basisContributions))
                    if(entry.relation==ownFace || joined[entry.relation]) faces.push_back(entry.face);
                if(first) { common=std::move(faces);first=false; }
                else common.erase(std::remove_if(common.begin(),common.end(),[&](std::size_t face) {
                    return !std::binary_search(faces.begin(),faces.end(),face);
                }),common.end());
            }
            std::vector<std::size_t> removed,affected;
            for(const auto corner:group.second)
                for(const auto& entry:(faceBases.empty()?corners[corner].contributions:corners[corner].basisContributions))
                    if(entry.relation!=ownFace && joined[entry.relation] &&
                       !std::binary_search(common.begin(),common.end(),entry.face)) removed.push_back(entry.relation);
            std::sort(removed.begin(),removed.end());
            removed.erase(std::unique(removed.begin(),removed.end()),removed.end());
            for(const auto relation:removed)
                affected.insert(affected.end(),relations[relation].corners.begin(),relations[relation].corners.end());
            std::sort(affected.begin(),affected.end());
            affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            double before{},after{};
            for(const auto corner:affected) before+=current[corner];
            for(const auto relation:removed) joined[relation]=0u;
            for(const auto corner:affected) after+=error(corner,joined);
            if(after<before) {
                for(const auto corner:affected) current[corner]=error(corner,joined);
            } else {
                for(const auto relation:removed) joined[relation]=1u;
            }
        }
        return joined;
    }

    using NormalKey=std::tuple<std::size_t,float,float,float>;
    std::map<NormalKey,std::vector<std::size_t>> normalCornerGroups() const {
        std::map<NormalKey,std::vector<std::size_t>> groups;
        for(std::size_t i=0;i<corners.size();++i) if(corners[i].scored) {
            const auto& c=corners[i];
            groups[{c.source,c.target.x,c.target.y,c.target.z}].push_back(i);
        }
        return groups;
    }

    // A whole normal group can improve where individual joins cannot.
    std::vector<std::uint8_t> fitNormalGroups(std::vector<std::uint8_t> joined) const {
        const auto groups=normalCornerGroups();
        auto current=errors(joined);
        for(auto a=groups.begin();a!=groups.end();++a)
            for(auto b=std::next(a);b!=groups.end() && std::get<0>(a->first)==std::get<0>(b->first);++b) {
                job.check();
                std::vector<std::size_t> changes,affected;
                for(const auto corner:a->second)
                    for(const auto& entry:corners[corner].contributions)
                        if(entry.relation!=ownFace && std::any_of(b->second.begin(),b->second.end(),
                           [&](std::size_t other){return other/3u==entry.face;})) changes.push_back(entry.relation);
                std::sort(changes.begin(),changes.end());
                changes.erase(std::unique(changes.begin(),changes.end()),changes.end());
                for(const auto relation:changes)
                    affected.insert(affected.end(),relations[relation].corners.begin(),relations[relation].corners.end());
                std::sort(affected.begin(),affected.end());
                affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
                for(const auto value:{0u,1u}) {
                    std::vector<std::uint8_t> previous;
                    double before{},after{};
                    for(const auto corner:affected) before+=current[corner];
                    for(const auto relation:changes) {previous.push_back(joined[relation]);joined[relation]=value;}
                    for(const auto corner:affected) after+=error(corner,joined);
                    if(after<before) {
                        for(const auto corner:affected) current[corner]=error(corner,joined);
                    } else for(std::size_t i=0;i<changes.size();++i) joined[changes[i]]=previous[i];
                }
            }
        return joined;
    }

    // Recover overlapping contributor groups from their stored normals.
    std::pair<std::vector<std::uint8_t>,std::vector<std::uint8_t>> contributionGroups() const {
        std::vector<std::uint8_t> any(relations.size()),all(relations.size(),1u);
        const auto groups=normalCornerGroups();
        for(auto first=groups.begin();first!=groups.end();) {
            auto last=first;
            while(last!=groups.end() && std::get<0>(last->first)==std::get<0>(first->first)) ++last;
            std::vector<std::vector<std::size_t>> blocks;
            for(auto it=first;it!=last;++it) blocks.push_back(it->second);
            for(std::size_t target=0;target<blocks.size();++target) {
                job.check();
                std::vector<bool> included(blocks.size());included[target]=true;
                const auto localScore=[&] {
                    double total{};
                    for(const auto corner:blocks[target]) {
                        Vec3 sum{};
                        for(const auto& entry:corners[corner].contributions) {
                            bool take=false;
                            for(std::size_t block=0;block<blocks.size();++block)
                                if(included[block] && std::any_of(blocks[block].begin(),blocks[block].end(),
                                   [&](std::size_t other){return other/3u==entry.face;})) take=true;
                            if(take) {const auto v=crosses[entry.face];sum={sum.x+v.x,sum.y+v.y,v.z+sum.z};}
                        }
                        total+=difference(normalizeSmoothing(sum),corners[corner].target);
                    }
                    return total;
                };
                auto best=localScore();
                bool changed;
                do {
                    changed=false;
                    for(std::size_t block=0;block<blocks.size();++block) if(block!=target) {
                        included[block]=!included[block];
                        const auto score=localScore();
                        if(score<best) {best=score;changed=true;} else included[block]=!included[block];
                    }
                } while(changed);
                for(const auto corner:blocks[target])
                    for(const auto& entry:corners[corner].contributions) if(entry.relation!=ownFace) {
                        bool take=false;
                        for(std::size_t block=0;block<blocks.size();++block)
                            if(included[block] && std::any_of(blocks[block].begin(),blocks[block].end(),
                               [&](std::size_t other){return other/3u==entry.face;})) take=true;
                        any[entry.relation]|=take;all[entry.relation]&=take;
                    }
            }
            first=last;
        }
        return {std::move(any),std::move(all)};
    }

    std::pair<std::vector<std::uint8_t>,std::vector<std::uint8_t>> normalGroups(bool useStoredRows=false) const {
        std::vector<std::uint8_t> any(relations.size()),all(relations.size(),1u);
        for(std::size_t relation=0;relation<relations.size();++relation) {
            const auto& affected=relations[relation].corners;
            for(std::size_t i=0;i<affected.size();++i)
                for(std::size_t j=i+1u;j<affected.size();++j) {
                    const auto& a=corners[affected[i]];const auto& b=corners[affected[j]];
                    if(affected[i]/3u==affected[j]/3u || a.source!=b.source) continue;
                    const bool same=useStoredRows?a.compiledVertex==b.compiledVertex:
                        a.target.x==b.target.x && a.target.y==b.target.y && a.target.z==b.target.z;
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
                std::set<std::vector<std::size_t>> visited;
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
                    if(!visited.insert(choice.relations).second) continue;
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

struct SourceFaceNeighbors {
    using Neighbor=std::pair<bool,std::size_t>;
    struct EdgeNeighbors {
        std::vector<std::size_t> faces;
        std::vector<Neighbor> neighbors;
    };
    bool animated{};
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::size_t>> edges;
    std::vector<EdgeNeighbors> expected;

    static std::vector<std::size_t> ranks(const std::vector<std::size_t>& order) {
        std::vector<std::size_t> result(order.size());
        for(std::size_t i=0;i<order.size();++i) result[order[i]]=i;
        return result;
    }
    Neighbor neighbor(const std::vector<std::size_t>& incident,std::size_t face,
                      const std::vector<std::size_t>& order,
                      const std::vector<std::size_t>& rank) const {
        auto first=ownFace;
        std::int16_t pending=-1;
        for(const auto other:incident) if(other!=face) {
            const auto narrowed=static_cast<std::int16_t>(rank[other]);
            if(animated?narrowed!=-1:narrowed>=0) {
                if(rank[other]<first) first=rank[other];
            } else if(rank[other]+1u==order.size() ||
                      (rank[other]+2u==order.size() && rank[face]+1u==order.size()))
                pending=narrowed;
        }
        const auto narrowed=first==ownFace?pending:static_cast<std::int16_t>(first);
        return narrowed<0
            ? Neighbor{false,static_cast<std::uint16_t>(narrowed)}
            : Neighbor{true,order[static_cast<std::uint16_t>(narrowed)]};
    }
    SourceFaceNeighbors(const Mesh& mesh,
            const std::vector<std::array<std::uint32_t,3>>& faces,
            const std::vector<std::size_t>& order):animated(mesh.animated) {
        for(std::size_t face=0;face<faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                auto& incident=edges[std::minmax(faces[face][corner],faces[face][(corner+1u)%3u])];
                if(incident.empty() || incident.back()!=face) incident.push_back(face);
            }
        const auto rank=ranks(order);
        for(const auto& edge:edges) {
            // Isolated edges have no neighbor; two-face edges always choose
            // each other while every emitted face index is nonnegative.
            if(edge.second.size()<2u || (edge.second.size()==2u &&
               order.size()<=static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max())+1u)) continue;
            auto& entry=expected.emplace_back();
            entry.faces=edge.second;
            for(const auto face:edge.second) entry.neighbors.push_back(neighbor(edge.second,face,order,rank));
        }
    }
    bool preserves(const std::vector<std::size_t>& order,
                   const std::vector<std::size_t>& rank) const {
        for(const auto& edge:expected) {
            for(std::size_t i=0;i<edge.faces.size();++i)
                if(neighbor(edge.faces,edge.faces[i],order,rank)!=edge.neighbors[i]) return false;
        }
        return true;
    }
    template<class Visitor>
    void permutations(const std::vector<std::size_t>& faces,
                      const std::vector<std::size_t>& current,
                      const neoshared::bif::JobControl& job,Visitor visit) const {
        auto rank=ranks(current);
        std::vector<bool> moving(current.size());
        std::vector<std::size_t> slots;
        for(const auto face:faces) {moving[face]=true;slots.push_back(rank[face]);}
        std::sort(slots.begin(),slots.end());
        std::set<std::pair<std::size_t,std::size_t>> precedence;
        for(const auto& edge:edges) {
            const auto& incident=edge.second;
            if(incident.size()<3u || std::none_of(incident.begin(),incident.end(),
                    [&](std::size_t face){return moving[face];})) continue;
            // Within the nonnegative short range, preserving the first two
            // edge users preserves every first-match neighbor. Other narrowed
            // indices retain the complete neighbor check at each leaf.
            if(std::any_of(incident.begin(),incident.end(),[&](std::size_t face) {
                    return (moving[face]?slots.back():rank[face])>
                        static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max());
                })) continue;
            auto sorted=incident;
            std::sort(sorted.begin(),sorted.end(),
                [&](std::size_t a,std::size_t b){return rank[a]<rank[b];});
            precedence.emplace(sorted[0],sorted[1]);
            for(std::size_t i=2u;i<sorted.size();++i) precedence.emplace(sorted[1],sorted[i]);
        }
        std::vector<std::vector<std::size_t>> before(current.size()),after(current.size());
        for(const auto& pair:precedence) {
            before[pair.second].push_back(pair.first);
            after[pair.first].push_back(pair.second);
        }
        for(const auto face:faces) rank[face]=ownFace;
        auto order=current;
        const auto search=[&](const auto& self,std::size_t depth,std::size_t changes)->void {
            job.check();
            if(depth==slots.size()) {visit(order,changes);return;}
            const auto slot=slots[depth];
            for(const auto face:faces) {
                if(rank[face]!=ownFace ||
                   std::any_of(before[face].begin(),before[face].end(),
                       [&](std::size_t other){return rank[other]>=slot;}) ||
                   std::any_of(after[face].begin(),after[face].end(),
                       [&](std::size_t other){return rank[other]<=slot;})) continue;
                rank[face]=slot;order[slot]=face;
                self(self,depth+1u,changes+(current[slot]!=face));
                rank[face]=ownFace;
            }
        };
        search(search,0u,0u);
    }
};

} // namespace

std::vector<std::size_t> reconstructNormalFaceOrder(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& currentOrder,
    const std::vector<Vec3>& positions,
    const neoshared::bif::JobControl& job,
    const std::vector<std::array<double,3>>* precisePositions) {
    using Triangle=std::array<std::array<float,3>,3>;
    const auto canonical=[](Triangle triangle) {
        auto result=triangle;
        for(std::size_t rotation=1u;rotation<3u;++rotation) {
            std::rotate(triangle.begin(),triangle.begin()+1,triangle.end());
            if(triangle<result) result=triangle;
        }
        return result;
    };
    struct Source {
        std::vector<std::size_t> faces,corners;
    };
    std::vector<Source> sources(sourceVertices.size());
    std::vector<Vec3> crosses;
    std::map<Triangle,std::vector<std::size_t>> coincident;
    for(std::size_t face=0;face<sourceFaces.size();++face) {
        job.check();
        const auto& indices=sourceFaces[face];
        Triangle triangle;
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto p=positions[indices[corner]];
            triangle[corner]={{p.x,p.y,p.z}};
            auto& source=sources[indices[corner]];
            if(source.faces.empty() || source.faces.back()!=face) source.faces.push_back(face);
            source.corners.push_back(face*3u+corner);
        }
        coincident[canonical(triangle)].push_back(face);
        const auto rotation=rotations[face];
        const auto a=positions[indices[rotation%3u]];
        const auto b=positions[indices[(rotation+1u)%3u]];
        const auto c=positions[indices[(rotation+2u)%3u]];
        if(precisePositions) {
            const std::array<std::array<double,3>,3> precise{{
                (*precisePositions)[indices[rotation%3u]],
                (*precisePositions)[indices[(rotation+1u)%3u]],
                (*precisePositions)[indices[(rotation+2u)%3u]]}};
            crosses.push_back(ascii::normalPositionFaceCross({{a,b,c}},precise));
        } else crosses.push_back(cross(subtract(b,a),subtract(c,b)));
    }
    std::vector<std::size_t> anchors;
    for(const auto& entry:coincident) {
        auto reversed=entry.first;
        std::swap(reversed[1],reversed[2]);
        const auto found=coincident.find(canonical(reversed));
        if(found!=coincident.end())
            for(const auto face:entry.second)
                if(std::any_of(found->second.begin(),found->second.end(),
                        [&](std::size_t other){return other!=face;})) anchors.push_back(face);
    }
    std::sort(anchors.begin(),anchors.end());
    if(anchors.empty()) return currentOrder;
    auto order=currentOrder;
    const auto initialRanks=SourceFaceNeighbors::ranks(order);
    // BuildNeighborLists keeps the first other source-index edge match whose
    // narrowed face index is nonnegative. Preserve the published neighbor's
    // identity as well as the triangles when changing accumulation order.
    const SourceFaceNeighbors neighbors(mesh,sourceFaces,order);
    const auto angle=[](Vec3 a,Vec3 b) {
        if(a.x==b.x && a.y==b.y && a.z==b.z) return 0.0;
        if((a.x==0.0f && a.y==0.0f && a.z==0.0f) ||
           (b.x==0.0f && b.y==0.0f && b.z==0.0f)) return std::acos(-1.0);
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    struct Error {std::size_t corner;double squared,angle;};
    const auto errors=[&](std::size_t source,const std::vector<std::size_t>& rank) {
        auto incident=sources[source].faces;
        std::sort(incident.begin(),incident.end(),[&](std::size_t a,std::size_t b){return rank[a]<rank[b];});
        std::map<std::uint32_t,Vec3> shared;
        std::vector<Error> result;
        for(const auto corner:sources[source].corners) {
            const auto face=corner/3u;
            const auto mask=masks[face];
            Vec3 normal;
            const auto found=shared.find(mask);
            if(mask && found!=shared.end()) normal=found->second;
            else {
                Vec3 sum{};
                for(const auto other:incident) if(other==face || (mask&masks[other])) {
                    const auto weighted=crosses[other];
                    sum={sum.x+weighted.x,sum.y+weighted.y,weighted.z+sum.z};
                }
                normal=normalizeSmoothing(sum);
                if(mask) shared.emplace(mask,normal);
            }
            const auto target=vertices[static_cast<std::size_t>(mesh.faces[face].indices[corner%3u])].normal;
            result.push_back({corner,difference(normal,target),precisePositions?angle(normal,target):0.0});
        }
        return result;
    };
    std::vector<double> current(sourceFaces.size()*3u);
    std::vector<double> baselineAngles(sourceFaces.size()*3u);
    for(std::size_t source=0;source<sources.size();++source)
        for(const auto& entry:errors(source,initialRanks)) {
            current[entry.corner]=entry.squared;baselineAngles[entry.corner]=entry.angle;
        }
    const auto baseline=current;
    for(;;) {
        const auto rank=SourceFaceNeighbors::ranks(order);
        auto bestOrder=order;
        std::vector<Error> bestErrors;
        double improvement{};
        const auto consider=[&](std::vector<std::size_t> candidate,std::vector<std::uint32_t> affected) {
            job.check();
            const auto candidateRanks=SourceFaceNeighbors::ranks(candidate);
            if(!neighbors.preserves(candidate,candidateRanks)) return;
            std::sort(affected.begin(),affected.end());
            affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            std::vector<Error> changed;
            double before{},after{};
            for(const auto source:affected)
                for(const auto& entry:errors(source,candidateRanks)) {
                    if(entry.squared>baseline[entry.corner] ||
                       (precisePositions && entry.angle>baselineAngles[entry.corner])) return;
                    before+=current[entry.corner];after+=entry.squared;
                    changed.push_back(entry);
                }
            if(before-after>improvement) {
                improvement=before-after;bestOrder=std::move(candidate);bestErrors=std::move(changed);
            }
        };
        for(std::size_t a=0;a<anchors.size();++a)
            for(std::size_t b=a+1u;b<anchors.size();++b) {
                auto candidate=order;
                std::swap(candidate[rank[anchors[a]]],candidate[rank[anchors[b]]]);
                const auto& first=sourceFaces[anchors[a]];
                const auto& second=sourceFaces[anchors[b]];
                consider(std::move(candidate),{first[0],first[1],first[2],second[0],second[1],second[2]});
            }
        for(const auto face:anchors)
            for(const auto destination:anchors) if(face!=destination) {
                auto candidate=order;
                candidate.erase(candidate.begin()+static_cast<std::ptrdiff_t>(rank[face]));
                candidate.insert(candidate.begin()+static_cast<std::ptrdiff_t>(rank[destination]),face);
                const auto& source=sourceFaces[face];
                consider(std::move(candidate),{source[0],source[1],source[2]});
            }
        if(!(improvement>0.0)) return order;
        order=std::move(bestOrder);
        for(const auto& entry:bestErrors) current[entry.corner]=entry.squared;
    }
}

ReconstructedNormalStars reconstructNormalCornerStars(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& faceOrder,
    const std::vector<Vec3>& positions,
    const neoshared::bif::JobControl& job,
    const std::vector<std::array<double,3>>* precisePositions,
    const std::vector<std::size_t>* movingFaces,
    const std::vector<Vec3>* baselineCornerNormals) {
    using Triangle=std::array<std::array<float,3>,3>;
    const auto canonical=[](Triangle triangle) {
        auto result=triangle;
        for(std::size_t i=1u;i<3u;++i) {
            std::rotate(triangle.begin(),triangle.begin()+1,triangle.end());
            if(triangle<result) result=triangle;
        }
        return result;
    };
    const auto same=[](Vec3 a,Vec3 b) {return a.x==b.x && a.y==b.y && a.z==b.z;};
    std::vector<std::vector<std::size_t>> incident(sourceVertices.size());
    std::vector<std::array<Vec3,3>> alternatives(sourceFaces.size());
    std::map<Triangle,std::vector<std::size_t>> coincident;
    for(const auto face:faceOrder) {
        job.check();
        const auto& indices=sourceFaces[face];
        Triangle triangle;
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto p=positions[indices[corner]];
            triangle[corner]={{p.x,p.y,p.z}};
            auto& faces=incident[indices[corner]];
            if(faces.empty() || faces.back()!=face) faces.push_back(face);
            const auto a=positions[indices[corner]];
            const auto b=positions[indices[(corner+1u)%3u]];
            const auto c=positions[indices[(corner+2u)%3u]];
            if(precisePositions) {
                const std::array<std::array<double,3>,3> precise{{
                    (*precisePositions)[indices[corner]],
                    (*precisePositions)[indices[(corner+1u)%3u]],
                    (*precisePositions)[indices[(corner+2u)%3u]]}};
                alternatives[face][corner]=ascii::normalPositionFaceCross({{a,b,c}},precise);
            } else alternatives[face][corner]=cross(subtract(b,a),subtract(c,b));
        }
        coincident[canonical(triangle)].push_back(face);
    }
    std::vector<bool> anchors(sourceFaces.size());
    for(const auto& entry:coincident) {
        auto reversed=entry.first;
        std::swap(reversed[1],reversed[2]);
        const auto found=coincident.find(canonical(reversed));
        if(found!=coincident.end())
            for(const auto face:entry.second)
                anchors[face]=std::any_of(found->second.begin(),found->second.end(),
                    [&](std::size_t other){return other!=face;});
    }
    struct Group {
        std::vector<std::size_t> faces,corners;
    };
    std::vector<Group> groups;
    std::vector<std::vector<std::size_t>> faceGroups(sourceFaces.size());
    for(std::size_t source=0;source<incident.size();++source) {
        std::map<std::pair<std::uint32_t,std::size_t>,std::size_t> shared;
        for(const auto face:incident[source]) {
            const auto key=std::make_pair(masks[face],masks[face]?ownFace:face);
            auto inserted=shared.emplace(key,groups.size());
            if(inserted.second) {
                auto& group=groups.emplace_back();
                for(const auto other:incident[source])
                    if(other==face || (masks[face]&masks[other])) {
                        group.faces.push_back(other);
                        faceGroups[other].push_back(inserted.first->second);
                    }
            }
            auto& group=groups[inserted.first->second];
            for(std::size_t corner=0;corner<3u;++corner)
                if(sourceFaces[face][corner]==source) group.corners.push_back(face*3u+corner);
        }
    }
    auto current=rotations;
    auto currentOrder=faceOrder;
    const SourceFaceNeighbors neighbors(mesh,sourceFaces,faceOrder);
    std::vector<Vec3> crosses(sourceFaces.size());
    for(std::size_t face=0;face<crosses.size();++face) crosses[face]=alternatives[face][current[face]];
    const auto angle=[](Vec3 a,Vec3 b) {
        if(a.x==b.x && a.y==b.y && a.z==b.z) return 0.0;
        if((a.x==0.0f && a.y==0.0f && a.z==0.0f) ||
           (b.x==0.0f && b.y==0.0f && b.z==0.0f)) return std::acos(-1.0);
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    const bool compareAngles=precisePositions || baselineCornerNormals;
    struct Error {double squared,angle;};
    const auto groupNormal=[&](std::size_t index) {
        Vec3 sum{};
        for(const auto face:groups[index].faces) {
            const auto weighted=crosses[face];
            sum={sum.x+weighted.x,sum.y+weighted.y,weighted.z+sum.z};
        }
        return normalizeSmoothing(sum);
    };
    const auto errors=[&](std::size_t index) {
        const auto normal=groupNormal(index);
        std::vector<Error> result;
        for(const auto corner:groups[index].corners) {
            const auto target=vertices[static_cast<std::size_t>(
                mesh.faces[corner/3u].indices[corner%3u])].normal;
            result.push_back({difference(normal,target),compareAngles?angle(normal,target):0.0});
        }
        return result;
    };
    std::vector<std::vector<Error>> baseline;
    for(std::size_t group=0;group<groups.size();++group) {
        if(!baselineCornerNormals) baseline.push_back(errors(group));
        else {
            auto& limits=baseline.emplace_back();
            for(const auto corner:groups[group].corners) {
                const auto target=vertices[static_cast<std::size_t>(
                    mesh.faces[corner/3u].indices[corner%3u])].normal;
                const auto normal=(*baselineCornerNormals)[corner];
                limits.push_back({difference(normal,target),angle(normal,target)});
            }
        }
    }
    std::vector<std::vector<std::size_t>> explicitDomains;
    if(movingFaces) explicitDomains.push_back(*movingFaces);
    const auto& domains=movingFaces?explicitDomains:incident;
    const bool repairMode=movingFaces && baselineCornerNormals;
    bool improved;
    do {
        improved=false;
        for(const auto& star:domains) {
            std::vector<std::size_t> faces;
            std::vector<std::vector<std::uint8_t>> choices;
            for(const auto face:star) if(movingFaces || anchors[face]) faces.push_back(face);
            if(movingFaces) {
                // Complete the smallest remaining contributor group first,
                // so its immutable corner limits can prune the joint search.
                std::vector<std::size_t> ordered;
                while(!faces.empty()) {
                    std::map<std::size_t,std::size_t> remaining;
                    for(const auto face:faces)
                        for(const auto group:faceGroups[face]) ++remaining[group];
                    const auto next=std::min_element(remaining.begin(),remaining.end(),
                        [](const auto& a,const auto& b) {
                            return std::make_pair(a.second,a.first)<std::make_pair(b.second,b.first);
                        })->first;
                    for(auto face=faces.begin();face!=faces.end();) {
                        const auto& contributions=faceGroups[*face];
                        if(std::find(contributions.begin(),contributions.end(),next)!=contributions.end()) {
                            ordered.push_back(*face);face=faces.erase(face);
                        } else ++face;
                    }
                }
                faces=std::move(ordered);
            }
            for(const auto face:faces) {
                std::vector<std::uint8_t> options{current[face]};
                for(std::uint8_t rotation=0u;rotation<3u;++rotation)
                    if(std::none_of(options.begin(),options.end(),[&](std::uint8_t other) {
                            return same(alternatives[face][rotation],alternatives[face][other]);
                        })) options.push_back(rotation);
                choices.push_back(std::move(options));
            }
            if(faces.empty()) continue;
            // Several cyclic starts can be necessary together: opposite
            // triangle crosses cancel in ProcessVerts' ordered float sum.
            std::map<std::size_t,std::size_t> last;
            for(std::size_t i=0;i<faces.size();++i)
                for(const auto group:faceGroups[faces[i]]) last[group]=i;
            std::vector<std::vector<std::size_t>> completed(faces.size());
            for(const auto& entry:last) completed[entry.second].push_back(entry.first);
            double initialScore{};
            for(const auto& ready:completed)
                for(const auto group:ready)
                    for(const auto error:errors(group)) initialScore+=error.squared;
            auto candidate=current,best=current;
            auto bestOrder=currentOrder;
            double bestScore=initialScore;
            if(repairMode) {
                bestScore=0.0;
                for(const auto& ready:completed)
                    for(const auto group:ready)
                        for(const auto error:baseline[group]) bestScore+=error.squared;
            }
            bool foundRepair=false;
            std::size_t bestChanges{};
            std::size_t orderChanges{};
            const auto visit=[&](const auto& self,std::size_t depth,double score,
                                 std::size_t changes)->void {
                job.check();
                if(depth==faces.size()) {
                    if(score<bestScore || (score==bestScore &&
                            ((repairMode && !foundRepair) || changes<bestChanges))) {
                        bestScore=score;bestChanges=changes;best=candidate;foundRepair=true;
                    }
                    return;
                }
                const auto face=faces[depth];
                for(const auto rotation:choices[depth]) {
                    candidate[face]=rotation;crosses[face]=alternatives[face][rotation];
                    double next=score;bool accepted=true;
                    for(const auto group:completed[depth]) {
                        const auto normal=groupNormal(group);
                        const auto& corners=groups[group].corners;
                        for(std::size_t i=0;i<corners.size();++i) {
                            const auto corner=corners[i];
                            const auto target=vertices[static_cast<std::size_t>(
                                mesh.faces[corner/3u].indices[corner%3u])].normal;
                            const auto squared=difference(normal,target);
                            if(squared>baseline[group][i].squared ||
                               (compareAngles && angle(normal,target)>baseline[group][i].angle)) {
                                accepted=false;break;
                            }
                            next+=squared;
                        }
                        if(!accepted) break;
                    }
                    const auto nextChanges=changes+(rotation!=current[face]);
                    if(accepted && (next<bestScore ||
                            (next==bestScore && ((repairMode && !foundRepair) || nextChanges<bestChanges))))
                        self(self,depth+1u,next,nextChanges);
                }
                candidate[face]=current[face];crosses[face]=alternatives[face][current[face]];
            };
            std::map<std::vector<std::size_t>,std::size_t> orderSignatures;
            const auto considerOrder=[&](const std::vector<std::size_t>& order) {
                const auto ranks=SourceFaceNeighbors::ranks(order);
                if(!neighbors.preserves(order,ranks)) return;
                for(const auto& entry:last) {
                    auto& contributions=groups[entry.first].faces;
                    std::sort(contributions.begin(),contributions.end(),
                        [&](std::size_t a,std::size_t b){return ranks[a]<ranks[b];});
                }
                if(movingFaces) {
                    std::vector<std::size_t> signature;
                    for(const auto& entry:last) {
                        const auto& contributions=groups[entry.first].faces;
                        signature.insert(signature.end(),contributions.begin(),contributions.end());
                    }
                    const auto found=orderSignatures.emplace(std::move(signature),orderChanges);
                    if(!found.second) {
                        if(found.first->second<=orderChanges) return;
                        found.first->second=orderChanges;
                    }
                }
                const auto previousScore=bestScore;
                const auto previousChanges=bestChanges;
                const auto previouslyFound=foundRepair;
                visit(visit,0u,0.0,orderChanges);
                if((!previouslyFound && foundRepair) || bestScore<previousScore ||
                   (bestScore==previousScore && bestChanges<previousChanges)) bestOrder=order;
            };
            considerOrder(currentOrder);
            // Moving zero contributions cannot alter the sum. Likewise, a
            // group containing only one exact summand value has no order
            // freedom to recover; avoid enumerating equivalent permutations.
            const auto movingZero=std::all_of(faces.begin(),faces.end(),[&](std::size_t face) {
                return std::all_of(alternatives[face].begin(),alternatives[face].end(),
                    [&](Vec3 value){return same(value,Vec3{});});
            });
            bool orderSensitive=false;
            if(!movingZero) for(const auto& entry:last) {
                const auto& contributions=groups[entry.first].faces;
                const auto first=crosses[contributions.front()];
                for(const auto face:contributions) {
                    const auto found=std::find(faces.begin(),faces.end(),face);
                    if(found==faces.end()) orderSensitive=orderSensitive || !same(crosses[face],first);
                    else for(const auto rotation:choices[static_cast<std::size_t>(found-faces.begin())])
                        orderSensitive=orderSensitive || !same(alternatives[face][rotation],first);
                }
            }
            if(orderSensitive) neighbors.permutations(faces,currentOrder,job,
                [&](const std::vector<std::size_t>& order,std::size_t changes) {
                    if(changes==0u) return;
                    orderChanges=changes;considerOrder(order);
                });
            if(bestScore<initialScore || (repairMode && foundRepair)) {
                current=std::move(best);currentOrder=std::move(bestOrder);improved=true;
                for(const auto face:faces) crosses[face]=alternatives[face][current[face]];
            }
            const auto ranks=SourceFaceNeighbors::ranks(currentOrder);
            for(const auto& entry:last) {
                auto& contributions=groups[entry.first].faces;
                std::sort(contributions.begin(),contributions.end(),
                    [&](std::size_t a,std::size_t b){return ranks[a]<ranks[b];});
            }
        }
    } while(improved && !movingFaces);
    return {std::move(current),std::move(currentOrder)};
}

ReconstructedNormalRounding reconstructNormalRounding(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& faceOrder,
    const std::vector<Vec3>& currentPositions,
    const neoshared::bif::JobControl& job) {
    using Point=std::array<float,3>;
    using Triangle=std::array<Point,3>;
    const auto canonical=[](Triangle triangle) {
        auto result=triangle;
        for(std::size_t i=1u;i<3u;++i) {
            std::rotate(triangle.begin(),triangle.begin()+1,triangle.end());
            if(triangle<result) result=triangle;
        }
        return result;
    };
    const auto same=[](Vec3 a,Vec3 b) {return a.x==b.x && a.y==b.y && a.z==b.z;};
    const auto component=[](Vec3 p,std::size_t axis) {return axis==0u?p.x:axis==1u?p.y:p.z;};
    const auto setComponent=[](Vec3& p,std::size_t axis,float value) {
        if(axis==0u) p.x=value;else if(axis==1u) p.y=value;else p.z=value;
    };
    std::vector<std::vector<std::size_t>> incident(sourceVertices.size());
    std::map<Triangle,std::vector<std::size_t>> triangles;
    std::map<Point,std::vector<std::size_t>> coincident;
    for(std::size_t source=0;source<sourceVertices.size();++source) {
        const auto p=vertices[sourceVertices[source]].position;
        coincident[{{p.x,p.y,p.z}}].push_back(source);
    }
    for(const auto face:faceOrder) {
        Triangle triangle;
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto source=sourceFaces[face][corner];
            const auto p=vertices[sourceVertices[source]].position;
            triangle[corner]={{p.x,p.y,p.z}};
            auto& faces=incident[source];
            if(faces.empty() || faces.back()!=face) faces.push_back(face);
        }
        triangles[canonical(triangle)].push_back(face);
    }
    std::vector<bool> anchors(sourceFaces.size());
    for(const auto& entry:triangles) {
        auto reversed=entry.first;std::swap(reversed[1],reversed[2]);
        const auto found=triangles.find(canonical(reversed));
        if(found!=triangles.end())
            for(const auto face:entry.second)
                anchors[face]=std::any_of(found->second.begin(),found->second.end(),
                    [&](std::size_t other){return other!=face;});
    }
    struct Group {std::vector<std::size_t> faces,corners;};
    std::vector<Group> groups;
    std::vector<std::vector<std::size_t>> faceGroups(sourceFaces.size());
    for(std::size_t source=0;source<incident.size();++source) {
        std::map<std::pair<std::uint32_t,std::size_t>,std::size_t> shared;
        for(const auto face:incident[source]) {
            const auto inserted=shared.emplace(std::make_pair(masks[face],masks[face]?ownFace:face),groups.size());
            if(inserted.second) {
                auto& group=groups.emplace_back();
                for(const auto other:incident[source])
                    if(other==face || (masks[face]&masks[other])) {
                        group.faces.push_back(other);
                        faceGroups[other].push_back(inserted.first->second);
                    }
            }
            auto& group=groups[inserted.first->second];
            for(std::size_t corner=0;corner<3u;++corner)
                if(sourceFaces[face][corner]==source) group.corners.push_back(face*3u+corner);
        }
    }
    using Crosses=std::vector<std::array<Vec3,3>>;
    const auto updateCross=[&](Crosses& crosses,std::size_t face,const std::vector<Vec3>& positions) {
        const auto& indices=sourceFaces[face];
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto a=positions[indices[corner]],b=positions[indices[(corner+1u)%3u]],
                c=positions[indices[(corner+2u)%3u]];
            crosses[face][corner]=cross(subtract(b,a),subtract(c,b));
        }
    };
    const auto normal=[&](std::size_t group,const Crosses& crosses,const std::vector<std::uint8_t>& starts) {
        Vec3 sum{};
        for(const auto face:groups[group].faces) {
            const auto weighted=crosses[face][starts[face]];
            sum={sum.x+weighted.x,sum.y+weighted.y,weighted.z+sum.z};
        }
        return normalizeSmoothing(sum);
    };
    const auto angle=[](Vec3 a,Vec3 b) {
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    ReconstructedNormalRounding current{currentPositions,rotations,faceOrder};
    for(;;) {
        Crosses crosses(sourceFaces.size());
        for(std::size_t face=0;face<sourceFaces.size();++face) updateCross(crosses,face,current.positions);
        std::vector<Vec3> baseline;
        for(std::size_t group=0;group<groups.size();++group) baseline.push_back(normal(group,crosses,current.rotations));
        auto best=current;
        double bestImprovement{};
        std::size_t bestChanges{};
        for(const auto& points:coincident) {
            job.check();
            std::set<std::size_t> changedFaces,stars,positionGroups;
            for(const auto source:points.second)
                changedFaces.insert(incident[source].begin(),incident[source].end());
            if(std::none_of(changedFaces.begin(),changedFaces.end(),[&](std::size_t face){return anchors[face];})) continue;
            for(const auto face:changedFaces) {
                stars.insert(sourceFaces[face].begin(),sourceFaces[face].end());
                positionGroups.insert(faceGroups[face].begin(),faceGroups[face].end());
            }
            for(std::size_t axis=0;axis<3u;++axis) {
                // Each domain remains centered on the retained source word;
                // repeated refinement cannot walk farther from the geometry.
                const float original=points.first[axis];
                const std::array<float,3> domain{{original,
                    std::nextafter(original,-std::numeric_limits<float>::infinity()),
                    std::nextafter(original,std::numeric_limits<float>::infinity())}};
                for(const auto value:domain) {
                    if(std::all_of(points.second.begin(),points.second.end(),[&](std::size_t source) {
                            return component(current.positions[source],axis)==value;
                        })) continue;
                    auto positions=current.positions;
                    for(const auto source:points.second) setComponent(positions[source],axis,value);
                    auto trialCrosses=crosses;
                    for(const auto face:changedFaces) updateCross(trialCrosses,face,positions);
                    if(std::all_of(changedFaces.begin(),changedFaces.end(),[&](std::size_t face) {
                            for(std::size_t start=0;start<3u;++start)
                                if(!same(trialCrosses[face][start],crosses[face][start])) return false;
                            return true;
                        })) continue;
                    for(const auto star:stars) {
                        std::vector<std::size_t> faces;
                        std::vector<std::vector<std::uint8_t>> choices;
                        for(const auto face:incident[star]) if(anchors[face]) {
                            std::vector<std::uint8_t> options{current.rotations[face]};
                            for(std::uint8_t start=0u;start<3u;++start)
                                if(std::none_of(options.begin(),options.end(),[&](std::uint8_t other) {
                                        return same(trialCrosses[face][start],trialCrosses[face][other]);
                                    })) options.push_back(start);
                            if(options.size()>1u) {faces.push_back(face);choices.push_back(std::move(options));}
                        }
                        std::map<std::size_t,std::size_t> last;
                        for(std::size_t i=0;i<faces.size();++i)
                            for(const auto group:faceGroups[faces[i]]) last[group]=i;
                        std::vector<std::vector<std::size_t>> completed(faces.size());
                        for(const auto& entry:last) completed[entry.second].push_back(entry.first);
                        auto candidate=current.rotations;
                        const auto gain=[&](std::size_t group,double& improvement) {
                            const auto value=normal(group,trialCrosses,candidate),before=baseline[group];
                            for(const auto corner:groups[group].corners) {
                                const auto target=vertices[static_cast<std::size_t>(
                                    mesh.faces[corner/3u].indices[corner%3u])].normal;
                                const auto oldError=difference(before,target),newError=difference(value,target);
                                if(newError>oldError) return false;
                                if(!same(before,Vec3{}) && !same(value,Vec3{}) && !same(target,Vec3{}) &&
                                   angle(value,target)>angle(before,target)) return false;
                                improvement+=oldError-newError;
                            }
                            return true;
                        };
                        double fixedGain{};bool accepted=true;
                        for(const auto group:positionGroups) if(last.find(group)==last.end())
                            if(!gain(group,fixedGain)) {accepted=false;break;}
                        if(!accepted) continue;
                        const auto visit=[&](const auto& self,std::size_t depth,double improvement,
                                             std::size_t changes)->void {
                            job.check();
                            if(depth==faces.size()) {
                                if(improvement>bestImprovement ||
                                   (improvement==bestImprovement && changes<bestChanges)) {
                                    bestImprovement=improvement;bestChanges=changes;
                                    best={positions,candidate,current.faceOrder};
                                }
                                return;
                            }
                            const auto face=faces[depth];
                            for(const auto start:choices[depth]) {
                                candidate[face]=start;
                                double next=improvement;bool valid=true;
                                for(const auto group:completed[depth])
                                    if(!gain(group,next)) {valid=false;break;}
                                if(valid) self(self,depth+1u,next,changes+(start!=current.rotations[face]));
                            }
                            candidate[face]=current.rotations[face];
                        };
                        visit(visit,0u,fixedGain,points.second.size());
                    }
                }
            }
        }
        if(!(bestImprovement>0.0)) return current;
        current=std::move(best);
        for(std::size_t face=0;face<sourceFaces.size();++face) updateCross(crosses,face,current.positions);
        std::vector<Vec3> acceptedNormals;
        for(std::size_t group=0;group<groups.size();++group)
            acceptedNormals.push_back(normal(group,crosses,current.rotations));
        // Let source order resolve cancellation before moving another point.
        auto order=reconstructNormalFaceOrder(mesh,vertices,sourceVertices,sourceFaces,masks,
            current.rotations,current.faceOrder,current.positions,job);
        auto stars=reconstructNormalCornerStars(mesh,vertices,sourceVertices,sourceFaces,masks,
            current.rotations,order,current.positions,job);
        const auto arrange=[&](const std::vector<std::size_t>& order) {
            const auto ranks=SourceFaceNeighbors::ranks(order);
            for(auto& group:groups)
                std::sort(group.faces.begin(),group.faces.end(),
                    [&](std::size_t a,std::size_t b){return ranks[a]<ranks[b];});
        };
        arrange(stars.faceOrder);
        bool retained=true;
        for(std::size_t group=0;group<groups.size() && retained;++group) {
            const auto before=acceptedNormals[group],after=normal(group,crosses,stars.rotations);
            for(const auto corner:groups[group].corners) {
                const auto target=vertices[static_cast<std::size_t>(
                    mesh.faces[corner/3u].indices[corner%3u])].normal;
                if(!same(before,Vec3{}) && !same(after,Vec3{}) && !same(target,Vec3{}) &&
                   angle(after,target)>angle(before,target)) {retained=false;break;}
            }
        }
        if(retained) {
            current.rotations=std::move(stars.rotations);
            current.faceOrder=std::move(stars.faceOrder);
        } else arrange(current.faceOrder);
    }
}

ReconstructedSmoothing reconstructSmoothing(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const neoshared::bif::JobControl& job,
    const std::vector<Vec3>* cornerTargets,bool refine,
    const std::vector<bool>* ignoredCorners,
    const std::vector<std::uint32_t>* seedMasks,bool useStoredRows,
    const TangentSmoothingSource* tangents,
    const std::vector<std::uint8_t>* cornerRotations,
    std::vector<std::vector<std::uint32_t>>* sharedCandidates) {
    Smoothing work{{},{},{},{},job,{}, {}};
    std::vector<ascii::TangentFace> tangentFaces;
    if(tangents) tangentFaces.reserve(sourceFaces.size());
    work.faceRelations.resize(sourceFaces.size());
    std::vector<std::vector<std::size_t>> incident(sourceVertices.size());
    std::map<std::array<float,3>,std::vector<std::size_t>> positionFaces;
    for(std::size_t face=0;face<sourceFaces.size();++face) {
        job.check();
        const auto& indices=sourceFaces[face];
        const auto rotation=cornerRotations?(*cornerRotations)[face]:0u;
        const auto a=vertices[sourceVertices[indices[rotation%3u]]].position;
        const auto b=vertices[sourceVertices[indices[(1u+rotation)%3u]]].position;
        const auto c=vertices[sourceVertices[indices[(2u+rotation)%3u]]].position;
        work.crosses.push_back(cross(subtract(b,a),subtract(c,b)));
        for(const auto source:indices) {
            auto& faces=incident[source];
            if(faces.empty() || faces.back()!=face) faces.push_back(face);
            if(tangents) {
                const auto p=vertices[sourceVertices[source]].position;
                positionFaces[{{p.x,p.y,p.z}}].push_back(face);
            }
        }
        if(tangents) {
            const auto& va=vertices[mesh.faces[face].indices[rotation%3u]];
            const auto& vb=vertices[mesh.faces[face].indices[(1u+rotation)%3u]];
            const auto& vc=vertices[mesh.faces[face].indices[(2u+rotation)%3u]];
            const auto p=subtract(b,a),q=subtract(c,a);
            const float pu=vb.texcoord0.x-va.texcoord0.x,qu=vc.texcoord0.x-va.texcoord0.x;
            const float pv=vb.texcoord0.y-va.texcoord0.y,qv=vc.texcoord0.y-va.texcoord0.y;
            TangentBasis basis;
            basis.bitangent=normalizeTangent({q.x*pu-p.x*qu,q.y*pu-p.y*qu,q.z*pu-p.z*qu});
            basis.tangent=normalizeTangent({q.x*pv-p.x*qv,q.y*pv-p.y*qv,q.z*pv-p.z*qv});
            basis.normal=ascii::tangentBasisNormal(basis.bitangent,basis.tangent,work.crosses.back());
            const auto n=tangents->firstCornerNormals[face];
            if((basis.normal.x*n.x+basis.normal.y*n.y)+basis.normal.z*n.z<0.0f) {
                basis.tangent={-basis.tangent.x,-basis.tangent.y,-basis.tangent.z};
                basis.normal={-basis.normal.x,-basis.normal.y,-basis.normal.z};
            }
            work.faceBases.push_back(basis);
            tangentFaces.push_back({{a,b,c},{va.texcoord0,vb.texcoord0,vc.texcoord0}});
        }
    }
    if(tangents) ascii::orientSingularTangentComponents(tangentFaces,work.faceBases,job);
    std::map<std::pair<std::size_t,std::size_t>,std::size_t> relationIndices;
    for(const auto& faces:incident) {
        job.check();
        for(std::size_t a=0;a<faces.size();++a)
            for(std::size_t b=a+1u;b<faces.size();++b)
                relationIndices.emplace(std::make_pair(faces[a],faces[b]),0u);
    }
    for(const auto& entry:positionFaces)
        for(const auto a:entry.second)
            for(const auto b:entry.second)
                if(a<b) relationIndices.emplace(std::make_pair(a,b),0u);
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
            Corner value{target,sourceFaces[face][corner],
                static_cast<std::size_t>(mesh.faces[face].indices[corner]),{},
                !ignoredCorners || !(*ignoredCorners)[face*3u+corner],{}, {},0u};
            for(const auto candidate:incident[sourceFaces[face][corner]]) {
                const auto relation=candidate==face?ownFace:
                    relationIndices.at(std::minmax(face,candidate));
                value.contributions.push_back({candidate,relation});
                if(relation!=ownFace) work.relations[relation].corners.push_back(index);
            }
            if(tangents) {
                value.basisTarget=vertices[value.compiledVertex].tangentBasis;
                const auto p=vertices[sourceVertices[value.source]].position;
                for(const auto candidate:positionFaces.at({{p.x,p.y,p.z}})) {
                    const auto relation=candidate==face?ownFace:relationIndices.at(std::minmax(face,candidate));
                    value.basisContributions.push_back({candidate,relation});
                }
            }
            work.corners.push_back(std::move(value));
        }
    if(tangents) {
        // The last face using a generated row overwrites its basis. Every
        // corner reading that row therefore depends on the writer's joins.
        std::map<std::int16_t,std::size_t> writers;
        for(std::size_t face=0;face<tangents->cornerRows.size();++face)
            for(std::size_t corner=0;corner<3u;++corner)
                writers[tangents->cornerRows[face][corner]]=face*3u+corner;
        for(auto& relation:work.relations) relation.corners.clear();
        for(std::size_t corner=0;corner<work.corners.size();++corner) {
            const auto writer=tangents->useGeneratedWriters?
                writers.at(tangents->cornerRows[corner/3u][corner%3u]):corner;
            work.corners[corner].basisWriter=writer;
            for(const auto& contribution:work.corners[writer].basisContributions)
                if(contribution.relation!=ownFace)
                    work.relations[contribution.relation].corners.push_back(corner);
        }
        for(auto& relation:work.relations) {
            std::sort(relation.corners.begin(),relation.corners.end());
            relation.corners.erase(std::unique(relation.corners.begin(),relation.corners.end()),relation.corners.end());
        }
    }
    ReconstructedSmoothing result;
    if(seedMasks) result.masks=*seedMasks;
    else result.masks.assign(sourceFaces.size(),0u);
    auto joined=work.intersections(result.masks);
    if(useStoredRows) {
        const auto retained=work.normalGroups(true).first;
        for(std::size_t i=0;i<work.relations.size();++i)
            if(std::all_of(work.relations[i].corners.begin(),work.relations[i].corners.end(),
                           [&](std::size_t corner) { return !work.corners[corner].scored; }))
                joined[i]=retained[i];
        result.masks=work.encode(joined);
        joined=work.intersections(result.masks);
    }
    double best=work.score(joined);
    std::set<std::vector<std::uint8_t>> evaluated;
    const auto consider=[&](const std::vector<std::uint8_t>& relations) {
        if(!evaluated.insert(relations).second) return;
        auto masks=work.encode(relations);
        auto candidate=work.intersections(masks);
        const auto score=work.score(candidate);
        if(score<best) {
            best=score;joined=std::move(candidate);result.masks=std::move(masks);
        }
    };
    if(!seedMasks && !useStoredRows) {
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
        if(!tangents) {
            const auto contributions=work.contributionGroups();
            if(contributions.first!=groups.first && contributions.first!=groups.second)
                considerGroup(contributions.first);
            if(contributions.second!=contributions.first && contributions.second!=groups.first &&
               contributions.second!=groups.second) considerGroup(contributions.second);
        }
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
    }
    // Two intersecting relations can improve together even when either
    // change alone increases error. Refit the encoded intersections until
    // the actual representable result stops improving.
    while(refine) {
        const auto previous=best;
        consider(work.fit(joined,false));
        consider(work.fit(joined,true));
        consider(work.fitPairs(joined));
        consider(work.fitSharedContributions(joined));
        if(!tangents) {
            consider(work.fitNormalGroups(joined));
            consider(work.fitSharedVertices(joined));
            consider(work.fitSharedVertices(joined,true));
            consider(work.fitSharedVertices(joined,true,true));
        }
        if(!(best<previous)) break;
    }
    if(sharedCandidates) {
        std::vector<std::vector<std::uint8_t>> relationCandidates;
        work.fitSharedVertices(joined,true,false,&relationCandidates);
        std::set<std::vector<std::uint32_t>> unique;
        for(const auto& candidate:relationCandidates) {
            auto masks=work.encode(candidate);
            if(masks!=result.masks) unique.insert(std::move(masks));
        }
        sharedCandidates->assign(unique.begin(),unique.end());
    }
    result.squaredNormalError=tangents?0.0:best;
    for(std::size_t corner=0;corner<work.corners.size();++corner) {
        const auto actual=work.normal(corner,joined),target=work.corners[corner].target;
        if(tangents) result.squaredNormalError+=difference(actual,target);
        const float delta=std::max({std::abs(actual.x-target.x),std::abs(actual.y-target.y),std::abs(actual.z-target.z)});
        result.changedCornerNormals+=delta!=0.0f;
        result.maxNormalComponentDifference=std::max(result.maxNormalComponentDifference,delta);
    }
    return result;
}

std::vector<std::vector<std::uint32_t>> sharedSeamSmoothingCandidates(
    const Mesh& mesh,const std::vector<Vertex>& vertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,const std::vector<bool>& seamCorners,
    const neoshared::bif::JobControl& job,bool differentNormals,bool missingComponents) {
    struct Group { Vec3 normal;std::vector<std::size_t> faces;bool seam{}; };
    std::map<std::size_t,std::vector<Group>> groups;
    for(std::size_t face=0;face<sourceFaces.size();++face) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto normal=vertices[mesh.faces[face].indices[corner]].normal;
            auto& source=groups[sourceFaces[face][corner]];
            auto found=std::find_if(source.begin(),source.end(),[&](const Group& group) {
                return group.normal.x==normal.x && group.normal.y==normal.y && group.normal.z==normal.z;
            });
            if(found==source.end()) {
                source.push_back({normal,{},false});found=source.end()-1;
            }
            if(found->faces.empty() || found->faces.back()!=face) found->faces.push_back(face);
            found->seam=found->seam || seamCorners[face*3u+corner];
        }
    }
    std::uint32_t bits{};
    for(const auto mask:masks) bits|=mask;
    for(std::uint32_t bit=1u;bit;bit<<=1u)
        if(!(bits&bit)) { bits|=bit;break; }
    std::set<std::vector<std::uint32_t>> proposals;
    const auto propose=[&](const std::vector<std::size_t>& faces) {
        for(std::uint32_t bit=1u;bit;bit<<=1u) if(bits&bit) {
            job.check();
            auto candidate=masks;
            for(const auto face:faces) candidate[face]|=bit;
            if(candidate!=masks) proposals.insert(std::move(candidate));
        }
    };
    for(const auto& source:groups) {
        if(!differentNormals) {
            for(const auto& group:source.second) if(group.seam) {
                bool missing=false;
                std::vector<std::vector<std::size_t>> unjoined(missingComponents?group.faces.size():0u);
                for(std::size_t a=0;a<group.faces.size();++a)
                    for(std::size_t b=a+1u;b<group.faces.size();++b)
                        if(!(masks[group.faces[a]]&masks[group.faces[b]])) {
                            missing=true;propose({group.faces[a],group.faces[b]});
                            if(missingComponents) {
                                unjoined[a].push_back(b);unjoined[b].push_back(a);
                            }
                        }
                if(missing) {
                    propose(group.faces);
                    if(!missingComponents) continue;
                    std::vector<bool> visited(group.faces.size());
                    for(std::size_t seed=0;seed<group.faces.size();++seed) {
                        if(visited[seed]) continue;
                        std::vector<std::size_t> pending{seed},component;
                        visited[seed]=true;
                        for(std::size_t i=0;i<pending.size();++i) {
                            const auto current=pending[i];component.push_back(group.faces[current]);
                            for(const auto next:unjoined[current]) if(!visited[next]) {
                                visited[next]=true;pending.push_back(next);
                            }
                        }
                        if(component.size()>1u) propose(component);
                    }
                }
            }
            continue;
        }
        // Ordered seams can leave different final normals on corners that
        // shared smoothing before the seam pass.
        if(!std::any_of(source.second.begin(),source.second.end(),
            [](const Group& group){return group.seam;})) continue;
        for(std::size_t a=0;a<source.second.size();++a)
            for(std::size_t b=a+1u;b<source.second.size();++b)
                for(const auto first:source.second[a].faces)
                    for(const auto second:source.second[b].faces)
                        if(first!=second && !(masks[first]&masks[second])) propose({first,second});
    }
    return {proposals.begin(),proposals.end()};
}

} // namespace neomdl::model::ascii_export
