#include <model/Wind.hpp>
#include <model/EngineArithmetic.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace neomdl::model {
namespace {
constexpr float kPiOver16 = 0.19634954631328583f;

constexpr std::array<float, 32> kCosineLut{{
    1.0f, 0.9807852506637573f, 0.9238795042037964f, 0.8314695954322815f,
    0.7071067690849304f, 0.5555702447891235f, 0.3826834261417389f,
    0.19509032368659973f, 0.0f, -0.19509032368659973f,
    -0.3826834261417389f, -0.5555702447891235f, -0.7071067690849304f,
    -0.8314695954322815f, -0.9238795042037964f, -0.9807852506637573f,
    -1.0f, -0.9807852506637573f, -0.9238795042037964f,
    -0.8314695954322815f, -0.7071067690849304f, -0.5555702447891235f,
    -0.3826834261417389f, -0.19509032368659973f, 0.0f,
    0.19509032368659973f, 0.3826834261417389f, 0.5555702447891235f,
    0.7071067690849304f, 0.8314695954322815f, 0.9238795042037964f,
    0.9807852506637573f
}};
constexpr std::array<float, 32> kSineLut{{
    0.0f, 0.19509032368659973f, 0.3826834261417389f, 0.5555702447891235f,
    0.7071067690849304f, 0.8314695954322815f, 0.9238795042037964f,
    0.9807852506637573f, 1.0f, 0.9807852506637573f,
    0.9238795042037964f, 0.8314695954322815f, 0.7071067690849304f,
    0.5555702447891235f, 0.3826834261417389f, 0.19509032368659973f,
    0.0f, -0.19509032368659973f, -0.3826834261417389f,
    -0.5555702447891235f, -0.7071067690849304f, -0.8314695954322815f,
    -0.9238795042037964f, -0.9807852506637573f, -1.0f,
    -0.9807852506637573f, -0.9238795042037964f, -0.8314695954322815f,
    -0.7071067690849304f, -0.5555702447891235f, -0.3826834261417389f,
    -0.19509032368659973f
}};

Vec3 add(Vec3 left, Vec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 subtract(Vec3 left, Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 multiply(Vec3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float length(Vec3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3 engineNormalize(Vec3 value) noexcept {
    const float magnitude = length(value);
    if (static_cast<double>(magnitude) < 1.0e-9) return {1.0f, 0.0f, 0.0f};
    const float reciprocal = 1.0f / magnitude;
    return multiply(value, reciprocal);
}

std::size_t lutIndex(float angle) noexcept {
    // The desktop engines use CVTTSS2SI before AND #31.
    const auto raw = engineFloatToInt(angle / kPiOver16);
    return static_cast<std::size_t>(static_cast<std::uint32_t>(raw) & 31u);
}

float cosineLut(float angle) noexcept {
    return kCosineLut[lutIndex(angle)];
}

float sineLut(float angle) noexcept {
    return kSineLut[lutIndex(angle)];
}

Vec3 rotate(Vec3 value, float x, float y, float z, float w) noexcept {
    // Expanded matrix arithmetic in K1 Desktop 0x376dd2 and K2 Desktop 0xcb461.
    // The LUT quaternion is intentionally not renormalized, and replacing
    // these products with cross products changes float32 rounding.
    const float xx=x*x, yy=y*y, zz=z*z;
    const float xy=x*y, xz=x*z, yz=y*z;
    const float wx=w*x, wy=w*y, wz=w*z;
    return {
        ((1.0f + -2.0f*(yy+zz))*value.x + (xy-wz)*2.0f*value.y) +
            (xz+wy)*2.0f*value.z,
        ((xy+wz)*2.0f*value.x + (1.0f + -2.0f*(zz+xx))*value.y) +
            (yz-wx)*2.0f*value.z,
        ((xz-wy)*2.0f*value.x + (wx+yz)*2.0f*value.y) +
            (1.0f + -2.0f*(xx+yy))*value.z
    };
}
} // namespace

Vec3 pointSourceWindVector(const PointSourceWind& source,
                           Vec3 position) noexcept {
    const Vec3 delta = subtract(position, source.position);
    const float x = delta.x < 0.0f ? -delta.x : delta.x;
    const float y = delta.y < 0.0f ? -delta.y : delta.y;
    float distance = y > x ? y : x;


    const float z = std::fabs(delta.z);
    if (z > distance)
        distance = delta.z < 0.0f ? -delta.z : delta.z;
    if (!(distance < source.radius)) return {};

    const float magnitude = ((source.radius - distance) * source.strength) /
                            source.radius;
    return multiply(engineNormalize(delta), magnitude);
}

std::optional<K2AreaWindPreset> k2AreaWindPreset(std::uint8_t mode) noexcept {
    constexpr float direction = 0.7071067690849304f;
    switch (mode) {
    case 0u:
        return K2AreaWindPreset{};
    case 1u:
        return K2AreaWindPreset{{direction, direction, 0.0f}, 100.0f, 3.0f};
    case 2u:
        return K2AreaWindPreset{{direction * 2.0f, direction * 2.0f, 0.0f},
                                150.0f, 5.0f};
    default:
        return {};
    }
}

WindManager::WindManager() {
    reset();
}

WindManager::WindManager(Vec3 globalWind, float maximumDeviationZRadians,
                         float maximumDeviationXRadians,
                         float interpolationPeriod,
                         std::uint32_t randomSeed) {
    reset(globalWind, maximumDeviationZRadians, maximumDeviationXRadians,
          interpolationPeriod, randomSeed);
}

void WindManager::reset(Vec3 globalWind, float maximumDeviationZRadians,
                        float maximumDeviationXRadians,
                        float interpolationPeriod,
                        std::uint32_t randomSeed) {
    pointSources_.clear();
    globalWind_ = globalWind;
    frameGlobalWind_ = {};
    maximumDeviationZ_ = maximumDeviationZRadians;
    maximumDeviationX_ = maximumDeviationXRadians;
    interpolation_ = 0.0f;
    timer_ = 0.0f;
    interpolationPeriod_ = interpolationPeriod;
    randomState_ = randomSeed;
    activeField_ = 0u;
    fillNoiseField(fields_[0]);
    fillNoiseField(fields_[1]);
    for (auto& emitter : windyEmitters_)
        if (emitter.calculateRadius) emitter.calculateRadius();
}

void WindManager::setGlobalWind(Vec3 wind) {
    globalWind_ = wind;
    for (auto& emitter : windyEmitters_)
        if (emitter.calculateRadius) emitter.calculateRadius();
}

void WindManager::setMaximumDeviation(float zRadians, float xRadians) noexcept {
    maximumDeviationZ_ = zRadians;
    maximumDeviationX_ = xRadians;
}

bool WindManager::setInterpolationPeriod(float seconds) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0.0f) return false;
    interpolationPeriod_ = seconds;
    return true;
}

bool WindManager::applyK2AreaWind(std::uint8_t mode) {
    const auto preset = k2AreaWindPreset(mode);
    if (!preset) return false;
    constexpr float degreesToRadians = 0.01745329238474369f;
    setGlobalWind(preset->globalWind);
    setMaximumDeviation(preset->maximumDeviationZDegrees * degreesToRadians,
                        preset->maximumDeviationXDegrees * degreesToRadians);
    return true;
}

void WindManager::updateTimer(float elapsedSeconds) {
    timer_ += elapsedSeconds;


    if (timer_ > interpolationPeriod_) {
        activeField_ = 1u - activeField_;
        fillNoiseField(fields_[1u - activeField_]);
        timer_ = 0.0f;
    }
    interpolation_ = timer_ / interpolationPeriod_;
    frameGlobalWind_ = multiply(globalWind_, elapsedSeconds);

    std::size_t index = 0u;
    while (index < pointSources_.size()) {
        pointSources_[index].remainingLifetime -= elapsedSeconds;
        if (pointSources_[index].remainingLifetime < 0.0f)
            pointSources_.erase(pointSources_.begin() +
                                static_cast<std::ptrdiff_t>(index));
        else
            ++index;
    }
}

Vec3 WindManager::getGlobalWind(Vec3 position,
                                float elapsedSeconds) const noexcept {
    const Vec3 point = getGlobalPointWind(position);
    if (globalWind_.x == 0.0f && globalWind_.y == 0.0f &&
        globalWind_.z == 0.0f && point.x == 0.0f && point.y == 0.0f &&
        point.z == 0.0f) return globalWind_;

    const std::size_t x = fieldIndex(position.x);
    const std::size_t y = fieldIndex(position.y);
    const float noise = sample(x, y);
    const float scale = elapsedSeconds * noise;
    const Vec3 combined = add(multiply(globalWind_, scale), point);
    return applyDeviation(combined, x, y);
}

Vec3 WindManager::getGlobalWindNoPoint(Vec3 position,
                                       float elapsedSeconds) const noexcept {
    if (globalWind_.x == 0.0f && globalWind_.y == 0.0f &&
        globalWind_.z == 0.0f) return globalWind_;

    const std::size_t x = fieldIndex(position.x);
    const std::size_t y = fieldIndex(position.y);
    const float noise = sample(x, y);
    return applyDeviation(multiply(globalWind_, elapsedSeconds * noise), x, y);
}

Vec3 WindManager::getGlobalPointWind(Vec3 position) const noexcept {
    Vec3 result{};
    for (const auto& source : pointSources_)
        result = add(result, pointSourceWindVector(source, position));
    return result;
}

void WindManager::addPointSourceWind(Vec3 position, float radius,
                                     float lifetime, float strength) {



    pointSources_.push_back({position, radius, lifetime, strength});
}

bool WindManager::registerWindyEmitter(const void* identity,
                                       RadiusCallback calculateRadius) {
    if (!identity) return false;
    const auto found = std::find_if(windyEmitters_.begin(), windyEmitters_.end(),
        [identity](const RegisteredEmitter& emitter) {
            return emitter.identity == identity;
        });
    if (found != windyEmitters_.end()) return false;
    windyEmitters_.push_back({identity, std::move(calculateRadius)});
    return true;
}

bool WindManager::deregisterWindyEmitter(const void* identity) noexcept {
    const auto found = std::find_if(windyEmitters_.begin(), windyEmitters_.end(),
        [identity](const RegisteredEmitter& emitter) {
            return emitter.identity == identity;
        });
    if (found == windyEmitters_.end()) return false;
    windyEmitters_.erase(found);
    return true;
}

std::uint32_t WindManager::nextRandom() {
    // K1 Desktop calls loaded rand; K2 Desktop calls rand_wincompatible. Their
    // callers take the remainder directly, without an additional 15-bit mask.
    if (randomSource_) return randomSource_();
    randomState_ = randomState_ * 0x343fdu + 0x269ec3u;
    return (randomState_ >> 16u) & 0x7fffu;
}

void WindManager::fillNoiseField(NoiseField& field) {
    for (auto& row : field) row.fill(0.0f);

    // K1 Desktop 0x37611f starts a five-iteration loop; K2 Desktop 0xcb03b
    // continues when the completed block size is >=2. Both include size 1.
    for (int blockSize = 16; blockSize >= 1; blockSize /= 2) {
        const int divisor = 500 / (16 / blockSize);
        for (int row = 0; row < 16; row += blockSize) {
            for (int column = 0; column < 16; column += blockSize) {
                const float value = static_cast<float>(nextRandom() %
                    static_cast<std::uint32_t>(divisor)) / 500.0f;
                for (int y = row; y < row + blockSize; ++y)
                    for (int x = column; x < column + blockSize; ++x)
                        field[static_cast<std::size_t>(y)]
                             [static_cast<std::size_t>(x)] += value;
            }
        }
    }
}

float WindManager::sample(std::size_t x, std::size_t y) const noexcept {
    const float active = fields_[activeField_][x][y];
    const float next = fields_[1u - activeField_][x][y];
    return active * (1.0f - interpolation_) + next * interpolation_;
}

Vec3 WindManager::applyDeviation(Vec3 displacement, std::size_t x,
                                 std::size_t y) const noexcept {
    float zAngle = 0.0f;
    if (maximumDeviationZ_ != 0.0f) {
        const float noise = sample(x, y);
        zAngle = (((noise + noise) * maximumDeviationZ_) -
                  maximumDeviationZ_) * 0.5f;
    }

    float xAngle = 0.0f;
    if (maximumDeviationX_ != 0.0f) {
        const float transposed = sample(y, x);
        xAngle = (((transposed + transposed) * maximumDeviationX_) -
                  maximumDeviationX_) * 0.5f;
    }

    const float cosineZ = cosineLut(zAngle);
    const float sineZ = sineLut(zAngle);
    const float cosineX = cosineLut(xAngle);
    const float sineX = sineLut(xAngle);

    // Scalar-first game quaternion Qz(zAngle) * Qx(xAngle), converted to the
    // x/y/z/w order used by NeoShared.
    const float w = cosineZ * cosineX;
    const float xComponent = cosineZ * sineX;
    const float yComponent = sineZ * sineX;
    const float zComponent = sineZ * cosineX;
    return rotate(displacement, xComponent, yComponent, zComponent, w);
}

std::size_t WindManager::fieldIndex(float coordinate) const noexcept {
    const auto integral = engineFloatToInt(coordinate);
    std::uint32_t scaled = static_cast<std::uint32_t>(integral) * 4u;
    if (integral < 0)
        scaled = 0u - scaled;
    return static_cast<std::size_t>(scaled & 15u);
}

} // namespace neomdl::model
