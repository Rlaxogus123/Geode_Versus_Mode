// 외부 모드별 금지 기능 목록과, 감지된 기능 키를 문자열로 인코딩/디코딩하고
// 화면용 문구로 바꾸는 헬퍼. 인코딩은 키를 ';'로 이은 문자열이며 최대 48개이다.
#pragma once

#include "IntegrityCodes.hpp"
#include "MegaHackRules.hpp"
#include "PrismRules.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace versus::integrity {

// 기능 하나: 모드 내부 설정 키와 화면에 보이는 이름.
struct CheatFeature {
    std::string_view key;
    std::string_view name;
};

// QOLMod에서 금지하는 기능 키 목록.
inline constexpr std::array<CheatFeature, 21> kQolCheatFeatures{{
    {"accurate-spike-hitboxes", "Accurate Spike Hitboxes"},
    {"custom-respawn-time", "Slower Respawn Time"},
    {"autoclicker", "Auto Clicker"},
    {"auto-coins", "Auto Collect Coins"},
    {"coin-tracers", "Coin Tracers"},
    {"force-plat", "Force Platformer"},
    {"gravity-arrow", "Gravity Arrow"},
    {"hitbox-multiplier", "Hitbox Multiplier"},
    {"instant", "Instant Complete"},
    {"instant-reverse", "Instant Mirror Portal"},
    {"jump-hack", "Jump Hack"},
    {"noclip", "Noclip"},
    {"no-invisible-objects", "No Invisible Objects"},
    {"no-reverse", "No Mirror Portal"},
    {"show-hitboxes", "Show Hitboxes"},
    {"show-player", "Force Player Visibility"},
    {"show-trajectory", "Show Trajectory"},
    {"tps-bypass", "TPS Bypass"},
    {"speedhack", "Speedhack"},
    {"show-layout", "Show Layout"},
    {"layout-mode", "Show Layout"},
}};

// Eclipse에서 금지하는 기능 키 목록.
inline constexpr std::array<CheatFeature, 22> kEclipseCheatFeatures{{
    {"player.respawndelay.toggle", "Slower Respawn Time"},
    {"global.tpsbypass.toggle", "TPS Bypass"},
    {"level.autopickupcoins", "Auto Collect Coins"},
    {"level.autopracticemode", "Auto Practice Mode"},
    {"level.forceplatformer", "Force Platformer"},
    {"level.hitbox_multiplier", "Hitbox Multiplier"},
    {"level.practicecoins", "Practice Coins"},
    {"level.randomseed", "Random Seed"},
    {"level.showhitboxes", "Show Hitboxes"},
    {"level.zeropercentpractice", "0% Practice Complete"},
    {"player.autoclick", "Auto Clicker"},
    {"player.clicktp", "Click Teleport"},
    {"player.framestepper", "Frame Stepper"},
    {"player.jumphack", "Jump Hack"},
    {"player.noclip", "Noclip"},
    {"player.noplatformersquish", "No Platformer Squish"},
    {"player.showtrajectory", "Show Trajectory"},
    {"bot.playback", "Replay Playback"},
    {"global.speedhack.toggle", "Speedhack"},
    {"bot.state", "Replay Bot"},
    {"level.showlayout", "Show Layout"},
    {"level.layoutmode", "Show Layout"},
}};

// 로드되어 있으면 안 되는 모드(Geode 모드 ID와 이름).
inline constexpr std::array<CheatFeature, 5> kBannedMods{{
    {"mythicalrev.noclip-at", "Noclip At"},
    {"tobyadd.gdh", "GDH"},
    {"zilko.xdbot", "xdBot"},
    {"peony.silicate", "Silicate"},
    {"decks.echo", "Echo"},  // 1.0.8: 리플레이 봇(노클립 등), 켜져 있기만 해도
}};
// 더 이상 금지하지 않지만 과거 기록을 표시하기 위해 이름만 유지하는 모드.
inline constexpr std::array<CheatFeature, 1> kRetiredBannedMods{{
    {"zsa.percentage-toggle", "Percentage Toggle"},
}};

// Eclipse의 불리언 기능 중 켜진 것을 모은다. Speedhack, bot 상태, respawn delay는
// 값까지 확인해야 하므로 여기서는 제외한다.
template <typename ReadBool> inline std::vector<std::string> activeEclipseBooleanFeatures(ReadBool&& readBool) {
    std::vector<std::string> detected;
    for (auto const& cheat : kEclipseCheatFeatures) {
        if (cheat.key == "global.speedhack.toggle" || cheat.key == "bot.state" ||
            cheat.key == "player.respawndelay.toggle")
            continue;
        if (readBool(cheat.key)) detected.emplace_back(cheat.key);
    }
    return detected;
}

// 이 코드가 기능 목록(features)을 동반하는지 여부.
inline bool codeUsesFeatures(std::string_view code) {
    return externalCode(code);
}

