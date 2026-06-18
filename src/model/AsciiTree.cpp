#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <functional>

namespace neomdl::model {
namespace {

std::size_t treeRoot(const std::vector<Node>& nodes) {
    return static_cast<std::size_t>(std::find_if(nodes.begin(), nodes.end(),
        [](const Node& node) { return !node.parent; }) - nodes.begin());
}

struct Sibling {
    std::size_t index;
    bool mesh;
    std::int32_t transparency;
};

// K1 0x5c1750 / K2 0x6df110 SiblingOrderCmp. Stable sorting retains
// source order for equal keys without adding a name or node-ID tie breaker.
int siblingOrder(const Sibling& a, const Sibling& b) {
    if (!a.mesh || !b.mesh) return a.mesh ? -1 : b.mesh ? 1 : 0;
    if (a.transparency == b.transparency) return 0;
    if (a.transparency == 0) return 1;
    if (b.transparency == 0) return -1;
    return a.transparency < b.transparency ? -1 : 1;
}

void reorderChildren(Model& model, std::size_t index) {
    auto& children = model.nodes[index].children;
    std::vector<Sibling> siblings;
    siblings.reserve(children.size());
    bool hasOrder = false;
    for (const auto child : children) {
        const auto& node = model.nodes[child];
        const auto hint = node.mesh
            ? static_cast<std::int32_t>(model.meshes[*node.mesh].transparencyHint) : 0;
        siblings.push_back({child, (node.flags & kNodeMesh) != 0, hint});
        if ((node.flags & kNodeMesh) != 0 && hint > 0) hasOrder = true;
    }
    // ReorderChildren only sorts this sibling list when at least one mesh
    // has a strictly positive hint. Recursion still occurs for every child.
    if (hasOrder) {
        std::stable_sort(siblings.begin(), siblings.end(),
            [](const Sibling& a, const Sibling& b) { return siblingOrder(a, b) < 0; });
        for (std::size_t i = 0; i < children.size(); ++i) children[i] = siblings[i].index;
    }
    for (const auto child : children) reorderChildren(model, child);
}

void synchronizeIds(std::vector<Node>& nodes, const std::vector<Node>& reference) {
    // SynchronizeTree is called for an actual parsed tree. Its root pair is
    // unconditional; only their child names take part in the search.
    const auto sourceRoot = treeRoot(nodes);
    const auto referenceRoot = treeRoot(reference);
    std::int32_t maximum = -1;
    std::function<void(std::size_t)> findMaximum = [&](std::size_t index) {
        maximum = std::max(maximum, static_cast<std::int32_t>(
            static_cast<std::int16_t>(reference[index].id)));
        for (const auto child : reference[index].children) findMaximum(child);
    };
    findMaximum(referenceRoot);
    auto next = maximum + 1;
    std::function<void(std::size_t, std::size_t)> synchronize =
        [&](std::size_t source, std::size_t target) {
            nodes[source].id = reference[target].id;
            for (const auto child : nodes[source].children) {
                const auto& candidates = reference[target].children;
                const auto match = std::find_if(candidates.begin(), candidates.end(),
                    [&](std::size_t candidate) {
                        return ascii::equal(nodes[child].name, reference[candidate].name);
                    });
                if (match != candidates.end()) synchronize(child, *match);
                else nodes[child].id = static_cast<std::uint16_t>(next++);
                // Engine does not recurse into an unmatched child. Its
                // descendants retain their original constructor IDs.
            }
        };
    synchronize(sourceRoot, referenceRoot);
}

void convertAnimationNode(Node& node) {
    // NWNEE ParseNode (0x447540) preserves the typed animmesh object and
    // its generated sample pools in animation trees. Retain that ownership
    // for the animmesh authoring backport; all other types keep KOTOR's
    // typed-to-base conversion below.
    if (node.flags == (kNodeBase | kNodeMesh | kNodeAnimation)) return;
    // ParseNode replaces every typed animation node with a fresh base node,
    // copying the name, ID, parent, controller keys and raw key data only.
    Node replacement;
    replacement.flags = kNodeBase;
    replacement.id = node.id;
    replacement.engineNameIndex = node.engineNameIndex;
    replacement.name = std::move(node.name);
    replacement.parent = node.parent;
    replacement.children = std::move(node.children);
    replacement.controllers = std::move(node.controllers);
    replacement.controllerDataWords = std::move(node.controllerDataWords);
    node = std::move(replacement);
}

void postProcessAnimation(Animation& animation) {
    // K1 0x5c2ee8 / K2 0x6e0f40: this entire operation, including the
    // negative length clamp, is conditional on resolving a real root.
    if (animation.root.empty() || ascii::equal(animation.root, "NULL")) return;
    auto& nodes = animation.nodes;
    const auto root = treeRoot(nodes);
    std::optional<std::size_t> selected;
    std::function<void(std::size_t)> findRoot = [&](std::size_t index) {
        if (nodes[index].name == animation.root) { selected = index; return; }
        for (const auto child : nodes[index].children) {
            findRoot(child);
            if (selected) return;
        }
    };
    findRoot(root);
    if (!selected) return;

    auto path = *selected;
    while (nodes[path].parent) {
        const auto ancestor = *nodes[path].parent;
        nodes[ancestor].controllers.clear();
        // Engine removes children whose pointer differs from this path;
        // repeated matching entries survive (K2 0x6e1007, NW 0x4536bb).
        auto& children = nodes[ancestor].children;
        children.erase(std::remove_if(children.begin(), children.end(),
            [path](std::size_t child) { return child != path; }), children.end());
        path = ancestor;
    }

    // pruneanimations is initialized to 1 in the engine.
    // Prune walks children backwards and removes uncontrolled empty leaves,
    // retaining the tree's root even when it has no controllers or children.
    std::function<bool(std::size_t)> prune = [&](std::size_t index) {
        auto& children = nodes[index].children;
        for (std::size_t i = children.size(); i != 0; --i) {
            if (prune(children[i - 1])) children.erase(children.begin() + (i - 1));
        }
        // NWNEE Animation::PostProcess (0x453600) retains sample-only
        // animmesh leaves. Their packed tracks are animation payload even
        // when no ordinary controller keys were authored.
        return index != root && children.empty() && nodes[index].controllers.empty()
            && !nodes[index].animmesh;
    };
    prune(root);

    // Engine deletes removed objects. Compact the value representation and
    // remap links so deleted siblings cannot participate in runtime lookup.
    std::vector<bool> retained(nodes.size());
    std::function<void(std::size_t)> retain = [&](std::size_t index) {
        retained[index] = true;
        for (const auto child : nodes[index].children) retain(child);
    };
    retain(root);
    std::vector<std::size_t> remap(nodes.size());
    std::vector<Node> result;
    result.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (retained[i]) { remap[i] = result.size(); result.push_back(std::move(nodes[i])); }
    }
    for (auto& node : result) {
        if (node.parent) node.parent = remap[*node.parent];
        for (auto& child : node.children) child = remap[child];
    }
    nodes = std::move(result);
    if (animation.length < 0.0f) animation.length = 0.0f;
}

} // namespace

namespace ascii {

void finalizeNodeTree(Model& model, std::vector<RawNode>&, Animation* animation) {
    if (animation) {
        for (auto& node : animation->nodes) convertAnimationNode(node);
        postProcessAnimation(*animation);
        synchronizeIds(animation->nodes, model.nodes);
    } else {
        reorderChildren(model, treeRoot(model.nodes));
    }
}

} // namespace ascii

void synchronizeAsciiModelTree(Model& model, const Model& supermodel) {
    // beginmodelgeom has already seeded its node counter before parsing.
    // This call belongs to that block only; previously loaded animations
    // retain the IDs assigned by their own newanim synchronization.
    synchronizeIds(model.nodes, supermodel.nodes);
}

} // namespace neomdl::model
