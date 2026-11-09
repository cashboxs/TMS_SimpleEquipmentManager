#include "SEMpch.h"
#include "SKSEMenuFramework.h"
#include "SEMrules.h"
#include "SEMitemkey.h"
#include "SEMcopyrules.h"
#include "SEMownership.h"
#include <cstring>
#include <bit>
#include <sstream>
#include <iomanip>
#include <optional>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace SEM {
namespace {
constexpr std::string_view kVersion = "1.0.10";
constexpr const wchar_t* kIniPath = L"Data/SKSE/Plugins/TMS_SimpleEquipmentManager.ini";
constexpr const char* kProfileDir = "Data/SKSE/Plugins/TMS_SimpleEquipmentManager/Outfits";
constexpr const char* kLegacyOutfitPath = "Data/SKSE/Plugins/TMS_SimpleEquipmentManager/Outfits.json";

constexpr std::array<const char*, 3> kPersistedGroupKeys{ "Combat", "Town", "Home" };

constexpr std::array<const char*, 3> kDisplayNames{ "Combat", "Town", "Home" };
constexpr std::uint32_t kRecordID = 0x4F4D4553; 
constexpr std::uint32_t kRecordVersion = 4; 
constexpr std::uint32_t kMaxSavedEntries = 512;

struct InventoryRow {
    ItemKey key;
    RE::FormID formID{};
    std::string name;
    std::uint32_t mask{};
    std::int32_t count{};
    bool worn{};
    bool ambiguousCopies{}; 
    bool soleVerifiedVariant{}; 
};
struct WornArmor {
    RE::TESObjectARMO* armor{};
    RE::FormID formID{};
    std::uint32_t mask{};
    ItemKey key; 
};
struct SaveHeader { std::uint32_t count{}; std::uint32_t activeScene{}; }; 
struct SaveEntry { std::uint32_t form{}; std::uint32_t mask{}; }; 
struct SaveEntryV4 { std::uint32_t form{}; std::uint32_t mask{}; char variant[17]{}; }; 

std::array<std::vector<ItemKey>, 3> gGroups;
std::string gCurrentProfileName;
std::filesystem::path gCurrentProfilePath;
std::vector<std::filesystem::path> gProfileFiles;
int gProfileDeleteSelection = -1;
bool gConfirmProfileDelete = false;
bool gProfileListDirty = true;

std::vector<CommittedPiece> gCommittedWorn;

struct EquipSignal {
    RE::FormID form{};
    bool equipped{};
};
constexpr std::size_t kMaxEquipSignals = 256;
std::mutex gEquipSignalMutex;
std::vector<EquipSignal> gEquipSignals;
bool gEquipSignalsOverflow{};
thread_local bool gInternalEquipMutation{};
struct InternalEquipMutationGuard {
    bool previous{gInternalEquipMutation};
    InternalEquipMutationGuard() { gInternalEquipMutation = true; }
    ~InternalEquipMutationGuard() { gInternalEquipMutation = previous; }
};

bool gSeedLegacySnapshotOnPostLoad{};
Scene gActiveScene{Scene::Unknown}; 
enum class SaveFeedback { Untouched, Saved, Failed };
SaveFeedback gSaveFeedback{SaveFeedback::Untouched};
bool gConfigLoadBlocked = false;
std::vector<InventoryRow> gInventoryRows;

std::atomic_bool gInventoryDirty{true};
int gLastMenuFrame{-1};
std::atomic_bool gEnabled{false};
std::atomic_int gLevel{static_cast<int>(spdlog::level::info)};
std::atomic_bool gGameReady{false};
std::atomic_bool gLoading{false};
std::atomic_bool gAttempted{false};
std::atomic_bool gFaderOpen{false};
std::atomic_uint64_t gEpoch{0};

std::int64_t NowUs();
std::atomic_int gLastObservedPlayerCombat{-1};
std::atomic_bool gCombatEndedObserved{false};

constexpr std::uint32_t kRequestCombat = 2u;
std::atomic_uint32_t gPendingRequests{0};
std::atomic_bool gRequestTaskQueued{false};
bool gCombatOverride{};                     
void QueueRuntimeCheck(std::uint32_t flags);
void ProcessRuntimeCheck();

void ObservePlayerCombatState(const char* origin, RE::PlayerCharacter* player) {
    if (!gGameReady.load() || !player) return;
    const int current = player->IsInCombat() ? 1 : 0;
    const int previous = gLastObservedPlayerCombat.exchange(current);
    if (previous == current) return;
    spdlog::debug("[COMBAT_SAMPLE] source={} previous={} current={} loading={}",
        origin, previous, current, gLoading.load());
    if (previous == 1 && current == 0) {
        gCombatEndedObserved.store(true);
        QueueRuntimeCheck(kRequestCombat);
    }
}

bool gMenuRegistered{};
int gSelectedGroup{0};
bool gInventorySeeded{false};
char gFilter[128]{};

struct ConfigStamp {
    bool exists{};
    std::uintmax_t size{};
    std::filesystem::file_time_type mtime{};
    bool operator==(const ConfigStamp&) const = default;
};
std::optional<ConfigStamp> gObservedConfigStamp;

std::string EncodeProfileName(std::string_view name) {
    std::string encoded;
    static constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : name) {
        if ((std::isalnum(c) && c < 128) || c == '_' || c == '-')
            encoded.push_back(static_cast<char>(c));
        else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 15]);
        }
    }
    if (encoded.empty() || encoded.size() > 220) return {};
    std::string upper = encoded;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static constexpr std::array<std::string_view, 4> devices{"CON", "PRN", "AUX", "NUL"};
    if (std::find(devices.begin(), devices.end(), upper) != devices.end() ||
        (upper.size() == 4 && ((upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' && upper[3] <= '9')))
        encoded.insert(0, "%00");
    return encoded;
}
std::filesystem::path ProfilesDir() {
    return std::filesystem::absolute(std::filesystem::path(kProfileDir));
}
std::filesystem::path ProfilePath(std::string_view name) {
    const auto encoded = EncodeProfileName(name);
    if (encoded.empty()) return {};
    return ProfilesDir() / std::filesystem::u8path(encoded + ".json");
}
std::filesystem::path CurrentOutfitPath() { return gCurrentProfilePath; }

std::optional<ConfigStamp> CurrentConfigStamp() {
    try {
        const auto p = CurrentOutfitPath();
        if (p.empty() || !std::filesystem::exists(p)) return ConfigStamp{};
        return ConfigStamp{true, std::filesystem::file_size(p), std::filesystem::last_write_time(p)};
    } catch (const std::exception& e) {
        spdlog::warn("[Config] cannot inspect profile: {}", e.what());
        return std::nullopt;
    }
}

void ClearEquipSignals();
const char* SceneName(Scene s);
Scene Classify(RE::PlayerCharacter* player);


std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}


const char* LevelName(int level) {
    static constexpr const char* names[] = {"trace","debug","info","warn","error","critical","off"};
    return level >= 0 && level < static_cast<int>(std::size(names)) ? names[level] : "info";
}
std::filesystem::path Ini() { return std::filesystem::absolute(std::filesystem::path(kIniPath)); }
void SetLevel(int level) {
    level = std::clamp(level, 0, 6);
    gLevel.store(level);
    spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}
void LoadSettings() {
    const auto p = Ini();
    gEnabled.store(::GetPrivateProfileIntW(L"General", L"Enabled", 0, p.c_str()) != 0);
    wchar_t buf[32]{};
    ::GetPrivateProfileStringW(L"Logging", L"Level", L"info", buf, 32, p.c_str());
    for (int i = 0; i <= 6; ++i) {
        const std::wstring candidate = [&] { std::wstring s; for (char c : std::string_view(LevelName(i))) s.push_back(static_cast<wchar_t>(c)); return s; }();
        if (::_wcsicmp(buf, candidate.c_str()) == 0) { SetLevel(i); return; }
    }
    SetLevel(static_cast<int>(spdlog::level::info));
}

std::optional<ItemKey> MakeKey(const RE::TESForm* form) {
    if (!form) return std::nullopt;
    const auto* file = form->GetFile(0);
    if (!file) return std::nullopt;
    return ItemKey{std::string(file->GetFilename()), form->GetLocalFormID()};
}
RE::TESObjectARMO* FindArmor(const ItemKey& key) {
    auto* dh = RE::TESDataHandler::GetSingleton();
    return dh ? dh->LookupForm<RE::TESObjectARMO>(key.localID, key.mod) : nullptr;
}
std::uint32_t Mask(RE::TESObjectARMO* armor) {
    if (!armor) return 0;
    auto* bip = armor->As<RE::BGSBipedObjectForm>();
    return bip ? static_cast<std::uint32_t>(bip->GetSlotMask()) : 0;
}

bool Contains(const std::vector<ItemKey>& v, const ItemKey& item) {
    return std::find(v.begin(), v.end(), item) != v.end();
}

bool SaveGroups() {
    if (gConfigLoadBlocked) {
        spdlog::error("[Config] SAVE BLOCKED reason=invalid_profile_reload_required");
        gSaveFeedback = SaveFeedback::Failed;
        return false;
    }
    const auto p = CurrentOutfitPath();
    if (p.empty() || gCurrentProfileName.empty()) {
        spdlog::error("[Config] SAVE FAILED reason=no_active_character_profile");
        gSaveFeedback = SaveFeedback::Failed;
        return false;
    }
    const std::filesystem::path temp = p.wstring() + L".tmp";
    try {
        nlohmann::json j;
        j["schema"] = 1;
        for (int i = 0; i < 3; ++i) {
            auto& a = j["groups"][kPersistedGroupKeys[i]];
            a = nlohmann::json::array();
            for (const auto& k : gGroups[i]) {
                nlohmann::json entry{{"mod", k.mod}, {"localID", k.localID}};
                if (!k.variant.empty()) entry["variant"] = k.variant;
                a.push_back(std::move(entry));
            }
        }
        std::filesystem::create_directories(p.parent_path());
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) {
                spdlog::error("[Config] SAVE FAILED stage=open temp='{}'", temp.string());
                gSaveFeedback = SaveFeedback::Failed;
                return false;
            }
            file << j.dump(2) << '\n';
            file.flush();
            if (!file.good()) {
                spdlog::error("[Config] SAVE FAILED stage=write temp='{}'", temp.string());
                gSaveFeedback = SaveFeedback::Failed;
                return false;
            }
        }
        if (!::MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = ::GetLastError();
            spdlog::error("[Config] SAVE FAILED stage=replace win32={} temp='{}' path='{}'", error, temp.string(), p.string());
            gSaveFeedback = SaveFeedback::Failed;
            return false;
        }
        gSaveFeedback = SaveFeedback::Saved;
        gConfigLoadBlocked = false;
        gObservedConfigStamp = CurrentConfigStamp();
        spdlog::info("[Config] saved combat={} town={} home={} path='{}'",
            gGroups[0].size(), gGroups[1].size(), gGroups[2].size(), p.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Config] SAVE FAILED path='{}' error='{}'", p.string(), e.what());
        gSaveFeedback = SaveFeedback::Failed;
        return false;
    }
}
bool LoadGroups() {
    if (gCurrentProfilePath.empty()) return false;
    const auto stamp = CurrentConfigStamp();
    const auto p = CurrentOutfitPath();
    
    if (!stamp || !stamp->exists) {
        if (stamp && !stamp->exists) {
            for (auto& g : gGroups) g.clear();
            gObservedConfigStamp = stamp;
            gSaveFeedback = SaveFeedback::Untouched;
            gConfigLoadBlocked = false;
            spdlog::info("[Config] no outfit file; empty groups");
        } else {
            gConfigLoadBlocked = true;
            spdlog::error("[Config] cannot inspect outfit file; retaining current groups");
        }
        return stamp && !stamp->exists;
    }
    try {
        
        constexpr std::uintmax_t kMaxConfigBytes = 1024 * 1024;
        if (stamp->size == 0 || stamp->size > kMaxConfigBytes)
            throw std::runtime_error("empty or oversized outfit profile");
        std::ifstream f(p, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open outfit profile");
        const nlohmann::json j = nlohmann::json::parse(f);
        if (!j.is_object() || j.value("schema", 0) != 1 || !j.contains("groups") || !j["groups"].is_object())
            throw std::runtime_error("invalid SEM schema");
        const auto& groups = j["groups"];
        std::array<std::vector<ItemKey>, 3> next;
        for (int i = 0; i < 3; ++i) {
            const char* keyName = kPersistedGroupKeys[i];
            if (!groups.contains(keyName) || !groups[keyName].is_array())
                throw std::runtime_error(fmt::format("missing or invalid group '{}' (Combat/Town/Home required)", keyName));
            for (const auto& x : groups[keyName]) {
                if (!x.is_object() || !x.contains("mod") || !x["mod"].is_string() ||
                    !x.contains("localID") || !x["localID"].is_number_unsigned()) continue;
                ItemKey key{x["mod"].get<std::string>(), x["localID"].get<std::uint32_t>()};
                if (x.contains("variant") && x["variant"].is_string()) {
                    key.variant = x["variant"].get<std::string>();
                    if (key.variant.size() != 16 || key.variant.find_first_not_of("0123456789ABCDEF") != std::string::npos) continue;
                }
                if (key.mod.empty() || key.mod.size() > 260 || Contains(next[i], key)) continue;
                if (next[i].size() < 128) next[i].push_back(std::move(key));
            }
        }
        gGroups = std::move(next);
        gObservedConfigStamp = stamp;
        gSaveFeedback = SaveFeedback::Untouched;
        gConfigLoadBlocked = false;
        spdlog::info("[Config] groups loaded combat={} town={} home={} path=\"{}\"",
            gGroups[0].size(), gGroups[1].size(), gGroups[2].size(), p.string());
        return true;
    } catch (const std::exception& e) {
        
        spdlog::error("[Config] invalid outfit profile: {}; keeping current groups (rename Wilderness to Combat if upgrading)", e.what());
        gSaveFeedback = SaveFeedback::Failed;
        gConfigLoadBlocked = true;
        return false;
    }
}

bool EnsureCharacterProfile(RE::PlayerCharacter* player) {
    if (!player) return false;
    const char* raw = player->GetName();
    const std::string name = raw ? raw : "";
    if (name.empty()) return false;
    const auto path = ProfilePath(name);
    if (path.empty()) {
        spdlog::error("[Profile] unsupported character name length={}", name.size());
        return false;
    }
    if (name == gCurrentProfileName && path == gCurrentProfilePath) return true;
    const auto priorName = gCurrentProfileName;
    const auto priorPath = gCurrentProfilePath;
    const auto priorGroups = gGroups;
    gCurrentProfileName = name;
    gCurrentProfilePath = path;
    if (!LoadGroups()) {
        gCurrentProfileName = priorName;
        gCurrentProfilePath = priorPath;
        gGroups = priorGroups;
        spdlog::error("[Profile] switch failed; retaining prior profile");
        return false;
    }
    if (!priorName.empty()) {
        gCommittedWorn.clear();
        ClearEquipSignals();
        gActiveScene = Scene::Unknown;
    }
    gInventoryRows.clear();
    gInventorySeeded = false;
    gInventoryDirty.store(true);
    gLastMenuFrame = -1;
    gProfileDeleteSelection = -1;
    gConfirmProfileDelete = false;
    gProfileListDirty = true;
    spdlog::info("[Profile] switched character='{}' file='{}' combat={} town={} home={}",
        gCurrentProfileName, gCurrentProfilePath.string(),
        gGroups[0].size(), gGroups[1].size(), gGroups[2].size());
    return true;
}

bool ReloadCurrentCharacterProfile() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        spdlog::error("[Profile] manual reload failed reason=no_player");
        return false;
    }
    const char* raw = player->GetName();
    const std::string name = raw ? raw : "";
    const auto path = ProfilePath(name);
    if (name.empty() || path.empty()) {
        spdlog::error("[Profile] manual reload failed reason=invalid_character_name");
        return false;
    }
    const bool sameProfile = name == gCurrentProfileName && path == gCurrentProfilePath;
    bool loaded = false;
    if (sameProfile) {
        loaded = LoadGroups();
    } else {
        loaded = EnsureCharacterProfile(player);
    }
    if (!loaded) {
        gConfigLoadBlocked = true;
        gSaveFeedback = SaveFeedback::Failed;
        spdlog::error("[Profile] manual reload failed character='{}' path='{}'; automatic changes blocked", name, path.string());
        return false;
    }
    if (!gGameReady.load()) {
        if (gActiveScene != Scene::Unknown) {
            const int index = static_cast<int>(gActiveScene) - 1;
            if (index >= 0 && index < 3) {
                std::erase_if(gCommittedWorn, [&](const CommittedPiece& piece) {
                    if (Contains(gGroups[index], piece.key)) return false;
                    ItemKey legacy = piece.key;
                    legacy.variant.clear();
                    return piece.key.variant.empty() || !Contains(gGroups[index], legacy);
                });
            }
        }
        gGameReady.store(true);
        gLoading.store(false);
        gAttempted.store(false);
        gCombatOverride = false;
        gCombatEndedObserved.store(false);
        spdlog::info("[Session] recovered after profile reload; active={} automatic changes resume on next valid transition", SceneName(gActiveScene));
    }
    gInventoryDirty.store(true);
    gProfileListDirty = true;
    spdlog::info("[Profile] manual reload complete character='{}' combat={} town={} home={} no_immediate_equip=true",
        name, gGroups[0].size(), gGroups[1].size(), gGroups[2].size());
    return true;
}

