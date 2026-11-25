// compat/GameHooks.hpp의 GD 2.2081 훅. 2.2081 바인딩에는 모든 플랫폼에 주소가 있다.
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

// 키 입력은 keyDown이 아니라 handleKeypress에서 받는다. macOS(ARM)의 UILayer::keyDown은 handleKeypress(key, true, ...)로
// 넘기기만 하는 8바이트 함수라서, 훅 패치(12바이트)가 바로 뒤에 붙은 this 보정 썽크(키보드 디스패처가 실제로 부르는
// 입구)를 덮어써 키보드 입력이 사라지거나(점프·일시정지 안 됨) 게임이 튕겼다.
class $modify(VersusKeyInput, UILayer) {
    void handleKeypress(enumKeyCodes key, bool down, double timestamp) {
        auto const kind = key == KEY_Escape ? Key::Pause : Key::Other;
        if (versus::hooks::loungeKey(this, kind, down) || versus::hooks::battleKey(this, kind, down)) return;
        UILayer::handleKeypress(key, down, timestamp);
    }
};

class $modify(VersusDelayedStart, PlayLayer) {
    void startGameDelayed() {
        if (versus::hooks::holdBattleStart(this) || versus::hooks::holdReplayStart(this)) return;
        PlayLayer::startGameDelayed();
    }
};

class $modify(VersusRewardGuard, GameStatsManager) {
    void completedStarLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::StarLevel, level)) GameStatsManager::completedStarLevel(level);
    }
    void completedDemonLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::DemonLevel, level)) GameStatsManager::completedDemonLevel(level);
    }
    GJRewardItem* completedDailyLevel(GJGameLevel* level) {
        if (versus::hooks::holdReward(Reward::Daily, level)) return nullptr;
        return GameStatsManager::completedDailyLevel(level);
    }
    void awardCurrencyForLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::Currency, level)) GameStatsManager::awardCurrencyForLevel(level);
    }
    void awardDiamondsForLevel(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::Diamonds, level)) GameStatsManager::awardDiamondsForLevel(level);
    }
    void markLevelAsCompletedAndClaimed(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::Claimed, level)) GameStatsManager::markLevelAsCompletedAndClaimed(level);
    }
    void storeUserCoin(char const* key) {
        if (!versus::hooks::holdUserCoin(key)) GameStatsManager::storeUserCoin(key);
    }
    void checkCoinAchievement(GJGameLevel* level) {
        if (!versus::hooks::holdReward(Reward::CoinAchievement, level)) GameStatsManager::checkCoinAchievement(level);
    }
};

// 레벨 검색 화면의 뒤로 가기. 뒤로 가기 버튼과 GD의 keyBackClicked(Esc / Android 뒤로 가기)가 모두 onBack을 부른다.
class $modify(VersusSearchBack, LevelSearchLayer) {
    void onBack(CCObject* sender) {
        if (!versus::hooks::holdLevelSearchBack(this)) LevelSearchLayer::onBack(sender);
    }
};

class $modify(VersusPercentReport, GameManager) {
    void reportPercentageForLevel(int levelID, int percentage, bool isPlatformer) {
        if (!versus::hooks::holdPercentage(levelID, percentage, isPlatformer))
            GameManager::reportPercentageForLevel(levelID, percentage, isPlatformer);
    }
};
