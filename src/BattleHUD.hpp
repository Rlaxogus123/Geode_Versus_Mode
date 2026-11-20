// 배틀 중 PlayLayer 위에 얹는 플레이어 카드 HUD의 선언.
// BattleHUD는 1v1(내 카드 + 상대 카드), GroupHUD는 그룹 배틀(내 카드 + 선두 카드)용이다.
// 카드 위치/크기/투명도 설정을 읽고 쓰는 함수도 함께 둔다.
#pragma once

#include "VersusService.hpp"
#include <Geode/Geode.hpp>
#include <memory>

namespace versus {
// 생명(하트) 아이콘을 만든다(size는 한 변 길이).
cocos2d::CCNodeRGBA* createLifeHeart(float size);
// parent 위에 플레이어 칭호 배지를 만들어 붙인다(구현은 다른 파일).
cocos2d::CCNode* createTitleBadge(cocos2d::CCNode* parent, int title, cocos2d::CCPoint position, float scale,
                                  float maxWidth, bool animated);
// 1v1 배틀 HUD. 왼쪽(호스트)/오른쪽(게스트) 카드에 이름, 현재 퍼센트, 남은 목숨/시도, 최고 기록을 표시한다.
class BattleHUD final : public cocos2d::CCNode {
    struct Impl;
    std::unique_ptr<Impl> m;
    bool init(PlayerProfile const&, PlayerProfile const&, GameRules const&);

public:
    BattleHUD();
    ~BattleHUD();
    static BattleHUD* create(PlayerProfile const& host, PlayerProfile const& guest, GameRules const& rules);
    // 양쪽 플레이어 상태로 카드 내용을 갱신한다.
    void updatePlayers(BattlePlayerState const& host, BattlePlayerState const& guest);
    // 좌석별 규칙(관전 중인 쪽의 무제한 시도 등)을 지정한다.
    void setSeatRules(GameRules const& host, GameRules const& guest);
    // 카드 투명도(0~255). 왼쪽, 오른쪽.
    void setCardOpacity(GLubyte left, GLubyte right);
    // 핵 사용 정보 문구를 양쪽 카드에 표시한다.
    void setHacks(std::string const& left, std::string const& right);
    // 저장된 위치/크기 설정에 맞춰 카드를 다시 배치한다.
    void applyLayout();
    // 한쪽 카드에 리롤(REROLL) 알림 줄을 띄운다.
    void showReroll(bool left);
    // 시간제: 내 카드(왼쪽)의 남은 시간(ms, 음수면 숨김)과 시간이 끝났는지(버저비터 색).
    void setTime(int64_t leftMs, bool timeUp);
    void update(float dt) override;
};
// 카드 투명도 설정(0~100%). own은 내 카드, 아니면 상대 카드.
int battleCardOpacity(bool own);
void setBattleCardOpacity(bool own, int percent);
GLubyte battleCardAlpha(bool own);
// 텍스트 투명도 설정(0~100%)과 그 값을 0~255로 바꾼 값.
int battleTextOpacity();
void setBattleTextOpacity(int percent);
GLubyte battleTextAlpha();
// 배치 설정의 대상: 내 카드 / 상대 카드 / 안내 텍스트.
enum BattlePart : int { kPartOwn = 0, kPartOpponent = 1, kPartText = 2 };
// 배치 기준 위치(기본은 부분별 기본 위치, 그 외 모서리).
enum BattleAnchor : int {
    kAnchorDefault = 0,
    kAnchorTopLeft = 1,
    kAnchorTopRight = 2,
    kAnchorBottomLeft = 3,
    kAnchorBottomRight = 4
};
// 배치 오프셋의 최대 절댓값(px)과 크기 비율의 범위(%).
inline constexpr int kPlacementOffsetMax = 240, kPlacementScaleMin = 50, kPlacementScaleMax = 150;
// HUD 부분별 배치 설정: 기준 모서리, 오프셋 x/y, 크기(%).
struct BattlePlacement {
    int anchor = kAnchorDefault, x = 0, y = 0, scale = 100;
    bool operator==(BattlePlacement const&) const = default;
};
// 설정을 읽고 쓴다(범위 안으로 제한).
BattlePlacement battlePlacement(int part);
void setBattlePlacement(int part, BattlePlacement const& value);
// 카드의 왼쪽 아래 원점 좌표를 계산한다. 설정과 창 크기, 카드 폭을 반영한다.
cocos2d::CCPoint battleCardOrigin(int part, cocos2d::CCSize window, float width);
// 안내 텍스트의 위치, 앵커, 배율.
struct BattleTextPlace {
    cocos2d::CCPoint at, anchor;
    float scale;
};
BattleTextPlace battleTextPlace(cocos2d::CCSize window);
// 그룹 배틀 HUD. 내 카드와 현재 가장 앞선 다른 플레이어(선두) 카드, 순위/생존자 수 헤더를 표시한다.
class GroupHUD final : public cocos2d::CCNode {
    struct Impl;
    std::unique_ptr<Impl> m;
    bool init(PlayerProfile const&, GameRules const&);

public:
    GroupHUD();
    ~GroupHUD();
    static GroupHUD* create(PlayerProfile const& self, GameRules const& rules);
    // 내 상태와 그룹 배틀 정보로 선두 카드와 헤더를 갱신한다.
    void updateGroup(BattlePlayerState const& self, GroupBattle const& battle, std::string const& selfUid);
    void setCardOpacity(GLubyte own, GLubyte other);
    // 시간제: 내 카드의 남은 시간(ms, 음수면 숨김)과 시간이 끝났는지.
    void setTime(int64_t leftMs, bool timeUp);
    void update(float dt) override;
};
}