bool ImportLegacyProfile() {
    if (gCurrentProfilePath.empty() || !gGameReady.load()) return false;
    if (std::any_of(gGroups.begin(), gGroups.end(), [](const auto& group) { return !group.empty(); })) return false;
    const auto legacy = std::filesystem::absolute(std::filesystem::path(kLegacyOutfitPath));
    if (!std::filesystem::exists(legacy)) return false;
    const auto active = gCurrentProfilePath;
    const auto oldStamp = gObservedConfigStamp;
    const auto oldGroups = gGroups;
    gCurrentProfilePath = legacy;
    const bool loaded = LoadGroups();
    gCurrentProfilePath = active;
    gObservedConfigStamp = oldStamp;
    if (!loaded) {
        gGroups = oldGroups;
        return false;
    }
    if (!SaveGroups()) {
        gGroups = oldGroups;
        gObservedConfigStamp = oldStamp;
        return false;
    }
    gInventoryDirty.store(true);
    spdlog::info("[Profile] legacy configuration imported character='{}' source='{}'", gCurrentProfileName, legacy.string());
    return true;
}

void RefreshProfileList() {
    gProfileFiles.clear();
    gProfileDeleteSelection = -1;
    gConfirmProfileDelete = false;
    gProfileListDirty = false;
    try {
        const auto dir = ProfilesDir();
        if (!std::filesystem::exists(dir)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::error_code equivalentEc;
            if (!gCurrentProfilePath.empty() && std::filesystem::exists(gCurrentProfilePath) &&
                std::filesystem::equivalent(entry.path(), gCurrentProfilePath, equivalentEc) && !equivalentEc) continue;
            gProfileFiles.push_back(entry.path());
        }
        std::sort(gProfileFiles.begin(), gProfileFiles.end());
    } catch (const std::exception& e) {
        spdlog::error("[Profile] listing failed: {}", e.what());
    }
}

bool SyncOutfitFileIfChanged() {
    const auto actual = CurrentConfigStamp();
    if (!actual) {
        gConfigLoadBlocked = true;
        spdlog::error("[Config] profile inspection failed; outfit application blocked");
        return false;
    }
    if (gObservedConfigStamp && *actual == *gObservedConfigStamp) return !gConfigLoadBlocked;
    spdlog::info("[Config] external change detected; reloading active character profile");
    if (!LoadGroups()) return false;
    gActiveScene = Scene::Unknown;  
    gCommittedWorn.clear();
    ClearEquipSignals();
    gInventoryRows.clear();
    gInventorySeeded = false;
    gInventoryDirty.store(true);
    spdlog::info("[Config] external reload applied combat={} town={} home={}",
        gGroups[0].size(), gGroups[1].size(), gGroups[2].size());
    return true;
}

struct InstanceInspection {
    std::int32_t count{};
    unsigned extraLists{};
    bool enchantment{};
    bool tempered{};
    bool renamed{};
};
InstanceInspection InspectEntry(std::int32_t count, const RE::InventoryEntryData* entry) {
    InstanceInspection result{};
    result.count = count;
    if (!entry || !entry->extraLists) return result;
    for (const auto* extra : *entry->extraLists) {
        if (!extra) continue;
        ++result.extraLists;
        result.enchantment |= extra->HasType<RE::ExtraEnchantment>();
        result.tempered |= extra->HasType<RE::ExtraHealth>();
        result.renamed |= extra->HasType<RE::ExtraTextDisplayData>();
    }
    return result;
}
InstanceInspection InspectPlayerCopies(RE::Actor* player, RE::TESObjectARMO* armor) {
    if (!player || !armor) return {};
    const auto inventory = player->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end()) return {};
    return InspectEntry(it->second.first, it->second.second.get());
}

void LogDuplicateVariants(RE::Actor* actor, RE::TESObjectARMO* armor, const char* context) {
    if (!actor || !armor) return;
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || it->second.first <= 1 || !it->second.second) return;
    const auto* entry = it->second.second.get();
    std::size_t n = 0;
    if (entry->extraLists) {
        for (const auto* extra : *entry->extraLists) {
            if (!extra) continue;
            if (++n > 12) break;
            const auto* ench = extra->GetByType<RE::ExtraEnchantment>();
            const auto* health = extra->GetByType<RE::ExtraHealth>();
            const auto* label = extra->GetByType<RE::ExtraTextDisplayData>();
            spdlog::debug(
                "[INSTANCE_VARIANT] context={} actor={:08X} form={:08X} record={} enchant={:08X} healthPresent={} health={} customName={} worn={}",
                context, actor->GetFormID(), armor->GetFormID(), n,
                ench && ench->enchantment ? ench->enchantment->GetFormID() : 0u,
                health != nullptr, health ? health->health : 1.0f,
                label != nullptr,
                extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>());
        }
    }
    spdlog::debug("[INSTANCE_VARIANT] context={} actor={:08X} form={:08X} total={} explicitLists={} noStandaloneIdentityGuaranteed=true",
        context, actor->GetFormID(), armor->GetFormID(), it->second.first, n);
}

