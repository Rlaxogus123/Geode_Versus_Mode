// Percentage Toggle 모드 때문에 레벨의 m_timestamp(측정 길이)가 0이 되거나 바뀌는 문제를 막는다.
// 배틀 시작 시 값을 기록하고(prepareLevel), 플레이 중 바뀌면 되돌린다(enforce).
#include "PercentGuard.hpp"
#include "PathCompat.hpp"
#include "WeakRefs.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/utils/file.hpp>

#include <string>

using namespace geode::prelude;

namespace {
// 현재 배틀 레벨(약한 참조)과 그 레벨의 기록된 m_timestamp.
versus::Weak<GJGameLevel> s_level;
int s_timestamp = 0;
// enforce 경고 로그를 한 번만 남기기 위한 플래그.
bool s_enforceLogged = false;

// Percentage Toggle의 저장 데이터. 모드가 로드되어 있으면 메모리의 값을, 아니면 saved.json을 직접 읽는다.
matjson::Value toggleSaveData() {
    if (auto* mod = Loader::get()->getLoadedMod(versus::percent::kPercentageToggle)) {
        matjson::Value const& saved = mod->getSaveContainer();
        return saved.isObject() && saved.contains("toggle-save-data") ? saved["toggle-save-data"] : matjson::Value{};
    }
    auto const path = dirs::getModsSaveDir() / versus::percent::kPercentageToggle / "saved.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(versus::stdPath(path), error)) return {};
    auto json = utils::file::readJson(path);
    if (json.isErr()) return {};
    matjson::Value const saved = std::move(json).unwrap();
    return saved.isObject() && saved.contains("toggle-save-data") ? saved["toggle-save-data"] : matjson::Value{};
}

// Percentage Toggle이 저장한 레벨별 항목(적용 여부, 원래 길이).
struct ToggleEntry {
    bool toggled = false;
    int originalTime = 0;
};

// levelID에 해당하는 저장 항목을 찾는다. 없거나 값이 이상하면 기본값.
ToggleEntry toggleEntry(int levelID) {
    ToggleEntry result;
    if (levelID <= 0) return result;
    auto const data = toggleSaveData();
    if (!data.isArray()) return result;
    auto const key = std::to_string(levelID);
    for (auto const& entry : data.asArray().unwrap()) {
        if (!entry.isObject() || entry["key"].asString().unwrapOr("") != key) continue;
        result.toggled = entry["toggled"].asBool().unwrapOr(false);
        auto const original = entry["original_time"].asInt().unwrapOr(0);
        result.originalTime = original > 0 && original < (1 << 30) ? static_cast<int>(original) : 0;
        break;
    }
    return result;
}
}

namespace versus::percent {

// 배틀 레벨을 등록하고 길이를 기록한다. 길이가 0이면 Percentage Toggle이 저장해 둔 원래 값으로 채운다.
void prepareLevel(GJGameLevel* level) {
    rebind(s_level, level);
    s_enforceLogged = false;
    if (!level) {
        s_timestamp = 0;
        return;
    }
    if (level->m_timestamp <= 0) {
        int const id = level->m_levelID.value();
        if (auto const entry = toggleEntry(id); entry.originalTime > 0) {
            log::info("Versus: map {} had no measured length (left at 0 by Percentage Toggle); using its recorded {}",
                      id, entry.originalTime);
            level->m_timestamp = entry.originalTime;
        }
    }
    s_timestamp = level->m_timestamp;
}

// 플레이 중 레벨 길이가 기록과 달라졌으면 되돌린다(경고 로그는 한 번만).
void enforce(PlayLayer* layer) {
    if (!layer || !layer->m_level) return;
    auto const expected = s_level.lock();
    if (!expected || expected.data() != layer->m_level || layer->m_level->m_timestamp == s_timestamp) return;
    if (!s_enforceLogged) {
        s_enforceLogged = true;
        log::warn(
            "Versus: the battle map's measured length changed ({} -> {}); put back so the percentage stays GD 2.2's",
            s_timestamp, layer->m_level->m_timestamp);
    }
    layer->m_level->m_timestamp = s_timestamp;
}

// 이 레벨에 Percentage Toggle이 적용될지 판단한다: 모드가 켜져 있고, 강제 설정이거나 레벨별로 켜져 있을 때.
bool toggleWouldApply(GJGameLevel* level) {
    auto* mod = Loader::get()->getLoadedMod(kPercentageToggle);
    if (!mod || !level || level->m_timestamp <= 0) return false;
    if (mod->getSettingValue<bool>("force-enable")) return true;
    return toggleEntry(level->m_levelID.value()).toggled;
}

}
