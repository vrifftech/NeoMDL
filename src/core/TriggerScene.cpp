#include "core/TriggerScene.hpp"

#include <algorithm>
#include <cmath>

namespace neomdl {
namespace m = neomdl::model;
namespace {

void extend(m::Bounds& bounds, m::Vec3 point) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z)) {
        return;
    }
    if (!bounds.valid) {
        bounds.minimum = bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

} // namespace

std::vector<m::Vec3> engineTriggerClientFootprint(const std::vector<m::Vec3>& points) {
    // K1 CreateNewLastUpdateObject 0x287592 / AddTriggerGeometry 0x27f37a
    // -> client Add 0x92b50. K2 Desktop Add 0x2c9e0e has the same projection.
    const auto count=static_cast<std::uint8_t>(points.size());
    std::vector<m::Vec3> result(points.begin(),points.begin()+count);
    for(auto& point:result)point.z+=0.05f;
    return result;
}

void TriggerScene::setDocument(const ModelDocument* document) {
    document_ = document;
    reset();
    if (!document_ || !document_->isOpen()) return;
    kotor2Runtime_ = document_->model().game == m::GameVersion::Kotor2;
    modelMarkerCount_ = static_cast<std::size_t>(std::count_if(
        document_->model().nodes.begin(), document_->model().nodes.end(),
        [](const m::Node& node) { return node.trigger.has_value(); }));
    ++revision_;
}

void TriggerScene::reset() {
    volumes_.clear();
    records_.clear();
    globalAlpha_ = 0.0f;
    scenePhase_ = 0.0f;
    modelMarkerCount_ = 0u;
    kotor2Runtime_ = false;
    ++revision_;
}

void TriggerScene::advance(float elapsedSeconds) noexcept {
    // Scene::DoHighlightTriggers K1 0x35623f / K2 Desktop 0xc4004:
    // the host calls once per admitted highlight pass, after stencil admission.
    constexpr float pi = 3.14159265358979323846f;
    scenePhase_ += (elapsedSeconds+elapsedSeconds)*pi;
    ++revision_;
}

std::size_t TriggerScene::createVolume(std::vector<m::Vec3> footprint,
                                       std::array<float, 4> color,
                                       std::uintptr_t owner) {
    volumes_.push_back(m::makeTriggerVolume(
        std::move(footprint), color, owner));
    records_.push_back({});
    ++revision_;
    return volumes_.size() - 1u;
}

bool TriggerScene::removeVolume(std::size_t index) {
    if (index >= volumes_.size()) return false;
    volumes_.erase(volumes_.begin() + static_cast<std::ptrdiff_t>(index));
    records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(index));
    ++revision_;
    return true;
}

void TriggerScene::clearVolumes() {
    if (volumes_.empty()) return;
    volumes_.clear();
    records_.clear();
    ++revision_;
}

bool TriggerScene::replaceAreaTriggers(
    const std::vector<AreaTriggerRecord>& loaded, std::string& error) {
    error.clear();
    std::vector<m::TriggerVolume> nextVolumes;
    std::vector<RuntimeTriggerRecord> nextRecords;
    nextVolumes.reserve(volumes_.size()+loaded.size());
    nextRecords.reserve(records_.size()+loaded.size());
    for(std::size_t i=0;i<volumes_.size();++i) if(
        i>=records_.size()||records_[i].source==RuntimeTriggerSource::Manual) {
        nextVolumes.push_back(volumes_[i]);
        nextRecords.push_back(i<records_.size()?records_[i]:RuntimeTriggerRecord{});
    }
    for(const auto& source:loaded) {
        // Engine Type 2 is the mine Gob path, never a PartTrigger footprint.
        if(source.type==2)continue;
        auto volume=m::makeTriggerVolume(engineTriggerClientFootprint(source.worldFootprint),
            {{0.0f,0.0f,0.0f,0.0f}},static_cast<std::uintptr_t>(source.objectId));
        if(!volume.valid) {
            error=source.label+": "+(volume.diagnostic.empty()
                ?std::string("invalid engine trigger footprint"):volume.diagnostic);
            return false;
        }
        RuntimeTriggerRecord record;
        record.source=RuntimeTriggerSource::AreaGit;
        record.sourceIndex=source.sourceIndex;
        record.objectId=source.objectId;record.label=source.label;record.tag=source.tag;
        record.templateResref=source.templateResref;
        record.templateIdentity=source.templateIdentity;
        record.type=source.type;record.authoredHighlightHeight=source.highlightHeight;
        // The message producer sends the Type 1 transition flag. Client Add
        // displays that class immediately; ordinary triggers start hidden.
        record.clientVisualState.areaTransitionMode=source.type==1;
        record.clientVisualState.displayEnabled=source.type==1;
        m::TriggerHighlightParameters parameters;
        parameters.color=source.type==1?m::Vec3{0.0f,0.0f,1.0f}:m::Vec3{1.0f,1.0f,0.0f};
        parameters.alpha=source.type==1?0.25f:0.0f;
        parameters.pulseRate=0.0f;
        m::setTriggerHighlightParameters(volume,parameters);
        m::setTriggerHighlighted(volume,true,source.highlightHeight,true);
        nextVolumes.push_back(std::move(volume));nextRecords.push_back(std::move(record));
    }
    volumes_=std::move(nextVolumes);records_=std::move(nextRecords);++revision_;
    return true;
}