std::uint64_t Mix64(std::uint64_t h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) { h ^= ((v >> (8 * i)) & 0xFFu); h *= 1099511628211ull; }
    return h;
}
std::uint64_t MixString(std::uint64_t h, std::string_view s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return Mix64(h, s.size());
}
std::string InstanceFingerprint(const RE::ExtraDataList* extra) {
    if (!extra) return {};

    if (auto* count = extra->GetByType<RE::ExtraCount>(); count && count->count != 1) return {};
    auto* enchant = extra->GetByType<RE::ExtraEnchantment>();
    auto* health = extra->GetByType<RE::ExtraHealth>();
    auto* display = extra->GetByType<RE::ExtraTextDisplayData>();
    const bool hasEnchant = enchant && enchant->enchantment;
    const bool hasName = display && display->IsPlayerSet() && display->displayName.c_str() && display->displayName.c_str()[0];
    if (!hasEnchant && !health && !hasName) return {};
    std::uint64_t h = 14695981039346656037ull;
    h = Mix64(h, 0x53454D303635ull);
    h = Mix64(h, hasEnchant);
    if (hasEnchant) {
        h = Mix64(h, enchant->charge);
        const auto& effects = enchant->enchantment->effects;
        h = Mix64(h, effects.size());
        for (auto* effect : effects) {
            if (!effect || !effect->baseEffect) return {}; 
            const auto* magic = effect->baseEffect;
            const auto* file = magic->GetFile(0);
            if (!file) return {};
            const auto filename = file->GetFilename();  
            if (filename.empty()) return {};
            h = MixString(h, filename);
            h = Mix64(h, magic->GetLocalFormID());
            h = Mix64(h, std::bit_cast<std::uint32_t>(effect->effectItem.magnitude));
            h = Mix64(h, effect->effectItem.area);
            h = Mix64(h, effect->effectItem.duration);
        }
    }
    h = Mix64(h, static_cast<bool>(health));
    if (health) h = Mix64(h, std::bit_cast<std::uint32_t>(health->health));
    h = Mix64(h, hasName);
    if (hasName) h = MixString(h, display->displayName.c_str());
    return fmt::format("{:016X}", h);
}

std::string InstanceLabel(const RE::ExtraDataList* extra, std::string_view base) {
    if (!extra) return std::string(base);
    std::string primary(base);
    if (const auto* text = extra->GetByType<RE::ExtraTextDisplayData>();
        text && text->IsPlayerSet() && text->displayName.c_str() && text->displayName.c_str()[0]) {
        primary = text->displayName.c_str();
    }
    std::vector<std::string> effects;
    if (const auto* ench = extra->GetByType<RE::ExtraEnchantment>(); ench && ench->enchantment) {

        for (const auto* effect : ench->enchantment->effects) {
            if (!effect || !effect->baseEffect) continue;
            const auto* name = effect->baseEffect->GetName();
            if (name && name[0]) effects.emplace_back(name);
        }
        if (effects.empty()) {
            const auto* name = ench->enchantment->GetName();
            if (name && name[0]) effects.emplace_back(name);
        }
    }
    std::optional<float> temper;
    if (const auto* health = extra->GetByType<RE::ExtraHealth>(); health &&
        std::isfinite(health->health) && health->health != 1.0f) {
        temper = health->health;
    }
    return FormatInstanceName(primary, effects, temper);
}

struct CopyLedger {
    std::int32_t total{};
    std::int32_t plain{};
    std::int32_t explicitPlain{};
    std::int32_t implicitPlain{};
    std::int32_t identifiedSpecial{};
    std::int32_t unresolved{};
    bool consistent{};
    std::string soleSpecialFingerprint;
};
std::int32_t UnitsInList(const RE::ExtraDataList* extra) {
    if (!extra) return 0;
    const auto* count = extra->GetByType<RE::ExtraCount>();
    return count ? static_cast<std::int32_t>(count->count) : 1;
}
bool HasSpecialIdentity(const RE::ExtraDataList* extra) {
    if (!extra) return false;
    const auto* ench = extra->GetByType<RE::ExtraEnchantment>();
    const auto* health = extra->GetByType<RE::ExtraHealth>();
    const auto* name = extra->GetByType<RE::ExtraTextDisplayData>();

    const bool custom = name && name->IsPlayerSet() && name->displayName.c_str() &&
        name->displayName.c_str()[0];
    return ench || health || custom;
}
CopyLedger InspectCopyLedger(std::int32_t total, const RE::InventoryEntryData* entry) {
    CopyLedger result{};
    result.total = total;
    if (!entry || total < 1) return result;
    std::int32_t explicitPlain = 0;
    std::int32_t identifiedSpecial = 0;
    std::int32_t unresolved = 0;
    bool bad = false;
    unsigned specialLists = 0;
    if (entry->extraLists) {
        for (const auto* extra : *entry->extraLists) {
            if (!extra) continue;
            const auto units = UnitsInList(extra);
            if (units < 1 || units > total) { bad = true; continue; }
            if (!HasSpecialIdentity(extra)) {
                explicitPlain += units;
            } else {
                const auto fingerprint = InstanceFingerprint(extra);
                if (fingerprint.empty()) {
                    unresolved += units;
                } else {
                    identifiedSpecial += units;
                    ++specialLists;
                    if (specialLists == 1) result.soleSpecialFingerprint = fingerprint;
                }
            }
        }
    }
    const auto plain = InferPlainCopies(total, identifiedSpecial, explicitPlain, unresolved);
    if (bad || !plain) return result;
    result.explicitPlain = explicitPlain;
    result.identifiedSpecial = identifiedSpecial;
    result.unresolved = unresolved;
    result.plain = *plain;
    result.implicitPlain = result.plain - result.explicitPlain;
    result.consistent = true;
    if (specialLists != 1 || identifiedSpecial != 1) result.soleSpecialFingerprint.clear();
    return result;
}
bool IsWornList(const RE::ExtraDataList* extra) {
    return extra && (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>());
}
bool IsPlainWorn(const RE::InventoryEntryData* entry, const CopyLedger& ledger) {
    if (!entry || !CanSelectPlain(ledger.consistent, ledger.plain, ledger.unresolved) || !entry->extraLists) return false;
    for (const auto* extra : *entry->extraLists) {
        if (extra && !HasSpecialIdentity(extra) && UnitsInList(extra) == 1 && IsWornList(extra)) return true;
    }
    
    return ledger.total == 1 && entry->IsWorn();
}

RE::ExtraDataList* FindLivePlain(RE::Actor* actor, RE::TESObjectARMO* armor) {
    if (!actor || !armor) return nullptr;
    auto* changes = actor->GetInventoryChanges();
    if (!changes || !changes->entryList) return nullptr;
    RE::ExtraDataList* found = nullptr;
    unsigned matches = 0;
    for (auto* entry : *changes->entryList) {
        if (!entry || entry->object != armor || !entry->extraLists) continue;
        for (auto* extra : *entry->extraLists) {
            if (extra && !HasSpecialIdentity(extra) && UnitsInList(extra) == 1) {
                found = extra;
                ++matches;
            }
        }
    }
    return matches == 1 ? found : nullptr;
}

RE::ExtraDataList* FindLiveVariant(RE::Actor* actor, RE::TESObjectARMO* armor, std::string_view fingerprint) {
    if (!actor || !armor || fingerprint.empty()) return nullptr;
    auto* changes = actor->GetInventoryChanges();
    if (!changes || !changes->entryList) return nullptr;
    RE::ExtraDataList* found = nullptr;
    unsigned matches = 0;
    for (auto* entry : *changes->entryList) {
        if (!entry || entry->object != armor || !entry->extraLists) continue;
        for (auto* extra : *entry->extraLists) {
            if (!extra || InstanceFingerprint(extra) != fingerprint) continue;
            ++matches;
            found = extra;
        }
    }
    return UniqueInstanceMatch(1, matches) ? found : nullptr;
}
unsigned CountSnapshotVariants(RE::Actor* actor, RE::TESObjectARMO* armor, std::string_view variant) {
    if (!actor || !armor || variant.empty()) return 0;
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || it->second.first < 1 || !it->second.second || !it->second.second->extraLists) return 0;
    unsigned count = 0;
    for (const auto* extra : *it->second.second->extraLists) {
        if (extra && InstanceFingerprint(extra) == variant) ++count;
    }
    return count;
}
bool CanManageKey(RE::Actor* actor, RE::TESObjectARMO* armor, const ItemKey& key) {

    if (!actor || !armor) return false;
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || it->second.first < 1 || !it->second.second) return false;
    if (key.variant.empty()) {
        const auto ledger = InspectCopyLedger(it->second.first, it->second.second.get());
        if (CanSelectPlain(ledger.consistent, ledger.plain, ledger.unresolved)) {

            return ledger.explicitPlain == 0 || FindLivePlain(actor, armor) != nullptr;
        }

        return ledger.consistent && ledger.total == 1 && ledger.plain == 0 &&
            !ledger.soleSpecialFingerprint.empty() &&
            FindLiveVariant(actor, armor, ledger.soleSpecialFingerprint) != nullptr;
    }
    return UniqueInstanceMatch(CountSnapshotVariants(actor, armor, key.variant),
        FindLiveVariant(actor, armor, key.variant) ? 1u : 0u);
}

bool IsKeyWorn(RE::Actor* actor, RE::TESObjectARMO* armor, const ItemKey& key) {

    if (!actor || !armor) return false;
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || !it->second.second) return false;
    const auto* entry = it->second.second.get();
    if (key.variant.empty()) {
        const auto ledger = InspectCopyLedger(it->second.first, entry);
        if (CanSelectPlain(ledger.consistent, ledger.plain, ledger.unresolved))
            return IsPlainWorn(entry, ledger);
        if (ledger.consistent && ledger.total == 1 && ledger.plain == 0 &&
            !ledger.soleSpecialFingerprint.empty()) {
            if (!entry->extraLists) return false;
            for (const auto* extra : *entry->extraLists) {
                if (extra && InstanceFingerprint(extra) == ledger.soleSpecialFingerprint && IsWornList(extra)) return true;
            }
        }
        return false;
    }
    if (!UniqueInstanceMatch(CountSnapshotVariants(actor, armor, key.variant), 1u)) return false;
    if (!entry->extraLists) return false;
    for (const auto* extra : *entry->extraLists) {
        if (extra && InstanceFingerprint(extra) == key.variant && IsWornList(extra)) return true;
    }
    return false;
}
void RefreshInventory() {
    if (!gGameReady.load()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell()) return;
    if (!EnsureCharacterProfile(player)) return;
    if (!SyncOutfitFileIfChanged()) return;
    gInventoryRows.clear();
    gInventorySeeded = false;
    RE::Actor* subject = player;
    const auto inv = subject->GetInventory();
    for (const auto& [object, data] : inv) {
        if (!object || data.first <= 0 || !data.second) continue;
        auto* armor = object->As<RE::TESObjectARMO>();
        if (!armor) continue;
        const auto mask = Mask(armor);
        if (!IsAllowedMask(mask)) continue;
        auto base = MakeKey(armor);
        if (!base) continue;

        const auto* name = armor->GetName();
        const auto baseName = std::string(name && name[0] ? name : "Unnamed equipment");
        const auto& groups = gGroups;
        const auto selectedInAnyGroup = [&](const ItemKey& key) {
            return std::any_of(groups.begin(), groups.end(),
                [&](const auto& group) { return Contains(group, key); });
        };
        const auto ledger = InspectCopyLedger(data.first, data.second.get());
        std::size_t variantRows = 0;
        std::size_t uniqueVariantRows = 0;
        const auto rowsBeforeVariants = gInventoryRows.size();
        std::unordered_set<std::string> seen;
        if (data.second->extraLists) {
            for (const auto* extra : *data.second->extraLists) {
                if (!extra) continue;
                const auto fingerprint = InstanceFingerprint(extra);
                if (fingerprint.empty() || !seen.insert(fingerprint).second) continue;
                ItemKey key = *base;
                key.variant = fingerprint;
                const bool unique = CountSnapshotVariants(subject, armor, fingerprint) == 1 &&
                    FindLiveVariant(subject, armor, fingerprint) != nullptr;
                const auto label = InstanceLabel(extra, baseName);
                gInventoryRows.push_back({std::move(key), armor->GetFormID(), label, mask,
                    data.first, IsWornList(extra), !unique});
                ++variantRows;
                if (unique) ++uniqueVariantRows;
            }
        }

        const bool soleVerified = ledger.consistent && ledger.plain == 0 &&
            HideRedundantBaseRow(data.first, uniqueVariantRows, variantRows) &&
            gInventoryRows.size() == rowsBeforeVariants + 1;
        if (soleVerified) {
            gInventoryRows.back().soleVerifiedVariant = true;
            spdlog::debug("[MENU_INSTANCE_ROW] form={:08X} count=1 action=show_verified_only legacySelected={}",
                armor->GetFormID(), selectedInAnyGroup(*base));
        }
        if (ledger.consistent && ledger.plain > 0) {

            const bool unique = CanSelectPlain(ledger.consistent, ledger.plain, ledger.unresolved);
            gInventoryRows.push_back({std::move(*base), armor->GetFormID(), baseName, mask,
                ledger.plain, IsPlainWorn(data.second.get(), ledger), !unique});
        } else if (!ledger.consistent) {
            spdlog::warn("[MENU_COPY_LEDGER] form={:08X} total={} status=inconsistent action=protect",
                armor->GetFormID(), data.first);
        }

    }
    std::sort(gInventoryRows.begin(),gInventoryRows.end(),[](const InventoryRow& a,const InventoryRow& b){ return a.name < b.name; });
    gInventorySeeded = true;

    spdlog::debug("[Inventory] auto-refreshed eligible={}",gInventoryRows.size());
}

