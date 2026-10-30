#include "core/RoomEditing.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace neomdl {
std::vector<std::uint8_t> moveLayoutRoom(const std::vector<std::uint8_t>& bytes,std::size_t room,model::Vec3 position) {
    std::string text(bytes.begin(),bytes.end());std::size_t remaining=0;
    std::vector<std::pair<std::size_t,std::size_t>> records;
    for(std::size_t start=0;start<text.size();) {
        const auto newline=text.find('\n',start);const auto end=newline==std::string::npos?text.size():newline+1;
        std::istringstream input(text.substr(start,end-start));std::string word;input>>word;
        std::transform(word.begin(),word.end(),word.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(word=="roomcount")input>>remaining;
        else if(remaining&&!word.empty()) {records.emplace_back(start,end-start);--remaining;}
        start=end;
    }
    const auto record=records.at(room);auto line=text.substr(record.first,record.second);
    std::vector<std::pair<std::size_t,std::size_t>> tokens;
    for(std::size_t at=0;at<line.size();) {
        if(std::isspace(static_cast<unsigned char>(line[at]))) {++at;continue;}
        const auto first=at;while(at<line.size()&&!std::isspace(static_cast<unsigned char>(line[at])))++at;
        tokens.emplace_back(first,at-first);
    }
    const float values[]={position.x,position.y,position.z};
    for(int axis=2;axis>=0;--axis) {
        std::ostringstream value;value.imbue(std::locale::classic());value<<std::setprecision(std::numeric_limits<float>::max_digits10)<<values[axis];
        const auto span=tokens.at(std::size_t(axis+1));line.replace(span.first,span.second,value.str());
    }
    text.replace(record.first,record.second,line);return {text.begin(),text.end()};
}

void translateRoomWalkmesh(neoshared::bwm::BwmFile& mesh,model::Vec3 offset) {
    const float delta[]={offset.x,offset.y,offset.z};
    for(auto& vertex:mesh.vertices)for(std::size_t axis=0;axis<3;++axis)vertex[axis]+=delta[axis];
    for(auto& node:mesh.nodes)for(std::size_t axis=0;axis<3;++axis) {node.minimum[axis]+=delta[axis];node.maximum[axis]+=delta[axis];}
    for(std::size_t i=0;i<mesh.faceDistances.size();++i) {
        const auto& normal=mesh.faceNormals[i];mesh.faceDistances[i]-=(normal[0]*offset.x+normal[1]*offset.y)+normal[2]*offset.z;
    }
}
}
