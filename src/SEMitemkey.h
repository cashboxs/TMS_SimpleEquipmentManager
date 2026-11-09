#pragma once
#include <cstdint>
#include <string>

namespace SEM {

struct ItemKey {
    std::string mod;
    std::uint32_t localID{};
    std::string variant;
    bool operator==(const ItemKey&) const = default;
};
} 
