#pragma once

// Records failed asserts instead of aborting, for tests that check that code asserts.
//
//   ith::test::AssertCapture capture;
//   functionThatAsserts();
//   CHECK(capture.count() == 1);
//
// Only one capture may be active at a time; it restores the previous handler when destroyed.

#include "ith/foundation/Assert.h"

#include <string>

namespace ith::test {

class AssertCapture {
public:
    AssertCapture();
    ~AssertCapture();

    AssertCapture(const AssertCapture&) = delete;
    AssertCapture& operator=(const AssertCapture&) = delete;

    // Number of failed asserts since the capture started.
    int count() const;

    // The expression and message of the most recent failure ("" when none).
    const std::string& lastExpression() const;
    const std::string& lastMessage() const;

private:
    AssertHandler m_previous = nullptr;
};

} // namespace ith::test
