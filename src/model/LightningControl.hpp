#pragma once

#include <model/Model.hpp>
#include <cstddef>
#include <vector>

namespace neomdl::model {
struct ParticleRandom;

// The six independently retained LightningEmitter control arrays.
struct EngineLightningControl {
    std::vector<Vec3> base, baseTangents;
    std::vector<Vec3> previous, current;
    std::vector<Vec3> previousTangents, currentTangents;
    Vec3 radial{};
};

Vec3 engineLightningNormalize(Vec3 value) noexcept;
// Engine direction accessors evaluate raw quaternion columns. Unlike vector
// rotation, they do not use the scalar-only identity optimization.
Vec3 engineQuaternionAxis(Quaternion orientation, unsigned axis) noexcept;
Quaternion engineQuaternionAxisAngle(Vec3 axis, float radians) noexcept;
Quaternion engineQuaternionRotationArc(Vec3 from, Vec3 to) noexcept;
Vec3 engineLightningYawPitchRollRotate(Vec3 value, float yawDegrees,
    float pitchDegrees, float rollDegrees) noexcept;
Vec3 engineLightningAxisRotate(Vec3 value, Vec3 axis, float radians) noexcept;
Vec3 engineLightningArcRotate(Vec3 value, Vec3 from, Vec3 to) noexcept;

void engineInitializeLightningControl(EngineLightningControl& control,
    Vec3 source, Vec3 target, Vec3 localSourceForward, Vec3 targetForward,
    const NodeEmitterState& state);

void engineRefreshLightningControl(EngineLightningControl& control,
    Vec3 source, Vec3 target, Vec3 localSourceForward, Vec3 worldSourceForward,
    Vec3 targetForward, const NodeEmitterState& state,
    ParticleRandom& random, bool child=false);

Vec3 engineEvaluateLightningControl(const EngineLightningControl& control,
    std::size_t particle, std::size_t particleCount, float blend,
    bool previousOnly=false);
Vec3 engineEvaluateLightningLastControl(const EngineLightningControl& control,
    std::size_t particle, std::size_t particleCount, float blend);
}
