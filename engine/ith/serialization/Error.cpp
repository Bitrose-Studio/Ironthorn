#include "ith/serialization/Error.h"

#include <string>

namespace ith::serialization {

std::string Error::toString() const {
    std::string text = source;
    if (line != 0) {
        text += ':' + std::to_string(line) + ':' + std::to_string(column);
    } else if (offset.has_value()) {
        text += '@' + std::to_string(*offset);
    }
    if (!text.empty()) {
        text += ": ";
    }
    if (!path.empty()) {
        text += path + ": ";
    }
    text += message + " [" + code + ']';
    return text;
}

} // namespace ith::serialization
