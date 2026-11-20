#include "ith/foundation/FixedTimestep.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cmath>

namespace ith {

FixedTimestep::FixedTimestep(double stepSeconds, std::uint32_t maxStepsPerFrame)
    : m_stepSeconds(stepSeconds), m_maxStepsPerFrame(maxStepsPerFrame) {
    const bool validStep = std::isfinite(stepSeconds) && stepSeconds > 0.0;
    ITH_ASSERT_MSG(validStep, "FixedTimestep: the step must be finite and positive");
    ITH_ASSERT_MSG(maxStepsPerFrame > 0, "FixedTimestep: maxStepsPerFrame must be at least 1");
    if (!validStep) {
        m_stepSeconds = 1.0 / 60.0;
    }
    m_maxStepsPerFrame = std::max(m_maxStepsPerFrame, std::uint32_t{1});
}

std::uint32_t FixedTimestep::advance(double frameSeconds) {
    if (!std::isfinite(frameSeconds) || frameSeconds <= 0.0) {
        return 0;
    }

    m_accumulator += frameSeconds;

    // The epsilon absorbs floating-point drift, so feeding exactly one step's worth of time
    // reliably gives one step.
    constexpr double kEpsilon = 1e-9;
    const double available = std::floor((m_accumulator + kEpsilon) / m_stepSeconds);
    if (available > static_cast<double>(m_maxStepsPerFrame)) {
        m_accumulator = 0.0;
        return m_maxStepsPerFrame;
    }

    const auto steps = static_cast<std::uint32_t>(available);
    m_accumulator = std::max(0.0, m_accumulator - static_cast<double>(steps) * m_stepSeconds);
    return steps;
}

double FixedTimestep::alpha() const {
    return std::clamp(m_accumulator / m_stepSeconds, 0.0, std::nextafter(1.0, 0.0));
}

} // namespace ith
