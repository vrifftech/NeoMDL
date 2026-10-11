#pragma once

#include "core/SceneOwnerLifetime.hpp"
#include "model/Wind.hpp"
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace neomdl {
class SceneAnimation;

// A separately allocated Gob keeps its marker across detach/reattach. The
// handle can move without moving the registered address (reference arrays).
class SceneAnimationOwner final {
public:
    struct State : std::enable_shared_from_this<State> {
        std::shared_ptr<SceneAnimation> scene;
        std::uint32_t frame{0xffffffffu};
        std::uint16_t classification{};
        bool camera{}, forceAnimated{}, hierarchyUpdated{}, alive{true},hasModel{true},isAnimating{};
        std::uint64_t controlRevision{};
        bool initialAnimation{true},dynamicLights{},inCutscene{},alwaysRendering{},inViewVolume{true};
        std::function<void(float)> preamble,control,procedural,parts;
        std::function<bool()> hasRuns;
        std::function<void()> lookAt;
        std::function<bool(float,bool)> animate;
        model::EngineFrame previousRenderedFrame{};
        model::WindManager* windManager{};
        std::function<model::Mat4()> resolveWorld;
        model::GameVersion windGame{model::GameVersion::Kotor2};

        bool windEnabled{};
        float windIntensity{},windRadius{},windLifetime{};
    };
    SceneAnimationOwner():state_(std::make_shared<State>()) {}
    ~SceneAnimationOwner();
    SceneAnimationOwner(SceneAnimationOwner&&) noexcept=default;
    SceneAnimationOwner& operator=(SceneAnimationOwner&&) noexcept;
    SceneAnimationOwner(const SceneAnimationOwner&)=delete;
    SceneAnimationOwner& operator=(const SceneAnimationOwner&)=delete;
    void setScene(std::shared_ptr<SceneAnimation>);
    void configure(std::uint16_t classification,std::function<bool(float,bool)> animate,
                   bool camera=false) {
        state_->classification=classification;state_->animate=std::move(animate);state_->camera=camera;
    }
    void bindWind(model::WindManager* manager,std::function<model::Mat4()> world,
                  model::GameVersion game) {
        state_->windManager=manager;state_->resolveWorld=std::move(world);
        state_->windGame=game;
    }
    void setWind(bool enabled,float intensity,float radius,float lifetime) noexcept {
        state_->windEnabled=enabled;state_->windIntensity=intensity;
        state_->windRadius=radius;state_->windLifetime=lifetime;
    }
    void updateCreatureWind(std::uint8_t mode,bool player,std::uint8_t fadeState,
                            std::uint8_t activePauseState,float intensity=2.5f) noexcept {
        const bool enabled=activePauseState!=2&&((mode==2&&fadeState==1)||(mode==1&&player));
        setWind(enabled,enabled?intensity:0.0f,0.0f,0.0f);
    }
    const model::EngineFrame& previousRenderedFrame() const noexcept {return state_->previousRenderedFrame;}
    void commitRenderedFrame(const model::EngineFrame& frame) const noexcept {state_->previousRenderedFrame=frame;}
    void setHasModel(bool value) noexcept {state_->hasModel=value;}
    void setAnimatePreamble(std::function<void(float)> callback) {state_->preamble=std::move(callback);}
    void setProcedural(std::function<void(float)> callback) {state_->procedural=std::move(callback);}
    void setPartAnimation(std::function<void(float)> callback) {state_->parts=std::move(callback);}
    void setHasAnimationRuns(std::function<bool()> callback) {state_->hasRuns=std::move(callback);}
    void setLookAt(std::function<void()> callback) {state_->lookAt=std::move(callback);}
    void setDynamicLights(bool value) noexcept {state_->dynamicLights=value;}
    void setInViewVolume(bool value) noexcept {state_->inViewVolume=value;}
    bool inViewVolume() const noexcept {return state_->inViewVolume;}
    void setInCutscene(bool value) noexcept {state_->inCutscene=value;}
    bool inCutscene() const noexcept {return state_->inCutscene;}
    void setAlwaysRendering(bool value) noexcept {state_->alwaysRendering=value;}
    bool alwaysRendering() const noexcept {return state_->alwaysRendering;}
    void setControl(std::function<void(float)> control) {
        state_->control=std::move(control);++state_->controlRevision;
    }
    std::uint64_t controlRevision() const noexcept {return state_->controlRevision;}
    bool hasBehavior() const noexcept {return bool(state_->control);}
    void setForceAnimated(bool value) noexcept {state_->forceAnimated=value;}
    bool forceAnimated() const noexcept {return state_->forceAnimated;}
    bool isAnimating() const noexcept {return state_->isAnimating;}
    void markHierarchyUpdated() noexcept {state_->hierarchyUpdated=true;}
    bool sceneMember() const noexcept {return bool(state_->scene);}
    std::shared_ptr<SceneAnimation> scene() const noexcept {return state_->scene;}
    SceneOwnerLifetime lifetime() const noexcept {return std::weak_ptr<const void>(lifetime_);}
    void animate(float seconds);
    void animateExternal(float seconds);
private:
    friend class SceneAnimation;
    void release();
    std::shared_ptr<State> state_;
    std::shared_ptr<const void> lifetime_=std::make_shared<const unsigned char>(0);
};

// Scene::ChildAdd/ChildRemove and Animate use a live List<Gob*>. Reverse
// traversal captures the initial count, then rereads the count and slot each
// iteration: callbacks may remove owners or register new ones in the list.
class SceneAnimation final {
public:
    void setAnimating(bool value) noexcept {animating_=value;}
    bool isAnimating() const noexcept {return animating_;}
    // Captured client GetPausedByCombat input; distinct from Scene pause.
    void setCombatPaused(bool value) noexcept {combatPaused_=value;}
    bool combatPaused() const noexcept {return combatPaused_;}
    void setInterleaved(bool value) noexcept {interleaved_=value;}
    bool interleaved() const noexcept {return interleaved_;}
    std::uint32_t token() const noexcept {return token_;}
    void animate(float seconds) {
        struct CurrentReset {~CurrentReset(){SceneAnimation::current_=nullptr;}} reset;
        current_=this;elapsed_=seconds;
        if(interleaved_)visit(cameras_,seconds,false);
        visit(owners_,seconds,interleaved_);current_=nullptr;
    }
    // Scene::Render, not Scene::Animate, owns the ordinary token increment.
    // K2 Desktop 0xc5e12 / 0xc5ffe; AnimateExternal brackets its call separately.
    void beginRender() noexcept {current_=this;if(!interleaved_)++token_;}
    void beginRender(float seconds) noexcept {elapsed_=seconds;beginRender();}
    void endRender() noexcept {if(interleaved_)++token_;current_=nullptr;}
    bool animateForRender(SceneAnimationOwner& owner) {
        if(!interleaved_)return false;
        const auto state=owner.state_;
        const bool mayHaveUpdated=state->preamble || !state->scene ||
            state->frame!=state->scene->token_;
        dispatch(state,elapsed_);
        return mayHaveUpdated;
    }
    static bool dependenciesAnimating() noexcept {return !current_||current_->animating_;}
    static SceneAnimation* currentScene() noexcept {return current_;}
    // PartLightsaber explicitly checks the current scene before history aging.
    static bool currentSceneAnimating() noexcept {return current_&&current_->animating_;}
    void animateRemainingForRender() {if(interleaved_)visit(owners_,elapsed_,false);}
    // Worker-prepared owners enter the UI scene in their retained engine
    // registration order, rather than being regrouped by host component.
    void transferOwnersTo(const std::shared_ptr<SceneAnimation>& destination) {
        if(destination.get()==this)return;
        const auto owners=owners_;
        for(auto* owner:owners) {
            remove(owner);owner->scene=destination;destination->add(owner);
        }
    }
private:
    friend class SceneAnimationOwner;
    void add(SceneAnimationOwner::State* owner) {
        owners_.push_back(owner);if(owner->camera)cameras_.push_back(owner);
    }
    void remove(SceneAnimationOwner::State* owner) {
        owners_.erase(std::remove(owners_.begin(),owners_.end(),owner),owners_.end());
        cameras_.erase(std::remove(cameras_.begin(),cameras_.end(),owner),cameras_.end());
    }
    void visit(std::vector<SceneAnimationOwner::State*>& list,float seconds,bool skipUpdated) {
        for(auto count=list.size();count!=0;--count) {
            if(count>list.size())continue;
            auto* owner=list[count-1];
            if(skipUpdated&&owner->hierarchyUpdated)continue;
            dispatch(owner->shared_from_this(),seconds);
        }
    }
    static void dispatch(std::shared_ptr<SceneAnimationOwner::State> owner,float seconds) {
        // Camera::Animate performs its interpolation before Gob::Animate.
        if(owner->preamble)owner->preamble(seconds);
        auto scene=owner->scene;
        if(scene) {
            if(owner->frame==scene->token_)return;
            owner->frame=scene->token_;
        }
        // Gob::Animate emits owner wind after the token check, before behavior,
        // even when the later dt/scene animation admission rejects this owner.
        if(owner->windEnabled&&scene&&owner->windManager) {
            const auto world=owner->resolveWorld();
            const model::Vec3 position{world.values[12],world.values[13],world.values[14]};
            const float radius=owner->windRadius==0.0f?1.2f:owner->windRadius;
            float lifetime=owner->windLifetime;
            if(lifetime==0.0f) {
                const auto& previous=owner->previousRenderedFrame.position;
                const float dx=position.x-previous.x,dy=position.y-previous.y,dz=position.z-previous.z;
                const float xx=dx*dx,yy=dy*dy,zz=dz*dz;
                float length;
                if(owner->windGame==model::GameVersion::Kotor1)length=std::sqrt((xx+yy)+zz);
                else length=std::sqrt(zz+(yy+xx));
                // K1 JP/JB / FMINNM select 1 for NaN; K2 MINSS(1,length)
                // retains the unordered source operand instead.
                lifetime=owner->windGame==model::GameVersion::Kotor1
                    ?(length<1.0f?length:1.0f):(1.0f<length?1.0f:length);
            }
            owner->windManager->addPointSourceWind(position,radius,lifetime,owner->windIntensity);
        }
        // Gob marks the token before CAurBehavior::Control. A dependency can
        // reenter Animate, and Control can delete this Gob (engine SafePointer).
        const auto control=owner->control;
        if(control)control(seconds);
        if(!owner->alive)return;
        scene=owner->scene;
        const bool saber=owner->hasModel&&(owner->classification&0x10u)!=0;
        if(seconds<=0.0f&&!saber)return;
        if(scene&&!scene->animating_&&!owner->forceAnimated&&!saber)return;
        bool evaluate=!scene||scene->animating_||owner->forceAnimated;
        if(owner->procedural)owner->procedural(seconds);
        if(owner->lookAt)owner->lookAt();
        if(owner->hasModel&&!owner->inCutscene&&!owner->initialAnimation&&!owner->dynamicLights&&
           !(owner->classification&0x41u)&&!owner->alwaysRendering&&
           !(owner->classification&0x08u)&&!owner->inViewVolume&&!saber) {
            if(!(owner->classification&0x04u))return;
            evaluate=false;
        }
        owner->initialAnimation=false;
        const bool hadRuns=owner->hasRuns&&owner->hasRuns();
        owner->isAnimating=hadRuns;
        if(owner->animate) {
            const bool applied=owner->animate(seconds,evaluate);
            if(!owner->alive)return;
            owner->hierarchyUpdated=applied||owner->hierarchyUpdated;
        }
        if(owner->parts&&((owner->scene&&hadRuns)||saber))owner->parts(seconds);
    }
    inline static thread_local SceneAnimation* current_{};
    std::vector<SceneAnimationOwner::State*> owners_,cameras_;
    std::uint32_t token_{};
    float elapsed_{};
    bool animating_{true},interleaved_{},combatPaused_{};
};
inline SceneAnimationOwner::~SceneAnimationOwner() {release();}
inline void SceneAnimationOwner::release() {
    lifetime_.reset();
    if(!state_)return;
    if(state_->scene)state_->scene->remove(state_.get());
    state_->scene.reset();state_->alive=false;
}
inline SceneAnimationOwner& SceneAnimationOwner::operator=(SceneAnimationOwner&& other) noexcept {
    release();state_=std::move(other.state_);lifetime_=std::move(other.lifetime_);return *this;
}
inline void SceneAnimationOwner::setScene(std::shared_ptr<SceneAnimation> scene) {
    if(state_->scene==scene)return;
    if(state_->scene)state_->scene->remove(state_.get());
    state_->scene=std::move(scene);
    if(state_->scene)state_->scene->add(state_.get());
}
inline void SceneAnimationOwner::animate(float seconds) {SceneAnimation::dispatch(state_,seconds);}
inline void SceneAnimationOwner::animateExternal(float seconds) {
    const auto state=state_;
    const bool hadScene=bool(state->scene);
    if(hadScene)++state->scene->token_;
    SceneAnimation::dispatch(state,seconds);
    if(hadScene) {
        // Engine rereads Gob.scene here and dereferences it unconditionally.
        // A deleted/detached owner violates AnimateExternal's lifetime contract.
        if(!state->alive||!state->scene)
            throw std::runtime_error("Engine AnimateExternal owner was destroyed or detached during animation");
        ++state->scene->token_;
    }
}
} // namespace neomdl
