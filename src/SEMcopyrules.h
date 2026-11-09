#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace SEM {

constexpr std::optional<std::int32_t> InferPlainCopies(std::int32_t total,
    std::int32_t special, std::int32_t explicitPlain, std::int32_t unresolved) noexcept {
    if (total < 1 || special < 0 || explicitPlain < 0 || unresolved < 0) return std::nullopt;
    const std::int64_t explicitTotal = static_cast<std::int64_t>(special) + explicitPlain + unresolved;
    if (explicitTotal > total) return std::nullopt;
    return total - special - unresolved; 
}
constexpr bool CanSelectPlain(bool consistent, std::int32_t plain, std::int32_t unresolved) noexcept {
    return consistent && plain == 1 && unresolved == 0;
}
constexpr bool SameSelectedInstance(std::string_view wanted, std::string_view worn) noexcept {
    
    return wanted == worn;
}

inline std::string FormatInstanceName(std::string_view name,
    const std::vector<std::string>& effects, std::optional<float> temper) {
    std::string out(name);
    std::vector<std::string> suffix;
    if (temper && std::isfinite(*temper)) {
        std::ostringstream os;
        os << "Tempered x" << std::fixed << std::setprecision(2) << *temper;
        suffix.push_back(os.str());
    }
    for (const auto& effect : effects) if (!effect.empty()) suffix.push_back(effect);
    if (suffix.empty()) return out;
    out += " (";
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (i) out += ", ";
        out += suffix[i];
    }
    out += ')';
    return out;
}
} 
