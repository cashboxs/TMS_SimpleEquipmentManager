#pragma once
#include <cstdint>
#include <cstddef>

namespace SEM {

enum class Scene : std::uint32_t { Unknown = 0, Combat = 1, Town = 2, Home = 3 };
constexpr bool ShouldSwitch(Scene active, Scene target) noexcept {
    return target != Scene::Unknown && active != target;
}

constexpr std::uint32_t SlotBit(unsigned slot) noexcept {
    return slot >= 30 && slot <= 61 ? (1u << (slot - 30)) : 0u;
}
constexpr std::uint32_t kProtectedMask = SlotBit(50) | SlotBit(52);
constexpr bool IsAllowedMask(std::uint32_t mask) noexcept {
    return mask != 0 && (mask & kProtectedMask) == 0;
}
constexpr bool Conflicts(std::uint32_t lhs, std::uint32_t rhs) noexcept {
    return (lhs & rhs) != 0;
}

constexpr bool HideRedundantBaseRow(std::int32_t inventoryCount,
    std::size_t uniqueVerifiedVariants, std::size_t shownVariantRows) noexcept {
    return inventoryCount == 1 && uniqueVerifiedVariants == 1 && shownVariantRows == 1;
}

constexpr bool MenuSelectionReplaces(std::uint32_t oldMask, std::uint32_t newMask) noexcept {
    return IsAllowedMask(oldMask) && IsAllowedMask(newMask) && Conflicts(oldMask, newMask);
}

constexpr bool UniqueInstanceMatch(unsigned snapshotMatches, unsigned liveMatches) noexcept {
    return snapshotMatches == 1 && liveMatches == 1;
}

constexpr bool IsUnambiguousBaseCopy(std::int32_t count, bool hasEntry) noexcept {
    return count == 1 && hasEntry;
}

constexpr bool IsTemporaryOverrideShape(std::uint32_t committedMask,
                                         std::uint32_t replacementMask) noexcept {
    return IsAllowedMask(committedMask) && IsAllowedMask(replacementMask) &&
           (replacementMask & committedMask) == replacementMask;
}

constexpr bool IsOutfitCommitComplete(int skipped, int failed, int rollbackFailed,
    bool finalReadbackMatches) noexcept {
    return skipped == 0 && failed == 0 && rollbackFailed == 0 && finalReadbackMatches;
}

constexpr bool CanReleaseTemporaryOverride(Scene previous, Scene next,
    bool previousStillWorn, bool foreignSelected, bool uniqueForeignCopy,
    unsigned missingMatches, unsigned candidatesForSource,
    std::uint32_t priorMask, std::uint32_t foreignMask) noexcept {
    return previous != Scene::Unknown && next != Scene::Unknown && previous != next &&
           !previousStillWorn && !foreignSelected && uniqueForeignCopy &&
           missingMatches == 1 && candidatesForSource == 1 &&
           IsTemporaryOverrideShape(priorMask, foreignMask);
}

constexpr bool MayReleaseUncovered(Scene previous, Scene target,
    bool exactCommittedAndWorn, bool resolvable, std::uint32_t oldMask,
    std::uint32_t targetMask) noexcept {
    return previous != Scene::Unknown && target != Scene::Unknown &&
        previous != target && exactCommittedAndWorn && resolvable &&
        IsAllowedMask(oldMask) && !Conflicts(oldMask, targetMask);
}

}
