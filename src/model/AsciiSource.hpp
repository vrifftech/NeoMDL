#pragma once

#include <model/AsciiModelInternal.hpp>
#include <model/AsciiControllerFields.hpp>

#include <algorithm>
#include <map>
#include <sstream>

namespace neomdl::model::ascii {

using Edits = std::map<std::string, std::optional<std::string>>;

// Focused edits retain every untouched line and go through the normal compiler.
// These are source ranges, not a second model representation.
struct SourceText {
    struct Block { std::size_t begin, end; std::string name; bool geometry; std::string animation; };
    struct Texture { std::size_t line, column, length; std::string name; bool policy; };
    std::vector<std::string> lines;

    explicit SourceText(const std::string& text) {
        std::istringstream input(text);
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
    }
    static std::string key(const std::string& line) {
        std::istringstream input(line);
        std::string result;
        input >> result;
        return lower(result);
    }
    static std::string value(const std::string& line) {
        const auto begin = line.find_first_not_of(" \t");
        const auto wordEnd = line.find_first_of(" \t\r", begin);
        const auto start = line.find_first_not_of(" \t\r", wordEnd);
        return start == std::string::npos ? std::string{} : line.substr(start);
    }
    std::size_t nextRecord(std::size_t line) const {
        const auto name = key(lines[line]);
        if (name == "node") {
            for (++line; key(lines[line]).compare(0, 7, "endnode") != 0; line = nextRecord(line)) {}
            return line + 1;
        }
        if (name == "vertexindices") {
            std::size_t count;
            std::istringstream(value(lines[line])) >> count;
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t rows;
                std::istringstream(lines[++line]) >> rows;
                line += rows;
            }
            return line + 1;
        }
        if (name == "aabb") {
            std::size_t pending = 1;
            bool first = true;
            while (pending) {
                std::istringstream row(first ? value(lines[line]) : lines[line]);
                float coordinate;
                for (int i = 0; i < 6; ++i) row >> coordinate;
                int face; row >> face;
                --pending;
                if (face == -1) pending += 2;
                ++line; first = false;
            }
            return line;
        }
        static const std::vector<std::string> arrays{
            "verts", "normals", "normalindices", "tverts", "tverts1", "tverts2", "tverts3",
            "texindices1", "texindices2", "texindices3", "colors", "colorsrgba", "colorindices",
            "tangentbasis", "tangentindices", "cpuverts", "cpuindices", "faceplanes", "faces",
            "vertexindicescount", "leftoverfaces", "constraints", "animverts", "animtverts",
            "tangents", "mirrorlist", "texturenames", "flaresizes", "flarepositions", "flarecolorshifts",
            "weights", "qbone_ref_inv", "tbone_ref_inv", "boneconstantindices", "skinbonemap",
            "skinpalette", "skinbindrotations", "skinbindtranslations", "skinweights", "controllerdata"};
        const auto keyword = std::string_view(name);
        const auto suffix = name.size() >= 9 && name.compare(name.size() - 9, 9, "bezierkey") == 0 ? 9u :
            name.size() >= 3 && name.compare(name.size() - 3, 3, "key") == 0 ? 3u : 0u;
        const bool controllerKeys = suffix && std::any_of(std::begin(controllerFields),
            std::end(controllerFields), [&](const auto& field) {
                return keyword.substr(0, keyword.size() - suffix) == field.name;
            });
        if (controllerKeys || name == "orientationpackedkey" || name == "orientationquatkey" ||
            std::find(arrays.begin(), arrays.end(), name) != arrays.end()) {
            std::size_t count;
            if (std::istringstream(value(lines[line])) >> count) return line + count + 1;
            while (key(lines[++line]).compare(0, 7, "endlist") != 0) {}
        }
        return line + 1;
    }
    std::vector<Block> nodes() const {
        std::vector<Block> result;
        bool geometry = false;
        std::string animation;
        for (std::size_t i = 0; i < lines.size(); i = nextRecord(i)) {
            const auto field = key(lines[i]);
            if (field == "newanim") std::istringstream(value(lines[i])) >> animation;
            else if (field == "beginmodelgeom") geometry = true;
            else if (field == "endmodelgeom") geometry = false;
            else if (field == "node") {
                std::string type, name;
                std::istringstream(value(lines[i])) >> type >> name;
                result.push_back({i, nextRecord(i) - 1, name, geometry, geometry ? std::string{} : animation});
            }
        }
        return result;
    }
    std::string field(const Block& block, const std::string& name) const {
        std::string result;
        for (auto i = block.begin + 1; i < block.end; i = nextRecord(i))
            if (key(lines[i]) == name) result = value(lines[i]);
        return result;
    }
    void edit(const Block& block, Edits edits) {
        std::vector<std::string> replacement;
        for (auto i = block.begin + 1; i < block.end; i = nextRecord(i)) {
            const auto name = key(lines[i]);
            if (!edits.count(name)) {
                replacement.insert(replacement.end(), lines.begin() + static_cast<std::ptrdiff_t>(i),
                    lines.begin() + static_cast<std::ptrdiff_t>(nextRecord(i)));
            }
        }
        for (const auto& [name, contents] : edits)
            if (contents) replacement.push_back("  " + name + " " + *contents);
        const auto start = lines.begin() + static_cast<std::ptrdiff_t>(block.begin + 1);
        lines.erase(start, lines.begin() + static_cast<std::ptrdiff_t>(block.end));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(block.begin + 1),
                     replacement.begin(), replacement.end());
    }
    void editNode(const std::string& name, const Edits& edits) {
        const auto blocks = nodes();
        const auto found = std::find_if(blocks.begin(), blocks.end(), [&](const auto& block) {
            return block.geometry && equal(block.name, name);
        });
        edit(*found, edits);
    }
    void materializeTangentPolicies() {
        std::map<std::string, std::string> policies;
        std::vector<std::pair<Block, std::string>> targets;
        std::size_t line = 0;
        for (const auto& block : nodes()) {
            for (; line < block.begin; line = nextRecord(line)) {
                if (key(lines[line]) == "newmodel") policies.clear();
                if (key(lines[line]) == "texturetangents") {
                    std::string name, enabled;
                    std::istringstream(value(lines[line])) >> name >> enabled;
                    policies[lower(name)] = enabled;
                }
            }
            std::string texture;
            for (auto i = block.begin + 1; i < block.end; i = nextRecord(i))
                if (key(lines[i]) == "texture0" || key(lines[i]) == "bitmap")
                    std::istringstream(value(lines[i])) >> texture;
            const auto policy = policies.find(lower(texture));
            if (policy != policies.end()) targets.emplace_back(block, policy->second);
            line = block.end + 1;
        }
        for (auto target = targets.rbegin(); target != targets.rend(); ++target)
            edit(target->first, {{"hastangentbasis", target->second}});
        for (std::size_t i = 0; i < lines.size();)
            if (key(lines[i]) == "texturetangents") lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            else i = nextRecord(i);
    }
    std::vector<Texture> textures() const {
        std::vector<Texture> result;
        const auto append = [&](std::size_t line, bool wholeLine, bool policy) {
            auto column = lines[line].find_first_not_of(" \t\r");
            if (!wholeLine) {
                column = lines[line].find_first_of(" \t", column);
                column = lines[line].find_first_not_of(" \t\r", column);
            }
            if (column == std::string::npos) return;
            const auto end = lines[line].find_first_of(" \t\r", column);
            const auto name = lines[line].substr(column, end - column);
            if (name.front() == '#' || equal(name, "null") || equal(name, "none")) return;
            result.push_back({line, column, name.size(), name, policy});
        };
        for (std::size_t i = 0; i < lines.size();) {
            const auto name = key(lines[i]);
            if (name == "texture0" || name == "bitmap" || name == "texture1" ||
                name == "texture" || name == "m_sdepthtexturename" || name == "texturetangents")
                append(i, false, name == "texturetangents");
            else if (name == "texturenames") {
                const auto end = nextRecord(i);
                for (auto row = i + 1; row < end; ++row)
                    if (key(lines[row]).compare(0, 7, "endlist") != 0) append(row, true, false);
            }
            i = name == "node" ? i + 1 : nextRecord(i);
        }
        return result;
    }
    std::string text() const {
        std::string result;
        for (const auto& line : lines) result += line + '\n';
        return result;
    }
};

} // namespace neomdl::model::ascii
