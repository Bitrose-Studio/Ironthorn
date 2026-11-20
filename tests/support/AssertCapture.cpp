#include "AssertCapture.h"

namespace ith::test {
namespace {

struct CaptureState {
    bool active = false;
    int count = 0;
    std::string expression;
    std::string message;
};

CaptureState g_state;

AssertAction recordAssert(const AssertInfo& info) {
    ++g_state.count;
    g_state.expression = info.expression != nullptr ? info.expression : "";
    g_state.message = info.message != nullptr ? info.message : "";
    return AssertAction::Continue;
}

} // namespace

AssertCapture::AssertCapture() {
    ITH_ASSERT_MSG(!g_state.active, "only one AssertCapture may be active at a time");
    g_state = CaptureState{.active = true};
    m_previous = setAssertHandler(&recordAssert);
}

AssertCapture::~AssertCapture() {
    setAssertHandler(m_previous);
    g_state.active = false;
}

int AssertCapture::count() const {
    return g_state.count;
}

const std::string& AssertCapture::lastExpression() const {
    return g_state.expression;
}

const std::string& AssertCapture::lastMessage() const {
    return g_state.message;
}

} // namespace ith::test