std::vector<WornArmor> ReadWorn(RE::Actor* actor) {

    std::vector<WornArmor> result;
    if (!actor) return result;
    for (const auto& [object, data] : actor->GetInventory()) {
        if (!object || !data.second || !data.second->IsWorn()) continue;
        auto* armor = object->As<RE::TESObjectARMO>();
        if (!armor) continue;
        auto base = MakeKey(armor);
        if (!base) continue;
        bool foundExplicitWorn = false;
        if (data.second->extraLists) {
            for (const auto* extra : *data.second->extraLists) {
                if (!IsWornList(extra)) continue;
                ItemKey instance = *base;
                instance.variant = InstanceFingerprint(extra);
                result.push_back({armor, armor->GetFormID(), Mask(armor), std::move(instance)});
                foundExplicitWorn = true;
            }
        }
        if (!foundExplicitWorn) {
            
            result.push_back({armor, armor->GetFormID(), Mask(armor), std::move(*base)});
        }
    }
    return result;
}

void ClearEquipSignals() {
    std::scoped_lock lock(gEquipSignalMutex);
    gEquipSignals.clear();
    gEquipSignalsOverflow = false;
}
struct EquipSignalBatch {
    std::vector<EquipSignal> signals;
    bool overflow{};
};
EquipSignalBatch ObserveEquipSignals() {
    std::scoped_lock lock(gEquipSignalMutex);
    return {gEquipSignals, gEquipSignalsOverflow};
}

bool HasUniqueCopy(RE::PlayerCharacter* player, RE::TESObjectARMO* armor);

bool IsRequestedWornCopy(RE::Actor* actor, RE::TESObjectARMO* armor,
    const ItemKey& target, const WornArmor& actual);

std::vector<CommittedPiece> BuildSourceEvidence(
    RE::PlayerCharacter* player, Scene source, const EquipSignalBatch& batch,
    bool emitLog = true) {
    auto evidence = gCommittedWorn;
    if (!player || source == Scene::Unknown) return evidence;
    if (batch.overflow) {
        if (emitLog) spdlog::warn("[OWNERSHIP_EVIDENCE] equip_event_overflow=true action=retain_proof_without_promotions");
        return evidence;
    }
    const auto before = evidence.size();
    std::erase_if(evidence, [&](const CommittedPiece& piece) {
        auto* armor = FindArmor(piece.key);
        return armor && std::any_of(batch.signals.begin(), batch.signals.end(),
            [&](const EquipSignal& event) {
                return event.equipped && event.form == armor->GetFormID();
            });
    });
    if (emitLog) spdlog::debug("[OWNERSHIP_EVIDENCE] committed={} events={} retiredManual={} total={} overflow={}",
        gCommittedWorn.size(), batch.signals.size(), before - evidence.size(), evidence.size(), batch.overflow);
    return evidence;
}

void CaptureCommittedWorn(Scene scene) {
    gCommittedWorn.clear();
    const int idx = static_cast<int>(scene) - 1;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || idx < 0 || idx >= 3) return;
    const auto current = ReadWorn(player);
    for (const auto& selectedKey : gGroups[idx]) {
        auto* armor = FindArmor(selectedKey);
        const auto mask = Mask(armor);
        if (!armor || !IsAllowedMask(mask) || !CanManageKey(player, armor, selectedKey) ||
            !IsKeyWorn(player, armor, selectedKey)) continue;

        const bool conflicted = std::any_of(current.begin(), current.end(), [&](const WornArmor& worn) {
            return Conflicts(worn.mask, mask) &&
                !IsRequestedWornCopy(player, armor, selectedKey, worn);
        });
        if (conflicted) continue;
        for (const auto& worn : current) {
            if (IsRequestedWornCopy(player, armor, selectedKey, worn)) {
                if (!HasCommittedIdentity(gCommittedWorn, worn.key)) {
                    gCommittedWorn.push_back({worn.key, mask});
                    spdlog::debug("[OWNERSHIP_SOURCE] scene={} form={:08X} variant={} mask={:08X} proof=worn_verified",
                        static_cast<int>(scene), worn.formID, worn.key.variant, mask);
                }
                break;
            }
        }
    }
    spdlog::debug("[Ownership] committed group={} items={} selected={}",
        static_cast<int>(scene), gCommittedWorn.size(), gGroups[idx].size());
}

const char* SceneName(Scene s) {
    switch(s){case Scene::Combat:return "Combat";case Scene::Town:return "Town";case Scene::Home:return "Home";default:return "Unknown";}
}
Scene Classify(RE::PlayerCharacter* player) {
    if (!player) return Scene::Unknown;
    auto* cell=player->GetParentCell();
    if (!cell) return Scene::Unknown;
    auto* loc=player->GetCurrentLocation();
    if (!loc) loc=cell->GetLocation();
    bool town=false,dungeon=false,home=false;
    std::unordered_set<RE::FormID> visited;
    for (int depth=0;loc && depth<24;++depth) {
        if (!visited.insert(loc->GetFormID()).second) break;
        home |= loc->HasKeywordString("LocTypePlayerHouse");
        dungeon |= loc->HasKeywordString("LocTypeDungeon");
        town |= loc->HasKeywordString("LocTypeCity") || loc->HasKeywordString("LocTypeTown") || loc->HasKeywordString("LocTypeHabitation");
        loc=loc->parentLoc;
    }
    if (home) return Scene::Home;
    
    if (dungeon) return Scene::Combat;
    if (town) return Scene::Town;
    return cell->IsExteriorCell()?Scene::Combat:Scene::Unknown;
}


bool HasUniqueCopy(RE::PlayerCharacter* player, RE::TESObjectARMO* armor) {
    if (!player || !armor) return false;
    auto inventory = player->GetInventory();
    const auto it = inventory.find(armor);
    return it != inventory.end() && IsUnambiguousBaseCopy(
        it->second.first, static_cast<bool>(it->second.second));
}

bool IsSourceCurrentlyWorn(const std::vector<WornArmor>& current,
    const CommittedPiece& source) {
    return std::any_of(current.begin(), current.end(), [&](const WornArmor& worn) {
        return worn.key == source.key;
    });
}

std::vector<ItemKey> FindTemporaryOverrides(
    RE::PlayerCharacter* player,
    const std::vector<WornArmor>& current,
    Scene previous, Scene next,
    const std::vector<CommittedPiece>& sourceEvidence) {
    std::vector<ItemKey> overrides;
    if (!player || previous == Scene::Unknown || previous == next || sourceEvidence.empty()) return overrides;
    std::vector<CommittedPiece> missing;
    const int previousIndex = static_cast<int>(previous) - 1;
    for (const auto& source : sourceEvidence) {
        if (previousIndex < 0 || previousIndex >= 3) continue;
        const bool stillConfigured = std::any_of(gGroups[previousIndex].begin(),
            gGroups[previousIndex].end(), [&](const ItemKey& selected) {
                return selected == source.key ||
                    (selected.variant.empty() && selected.mod == source.key.mod &&
                        selected.localID == source.key.localID);
            });
        if (stillConfigured && !IsSourceCurrentlyWorn(current, source))
            missing.push_back(source);
    }
    if (missing.empty()) return overrides;

    struct Candidate {
        WornArmor worn;
        std::size_t sourceIndex{};
        unsigned sourceMatches{};
    };
    std::vector<Candidate> candidates;
    std::vector<unsigned> perSourceCandidates(missing.size());
    for (const auto& worn : current) {
        if (!IsAllowedMask(worn.mask)) continue;
        Candidate c{worn};
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (IsTemporaryOverrideShape(missing[i].mask, worn.mask)) {
                ++c.sourceMatches;
                c.sourceIndex = i;
            }
        }
        if (c.sourceMatches) {
            if (c.sourceMatches == 1) ++perSourceCandidates[c.sourceIndex];
            candidates.push_back(std::move(c));
        }
    }
    for (const auto& c : candidates) {
        auto* armor = c.worn.armor;

        const bool manageable = armor && CanManageKey(player, armor, c.worn.key);
        const bool addressable = manageable &&
            (!c.worn.key.variant.empty() || FindLivePlain(player, armor) != nullptr ||
             HasUniqueCopy(player, armor));
        const unsigned count = c.sourceMatches == 1 ? perSourceCandidates[c.sourceIndex] : 0;
        const auto& from = missing[c.sourceIndex];
        if (CanReleaseTemporaryOverride(previous, next, false, false, addressable,
                                        c.sourceMatches, count, from.mask, c.worn.mask) &&
            !Contains(overrides, c.worn.key)) {
            overrides.push_back(c.worn.key);
            spdlog::info("[TEMP_OVERRIDE] previous={} target={} source={:08X} sourceVariant={} replacement={:08X} replacementVariant={} mask={:08X} proof=previous_exact_worn",
                static_cast<int>(previous), static_cast<int>(next),
                FindArmor(from.key) ? FindArmor(from.key)->GetFormID() : 0u,
                from.key.variant, c.worn.formID, c.worn.key.variant, c.worn.mask);
        } else {
            spdlog::debug("[TEMP_OVERRIDE_SKIP] form={:08X} variant={} missingMatches={} replacementCandidates={} manageable={} addressable={} action=protect",
                c.worn.formID, c.worn.key.variant, c.sourceMatches, count, manageable, addressable);
        }
    }
    return overrides;
}

