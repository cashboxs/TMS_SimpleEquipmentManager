#pragma once
#include "SEMitemkey.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace SEM {

struct CommittedPiece {
    ItemKey key;
    std::uint32_t mask{};
};
inline bool HasCommittedIdentity(const std::vector<CommittedPiece>& sources,
    const ItemKey& identity) {
    return std::any_of(sources.begin(), sources.end(), [&](const CommittedPiece& s) {
        return s.key == identity;
    });
}

inline bool HasCommittedCopy(const std::vector<CommittedPiece>& sources,
    const ItemKey& identity, std::uint32_t mask) {
    return std::any_of(sources.begin(), sources.end(), [&](const CommittedPiece& s) {
        return s.key == identity && s.mask == mask;
    });
}

inline std::vector<CommittedPiece> MergeVerifiedOwnership(
    std::vector<CommittedPiece> verifiedTarget,
    const std::vector<CommittedPiece>& previous,
    const std::vector<CommittedPiece>& actuallyWorn) {
    for (const auto& old : previous) {
        if (HasCommittedIdentity(verifiedTarget, old.key)) continue;
        const bool stillSame = std::any_of(actuallyWorn.begin(), actuallyWorn.end(),
            [&](const CommittedPiece& worn) {
                return worn.key == old.key && worn.mask == old.mask;
            });
        if (stillSame) verifiedTarget.push_back(old);
    }
    return verifiedTarget;
}

}
