// compat/GameHooks.hpp의 GD 2.2091 훅.
// 2.2091 바인딩에서 macOS / iOS 주소가 없는 함수는 그 플랫폼에서만 훅을 뺀다(주소 없는 훅은 컴파일되지 않는다).
// 빠지는 곳과 그때 무엇이 대신 막는지는 각 훅 위에 적었다.
#include "../GameHooks.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>

using namespace geode::prelude;
using versus::hooks::Key;
using versus::hooks::Reward;

namespace {
Key keyFor(GJActionID action) {
    switch (action) {
        case GJActionID::Pause: return Key::Pause;
        case GJActionID::StartPosPrev:
        case GJActionID::StartPosNext:
        case GJActionID::StartPosNone: return Key::StartPos;
        case GJActionID::ToggleIgnoreDamage: return Key::IgnoreDamage;
        case GJActionID::TestModeIncrease:
        case GJActionID::TestModeDecrease: return Key::GameSpeed;
        default: return Key::Other;
    }
}
}

// GD 2.209부터 키보드 / 컨트롤러 입력은 키 바인드 액션이 되어 handleAction으로 들어온다.
// Esc도 keyDown 안에서 handleAction(Pause)로 넘어온다. handleKeypress는 2.2091에서 인라인이라 훅을 걸 수 없다.
class $modify(VersusKeyInput, UILayer) {
    bool handleAction(int action, bool down, double timestamp) {
        auto const kind = keyFor(static_cast<GJActionID>(action));
        if (versus::hooks::loungeKey(this, kind, down) || versus::hooks::battleKey(this, kind, down)) return true;
        return UILayer::handleAction(action, down, timestamp);
    }
};

// iOS에는 startGameDelayed 주소가 없다. 그때도 음악(startMusic)과 진행(update)은 배틀 / 리플레이 훅이 막는다.
#ifndef GEODE_IS_IOS
class $modify(VersusDelayedStart, PlayLayer) {
    void startGameDelayed() {
        if (versus::hooks::holdBattleStart(this) || versus::hooks::holdReplayStart(this)) return;
        PlayLayer::startGameDelayed();
    }
};
#endif

// 보상 함수들. GD는 클리어(levelComplete)와 새 최고 기록(destroyPlayer)에서 오브 / 다이아몬드를 주고,
// 나머지는 completedLevel, 레벨 정보 화면, 데이터 로드에서 부른다. 배틀의 통계 변경(incrementStat)과
// 완료 기록(completedLevel), 업적(reportAchievementWithID)은 모든 플랫폼에서 MatchLaunch.cpp가 막는다.
// awardCurrencyForLevel / awardDiamondsForLevel은 준 개수를 돌려주고 GD가 그 값으로 보상 연출을 하므로, 막을 때는 0이다.
class $modify(VersusRewardGuard, GameStatsManager) {
#ifndef GEODE_IS_IOS
    void completedStarLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::StarLevel, level)) GameStatsManager::completedStarLevel(level);
    }
    void completedDemonLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::DemonLevel, level)) GameStatsManager::completedDemonLevel(level);
    }
    int awardCurrencyForLevel(GJGameLevel* level) {
        if (versus::hooks::holdReward(Reward::Currency, level)) return 0;
        return GameStatsManager::awardCurrencyForLevel(level);
    }
    int awardDiamondsForLevel(GJGameLevel* level) {
        if (versus::hooks::holdReward(Reward::Diamonds, level)) return 0;
        return GameStatsManager::awardDiamondsForLevel(level);
    }
    void checkCoinAchievement(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::CoinAchievement, level)) GameStatsManager::checkCoinAchievement(level);
    }
#endif
#ifndef GEODE_IS_INTEL_MAC
    GJRewardItem* completedDailyLevel(GJGameLevel* level) {
        if (versus::hooks::holdReward(Reward::Daily, level)) return nullptr;
        return GameStatsManager::completedDailyLevel(level);
    }
#endif
#if !defined(GEODE_IS_INTEL_MAC) && !defined(GEODE_IS_IOS)
    void markLevelAsCompletedAndClaimed(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::Claimed, level)) GameStatsManager::markLevelAsCompletedAndClaimed(level);
    }
#endif
#ifndef GEODE_IS_ARM_MAC
    void storeUserCoin(char const* key) {
        if (!versus::hooks::holdUserCoin(key)) GameStatsManager::storeUserCoin(key);
    }
#endif
};

// 레벨 검색 화면의 뒤로 가기. 뒤로 가기 버튼과 GD의 keyBackClicked(Esc / Android 뒤로 가기)가 모두 onBack을 부른다.
// iOS에는 onBack 주소가 없다. 그때는 LevelSelector.cpp가 뒤로 가기 버튼의 콜백을 바꿔 두고, iOS에는 뒤로 가기 키가 없다.
#ifndef GEODE_IS_IOS
class $modify(VersusSearchBack, LevelSearchLayer) {
    void onBack(CCObject* sender) {
        if (!versus::hooks::holdLevelSearchBack(this)) LevelSearchLayer::onBack(sender);
    }
};
#endif

// iOS에는 reportPercentageForLevel 주소가 없다. 그 안에서 하는 업적 보고는 reportAchievementWithID 훅이 막는다.
#ifndef GEODE_IS_IOS
class $modify(VersusPercentReport, GameManager) {
    void reportPercentageForLevel(int levelID, int percentage, bool isPlatformer) {
        if (!versus::hooks::holdPercentage(levelID, percentage, isPlatformer))
            GameManager::reportPercentageForLevel(levelID, percentage, isPlatformer);
    }
};
#endif