struct ApplySummary {
    int equipped{};
    int unequipped{};
    int alreadyWorn{};
    int skipped{};
    int nativeFailed{};
    int rollbackFailed{};
};
void LogItem(Scene scene, RE::FormID formID, std::uint32_t mask,
             const char* status, RE::FormID conflict = 0) {
    spdlog::debug("[OUTFIT_ITEM] group={} form={:08X} mask={:08X} status={} conflict={:08X}",
        SceneName(scene), formID, mask, status, conflict);
}

struct EquipPolicy {
    bool forceEquip{};
    const std::vector<ItemKey>* temporary{};
    bool crossGroup{};
};
RE::ExtraDataList* NativeInstance(RE::Actor* actor, RE::TESObjectARMO* armor, const ItemKey& key) {

    if (!key.variant.empty()) return FindLiveVariant(actor, armor, key.variant);
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || !it->second.second) return nullptr;
    const auto ledger = InspectCopyLedger(it->second.first, it->second.second.get());
    if (CanSelectPlain(ledger.consistent, ledger.plain, ledger.unresolved))
        return FindLivePlain(actor, armor); 
    if (ledger.consistent && ledger.total == 1 && ledger.plain == 0 &&
        !ledger.soleSpecialFingerprint.empty())
        return FindLiveVariant(actor, armor, ledger.soleSpecialFingerprint); 
    return nullptr;
}
bool EquipNative(RE::Actor* actor, RE::TESObjectARMO* armor, const ItemKey& key, const EquipPolicy& policy) {
    auto* manager = RE::ActorEquipManager::GetSingleton();
    if (!manager || !CanManageKey(actor, armor, key)) return false;
    auto* extra = NativeInstance(actor, armor, key);
    if (!key.variant.empty() && !extra) return false;
    {

        manager->EquipObject(actor, armor, extra, 1, armor->GetEquipSlot(), false, policy.forceEquip, false, true);
    }
    return true;
}
bool UnequipNative(RE::Actor* actor, RE::TESObjectARMO* armor, const ItemKey& key, const EquipPolicy& policy) {
    auto* manager = RE::ActorEquipManager::GetSingleton();
    if (!manager || !CanManageKey(actor, armor, key)) return false;
    auto* extra = NativeInstance(actor, armor, key);
    if (!key.variant.empty() && !extra) return false;
    if (key.variant.empty() && !extra) {
        const auto inspection = InspectPlayerCopies(actor, armor);
        if (inspection.count != 1) {
            spdlog::warn("[PLAIN_UNEQUIP_BLOCKED] actor={:08X} form={:08X} reason=no_live_plain_instance_in_multicopy",
                actor->GetFormID(), armor->GetFormID());
            return false;
        }
    }

    return manager->UnequipObject(actor, armor, extra, 1, armor->GetEquipSlot(), false,
        policy.forceEquip, false, true, nullptr);
}
void RestoreRemoved(RE::Actor* actor, const std::vector<WornArmor>& removed,
                    const EquipPolicy& policy, ApplySummary& summary) {
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
        bool blocked = false;
        for (const auto& now : ReadWorn(actor)) {
            if ((now.formID != it->formID || now.key.variant != it->key.variant) && Conflicts(now.mask, it->mask)) { blocked = true; break; }
        }
        const bool requested = !blocked && EquipNative(actor, it->armor, it->key, policy);
        if (requested && IsKeyWorn(actor, it->armor, it->key)) {
            spdlog::debug("[OUTFIT_ROLLBACK] actor={:08X} form={:08X} status=restored",
                actor->GetFormID(), it->formID);
        } else {
            ++summary.rollbackFailed;
            spdlog::error("[OUTFIT_ROLLBACK] actor={:08X} form={:08X} status=FAILED",
                actor->GetFormID(), it->formID);
        }
    }
}

bool IsRequestedWornCopy(RE::Actor* actor, RE::TESObjectARMO* armor,
    const ItemKey& target, const WornArmor& actual) {
    if (actual.armor != armor) return false;
    if (SameSelectedInstance(target.variant, actual.key.variant)) return true;
    if (!target.variant.empty() || actual.key.variant.empty()) return false;
    const auto inventory = actor->GetInventory();
    const auto it = inventory.find(armor);
    if (it == inventory.end() || !it->second.second) return false;
    const auto ledger = InspectCopyLedger(it->second.first, it->second.second.get());
    return ledger.consistent && ledger.total == 1 && ledger.plain == 0 &&
        ledger.soleSpecialFingerprint == actual.key.variant;
}
ApplySummary ApplyOutfitCore(RE::Actor* actor, const std::vector<ItemKey>& group,
    Scene scene, std::uint64_t epoch, const EquipPolicy& policy) {
    ApplySummary summary;
    if (!actor || actor->GetFormID() != 0x14) { ++summary.nativeFailed; return summary; }
    const auto started = std::chrono::steady_clock::now();
    std::uint32_t groupMaskSeen = 0;
    spdlog::debug("[UNIFIED_PLAN] epoch={} actor={:08X} owner=player group={} selected={} worn={}",
        epoch, actor->GetFormID(), SceneName(scene), group.size(), ReadWorn(actor).size());
    for (const auto& key : group) {
        auto* target = FindArmor(key);
        const auto targetID = target ? target->GetFormID() : 0u;
        const auto mask = Mask(target);
        if (!target || !IsAllowedMask(mask) || Conflicts(groupMaskSeen, mask)) {
            ++summary.skipped;
            spdlog::warn("[UNIFIED_ITEM] actor={:08X} form={:08X} status=invalid_or_overlapping",
                actor->GetFormID(), targetID);
            continue;
        }
        groupMaskSeen |= mask;
        auto worn = ReadWorn(actor);
        const auto inspection = InspectPlayerCopies(actor, target);
        if (!CanManageKey(actor, target, key)) {
            ++summary.skipped;
            spdlog::warn("[INSTANCE_UNRESOLVED] actor={:08X} form={:08X} variant={} count={} action=skip",
                actor->GetFormID(), targetID, key.variant, inspection.count);
            continue;
        }
        const bool targetInitiallyWorn = IsKeyWorn(actor, target, key);
        const bool otherWornConflict = std::any_of(worn.begin(), worn.end(), [&](const WornArmor& old) {
            return Conflicts(old.mask, mask) && !IsRequestedWornCopy(actor, target, key, old);
        });
        if (targetInitiallyWorn && !otherWornConflict) {
            ++summary.alreadyWorn;
            spdlog::debug("[UNIFIED_ITEM] actor={:08X} form={:08X} variant={} status=already_worn_exact",
                actor->GetFormID(), targetID, key.variant);
            continue;
        }
        std::vector<WornArmor> releases;
        RE::FormID conflict{};
        for (const auto& old : worn) {
            if (!Conflicts(old.mask, mask) ||
                IsRequestedWornCopy(actor, target, key, old)) continue;

            const bool managed = CanManageKey(actor, old.armor, old.key);
            const bool committed = HasCommittedCopy(gCommittedWorn, old.key, old.mask);
            const bool targetOverride = managed && IsAllowedMask(old.mask) &&
                IsAllowedMask(mask) && Conflicts(old.mask, mask);
            const bool permitted = targetOverride;
            spdlog::debug("[PLAYER_RELEASE_AUTH] actor={:08X} worn={:08X} variant={} committed={} manageable={} targetConfigured=true targetOverride={} allowed={}",
                actor->GetFormID(), old.formID, old.key.variant,
                committed, managed, targetOverride, permitted);
            if (!permitted) { conflict = old.formID; break; }
            releases.push_back(old);
        }
        if (conflict) {
            ++summary.skipped;
            spdlog::debug("[UNIFIED_ITEM] actor={:08X} form={:08X} status=external_or_ambiguous_conflict block={:08X}",
                actor->GetFormID(), targetID, conflict);
            continue;
        }
        std::vector<WornArmor> removed;
        bool failed = false;
        for (const auto& old : releases) {
            const bool native = UnequipNative(actor, old.armor, old.key, policy);

            const bool removedNow = !IsKeyWorn(actor, old.armor, old.key);
            spdlog::debug("[UNIFIED_UNEQUIP] actor={:08X} form={:08X} native={} removed={}",
                actor->GetFormID(), old.formID, native, removedNow);
            if (!removedNow) { failed = true; break; }
            removed.push_back(old);
            ++summary.unequipped;
        }
        if (!failed) {

            if (targetInitiallyWorn && IsKeyWorn(actor, target, key)) {
                ++summary.alreadyWorn;
                spdlog::info("[PLAYER_INSTANCE_CONFLICT_CLEARED] actor={:08X} form={:08X} variant={} removed={}",
                    actor->GetFormID(), targetID, key.variant, removed.size());
            } else {
                const bool requested = EquipNative(actor, target, key, policy);
                const bool confirmed = requested && IsKeyWorn(actor, target, key);
                spdlog::debug("[UNIFIED_EQUIP] actor={:08X} form={:08X} variant={} requested={} wornAfter={} blocked=false",
                    actor->GetFormID(), targetID, key.variant, requested, confirmed);
                if (confirmed) ++summary.equipped;
                else {
                    failed = true;
                    if (key.variant.empty()) {
                        spdlog::warn("[PLAIN_EQUIP_UNVERIFIED] actor={:08X} form={:08X} nativeRequested={} action=rollback_safe",
                            actor->GetFormID(), targetID, requested);
                        LogDuplicateVariants(actor, target, "plain_after_unverified_equip");
                    }
                }
            }
        }
        if (failed) {
            ++summary.nativeFailed;
            RestoreRemoved(actor, removed, policy, summary);
        }
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    spdlog::info("[OUTFIT_APPLY] epoch={} group={} equip={} unequip={} already={} skip={} failed={} rollbackFailed={} apply_us={}",
        epoch, SceneName(scene), summary.equipped, summary.unequipped,
        summary.alreadyWorn, summary.skipped, summary.nativeFailed, summary.rollbackFailed, micros);
    return summary;
}
bool VerifyPlayerOutfit(RE::PlayerCharacter* player, Scene scene) {
    const int index = static_cast<int>(scene) - 1;
    if (!player || index < 0 || index >= 3) return false;
    const auto worn = ReadWorn(player);
    for (const auto& key : gGroups[index]) {
        auto* armor = FindArmor(key);
        if (!armor || !IsAllowedMask(Mask(armor)) ||
            !CanManageKey(player, armor, key) || !IsKeyWorn(player, armor, key)) {
            spdlog::debug("[PLAYER_VERIFY] scene={} form={:08X} variant={} status=missing_or_ambiguous",
                SceneName(scene), armor ? armor->GetFormID() : 0u, key.variant);
            return false;
        }
        for (const auto& old : worn) {
            if (Conflicts(old.mask, Mask(armor)) && !IsRequestedWornCopy(player, armor, key, old)) {
                spdlog::warn("[PLAYER_VERIFY] scene={} form={:08X} variant={} status=conflicting_worn_copy other={:08X} otherVariant={}",
                    SceneName(scene), armor->GetFormID(), key.variant, old.formID, old.key.variant);
                return false;
            }
        }
    }
    return true;
}
ApplySummary ApplyGroup(Scene scene, std::uint64_t epoch) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto idx = static_cast<int>(scene) - 1;
    ApplySummary fail;
    if (!player || idx < 0 || idx >= 3) { ++fail.nativeFailed; return fail; }
    const auto events = ObserveEquipSignals();
    const auto evidence = BuildSourceEvidence(player, gActiveScene, events);
    if (!events.overflow) {
        const auto before = gCommittedWorn.size();
        std::erase_if(gCommittedWorn, [&](const CommittedPiece& piece) {
            return !HasCommittedIdentity(evidence, piece.key);
        });
        if (before != gCommittedWorn.size())
            spdlog::info("[OWNERSHIP_MANUAL_TAKEOVER] retired={} reason=manual_rewear", before - gCommittedWorn.size());
    }
    const auto temporary = FindTemporaryOverrides(player, ReadWorn(player),
        gActiveScene, scene, evidence);
    spdlog::debug("[OWNERSHIP_PLAN] epoch={} previous={} committedItems={} evidenceSources={} temporaryOverrides={}",
        epoch, static_cast<int>(gActiveScene), gCommittedWorn.size(), evidence.size(), temporary.size());
    return ApplyOutfitCore(player, gGroups[idx], scene, epoch, EquipPolicy{false, &temporary, gActiveScene != Scene::Unknown && gActiveScene != scene});
}

