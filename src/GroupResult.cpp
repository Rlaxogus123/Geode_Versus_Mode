// 그룹 배틀의 결과 화면을 만든다. 왼쪽에 우승자(또는 무승부 공동 선두) 쇼케이스,
// 오른쪽에 스크롤 순위표(순위, 아이콘, 이름, 이전 기록, 퍼센트, 결과 문구)를 그리고,
// 하단에 나가기 버튼과 자동 복귀 타이머를 둔다.
#include "GroupResult.hpp"
#include "BattleHUD.hpp"
#include "BattleProgress.hpp"
#include "Edition.hpp"
#include "MenuCursor.hpp"
#include "MapRecord.hpp"
#include "compat/Compat.hpp"
#include <Geode/binding/CCCircleWave.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <tuple>

using namespace geode::prelude;
namespace versus {
namespace {
// 프로필의 아이콘/색/오라를 입힌 결과 화면용 아이콘.
SimplePlayer* resultIcon(PlayerProfile const& profile, float scale) {
    auto* manager = GameManager::sharedState();
    int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
    auto* icon = SimplePlayer::create(std::clamp(profile.icon, 1, count));
    applyIconLook(icon, profile.color1, profile.color2, profile.glow);
    addMembershipAura(icon, profile.membership, profile.cosmetics);
    icon->setScale(scale);
    return icon;
}

// 위치와 최대 폭을 지정해 라벨을 만들고 parent에 붙이는 헬퍼.
CCLabelBMFont* text(CCNode* parent, std::string const& value, char const* font, float scale, CCPoint at,
                    ccColor3B color = ccWHITE, float maxWidth = 0.f, CCPoint anchor = {.5f, .5f}) {
    auto* label = createCosmeticLabel(value.c_str(), font);
    label->setAnchorPoint(anchor);
    label->setPosition(at);
    label->setColor(color);
    if (maxWidth > 0.f)
        label->limitLabelWidth(maxWidth, scale, .1f);
    else
        label->setScale(scale);
    parent->addChild(label, 2);
    return label;
}

// 순위 비교 키: (클리어면 100, 아니면 최고 퍼센트), 적게 쓴 시도 우선, 유저 코인.
std::tuple<int, int, int> finalKey(BattlePlayerState const& state) {
    return {state.cleared ? 100 : state.bestPercent, -state.attemptsUsed, state.userCoins};
}

// 1.0.8 시간제(Time) 순위 키: 클리어한 좌석이 먼저(서버가 본 클리어 순서), 나머지는 최고 %(같으면 공동 순위).
std::tuple<int, int64_t, int> timeKey(GroupSeat const& seat) {
    if (seat.state.cleared) return {1, -seat.clearedAt, 0};
    return {0, 0, seat.state.bestPercent};
}

// 두 좌석의 순위가 같은지(공동 순위). 시간제만 순위를 나눈다.
bool sameRank(GroupBattle const& battle, GroupSeat const& left, GroupSeat const& right) {
    return battle.rules.mode == 2 && !left.state.forfeited && !right.state.forfeited &&
           timeKey(left) == timeKey(right);
}

// 경과 시간 문구(m:ss, 내림).
std::string elapsedText(int64_t ms) {
    int64_t const seconds = std::clamp<int64_t>(ms / 1000, 0, 5999);
    return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// 시간제: 출발(카운트다운이 끝난 때)부터 클리어까지 걸린 시간(ms). 모르면 -1.
int64_t clearTime(GroupBattle const& battle, GroupSeat const& seat) {
    if (!seat.state.cleared || seat.clearedAt <= 0 || battle.releasedAt <= 0) return -1;
    return std::max<int64_t>(0, seat.clearedAt - battle.releasedAt - kBattleCountdownMs);
}

// 순위표 오른쪽에 쓸 결과 문구. 치트/포기/클리어를 우선하고, 레이스는 목표 도달 여부,
// 연습은 실패 횟수, 시도 모드는 사용한 목숨을 표시한다.
std::string outcome(GroupBattle const& battle, GroupSeat const& seat, GameRules const& rules) {
    if (seat.disqualified) return "CHEAT";
    if (seat.state.forfeited) return "LEFT";
    if (rules.mode == 2) {  // 시간제: 클리어는 걸린 시간, 아니면 시간 종료(마지막 판까지 했을 때)
        if (seat.state.cleared) {
            auto const at = clearTime(battle, seat);
            return at >= 0 ? fmt::format("CLEAR {}", elapsedText(at)) : std::string("CLEAR");
        }
        return seat.state.lastRun > 0 ? std::string("TIME UP") : fmt::format("{} FAILS", seat.state.attemptsUsed);
    }
    if (seat.state.cleared) return "CLEAR";
    if (rules.mode == 1)
        return battle::raceReached(seat.state) >= rules.targetPercent
                   ? std::string("TARGET")
                   : fmt::format("{} FAILS", seat.state.attemptsUsed);
    if (rules.practice) return fmt::format("{} FAILS", seat.state.attemptsUsed);
    int const used = std::clamp(seat.state.attemptsUsed, 0, rules.attempts);
    return fmt::format("{} / {} LIVES", used, rules.attempts);
}

// 표시 순서. 서버의 ranking을 우선 쓰되 누락이 있으면 포기하지 않은 쪽을 앞에 두고 finalKey(시간제는 timeKey)로
// 정렬한다.
std::vector<int> order(GroupBattle const& battle) {
    std::vector<int> result;
    for (int index : battle.ranking)
        if (index >= 0 && index < static_cast<int>(battle.seats.size()) &&
            std::find(result.begin(), result.end(), index) == result.end())
            result.push_back(index);
    if (result.size() == battle.seats.size()) return result;
    result.resize(battle.seats.size());
    std::iota(result.begin(), result.end(), 0);
    std::stable_sort(result.begin(), result.end(), [&battle](int a, int b) {
        auto const& left = battle.seats[a];
        auto const& right = battle.seats[b];
        if (left.state.forfeited != right.state.forfeited) return !left.state.forfeited;
        if (battle.rules.mode == 2) return timeKey(left) > timeKey(right);
        return finalKey(left.state) > finalKey(right.state);
    });
    return result;
}

// 우승자 아이콘 뒤에서 1.1초마다 퍼져 나가는 원형 파동 연출.
void pulseWaves(CCNode* parent, CCPoint at, ccColor3B color) {
    auto* holder = CCNode::create();
    parent->addChild(holder, 1);
    holder->runAction(CCRepeatForever::create(CCSequence::create(CallFuncExt::create([holder, at, color] {
                                                                     if (auto* wave = CCCircleWave::create(
                                                                             10.f, 78.f, .9f, false, true)) {
                                                                         wave->m_color = color;
                                                                         wave->setPosition(at);
                                                                         holder->addChild(wave);
                                                                     }
                                                                 }),
                                                                 CCDelayTime::create(1.1f), nullptr)));
}
}

// 결과 화면 루트 노드를 만든다. 우승자는 서버가 정한 winnerUid, 무승부면 공동 선두(최대 3명)를 보여 준다.
CCNode* createGroupResult(GroupBattle const& battle, std::string const& selfUid, char const* exitLabel,
                          CCObject* target, SEL_MenuHandler onExit, std::map<std::string, int> const* before) {
    auto const window = CCDirector::sharedDirector()->getWinSize();
    auto* root = CCNode::create();
    root->setID("group-result"_spr);
    root->setContentSize(window);
    keepMenuCursorVisible(root);
    root->addChild(CCLayerColor::create(ccc4(5, 18, 46, 222)), 0);
    text(root, "BATTLE RESULT", "goldFont.fnt", .75f, {window.width / 2.f, window.height - 20.f});

    auto const ranking = order(battle);
    float const showcaseX = window.width * .27f;
    float const middle = window.height / 2.f + 6.f;
    std::vector<int> leaders;
    // 쇼케이스에 올릴 선두 좌석 결정: 승자가 있으면 그 사람, 무승부면 같은 결과인 상위 좌석들.
    if (!battle.draw) {
        int const winner = battle.seatOf(battle.winnerUid);
        if (winner >= 0) leaders.push_back(winner);
    } else
        for (int index : ranking) {
            auto const& seat = battle.seats[index];
            if (seat.state.forfeited) break;
            bool const tied =
                battle.rules.mode == 1
                    ? std::find(battle.first.begin(), battle.first.end(), index) != battle.first.end()
                : battle.rules.mode == 2
                    ? leaders.empty() || timeKey(seat) == timeKey(battle.seats[leaders.front()])
                    : leaders.empty() || finalKey(seat.state) == finalKey(battle.seats[leaders.front()].state);
            if (!tied) break;
            leaders.push_back(index);
            if (leaders.size() == 3) break;
        }
    text(root, battle.draw ? "DRAW" : "WINNER", "bigFont.fnt", .6f, {showcaseX, window.height - 52.f},
         battle.draw ? ccc3(190, 220, 255) : ccc3(255, 214, 90));
    // 선두가 1명이면 크게 표시하고(이름, 칭호, 기록, 본인이면 YOU WON!), 여러 명이면 나란히 작게 표시한다.
    if (leaders.size() == 1) {
        auto const& seat = battle.seats[leaders.front()];
        pulseWaves(root, {showcaseX, middle + 14.f}, ccc3(255, 225, 140));
        auto* icon = resultIcon(seat.profile, 2.5f);
        icon->setPosition({showcaseX, middle + 14.f});
        root->addChild(icon, 3);
        icon->runAction(CCRepeatForever::create(
            CCSequence::create(CCEaseSineInOut::create(CCMoveBy::create(.9f, {0.f, 5.f})),
                               CCEaseSineInOut::create(CCMoveBy::create(.9f, {0.f, -5.f})), nullptr)));
        auto* name =
            text(root, seat.profile.name, "bigFont.fnt", .7f, {showcaseX, middle - 38.f}, ccWHITE, window.width * .44f);
        styleNickname(name, seat.profile.membership, seat.profile.cosmetics);
        createTitleBadge(root, seat.profile.cosmetics.title, {showcaseX, middle - 58.f}, .85f, window.width * .4f,
                         true);
        // 기록 줄: 시간제는 클리어에 걸린 시간(또는 제한 시간 안의 최고 %), 다른 모드는 실패한 판 수.
        std::string line;
        if (battle.rules.mode == 2) {
            auto const at = clearTime(battle, seat);
            line = seat.state.cleared ? (at >= 0 ? fmt::format("100%   |   cleared in {}", elapsedText(at))
                                                 : std::string("100%   |   cleared"))
                   : battle.timeLimitMs > 0
                       ? fmt::format("{}%   |   best in {}", seat.state.bestPercent, timeClockText(battle.timeLimitMs))
                       : fmt::format("{}%", seat.state.bestPercent);
        } else
            line = fmt::format("{}%   |   {} failed {}", seat.state.cleared ? 100 : seat.state.bestPercent,
                               seat.state.attemptsUsed, seat.state.attemptsUsed == 1 ? "run" : "runs");
        text(root, line, "chatFont.fnt", .75f, {showcaseX, middle - 77.f}, ccc3(180, 230, 255), window.width * .44f);
        if (seat.profile.uid == selfUid)
            text(root, "YOU WON!", "goldFont.fnt", .6f, {showcaseX, middle - 96.f}, ccc3(255, 235, 150));
    } else if (!leaders.empty()) {
        float const spacing = std::min(70.f, window.width * .4f / static_cast<float>(leaders.size()));
        for (size_t i = 0; i < leaders.size(); ++i) {
            auto const& seat = battle.seats[leaders[i]];
            float const x = showcaseX + (static_cast<float>(i) - (leaders.size() - 1) / 2.f) * spacing;
            auto* icon = resultIcon(seat.profile, 1.5f);
            icon->setPosition({x, middle + 10.f});
            root->addChild(icon, 3);
            auto* name = text(root, seat.profile.name, "bigFont.fnt", .38f, {x, middle - 22.f}, ccWHITE, spacing - 6.f);
            styleNickname(name, seat.profile.membership, seat.profile.cosmetics);
        }
        text(root, "Equal result - nobody wins this round", "chatFont.fnt", .7f, {showcaseX, middle - 52.f},
             ccc3(190, 220, 255), window.width * .44f);
    } else
        text(root, "Nobody finished the battle", "chatFont.fnt", .7f, {showcaseX, middle}, ccc3(190, 220, 255));

    // 오른쪽 순위표: 패널 위에 ScrollLayer를 올리고 행마다 순위, 아이콘, 이름, 이전 기록, 퍼센트, 결과를 그린다.
    float const listX = window.width * .53f;
    float const listWidth = window.width - listX - 14.f;
    float const listTop = window.height - 44.f, listBottom = 56.f;
    auto* panel = CCScale9Sprite::create("square02b_001.png");
    panel->setColor(ccc3(8, 32, 70));
    panel->setOpacity(230);
    panel->setContentSize({listWidth + 8.f, listTop - listBottom + 22.f});
    panel->setAnchorPoint({0.f, 0.f});
    panel->setPosition({listX - 4.f, listBottom - 4.f});
    root->addChild(panel, 1);
    text(root, "RANKING", "goldFont.fnt", .5f, {listX + listWidth / 2.f, listTop + 8.f});
    float const rowHeight = 19.f;
    float const viewHeight = listTop - listBottom - 6.f;
    auto* scroll = versus::compat::scrollLayer({listWidth, viewHeight});
    scroll->setPosition({listX, listBottom});
    root->addChild(scroll, 3);
    float const contentHeight = std::max(viewHeight, rowHeight * ranking.size());
    scroll->m_contentLayer->setContentSize({listWidth, contentHeight});
    auto const& rules = battle.rules;
    size_t shown = 0;  // 표시할 순위 번호(시간제는 공동 순위가 같은 번호를 쓴다)
    for (size_t place = 0; place < ranking.size(); ++place) {
        auto const& seat = battle.seats[ranking[place]];
        if (place == 0 || !sameRank(battle, seat, battle.seats[ranking[place - 1]])) shown = place + 1;
        bool const mine = seat.profile.uid == selfUid;
        float const y = contentHeight - rowHeight * (place + .5f);
        auto* row = CCNode::create();
        row->setContentSize({listWidth, rowHeight});
        row->setAnchorPoint({0.f, .5f});
        row->setPosition({0.f, y});
        scroll->m_contentLayer->addChild(row);
        auto* strip = CCLayerColor::create(mine        ? ccc4(140, 105, 20, 150)
                                           : place % 2 ? ccc4(20, 60, 110, 120)
                                                       : ccc4(12, 44, 90, 120),
                                           listWidth, rowHeight - 2.f);
        strip->setPosition({0.f, 1.f});
        row->addChild(strip);
        bool const winner = !battle.draw && seat.profile.uid == battle.winnerUid;
        text(row, fmt::format("#{}", shown), "goldFont.fnt", .42f, {16.f, rowHeight / 2.f},
             winner ? ccc3(255, 220, 90) : ccWHITE, 26.f);
        auto* icon = resultIcon(seat.profile, .42f);
        icon->setPosition({40.f, rowHeight / 2.f});
        row->addChild(icon, 2);
        std::string record;
        ccColor3B recordColor = ccWHITE;
        if (before)
            if (auto found = before->find(seat.profile.uid); found != before->end())
                std::tie(record, recordColor) = mapRecordText(found->second, true);
        float const recordWidth = record.empty() ? 0.f : 50.f;
        auto* name = text(row, seat.profile.name, "bigFont.fnt", .32f, {54.f, rowHeight / 2.f}, ccWHITE,
                          listWidth - 150.f - recordWidth, {0.f, .5f});
        styleNickname(name, seat.profile.membership, seat.profile.cosmetics);
        if (!record.empty())
            text(row, record, "chatFont.fnt", .5f, {listWidth - 110.f, rowHeight / 2.f}, recordColor, 40.f, {1.f, .5f});
        text(row, fmt::format("{}%", seat.state.cleared ? 100 : seat.state.bestPercent), "bigFont.fnt", .36f,
             {listWidth - 66.f, rowHeight / 2.f}, seat.state.forfeited ? ccc3(170, 170, 185) : ccc3(180, 236, 255),
             40.f, {1.f, .5f});
        auto const result = outcome(battle, seat, rules);
        text(row, result, "bigFont.fnt", .22f, {listWidth - 4.f, rowHeight / 2.f},
             seat.disqualified || seat.state.forfeited ? ccc3(255, 140, 150)
             : seat.state.cleared                      ? ccc3(138, 255, 193)
                                                       : ccc3(150, 190, 225),
             58.f, {1.f, .5f});
    }
    scroll->scrollToTop();

    // 나가기 버튼, 남은 시간 문구, 하단 타이머 바.
    auto* menu = CCMenu::create();
    menu->setPosition({window.width / 2.f, 26.f});
    auto* sprite = ButtonSprite::create(exitLabel, "goldFont.fnt", "GJ_button_01.png");
    sprite->setScale(.7f);
    menu->addChild(CCMenuItemSpriteExtra::create(sprite, target, onExit));
    root->addChild(menu, 5);
    auto* countdown = text(root, "", "chatFont.fnt", .6f, {window.width / 2.f, 47.f}, ccc3(190, 220, 255));
    countdown->setID("group-result-countdown"_spr);
    float const timerWidth = std::min(440.f, window.width - 48.f);
    auto* track = CCLayerColor::create(ccc4(255, 255, 255, 48), timerWidth, 2.f);
    track->setPosition({(window.width - timerWidth) / 2.f, 6.f});
    root->addChild(track, 5);
    auto* fill = CCLayerColor::create(ccc4(255, 255, 255, 240), timerWidth, 2.f);
    fill->setPosition(track->getPosition());
    fill->setID("group-result-timer"_spr);
    root->addChild(fill, 6);
    return root;
}

// 남은 시간으로 카운트다운 문구와 바 길이를 갱신한다.
void updateGroupResult(CCNode* result, float remaining, float total) {
    if (!result) return;
    remaining = std::clamp(remaining, 0.f, total);
    if (auto* label = typeinfo_cast<CCLabelBMFont*>(result->getChildByID("group-result-countdown"_spr))) {
        auto value = fmt::format("Auto-return in {}s", static_cast<int>(std::ceil(remaining)));
        if (value != label->getString()) label->setString(value.c_str());
    }
    if (auto* fill = typeinfo_cast<CCLayerColor*>(result->getChildByID("group-result-timer"_spr))) {
        float const width = std::min(440.f, CCDirector::sharedDirector()->getWinSize().width - 48.f);
        fill->setContentSize({width * (total > 0.f ? remaining / total : 0.f), 2.f});
    }
}
}