void TriggerScene::clearAreaTriggers() {
    if(records_.size()!=volumes_.size()) {clearVolumes();return;}
    if(std::none_of(records_.begin(),records_.end(),[](const auto& record) {
        return record.source==RuntimeTriggerSource::AreaGit;
    })) return;
    std::vector<m::TriggerVolume> nextVolumes;
    std::vector<RuntimeTriggerRecord> nextRecords;
    nextVolumes.reserve(volumes_.size());nextRecords.reserve(records_.size());
    for(std::size_t i=0;i<volumes_.size();++i)
        if(records_[i].source==RuntimeTriggerSource::Manual) {
            nextVolumes.push_back(std::move(volumes_[i]));
            nextRecords.push_back(std::move(records_[i]));
        }
    if(nextVolumes.size()==volumes_.size())return;
    volumes_=std::move(nextVolumes);records_=std::move(nextRecords);++revision_;
}

bool TriggerScene::applyClientVisualState(
    std::size_t index,const TriggerClientVisualState& state) {
    if(index>=volumes_.size()||index>=records_.size())return false;
    records_[index].clientVisualState=state;
    // CSWCTrigger::UpdateTriggerColor returns before SetHighlighted when the
    // first live gate is set. Preserve the previously generated state.
    if(state.updateSuppressed)return true;
    m::TriggerHighlightParameters parameters;
    parameters.color=state.areaTransitionMode?m::Vec3{0.0f,0.0f,1.0f}
                                               :m::Vec3{0.3f,1.0f,1.0f};
    parameters.alpha=state.displayEnabled?0.25f:0.0f;
    parameters.pulse=false;parameters.pulseRate=0.0f;parameters.additive=false;
    m::setTriggerHighlightParameters(volumes_[index],parameters);
    m::setTriggerHighlighted(volumes_[index],true,
        records_[index].authoredHighlightHeight,true);
    ++revision_;return true;
}

bool TriggerScene::applySpecialDisplay(std::size_t index,bool displayAll) {
    if(index>=volumes_.size()||index>=records_.size())return false;
    const auto& state=records_[index].clientVisualState;
    if(state.updateSuppressed)return true;
    // CSWCTrigger::SpecialDisplayTrigger K1 0xcf2f6 / K2 Desktop 0x1e8bb4.
    m::TriggerHighlightParameters parameters;
    parameters.color=state.areaTransitionMode?m::Vec3{0.0f,0.0f,1.0f}:m::Vec3{1.0f,1.0f,0.0f};
    parameters.alpha=(displayAll||state.displayEnabled)?(state.areaTransitionMode?0.25f:0.35f):0.0f;
    parameters.pulseRate=0.0f;
    m::setTriggerHighlightParameters(volumes_[index],parameters);
    m::setTriggerHighlighted(volumes_[index],true,records_[index].authoredHighlightHeight,true);
    ++revision_;return true;
}

bool TriggerScene::setHighlighted(std::size_t index, bool highlighted,
                                  float height, bool group) {
    if (index >= volumes_.size()) return false;
    m::setTriggerHighlighted(volumes_[index], highlighted, height, group);
    ++revision_;
    return true;
}

bool TriggerScene::setHighlightParameters(
    std::size_t index, const m::TriggerHighlightParameters& parameters) {
    if (index >= volumes_.size()) return false;
    m::setTriggerHighlightParameters(volumes_[index], parameters);
    ++revision_;
    return true;
}

bool TriggerScene::setColor(std::size_t index, std::array<float, 4> color) {
    if (index >= volumes_.size()) return false;
    volumes_[index].color = color;
    ++revision_;
    return true;
}

std::optional<RuntimeTriggerHit> TriggerScene::hitTest(
    const m::Vec3& start, const m::Vec3& end,
    const m::TriggerHitOptions& options) const {
    // Scene visits every trigger in creation order, shortening the segment
    // after each successful PartTrigger hit. Each Part's own triangle walk
    // still stops at its first matching decomposed triangle.
    std::optional<RuntimeTriggerHit> result;
    auto endpoint=end;
    float fraction=1.0f;
    for (std::size_t index = 0u; index < volumes_.size(); ++index) {
        if (auto hit = m::hitTestTrigger(
                volumes_[index], start, endpoint, options)) {
            endpoint=hit->position;
            fraction*=hit->segmentFraction;
            hit->segmentFraction=fraction;
            result=RuntimeTriggerHit{index,*hit};
        }
    }
    return result;
}

void TriggerScene::setGlobalAlpha(float alpha) noexcept {
    if (!std::isfinite(alpha)) return;
    globalAlpha_ = alpha;
    ++revision_;
}

m::Bounds TriggerScene::bounds() const noexcept {
    m::Bounds result;
    for (const auto& volume : volumes_) {
        if (!volume.bounds.valid) continue;
        extend(result, volume.bounds.minimum);
        extend(result, volume.bounds.maximum);
    }
    return result;
}

} // namespace neomdl