void ReleaseUncoveredCommitted(RE::PlayerCharacter* player, Scene target,
    std::uint64_t epoch, ApplySummary& summary) {
    if (!player || gActiveScene == Scene::Unknown || gActiveScene == target) return;
    const auto index = static_cast<int>(target) - 1;
    if (index < 0 || index >= 3) return;
    std::uint32_t targetMask = 0;
    for (const auto& key : gGroups[index]) {
        auto* armor = FindArmor(key);
        if (armor && IsAllowedMask(Mask(armor))) targetMask |= Mask(armor);
    }
    std::vector<WornArmor> removed;
    const EquipPolicy policy{false, nullptr};
    for (const auto& proof : gCommittedWorn) {
        if (!MayReleaseUncovered(gActiveScene, target, true, true,
            proof.mask, targetMask)) continue;
        const auto worn = ReadWorn(player);
        const auto found = std::find_if(worn.begin(), worn.end(), [&](const WornArmor& w) {
            return w.key == proof.key && w.mask == proof.mask;
        });
        if (found == worn.end()) continue; 
        const WornArmor old = *found;
        if (!old.armor || !CanManageKey(player, old.armor, old.key)) {
            spdlog::warn("[EMPTY_SLOT_RELEASE] epoch={} form={:08X} reason=ambiguous", epoch, old.formID);
            ++summary.skipped;
            continue;
        }
        const bool native = UnequipNative(player, old.armor, old.key, policy);
        const bool confirmed = !IsKeyWorn(player, old.armor, old.key);
        spdlog::info("[EMPTY_SLOT_RELEASE] epoch={} form={:08X} variant={} mask={:08X} native={} removed={}",
            epoch, old.formID, old.key.variant, old.mask, native, confirmed);
        if (confirmed) {
            ++summary.unequipped;
            removed.push_back(old);
        } else {
            ++summary.nativeFailed;
            RestoreRemoved(player, removed, policy, summary);
            return;
        }
    }
}

void CommitGroupChange(Scene target, std::uint64_t epoch, bool combatEntry = false) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || target == Scene::Unknown || !gEnabled.load()) return;
    if (!combatEntry && !ShouldSwitch(gActiveScene, target)) {
        spdlog::debug("[OUTFIT_SKIP] epoch={} reason=same_group group={}", epoch, SceneName(target));
        return;
    }

    InternalEquipMutationGuard mutationGuard;
    const auto perfStartUs = NowUs();
    if (!EnsureCharacterProfile(player)) return;
    if (!SyncOutfitFileIfChanged()) return;
    const Scene previous = gActiveScene;
    spdlog::info("[GROUP_CHANGE] epoch={} {} -> {} combatEntry={}",
        epoch, SceneName(previous), SceneName(target), combatEntry);
    auto summary = ApplyGroup(target, epoch);

    if (!summary.rollbackFailed)
        ReleaseUncoveredCommitted(player, target, epoch, summary);
    const bool matched = VerifyPlayerOutfit(player, target);
    const bool complete = IsOutfitCommitComplete(summary.skipped,
        summary.nativeFailed, summary.rollbackFailed, matched);

    const auto priorProof = gCommittedWorn;
    gActiveScene = target; 
    CaptureCommittedWorn(target);
    const auto stillWorn = ReadWorn(player);
    std::vector<CommittedPiece> wornProof;
    for (const auto& worn : stillWorn)
        if (IsAllowedMask(worn.mask)) wornProof.push_back({worn.key, worn.mask});
    const auto verifiedTargetCount = gCommittedWorn.size();
    gCommittedWorn = MergeVerifiedOwnership(gCommittedWorn, priorProof, wornProof);
    const auto retained = gCommittedWorn.size() - verifiedTargetCount;
    ClearEquipSignals();
    if (complete) {
        spdlog::info("[GROUP_COMMIT] epoch={} active={} status=complete retainedPrior={}",
            epoch, SceneName(target), retained);
    } else {
        spdlog::warn("[GROUP_COMMIT] epoch={} from={} target={} status=incomplete skipped={} failed={} rollbackFailed={} matched={} verifiedSources={} retainedPrior={}",
            epoch, SceneName(previous), SceneName(target), summary.skipped,
            summary.nativeFailed, summary.rollbackFailed, matched,
            gCommittedWorn.size(), retained);
    }
    
    spdlog::debug("[SEM_APPLY_TIME] epoch={} group={} total_us={} complete={}",
        epoch, SceneName(target), NowUs() - perfStartUs, complete);
}

void ProcessRuntimeCheck() {
    const auto flags = gPendingRequests.exchange(0);
    gRequestTaskQueued.store(false);
    if (!flags || !gGameReady.load() || !gEnabled.load() || gLoading.load() || gFaderOpen.load()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell() || !player->Is3DLoaded()) return;
    const bool combat = player->IsInCombat();
    const bool endedSinceLastCheck = gCombatEndedObserved.exchange(false);
    if (endedSinceLastCheck && gCombatOverride) {
        gCombatOverride = false;
        spdlog::info("[COMBAT_OVERRIDE] state=ended action=hold_outfit_reset_combat_gate");
    }
    if (combat && !gCombatOverride) {
        gCombatOverride = true;
        spdlog::info("[COMBAT_OVERRIDE] state=entered location={} effective=Combat", SceneName(Classify(player)));
        CommitGroupChange(Scene::Combat, gEpoch.load(), true);
    } else if (!combat && gCombatOverride) {
        gCombatOverride = false;
        spdlog::info("[COMBAT_OVERRIDE] state=ended action=hold_outfit_reset_combat_gate");
    }
}

void QueueRuntimeCheck(std::uint32_t flags) {
    if (!gGameReady.load() || !gEnabled.load() || gLoading.load()) return;
    gPendingRequests.fetch_or(flags);
    if (gRequestTaskQueued.exchange(true)) return;
    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask([] { ProcessRuntimeCheck(); });
    } else {
        gRequestTaskQueued.store(false);
        gPendingRequests.store(0);
    }
}

void TryApplyAtLoadingClose(std::uint64_t epoch) {
    if (!gGameReady.load() || !gEnabled.load() || !gLoading.load()) {
        spdlog::debug("[Transition] skip epoch={} reason=not_ready_or_disabled", epoch);
        return;
    }
    if (gAttempted.exchange(true)) {
        spdlog::debug("[Transition] skip epoch={} reason=already_attempted", epoch);
        return;
    }
    const auto faderOpen = gFaderOpen.load();
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* cell = player ? player->GetParentCell() : nullptr;
    const auto* loc = player ? player->GetCurrentLocation() : nullptr;
    const auto cellID = cell ? cell->GetFormID() : 0u;
    const auto locID = loc ? loc->GetFormID() : 0u;
    spdlog::debug("[CLOSE_BOUNDARY] epoch={} fader={} player3D={} cell={:08X} location={:08X}",
        epoch, faderOpen, player && player->Is3DLoaded(), cellID, locID);

    if (!faderOpen || !cell || !player->Is3DLoaded()) {
        spdlog::warn("[Transition] skip epoch={} reason=no_fader_or_player_3d", epoch);
        return;
    }
    if (!EnsureCharacterProfile(player)) return;
    if (!SyncOutfitFileIfChanged()) return;
    const auto target = Classify(player);
    spdlog::debug("[DESTINATION_READY] epoch={} cell={:08X} location={:08X} target={} active={}",
        epoch, cellID, locID, SceneName(target), SceneName(gActiveScene));
    if (target == Scene::Unknown) {
        spdlog::debug("[Transition] skip epoch={} reason=unknown_destination", epoch);
        return;
    }
    
    if (player->IsInCombat()) {
        if (gCombatEndedObserved.exchange(false)) gCombatOverride = false;
        if (!gCombatOverride) {
            gCombatOverride = true;
            CommitGroupChange(Scene::Combat, epoch, true);
        }
        return;
    }
    if (gCombatOverride) gCombatOverride = false;
    gCombatEndedObserved.store(false);
    CommitGroupChange(target, epoch);
}

class SEMEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                           public RE::BSTEventSink<RE::TESContainerChangedEvent>,
                           public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>,
                           public RE::BSTEventSink<RE::TESActorLocationChangeEvent>,
                           public RE::BSTEventSink<RE::TESEquipEvent>,
                           public RE::BSTEventSink<RE::TESCombatEvent>,
                           public RE::BSTEventSink<RE::TESHitEvent> {
public:
    static SEMEventSink* Get() { static SEMEventSink sink; return &sink; }

    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                           RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!e) return RE::BSEventNotifyControl::kContinue;
        if (e->menuName == "Loading Menu") {
            if (e->opening) {
                const auto epoch = gEpoch.fetch_add(1) + 1;
                gLoading.store(true);
                gAttempted.store(false);
                if (auto* player = RE::PlayerCharacter::GetSingleton())
                    ObservePlayerCombatState("loading_open", player);
                spdlog::debug("[TRANSITION_BEGIN] epoch={} ready={}", epoch, gGameReady.load());
            } else {
                const auto epoch = gEpoch.load();
                TryApplyAtLoadingClose(epoch);
                if (auto* player = RE::PlayerCharacter::GetSingleton())
                    ObservePlayerCombatState("loading_close", player);
                gLoading.store(false);
                spdlog::debug("[TRANSITION_END] epoch={} attempted={}", epoch, gAttempted.load());
                gEpoch.fetch_add(1);
                if (gCombatEndedObserved.load()) QueueRuntimeCheck(kRequestCombat);
            }
        } else if (e->menuName == "Fader Menu") {
            gFaderOpen.store(e->opening);
            if (!e->opening && gCombatEndedObserved.load()) QueueRuntimeCheck(kRequestCombat);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* e,
                                           RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (e && (e->oldContainer == 0x14 || e->newContainer == 0x14)) {
            gInventoryDirty.store(true);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e,
                                           RE::BSTEventSource<RE::TESEquipEvent>*) override {
        if (!e || gInternalEquipMutation || !e->actor || e->actor->GetFormID() != 0x14 || !e->baseObject ||
            !gGameReady.load() || !gEnabled.load() || gLoading.load() || gFaderOpen.load())
            return RE::BSEventNotifyControl::kContinue;

        if (!RE::TESForm::LookupByID<RE::TESObjectARMO>(e->baseObject))
            return RE::BSEventNotifyControl::kContinue;
        bool overflowNow = false;
        {
            std::scoped_lock lock(gEquipSignalMutex);
            if (gEquipSignals.size() >= kMaxEquipSignals) {
                if (!gEquipSignalsOverflow) overflowNow = true;
                gEquipSignalsOverflow = true;
            } else if (!gEquipSignalsOverflow) {
                gEquipSignals.push_back({e->baseObject, e->equipped});
            }
        }
        if (overflowNow)
            spdlog::warn("[EQUIP_SIGNAL] overflow={} evidence_promotions_disabled_until_commit", kMaxEquipSignals);
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* e,
                                           RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override {
        if (e && e->cell && gGameReady.load())
            ObservePlayerCombatState("cell_fully_loaded", RE::PlayerCharacter::GetSingleton());
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* e,
                                           RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override {
        if (e && e->actor && e->actor->GetFormID() == 0x14)
            ObservePlayerCombatState("player_location_event", RE::PlayerCharacter::GetSingleton());
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* e,
                                           RE::BSTEventSource<RE::TESCombatEvent>*) override {
        if (!e || !e->actor || !gGameReady.load())
            return RE::BSEventNotifyControl::kContinue;
        const auto actorID = e->actor->GetFormID();
        const auto targetID = e->targetActor ? e->targetActor->GetFormID() : 0u;

        if (actorID != 0x14 && targetID != 0x14)
            return RE::BSEventNotifyControl::kContinue;
        const auto state = e->newState.underlying();
        auto* player = RE::PlayerCharacter::GetSingleton();
        spdlog::debug("[COMBAT_EVENT] actor={:08X} target={:08X} state={} playerInCombat={} loading={}",
            actorID, targetID, state, player && player->IsInCombat(), gLoading.load());
        ObservePlayerCombatState("combat_event", player);
        QueueRuntimeCheck(kRequestCombat);
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e,
                                           RE::BSTEventSource<RE::TESHitEvent>*) override {
        if (!e || !gGameReady.load()) return RE::BSEventNotifyControl::kContinue;
        const auto target = e->target ? e->target->GetFormID() : 0u;
        const auto cause = e->cause ? e->cause->GetFormID() : 0u;
        if (target != 0x14 && cause != 0x14)
            return RE::BSEventNotifyControl::kContinue;
        auto* player = RE::PlayerCharacter::GetSingleton();
        ObservePlayerCombatState("hit_event", player);
        QueueRuntimeCheck(kRequestCombat);
        return RE::BSEventNotifyControl::kContinue;
    }

};

void __stdcall RenderMenu() {

    const int frame = ImGuiMCP::GetFrameCount();
    const bool newVisit = gLastMenuFrame < 0 || frame < gLastMenuFrame || frame > gLastMenuFrame + 1;
    gLastMenuFrame = frame;
    if (newVisit) {
        gInventoryDirty.store(true);
        gProfileListDirty = true;
    }
    if (gGameReady.load()) {
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            if (!EnsureCharacterProfile(p)) {
                ImGuiMCP::Text("SEM: character profile unavailable (see log)");
                return;
            }
        }
        const bool changed = gInventoryDirty.exchange(false);
        if (!gInventorySeeded || changed) {
            RefreshInventory();
            if (!gInventorySeeded) gInventoryDirty.store(true); 
        }
    }
    ImGuiMCP::SeparatorText("Simple Equipment Manager");
    bool enabled = gEnabled.load();
    if (ImGuiMCP::Checkbox("Enable SEM", &enabled)) {
        const auto path = Ini();
        if (::WritePrivateProfileStringW(L"General", L"Enabled", enabled ? L"1" : L"0", path.c_str())) {
            gEnabled.store(enabled);
            gCombatOverride = false;
            gCombatEndedObserved.store(false);
            gLastObservedPlayerCombat.store(-1);
            if (enabled) { gActiveScene = Scene::Unknown; gCommittedWorn.clear(); ClearEquipSignals(); } 
            spdlog::info("[Settings] enabled={} active={}", enabled, SceneName(gActiveScene));
        } else spdlog::error("[Settings] failed to write Enabled win32={}", ::GetLastError());
    }
    ImGuiMCP::TextDisabled("Player outfit manager | Protected: 50, 52");
    ImGuiMCP::SeparatorText("Character JSON profiles");
    ImGuiMCP::Text("Current: %s", gCurrentProfileName.empty() ? "(unavailable - reload required)" : gCurrentProfileName.c_str());
    if (ImGuiMCP::Button("Reload Outfit JSON")) {
        const bool reloaded = ReloadCurrentCharacterProfile();
        RE::DebugNotification(reloaded ? "SEM: outfit JSON reloaded" : "SEM: reload failed; see log");
    }
    ImGuiMCP::TextDisabled("Read current character JSON; does not equip immediately.");
    if (ImGuiMCP::TreeNode("Manage saved profiles / Delete JSON")) {
        if (gProfileListDirty) RefreshProfileList();
        ImGuiMCP::TextDisabled("Same-name saves share one profile. Deleted JSON cannot be restored here.");
        for (int i = 0; i < static_cast<int>(gProfileFiles.size()); ++i) {
            const auto label = gProfileFiles[i].stem().string();
            if (ImGuiMCP::Selectable(label.c_str(), gProfileDeleteSelection == i)) {
                gProfileDeleteSelection = i;
                gConfirmProfileDelete = false;
            }
        }
        if (gProfileDeleteSelection >= 0 && gProfileDeleteSelection < static_cast<int>(gProfileFiles.size())) {
            if (!gConfirmProfileDelete) {
                if (ImGuiMCP::Button("Delete selected profile...")) gConfirmProfileDelete = true;
            } else {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.24f, 0.24f, 1.0f),
                    "Delete %s permanently?", gProfileFiles[gProfileDeleteSelection].filename().string().c_str());
                if (ImGuiMCP::Button("Confirm delete")) {
                    const auto file = gProfileFiles[gProfileDeleteSelection];
                    std::error_code ec;
                    std::error_code equivalentEc;
                    const bool isCurrent = !gCurrentProfilePath.empty() &&
                        std::filesystem::exists(gCurrentProfilePath) &&
                        std::filesystem::equivalent(file, gCurrentProfilePath, equivalentEc) && !equivalentEc;
                    const bool safe = file.parent_path() == ProfilesDir() &&
                        file.extension() == ".json" && !isCurrent &&
                        !std::filesystem::is_symlink(std::filesystem::symlink_status(file, ec)) && !ec &&
                        std::filesystem::is_regular_file(file, ec);
                    if (safe && !ec && std::filesystem::remove(file, ec) && !ec)
                        spdlog::info("[Profile] deleted file='{}'", file.string());
                    else
                        spdlog::error("[Profile] DELETE FAILED file='{}' safe={} error={}", file.string(), safe, ec.message());
                    RefreshProfileList();
                }
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Cancel deletion")) gConfirmProfileDelete = false;
            }
        }
        ImGuiMCP::TreePop();
    }
    ImGuiMCP::Spacing();

    const int previousGroup = gSelectedGroup;
    if (ImGuiMCP::Combo("Equipment group", &gSelectedGroup, kDisplayNames.data(), static_cast<int>(kDisplayNames.size())) &&
        previousGroup != gSelectedGroup) {
        gFilter[0] = '\0';
        spdlog::debug("[Menu] selected group={}", kDisplayNames[gSelectedGroup]);
    }
    auto& visibleGroups = gGroups;
    auto& feedback = gSaveFeedback;
    ImGuiMCP::Text("Selected: Combat %zu  |  Town %zu  |  Home %zu",
        visibleGroups[0].size(), visibleGroups[1].size(), visibleGroups[2].size());
    if (gGameReady.load() && !gCurrentProfilePath.empty() &&
        std::all_of(gGroups.begin(), gGroups.end(), [](const auto& v) { return v.empty(); }) &&
        std::filesystem::exists(std::filesystem::path(kLegacyOutfitPath))) {
        if (ImGuiMCP::Button("Import legacy Outfits.json into this character"))
            ImportLegacyProfile();
    }
    if (feedback == SaveFeedback::Saved)
        ImGuiMCP::Text("Auto-saved to character JSON");
    else if (feedback == SaveFeedback::Failed) {
        ImGuiMCP::Text("SAVE FAILED - see SEM log");
    } else ImGuiMCP::TextDisabled("Auto-save on every selection change");

    if (gConfigLoadBlocked || !gGameReady.load()) {
        ImGuiMCP::Text("PROFILE UNAVAILABLE - outfit edits and automatic changes blocked");
        ImGuiMCP::Text("Repair or delete the JSON, then click Reload Outfit JSON.");
        return;
    }
    ImGuiMCP::InputText("Search", gFilter, sizeof(gFilter));
    if (gInventoryRows.empty()) ImGuiMCP::TextDisabled("No eligible inventory items in player inventory.");
    if (ImGuiMCP::BeginChild("SEMInventory", ImGuiMCP::ImVec2(0, 280), true)) {
        auto& missingGroup = visibleGroups[gSelectedGroup];
        for (std::size_t index = 0; index < missingGroup.size();) {
            const ItemKey key = missingGroup[index];
            const bool available = std::any_of(gInventoryRows.begin(), gInventoryRows.end(), [&](const InventoryRow& r) {
                if (r.key == key) return !r.ambiguousCopies;
                if (key.variant.empty() && r.soleVerifiedVariant && r.key.mod == key.mod && r.key.localID == key.localID)
                    return !r.ambiguousCopies;
                return false;
            });
            if (available) { ++index; continue; }
            auto* armor = FindArmor(key);
            const char* rawName = armor ? armor->GetName() : nullptr;
            const std::string name = rawName && rawName[0] ? rawName :
                fmt::format("{} [0x{:08X}]", key.mod, key.localID);
            if (gFilter[0] && !std::strstr(name.c_str(), gFilter)) { ++index; continue; }
            bool selected = true;
            const auto label = fmt::format("{}##SEMMissing{}_{}", name, gSelectedGroup, index);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4(1.0f, 0.24f, 0.24f, 1.0f));
            const bool changed = ImGuiMCP::Checkbox(label.c_str(), &selected);
            ImGuiMCP::PopStyleColor();
            if (changed && !selected) {
                missingGroup.erase(missingGroup.begin() + static_cast<std::ptrdiff_t>(index));
                ClearEquipSignals();
                SaveGroups();
                gInventoryDirty.store(true);
                spdlog::info("[Menu] group={} removed_missing mod='{}' local={:08X} variant={} count={}",
                    kDisplayNames[gSelectedGroup], key.mod, key.localID, key.variant, missingGroup.size());
                continue;
            }
            ++index;
        }
        for (const auto& r : gInventoryRows) {
                if (gFilter[0] && !std::strstr(r.name.c_str(), gFilter)) continue;
                auto& group = visibleGroups[gSelectedGroup];
                ItemKey legacyAlias = r.key;
                legacyAlias.variant.clear();
                const bool selectedExact = Contains(group, r.key);
                const bool selectedViaLegacy = r.soleVerifiedVariant && Contains(group, legacyAlias);
                bool selected = selectedExact || selectedViaLegacy;

                const auto label = fmt::format("{}##SEM{:08X}{}", r.name, r.formID, r.key.variant);
                if (!ImGuiMCP::Checkbox(label.c_str(), &selected)) continue;
                if (selected) {
                    if (r.ambiguousCopies) {
                        spdlog::warn("[Menu] rejected group={} form={:08X} reason=multiple_copies_unresolved count={}",
                            kDisplayNames[gSelectedGroup], r.formID, r.count);
                        RE::DebugNotification("SEM: multiple copies; keep one to select");
                        continue;
                    }

                    const auto replaceCount = std::count_if(group.begin(), group.end(), [&](const ItemKey& oldKey) {
                        auto* old = FindArmor(oldKey);
                        return old && MenuSelectionReplaces(Mask(old), r.mask);
                    });
                    if (group.size() - static_cast<std::size_t>(replaceCount) >= 128) {
                        RE::DebugNotification("SEM: group is full");
                        continue;
                    }
                    std::vector<RE::FormID> removedForms;
                    group.erase(std::remove_if(group.begin(), group.end(), [&](const ItemKey& oldKey) {
                        auto* old = FindArmor(oldKey);
                        if (!old || !MenuSelectionReplaces(Mask(old), r.mask)) return false;
                        removedForms.push_back(old->GetFormID());
                        return true;
                    }), group.end());
                    group.push_back(r.key);
                    spdlog::info("[Menu] group={} select={:08X} name='{}' replaced={} count={}",
                        kDisplayNames[gSelectedGroup], r.formID, r.name, removedForms.size(), group.size());
                    for (const auto oldID : removedForms) {
                        spdlog::debug("[MENU_SLOT_REPLACE] group={} old={:08X} new={:08X} mask={:08X}",
                            kDisplayNames[gSelectedGroup], oldID, r.formID, r.mask);
                    }
                } else {

                    group.erase(std::remove_if(group.begin(), group.end(), [&](const ItemKey& oldKey) {
                        return oldKey == r.key || (r.soleVerifiedVariant && oldKey == legacyAlias);
                    }), group.end());
                    spdlog::info("[Menu] group={} remove={:08X} name='{}' legacyAlias={} count={}",
                        kDisplayNames[gSelectedGroup], r.formID, r.name, selectedViaLegacy, group.size());
                }
                gInventoryDirty.store(true); 

                ClearEquipSignals();
                spdlog::debug("[Menu] group config edited; active={} verifiedOwnershipRetained={} next_switch=replan",
                    SceneName(gActiveScene), gCommittedWorn.size());
                SaveGroups();
            }
        }
    ImGuiMCP::EndChild();
    ImGuiMCP::Spacing();
    ImGuiMCP::Spacing();
    static constexpr const char* levels[]{"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};
    int level = gLevel.load();
    if (ImGuiMCP::Combo("Log level", &level, levels, static_cast<int>(std::size(levels)))) {
        const auto path = Ini();
        const auto v = LevelName(level);
        std::wstring wide;
        for (char c : std::string_view(v)) wide.push_back(static_cast<wchar_t>(c));
        if (::WritePrivateProfileStringW(L"Logging", L"Level", wide.c_str(), path.c_str())) {
            spdlog::info("[Settings] log level={}", v);
            SetLevel(level);
        } else spdlog::error("[Settings] failed to write log level win32={}", ::GetLastError());
    }
}

