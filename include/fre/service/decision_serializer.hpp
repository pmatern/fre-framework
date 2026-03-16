#pragma once

/// DecisionSerializer: converts fre::Decision to/from serialised formats.
///
/// JSON:       Available unconditionally via nlohmann/json.
/// FlatBuffers: Available when FRE_BUILD_SERVICE=ON and flatc has been run to
///             generate include/fre/service/decision_generated.h from
///             schemas/decision.fbs.  The generated header is included only
///             when FRE_FLATBUFFERS_GENERATED is defined.

#include <fre/core/decision.hpp>
#include <fre/core/error.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace fre::service {

class DecisionSerializer {
public:
    DecisionSerializer() = delete;

    /// Serialise a Decision to a compact JSON string.
    [[nodiscard]] static std::string to_json(const Decision& decision);

    /// Parse a Decision from a JSON string.
    /// Returns Error on malformed input.
    [[nodiscard]] static std::expected<Decision, Error>
    from_json(std::string_view json);
};

}  // namespace fre::service