// 코드별 기능 키의 표시 이름. 목록에 없는 키면 빈 문자열.
inline std::string_view featureName(std::string_view code, std::string_view key) {
    if (code == kQolEnabled) {
        for (auto const& feature : kQolCheatFeatures)
            if (feature.key == key) return feature.name;
    } else if (code == kEclipseEnabled || code == kEclipseAttempt) {
        for (auto const& feature : kEclipseCheatFeatures)
            if (feature.key == key) return feature.name;
    } else if (code == kMegaHackEnabled || code == kMegaHackAttempt) {
        return megaHackGameplayCheatName(key);
    } else if (code == kPrismEnabled) {
        return prismGameplayCheatName(key);
    } else if (code == kBannedModEnabled) {
        for (auto const& mod : kBannedMods)
            if (mod.key == key) return mod.name;
        for (auto const& mod : kRetiredBannedMods)
            if (mod.key == key) return mod.name;
    }
    return {};
}

// ';'로 구분된 인코딩 문자열을 키 목록으로 복원한다.
// 알 수 없는 키, 빈 키, 중복이 하나라도 있으면 전체를 무효(빈 목록)로 본다.
inline std::vector<std::string> decodeFeatures(std::string_view code, std::string_view encoded) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < encoded.size()) {
        auto const end = encoded.find(';', start);
        auto const key = encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
        if (key.empty() || featureName(code, key).empty()) return {};
        if (std::find(result.begin(), result.end(), key) != result.end()) return {};
        result.emplace_back(key);
        if (result.size() > 48 || end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

// 서버나 상대에게서 받은 코드와 기능 문자열이 형식에 맞는지 검증한다.
inline bool validFeatureDetails(std::string_view code, std::string_view encoded) {
    if (!validCode(code)) return false;
    if (!codeUsesFeatures(code)) return encoded.empty();
    if (encoded.empty() || encoded.size() > 4096) return false;
    auto const decoded = decodeFeatures(code, encoded);
    return !decoded.empty() && decoded.size() <= 48;
}

// 기능 키 목록을 ';'로 이어 인코딩한다. 알 수 없는 키와 중복은 건너뛰고 48개까지만 담는다.
inline std::string encodeFeatures(std::string_view code, std::vector<std::string> const& keys) {
    std::string result;
    std::vector<std::string_view> seen;
    for (auto const& key : keys) {
        if (featureName(code, key).empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        if (!result.empty()) result.push_back(';');
        result += key;
        seen.emplace_back(key);
        if (seen.size() == 48) break;
    }
    return result;
}

// 기능 이름을 ", "로 이어 한 줄 요약으로 만든다.
inline std::string featureSummary(std::string_view code, std::string_view encoded) {
    std::string result;
    for (auto const& key : decodeFeatures(code, encoded)) {
        if (!result.empty()) result += ", ";
        result += featureName(code, key);
    }
    return result;
}

// (코드, 기능) 목록을 "출처: 기능, ..." 형태로 이어 붙인 표시용 문자열. 비어 있으면 "No
// hacks".
inline std::string hackListText(std::vector<std::pair<std::string, std::string>> const& list) {
    std::string result;
    for (auto const& [code, features] : list) {
        auto const summary = featureSummary(code, features);
        if (summary.empty()) continue;
        if (!result.empty()) result += "  |  ";
        result += sourceName(code) + ": " + summary;
    }
    return result.empty() ? std::string("No hacks") : result;
}

// 기능 이름을 줄마다 "- "를 붙여 나열한다.
inline std::string featureLines(std::string_view code, std::string_view encoded) {
    std::string result;
    for (auto const& key : decodeFeatures(code, encoded)) {
        if (!result.empty()) result.push_back('\n');
        result += "- ";
        result += featureName(code, key);
    }
    return result;
}

// 안티치트 감지 오버레이에 표시할 문구. opponent면 상대 감지로 표시한다.
inline std::string detectionText(std::string_view code, std::string_view encoded, bool opponent = false) {
    auto const summary = featureSummary(code, encoded);
    if (summary.empty()) return {};
    return "ANTI-CHEAT DETECTING\n" + std::string(opponent ? "Opponent - " : "") + sourceName(code) + ": " + summary;
}

// 실격 처리 때 전달할 상세 설명. 금지 모드는 별도 안내 문구를 쓴다.
inline std::string detectionDetail(std::string_view code, std::string_view encoded) {
    auto const lines = featureLines(code, encoded);
    if (lines.empty()) return {};
    if (code == kBannedModEnabled)
        return "Unauthorized mods detected (disable them in Geode, then restart the game):\n" + lines;
    return sourceName(code) + " banned gameplay features detected:\n" + lines;
}

// 코드별 사유 문구에 기능 요약을 괄호로 덧붙인다.
inline std::string resultReason(std::string_view code, bool opponent, std::string_view encoded) {
    auto result = resultReason(code, opponent);
    auto const summary = featureSummary(code, encoded);
    if (!summary.empty()) result += " (" + summary + ")";
    return result;
}

}