void ClearSession(bool clearOwnership) {
    gGameReady.store(false);
    gLoading.store(false);
    gAttempted.store(false);
    gFaderOpen.store(false);
    gEpoch.fetch_add(1);
    gLastObservedPlayerCombat.store(-1);
    gCombatEndedObserved.store(false);
    gPendingRequests.store(0);
    gRequestTaskQueued.store(false);
    gCombatOverride = false;
    gInventoryRows.clear();
    gInventorySeeded = false;
    gInventoryDirty.store(true);
    gLastMenuFrame = -1;
    gCurrentProfileName.clear();
    gCurrentProfilePath.clear();
    gProfileListDirty = true;
    gProfileFiles.clear();
    gProfileDeleteSelection = -1;
    gConfirmProfileDelete = false;
    gObservedConfigStamp.reset();
    gConfigLoadBlocked = false;
    if (clearOwnership) {
        ClearEquipSignals();
        gCommittedWorn.clear();
        gActiveScene = Scene::Unknown;
        gSeedLegacySnapshotOnPostLoad = false;
    }
}

void Save(SKSE::SerializationInterface* s) {
    if (!s || !s->OpenRecord(kRecordID, kRecordVersion)) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto evidence = BuildSourceEvidence(player, gActiveScene, ObserveEquipSignals(), false);
    std::vector<SaveEntryV4> records;
    records.reserve(std::min<std::size_t>(evidence.size(), kMaxSavedEntries));
    for (const auto& source : evidence) {
        if (records.size() >= kMaxSavedEntries) break;
        auto* armor = FindArmor(source.key);
        if (!armor || (!source.key.variant.empty() && source.key.variant.size() != 16)) {
            spdlog::warn("[CoSave] invalid source identity action=skip");
            continue;
        }
        SaveEntryV4 entry{};
        entry.form = armor->GetFormID();
        entry.mask = source.mask;
        if (!source.key.variant.empty())
            std::memcpy(entry.variant, source.key.variant.data(), source.key.variant.size());
        records.push_back(entry);
    }

    const SaveHeader header{static_cast<std::uint32_t>(records.size()),
        static_cast<std::uint32_t>(gActiveScene)};
    if (!s->WriteRecordData(header)) return;
    for (const auto& record : records)
        if (!s->WriteRecordData(record)) return;
    spdlog::debug("[CoSave] saved version={} evidenceSources={} active={}",
        kRecordVersion, records.size(), SceneName(gActiveScene));
}

void Load(SKSE::SerializationInterface* s) {
    ClearEquipSignals();
    gCommittedWorn.clear();
    gActiveScene = Scene::Unknown;
    gSeedLegacySnapshotOnPostLoad = false;
    if (!s) return;
    std::uint32_t type = 0, version = 0, length = 0;
    while (s->GetNextRecordInfo(type, version, length)) {
        if (type != kRecordID || version < 1 || version > kRecordVersion ||
            length < sizeof(SaveHeader)) continue;
        SaveHeader header{};
        const std::size_t entrySize = version >= 4 ? sizeof(SaveEntryV4) : sizeof(SaveEntry);
        if (s->ReadRecordData(header) != sizeof(header) || header.count > kMaxSavedEntries ||
            length < sizeof(header) + static_cast<std::size_t>(header.count) * entrySize) {
            spdlog::warn("[CoSave] malformed record version={}", version);
            return;
        }
        if (version >= 2 && header.activeScene <= static_cast<std::uint32_t>(Scene::Home))
            gActiveScene = static_cast<Scene>(header.activeScene);

        gSeedLegacySnapshotOnPostLoad = (version == 2 || version == 3);
        for (std::uint32_t i = 0; i < header.count; ++i) {
            if (version != 4) {
                SaveEntry ignored{};
                if (s->ReadRecordData(ignored) != sizeof(ignored)) break;
                continue;
            }
            SaveEntryV4 e{};
            if (s->ReadRecordData(e) != sizeof(e)) break;
            if (gActiveScene == Scene::Unknown || !IsAllowedMask(e.mask)) continue;
            RE::FormID resolved{};
            if (!s->ResolveFormID(e.form, resolved)) continue;
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(resolved);
            auto key = MakeKey(armor);
            if (!key || Mask(armor) != e.mask) continue;
            const auto end = std::find(std::begin(e.variant), std::end(e.variant), '\0');
            if (end == std::end(e.variant)) continue; 
            key->variant.assign(std::begin(e.variant), end);
            if (!key->variant.empty() && key->variant.size() != 16) continue;
            if (!HasCommittedIdentity(gCommittedWorn, *key))
                gCommittedWorn.push_back({*key, e.mask});
        }
        spdlog::info("[CoSave] loaded version={} committed={} active={} legacySeed={}",
            version, gCommittedWorn.size(), SceneName(gActiveScene), gSeedLegacySnapshotOnPostLoad);
    }
}
void Revert(SKSE::SerializationInterface*) { ClearSession(true); spdlog::debug("[CoSave] revert"); }

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (!message) return;
    switch(message->type) {
    case SKSE::MessagingInterface::kDataLoaded: {
        auto* sink=SEMEventSink::Get();
        if (auto* ui=RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(sink);
        if (auto* holder=RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESCellFullyLoadedEvent>(sink);
            holder->AddEventSink<RE::TESActorLocationChangeEvent>(sink);
            holder->AddEventSink<RE::TESCombatEvent>(sink);
            holder->AddEventSink<RE::TESHitEvent>(sink);
            holder->AddEventSink<RE::TESEquipEvent>(sink);
            holder->AddEventSink<RE::TESContainerChangedEvent>(sink);
        }
        if (!gMenuRegistered && SKSEMenuFramework::IsInstalled() && GetMenuFrameworkModule()) {
            SKSEMenuFramework::SetSection("Simple Equipment Manager");
            SKSEMenuFramework::AddSectionItem("Equipment groups",RenderMenu);
            gMenuRegistered=true;
            spdlog::info("[Menu] SKSEMF registered");
        }
        spdlog::info("[Events] installed loading/combat/hit/equip/container observers (cell/location combat sampling only)");
        break;
    }
    case SKSE::MessagingInterface::kPreLoadGame: ClearSession(true);spdlog::debug("[Session] pre-load reset");break;
    case SKSE::MessagingInterface::kPostLoadGame:
        gLoading.store(false);gAttempted.store(false);gEpoch.fetch_add(1);gGameReady.store(true);
        gInventoryDirty.store(true);
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            if (!EnsureCharacterProfile(p)) { gGameReady.store(false); spdlog::error("[Profile] load blocked; no valid profile"); break; }
            if (gActiveScene != Scene::Unknown) {
                const int idx = static_cast<int>(gActiveScene) - 1;
                if (idx >= 0 && idx < 3) {
                    std::erase_if(gCommittedWorn, [&](const CommittedPiece& piece) {
                        if (Contains(gGroups[idx], piece.key)) return false;
                        ItemKey legacy = piece.key;
                        legacy.variant.clear();
                        if (!piece.key.variant.empty() && Contains(gGroups[idx], legacy)) {
                            gSeedLegacySnapshotOnPostLoad = true;
                        }
                        return true;
                    });
                }
            }
        }
        if (gSeedLegacySnapshotOnPostLoad && gActiveScene != Scene::Unknown) {

            CaptureCommittedWorn(gActiveScene);
            gSeedLegacySnapshotOnPostLoad = false;
            spdlog::info("[CoSave] legacy scene snapshot seeded from current equipment");
        }
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {

            gCombatOverride = false;
            gCombatEndedObserved.store(false);
        }
        spdlog::info("[Session] game ready; active={} automatic outfit waits for NEXT LoadingMenu transition", SceneName(gActiveScene));
        break;
    case SKSE::MessagingInterface::kNewGame:
        ClearSession(true);
        gGameReady.store(true);
        gInventoryDirty.store(true);
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            if (!EnsureCharacterProfile(p)) gGameReady.store(false);
        }
        spdlog::info("[Session] new game ready, no active group");
        break;
    default:break;
    }
}
} 
} 

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    auto log=SKSE::log::log_directory();
    if (!log) return false;
    auto logger=spdlog::basic_logger_mt("TMS_SimpleEquipmentManager",(*log/"TMS_SimpleEquipmentManager.log").string(),true);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    SEM::LoadSettings();
    spdlog::flush_on(spdlog::level::info);
    const auto version=REL::Module::get().version();
    spdlog::info("TMS Simple Equipment Manager v{} | Skyrim {} | enabled={} | log={}",SEM::kVersion,version.string(),SEM::gEnabled.load(),SEM::LevelName(SEM::gLevel.load()));
    if (auto* serialization=SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(0x4D4D4553); 
        serialization->SetSaveCallback(SEM::Save);
        serialization->SetLoadCallback(SEM::Load);
        serialization->SetRevertCallback(SEM::Revert);
    } else {spdlog::error("SKSE serialization unavailable");return false;}
    auto* messaging=SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(SEM::OnMessage)) {spdlog::error("SKSE messaging registration failed");return false;}
    return true;
}
