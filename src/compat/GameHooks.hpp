// 버전마다 이름이나 시그니처가 다르거나, 2.2091의 일부 플랫폼에서 훅을 걸 수 없는 GD 함수들.
// 훅 자체는 compat/gd2081/GameHooks.cpp, compat/gd2091/GameHooks.cpp에 있고, 무엇을 막을지는 여기 선언된
// 기능 쪽 함수가 정한다(구현 위치는 각 줄에 적었다). 훅이 없는 플랫폼에서는 그 함수가 불리지 않을 뿐이다.
#pragma once

class GJGameLevel;
class LevelSearchLayer;
class PlayLayer;
class UILayer;

namespace versus::hooks {
// 게임 키 입력. GD 2.2081은 키 코드(UILayer::handleKeypress), 2.2091은 키 바인드 액션(UILayer::handleAction)으로
// 들어오고, 훅이 그것을 이 종류로 바꿔 넘긴다. 2.2081에는 일시 정지 말고는 해당하는 액션이 없다.
enum class Key {
    Pause,        // 일시 정지(Esc)
    StartPos,     // 시작 위치 전환
    IgnoreDamage, // 피해 무시
    GameSpeed,    // 테스트 모드 속도 조절
    Other
};
// 배틀(MatchLaunch.cpp) / 라운지(Season2Lounge.cpp) 규칙이 이 입력을 GD에 넘기지 않으면 true.
bool battleKey(UILayer* ui, Key key, bool down);
bool loungeKey(UILayer* ui, Key key, bool down);

// GD의 지연 시작(PlayLayer::startGameDelayed)을 지금 막아야 하면 true.
bool holdBattleStart(PlayLayer* layer); // MatchLaunch.cpp
bool holdReplayStart(PlayLayer* layer); // Replay.cpp

// 배틀 / 리플레이 / free play / 라운지 중이면 GD의 보상과 기록 처리를 막는다(진행 기록 중이면 장부에 남긴다).
// 막았으면 true (MatchLaunch.cpp).
enum class Reward { StarLevel, DemonLevel, Daily, Currency, Diamonds, Claimed, CoinAchievement };
bool holdReward(Reward reward, GJGameLevel* level);
bool holdUserCoin(char const* key);
bool holdPercentage(int levelID, int percentage, bool isPlatformer);

// 맵을 고르는 중인 레벨 검색 화면에서 뒤로 가기(버튼, Esc / Android 뒤로 가기)를 방 복귀로 처리했으면 true
// (LevelSelector.cpp). 훅은 LevelSearchLayer::onBack에 건다. keyBackClicked는 onBack을 부르기만 하는 몇 바이트짜리라,
// ARM 빌드(Android, iOS 등)에서는 훅 패치가 바로 뒤의 this 보정 썽크(키 입력이 실제로 들어오는 입구)를 덮어써
// 뒤로 가기 키에서 게임이 튕길 수 있다(v0.5.22의 Android 크래시가 이 모양이었다).
bool holdLevelSearchBack(LevelSearchLayer* layer);
}
