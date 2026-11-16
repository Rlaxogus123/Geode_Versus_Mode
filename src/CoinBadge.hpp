// 1.0.8: 결과 화면의 유저 코인 수 배지와, 코인 수로 승패가 갈렸는지 판정 (서버 resolve_battle 과 같은 규칙).
#pragma once
#include "VersusService.hpp"
#include <Geode/Geode.hpp>

namespace versus {
// 클리어(또는 같은 최고 %까지)와 실패 횟수가 같아 유저 코인 수로 승패가 갈렸는지.
// 무승부, 진행도 목표 규칙, 기권/안티치트/일시정지 초과, 아직 시도 중인 쪽이 있으면 아니다.
bool coinsDecided(BattlePlayerState const& a, BattlePlayerState const& b, GameRules const& rules, bool draw);
// 코인 아이콘 + 숫자(total 이 0보다 크면 "n/total"). decisive 면 delay 초 뒤 grow 배로 튀어나와 뒤에서 빛이
// 맥동하고 반짝이가 튄다(코인 수로 이긴 쪽). 노드의 (0,0)이 배지 가운데다.
cocos2d::CCNode* coinBadge(int coins, int total, float scale, bool decisive, float delay = 0.f, float grow = 1.75f);
// coinBadge 의 폭 (배율 1, 커지기 전).
float coinBadgeWidth(int coins, int total);
}
