// 그룹 배틀 free play / LIVE 관전 모드. 배틀 맵을 PlayLayer로 열어 자유롭게 플레이하게 하고
// (기록은 반영되지 않음), 오른쪽 위에 현재 선두 카드, 오른쪽 아래에 Spectate/Switch 버튼을 둔다.
// 관전을 고르면 replay::liveWatch로 다른 플레이어의 진행을 실시간으로 보여 준다.
// 그룹 멤버는 서비스의 group 상태를, 방 밖 관전자(outside)는 서버를 주기적으로 조회해 배틀 상태를 얻는다.
#include "GroupFreePlay.hpp"
#include "PercentGuard.hpp"
#include "BattleHUD.hpp"
#include "BattleLevelData.hpp"
#include "BattleProgress.hpp"
#include "BattleSession.hpp"
#include "Edition.hpp"
#include "MapCache.hpp"
#include "MatchLaunch.hpp"
#include "MenuCursor.hpp"
#include "Replay.hpp"
#include "Season2Lounge.hpp"
#include "VersusService.hpp"
#include "VersusUI.hpp"
#include "WeakRefs.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

using namespace geode::prelude;

namespace versus::freeplay {
namespace {
// 맵을 클리어한 뒤 자동으로 처음부터 다시 시작하기까지의 시간(초).
constexpr float kClearRestartSeconds = 3.f;
// 상태 문구/선두 카드를 갱신하는 주기(초).
constexpr float kWatchSeconds = .5f;
// 방 밖 관전자가 서버에 상태를 다시 묻는 주기(초): 그룹 방 / 1v1 LIVE 목록.
constexpr float kOutsideGroupSeconds = 1.f, kOutsideListSeconds = 3.f;
// 선두 카드의 크기와 진행 바 폭.
constexpr float kLeaderW = 166.f, kLeaderH = 58.f, kLeaderBarW = 112.f;

// 탈락한 좌석인지(포기, 탈락, 실격).
bool outOf(GroupSeat const& seat) {
    return seat.state.forfeited || seat.out || seat.disqualified;
}
// 선두 카드에 올릴 좌석. 종료됐으면 승자, 서버 ranking이 있으면 1위,
// 아니면 탈락하지 않은 쪽, 클리어, 최고/현재 퍼센트 순으로 비교한다.
int leaderSeat(GroupBattle const& battle) {
    int const count = static_cast<int>(battle.seats.size());
    if (battle.finishedAt > 0 && !battle.draw)
        if (int const winner = battle.seatOf(battle.winnerUid); winner >= 0) return winner;
    if (battle.ranking.size() == battle.seats.size() && !battle.ranking.empty() && battle.ranking.front() >= 0 &&
        battle.ranking.front() < count)
        return battle.ranking.front();
    auto better = [](GroupSeat const& x, GroupSeat const& y) {
        if (outOf(x) != outOf(y)) return !outOf(x);
        if (x.state.cleared != y.state.cleared) return x.state.cleared;
        if (x.state.bestPercent != y.state.bestPercent) return x.state.bestPercent > y.state.bestPercent;
        return x.state.currentPercent > y.state.currentPercent;
    };
    int best = count > 0 ? 0 : -1;
    for (int i = 1; i < count; ++i)
        if (better(battle.seats[i], battle.seats[best])) best = i;
    return best;
}
// 좌석 상태를 문구로 만들고 상태에 맞는 색을 color에 채운다.
std::string seatStatus(GroupSeat const& seat, GameRules const& rules, ccColor3B& color) {
    auto const& state = seat.state;
    if (seat.disqualified) {
        color = ccc3(255, 90, 90);
        return "DISQUALIFIED";
    }
    if (state.cleared) {
        color = ccc3(120, 255, 150);
        return "CLEARED";
    }
    if (state.forfeited || seat.out) {
        color = ccc3(150, 155, 170);
        return "OUT";
    }
    if (battle::resultFinal(state, rules)) {
        color = ccc3(255, 200, 110);
        return rules.mode == 1 ? "TARGET REACHED" : "NO LIVES LEFT";
    }
    if (state.paused) {
        color = ccc3(255, 230, 90);
        return "PAUSED";
    }
    if (state.inAttempt) {
        color = ccc3(110, 220, 255);
        return fmt::format("PLAYING - now {}%", state.currentPercent);
    }
    color = ccc3(190, 200, 220);
    return "RESTARTING";
}
CCLabelBMFont* text(CCNode* parent, char const* font, float scale, CCPoint at, CCPoint anchor,
                    ccColor3B color = {255, 255, 255}) {
    auto* label = createCosmeticLabel("", font);
    label->setScale(scale);
    label->setAnchorPoint(anchor);
    label->setPosition(at);
    label->setColor(color);
    parent->addChild(label);
    return label;
}

// free play 세션을 관리하는 싱글턴 노드. 현재 PlayLayer, HUD, 선두 카드, 관전 대상,
// 방 밖 관전 상태(outside)를 들고 매 프레임 update로 갱신한다.
class Controller final : public CCNode {
public:
    Weak<PlayLayer> play;
    Weak<CCNode> hud;
    std::string battleID, shownNote;
    bool constructing = false, leaving = false;
    float restart = 0.f, watch = 0.f;
    Weak<CCNode> leader;
    CCNode* leaderIcon = nullptr;
    CCLabelBMFont *leaderTitle = nullptr, *leaderName = nullptr, *leaderStatus = nullptr, *leaderBest = nullptr,
                  *leaderLives = nullptr;
    CCNode* leaderHeart = nullptr;
    CCLayerColor *leaderFill = nullptr, *leaderMarker = nullptr;
    std::string leaderUid, shownLeader;
    std::string watchUid, watchName;
    float watchOver = 0.f;
    Weak<CCNode> buttons;
    bool buttonsShown = false;
    // 방 밖(LIVE)에서 보는 배틀의 상태. 서버에서 주기적으로 받아 온 배틀 스냅샷이다.
    struct Outside {
        std::string roomId, roomName;
        bool group = false;
        GroupBattle battle;
        std::string etag;
        bool busy = false, gone = false;
        float wait = 0.f;
    };
    std::optional<Outside> outside;
    // reset 때마다 올려서 이전 세션의 늦은 응답을 버리는 번호.
    std::uint64_t outsideEpoch = 0;

    inline static Controller* instance = nullptr;
    static Controller* peek() { return instance; }
    static Controller& get() {
        static auto* controller = [] {
            auto* node = new Controller;
            node->init();
            instance = node;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 4, false);
            node->release();
            return node;
        }();
        return *controller;
    }
    bool owns(GJBaseGameLayer const* layer) const {
        if (!layer) return false;
        auto owner = play.lock();
        return owner && static_cast<GJBaseGameLayer const*>(owner.data()) == layer;
    }
    // 세션 상태를 모두 초기화하고 관전 연결을 끊는다.
    void reset() {
        play = nullptr;
        hud = nullptr;
        leader = nullptr;
        buttons = nullptr;
        battleID.clear();
        shownNote.clear();
        leaderUid.clear();
        shownLeader.clear();
        watchUid.clear();
        watchName.clear();
        watchOver = 0.f;
        buttonsShown = false;
        constructing = leaving = false;
        restart = 0.f;
        watch = 0.f;
        outside.reset();
        ++outsideEpoch;
        replay::liveWatch(nullptr, {}, {});
    }
    // 지금 보고 있는 배틀 스냅샷. 방 밖이면 서버에서 받은 것, 멤버면 서비스의 group 배틀. 끝났거나 ID가 다르면 nullptr.
    GroupBattle const* battleNow() const {
        if (outside) return !outside->gone && outside->battle.id == battleID ? &outside->battle : nullptr;
        auto const& group = Service::get().group();
        return group && group->battle && group->battle->id == battleID ? &*group->battle : nullptr;
    }
    // 아직 진행 중이라 관전할 만한 플레이어가 있는지.
    bool someoneToWatch() const {
        auto const* battle = battleNow();
        if (!battle || battle->finishedAt > 0 || battle->releasedAt == 0) return false;
        if (!battle::watchableRules(battle->rules)) return false;
        for (auto const& seat : battle->seats)
            if (!seat.out && !seat.disqualified && !battle::resultFinal(seat.state, battle->rules)) return true;
        return false;
    }
    // 관전을 끝낸다. restartMap이면 관전하던 맵을 처음부터 다시 시작한다.
    void stopWatching(bool restartMap) {
        auto owner = play.lock();
        bool const was = owner && replay::liveWatching(owner.data());
        watchUid.clear();
        watchName.clear();
        watchOver = 0.f;
        replay::liveWatch(nullptr, {}, {});
        if (was && restartMap) owner->resetLevelFromStart();
        watch = 0.f;
    }
    // 관전할 플레이어를 고른다: 본인과 except를 제외하고 아직 끝나지 않은 사람 중 가장 멀리 간 사람의 uid를 돌려주고 이름은 name에 채운다.
    std::string watchable(std::string const& except, std::string& name) const {
        auto const* battle = battleNow();
        if (!battle || battle->finishedAt > 0 || battle->releasedAt == 0 || !battle::watchableRules(battle->rules))
            return {};
        int best = -1;
        for (int i = 0; i < static_cast<int>(battle->seats.size()); ++i) {
            auto const& seat = battle->seats[i];
            if (seat.out || seat.disqualified || battle::resultFinal(seat.state, battle->rules) ||
                seat.profile.uid.empty() || seat.profile.uid == except ||
                seat.profile.uid == Service::get().profile().uid)
                continue;
            auto const reach = [&](GroupSeat const& x) {
                return std::max(x.state.bestPercent, x.state.currentPercent);
            };
            if (best < 0 || reach(seat) > reach(battle->seats[best])) best = i;
        }
        if (best < 0) return {};
        name = battle->seats[best].profile.name.empty() ? std::string("Player") : battle->seats[best].profile.name;
        return battle->seats[best].profile.uid;
    }
    // 다음(또는 다른) 관전 대상으로 바꾼다. 없으면 알림.
    void watchNext(bool switching) {
        std::string name;
        auto uid = watchable(switching ? watchUid : std::string(), name);
        if (uid.empty()) {
            Notification::create(switching ? "The other player cannot be watched now." : "Nobody can be watched now.",
                                 NotificationIcon::Info, 2.f)
                ->show();
            return;
        }
        watchUid = std::move(uid);
        watchName = std::move(name);
        watchOver = 0.f;
        watch = 0.f;
    }
    // Spectate / Stop Spectate / Switch 버튼 메뉴를 만든다.
    // 방 밖 1v1이면 바로 다음 대상으로, 그 외에는 그룹 순위 화면에서 고르게 한다.
    void buildButtons(CCNode* root) {
        auto* holder = CCNode::create();
        holder->setID("group-freeplay-spectate"_spr);
        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        auto add = [menu](char const* text, char const* id, std::function<void()> action) {
            auto* sprite = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_05.png", 1.f);
            sprite->setScale(battle::kSpectateButtonScale);
            auto* item = CCMenuItemExt::createSpriteExtra(sprite, [action](CCMenuItemSpriteExtra*) { action(); });
            item->setID(id);
            menu->addChild(item);
        };
        auto choose = [](bool switching) {
            auto* c = Controller::peek();
            if (c && c->outside && !c->outside->group)
                c->watchNext(switching);
            else
                versus::showGroupStandings();
        };
        add("Spectate", "freeplay-spectate-button"_spr, [choose] { choose(false); });
        add("Stop Spectate", "freeplay-stop-button"_spr, [] {
            if (auto* c = Controller::peek()) c->stopWatching(true);
        });
        add("Switch", "freeplay-switch-button"_spr, [choose] { choose(true); });
        holder->addChild(menu);
        holder->setVisible(false);
        root->addChild(holder, 61);
        rebind(buttons, holder);
    }
    // 관전 가능 상태일 때만 버튼을 보이고, 관전 중 여부에 따라 버튼을 바꿔 오른쪽 아래에 배치한다.
    // 처음 보일 때 데스크톱에서는 커서를 풀어 준다.
    void updateButtons(PlayLayer* owner) {
        auto holder = buttons.lock();
        if (!holder) return;
        bool const watching = !watchUid.empty();
        bool const shown = !owner->m_isPaused && !leaving && (watching || someoneToWatch());
        holder->setVisible(shown);
        if (!shown) {
            buttonsShown = false;
            releaseMenuCursor(holder.data());
            return;
        }
        keepMenuCursorVisible(holder.data());
        auto* start = holder->getChildByIDRecursive("freeplay-spectate-button"_spr);
        auto* stop = holder->getChildByIDRecursive("freeplay-stop-button"_spr);
        auto* swap = holder->getChildByIDRecursive("freeplay-switch-button"_spr);
        if (start && stop && swap) {
            start->setVisible(!watching);
            stop->setVisible(watching);
            swap->setVisible(watching);
            float right = CCDirector::sharedDirector()->getWinSize().width - battle::kSpectateButtonMargin;
            for (auto* item : {watching ? stop : start, swap}) {
                if (!item->isVisible()) continue;
                float const width = item->getScaledContentSize().width;
                item->setPosition({right - width / 2.f, battle::kSpectateButtonY});
                right -= width + battle::kSpectateButtonGap;
            }
        }
        if (buttonsShown) return;
        buttonsShown = true;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
    }
    // 관전 상태 갱신. 대상의 시도가 끝났거나 배틀이 끝나면 3초 뒤 다음 대상으로 넘어가거나 free play로 돌아간다.
    // 대상이 플레이 중이면 replay::liveWatch로 실시간 진행을 따라 본다.
    void updateWatch(PlayLayer* owner, float dt) {
        if (watchUid.empty()) return;
        auto const* battle = battleNow();
        GroupSeat const* seat = nullptr;
        if (battle)
            if (int const index = battle->seatOf(watchUid); index >= 0) seat = &battle->seats[index];
        bool const over = !seat || battle->finishedAt > 0 || seat->out || seat->disqualified ||
                          battle::resultFinal(seat->state, battle->rules);
        replay::liveTargetActive(seat && seat->state.inAttempt && !seat->state.paused);
        if (over && !replay::livePlaying()) {
            watchOver += dt;
            if (watchOver >= 3.f) {
                std::string name;
                auto next = outside ? watchable(watchUid, name) : std::string();
                if (!next.empty()) {
                    Notification::create(fmt::format("{} is done - watching {} now.", watchName, name),
                                         NotificationIcon::Info, 2.f)
                        ->show();
                    watchUid = std::move(next);
                    watchName = std::move(name);
                    watchOver = 0.f;
                    watch = 0.f;
                    return;
                }
                Notification::create(battle ? fmt::format("{} is done - back to free play.", watchName)
                                            : std::string("The battle is over - back to free play."),
                                     NotificationIcon::Info, 2.f)
                    ->show();
                stopWatching(true);
                return;
            }
        } else
            watchOver = 0.f;
        if (outside) {
            replay::liveWatch(owner, LiveTarget{outside->roomId, battleID, outside->group, watchUid}, watchName);
            return;
        }
        auto const& group = Service::get().group();
        if (!group) {
            stopWatching(true);
            return;
        }
        replay::liveWatch(owner, LiveTarget{group->id, battleID, true, watchUid}, watchName);
    }
    // 방 밖 관전자용 서버 조회. 그룹이면 watchGroup(etag로 변경 여부만 받음)을 1초마다,
    // 1v1이면 fetchLiveBattle을 3초마다 호출하고, 실패하면 간격을 늘리며 방/배틀이 사라지면 gone으로 표시한다.
    void pollOutside(float dt) {
        if (!outside || outside->busy || outside->gone || outside->battle.finishedAt > 0) return;
        outside->wait -= dt;
        if (outside->wait > 0.f) return;
        outside->busy = true;
        auto const epoch = outsideEpoch;
        if (outside->group) {
            outside->wait = kOutsideGroupSeconds;
            Service::get().watchGroup(outside->roomId, outside->battle, outside->etag, [epoch](GroupWatch answer) {
                auto* c = Controller::peek();
                if (!c || c->outsideEpoch != epoch || !c->outside || !c->outside->group) return;
                auto& view = *c->outside;
                view.busy = false;
                if (!answer.ok) {
                    view.wait = 3.f;
                    return;
                }
                if (!answer.etag.empty()) view.etag = answer.etag;
                if (answer.closed) {
                    view.gone = true;
                    return;
                }
                if (!answer.roomName.empty()) view.roomName = answer.roomName;
                if (answer.changed) view.battle = std::move(answer.battle);
            });
            return;
        }
        outside->wait = kOutsideListSeconds;
        Service::get().fetchLiveBattle(outside->roomId, [epoch](LiveBattleView answer) {
            auto* c = Controller::peek();
            if (!c || c->outsideEpoch != epoch || !c->outside || c->outside->group) return;
            auto& view = *c->outside;
            view.busy = false;
            if (!answer.ok) {
                view.wait = 4.f;
                return;
            }
            if (answer.removed || answer.match.battleId != c->battleID) {
                view.gone = true;
                return;
            }
            auto fresh = battleOf(answer.match);
            fresh.finishedAt = answer.finishedAt;
            fresh.winnerUid = answer.winnerUid;
            fresh.draw = answer.draw;
            view.battle = std::move(fresh);
        });
    }
    // 오른쪽 위 선두 카드(아이콘, 이름, 상태, 최고 퍼센트 바, 현재 위치 마커, 남은 목숨)를 만든다.
    void buildLeader(CCNode* root) {
        auto const window = CCDirector::sharedDirector()->getWinSize();
        auto* card = CCNode::create();
        card->setID("group-spectator-leader"_spr);
        card->setContentSize({kLeaderW, kLeaderH});
        card->setAnchorPoint({1.f, 1.f});
        card->ignoreAnchorPointForPosition(false);
        card->setPosition({window.width - 8.f, window.height - 42.f});
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({kLeaderW, kLeaderH});
        back->setAnchorPoint({0.f, 0.f});
        back->setColor({0, 0, 0});
        back->setOpacity(125);
        card->addChild(back);
        leaderTitle = text(card, "goldFont.fnt", .36f, {8.f, kLeaderH - 9.f}, {0.f, .5f});
        leaderLives = text(card, "bigFont.fnt", .3f, {kLeaderW - 8.f, kLeaderH - 9.f}, {1.f, .5f}, ccc3(255, 150, 170));
        leaderHeart = createLifeHeart(9.f);
        card->addChild(leaderHeart);
        leaderIcon = CCNode::create();
        leaderIcon->setPosition({18.f, 30.f});
        card->addChild(leaderIcon);
        leaderName = text(card, "bigFont.fnt", .32f, {34.f, 35.f}, {0.f, .5f});
        leaderStatus = text(card, "chatFont.fnt", .42f, {34.f, 24.f}, {0.f, .5f});
        auto* track = CCLayerColor::create(ccc4(255, 255, 255, 40), kLeaderBarW, 5.f);
        track->setPosition({8.f, 8.f});
        card->addChild(track);
        leaderFill = CCLayerColor::create(ccc4(255, 255, 255, 255), kLeaderBarW, 5.f);
        leaderFill->ignoreAnchorPointForPosition(false);
        leaderFill->setAnchorPoint({0.f, 0.f});
        leaderFill->setPosition({8.f, 8.f});
        leaderFill->setScaleX(0.f);
        card->addChild(leaderFill);
        leaderMarker = CCLayerColor::create(ccc4(255, 255, 255, 230), 2.f, 9.f);
        leaderMarker->setPosition({8.f, 6.f});
        card->addChild(leaderMarker);
        leaderBest = text(card, "bigFont.fnt", .36f, {kLeaderW - 8.f, 10.5f}, {1.f, .5f});
        root->addChild(card, 60);
        rebind(leader, card);
        leaderUid.clear();
        shownLeader.clear();
        showLeader();
    }
    // 선두 카드를 갱신한다. 표시 내용이 바뀐 경우에만 다시 그린다(key로 비교).
    void showLeader() {
        auto card = leader.lock();
        if (!card) return;
        GroupBattle const* battle = battleNow();
        int const index = battle ? leaderSeat(*battle) : -1;
        card->setVisible(index >= 0);
        if (index < 0) return;
        auto const& seat = battle->seats[index];
        auto const& state = seat.state;
        auto const& rules = battle->rules;
        bool const lives = rules.mode == 0 && !rules.practice;
        std::string const key =
            fmt::format("{}|{}|{}|{},{},{},{}{}{}{}{}{}", seat.profile.uid, battle->finishedAt, battle->draw,
                        state.bestPercent, state.currentPercent, state.attemptsUsed, state.cleared, state.forfeited,
                        state.paused, state.inAttempt, seat.disqualified, seat.out);
        if (key == shownLeader) return;
        shownLeader = key;
        if (seat.profile.uid != leaderUid) {
            leaderUid = seat.profile.uid;
            leaderIcon->removeAllChildrenWithCleanup(true);
            auto* manager = GameManager::sharedState();
            int const icons = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
            auto* icon = SimplePlayer::create(std::clamp(seat.profile.icon, 1, icons));
            applyIconLook(icon, seat.profile.color1, seat.profile.color2, seat.profile.glow);
            addMembershipAura(icon, seat.profile.membership, seat.profile.cosmetics);
            icon->setScale(.52f);
            leaderIcon->addChild(icon);
            leaderName->setString(seat.profile.name.empty() ? "Player" : seat.profile.name.c_str());
            styleNickname(leaderName, seat.profile.membership, seat.profile.cosmetics);
            leaderName->limitLabelWidth(kLeaderW - 42.f, .32f, .18f);
        }
        leaderTitle->setString(battle->finishedAt == 0 ? "LEADER" : battle->draw ? "TOP PLAYER" : "WINNER");
        ccColor3B color;
        leaderStatus->setString(seatStatus(seat, rules, color).c_str());
        leaderStatus->setColor(color);
        leaderStatus->limitLabelWidth(kLeaderW - 42.f, .42f, .25f);
        int const best = state.cleared ? 100 : std::clamp(state.bestPercent, 0, 100);
        leaderBest->setString(fmt::format("{}%", best).c_str());
        leaderFill->setScaleX(best / 100.f);
        leaderFill->setColor(state.cleared ? ccc3(110, 240, 140) : ccc3(255, 205, 80));
        bool const running = state.inAttempt && !state.cleared && battle->finishedAt == 0;
        leaderMarker->setVisible(running);
        leaderMarker->setPositionX(8.f + kLeaderBarW * std::clamp(state.currentPercent, 0, 100) / 100.f - 1.f);
        leaderLives->setVisible(lives);
        leaderHeart->setVisible(lives);
        if (lives) {
            leaderLives->setString(std::to_string(std::max(0, rules.attempts - state.attemptsUsed)).c_str());
            leaderHeart->setPosition({kLeaderW - 14.f - leaderLives->getScaledContentSize().width, kLeaderH - 9.f});
        }
    }
    // 상단 안내 문구: 배틀 진행/종료/무승부/승자 상황에 맞춰 free play임을 알린다.
    std::string note() const {
        if (outside) {
            auto const* battle = battleNow();
            if (!battle) return "The battle has ended. Play freely - nothing here counts.";
            if (battle->finishedAt > 0) {
                int const winner = battle->seatOf(battle->winnerUid);
                if (battle->draw || winner < 0)
                    return "The battle has ended in a draw. Play freely - nothing here counts.";
                return fmt::format("The battle has ended - {} wins. Play freely - nothing here counts.",
                                   battle->seats[winner].profile.name.empty() ? "Player"
                                                                              : battle->seats[winner].profile.name);
            }
            return "Spectating from LIVE. Play freely - practice mode works too, nothing here counts.";
        }
        auto const& group = Service::get().group();
        if (!group) return "The room has closed. Leave the map to go back.";
        auto const& battle = group->battle;
        if (!battle || battle->id != battleID) return "The battle has ended. Play freely - nothing here counts.";
        if (battle->finishedAt > 0) {
            int const winner = battle->seatOf(battle->winnerUid);
            if (battle->draw || winner < 0) return "The battle has ended in a draw. Play freely - nothing here counts.";
            return fmt::format("The battle has ended - {} wins. Play freely - nothing here counts.",
                               battle->seats[winner].profile.name.empty() ? "Player"
                                                                          : battle->seats[winner].profile.name);
        }
        return "Spectating the battle's map. Play freely - practice mode works too, nothing here counts.";
    }
    // 안내 문구를 갱신해 HUD에 반영한다.
    void showNote() {
        auto root = hud.lock();
        if (!root) return;
        shownNote = note();
        battle::showLiveState(root->getChildByID("spectator-mode"_spr), play.lock().data(), shownNote);
    }
    // PlayLayer에 진입하면 안내 프레임, 선두 카드, 관전 버튼을 붙인다.
    void entered(PlayLayer* layer) {
        versus::percent::enforce(layer);
        if (hud.lock()) return;
        auto* root = CCNode::create();
        root->setID("versus-group-spectator-play"_spr);
        layer->addChild(root, 100005);
        rebind(hud, root);
        shownNote = note();
        battle::addSpectatorFrame(root, 50, shownNote.c_str());
        buildLeader(root);
        buildButtons(root);
    }
    // 매 프레임: PlayLayer가 사라졌으면 세션을 정리하고, 아니면 방 밖 조회, 클리어 후 자동 재시작,
    // 관전 갱신을 하고 0.5초마다 문구와 선두 카드를 갱신한다.
    // 멤버인데 새 배틀이 시작되면 맵을 나가 다음 배틀로 보낸다.
    void update(float dt) override {
        auto owner = play.lock();
        if (!owner) {
            if (!constructing && !battleID.empty()) {
                if (!outside) Service::get().setGroupSpectating(false);
                reset();
            }
            return;
        }
        pollOutside(dt);
        if (restart > 0.f && !owner->m_isPaused && !replay::liveWatching(owner.data())) {
            restart -= dt;
            if (restart <= 0.f) {
                restart = 0.f;
                owner->resetLevelFromStart();
            }
        }
        battle::markSpectatorPause(owner.data());
        updateWatch(owner.data(), dt);
        updateButtons(owner.data());
        watch -= dt;
        if (watch > 0.f) return;
        watch = kWatchSeconds;
        showNote();
        showLeader();
        auto const& group = Service::get().group();
        if (!outside && !leaving && group && group->battle && group->battle->id != battleID &&
            group->battle->finishedAt == 0 && group->battle->seatOf(Service::get().profile().uid) >= 0) {
            leaving = true;
            Notification::create("Your next battle is starting!", NotificationIcon::Info, 2.5f)->show();
            owner->onQuit();
        }
    }
};
}

// 방 밖 LIVE 보기를 시작한다. 배틀용 레벨 복사본으로 PlayLayer를 만들고 통계를 저장한 뒤 장면을 전환한다.
// 이미 다른 레벨이 열려 있으면 거부한다.
namespace {
bool startLive(GJGameLevel* source, Controller::Outside view, std::string watchUid, std::string watchName) {
    auto& controller = Controller::get();
    if (!source || controller.constructing || controller.play.lock() || PlayLayer::get()) return false;
    std::uint64_t hash = 0;
    auto* copy = createBattleLevel(source, source->m_lowDetailModeToggled, hash);
    if (!copy) return false;
    controller.reset();
    controller.battleID = view.battle.id;
    if (!watchUid.empty() && watchUid != Service::get().profile().uid) {
        controller.watchUid = std::move(watchUid);
        controller.watchName = watchName.empty() ? std::string("Player") : std::move(watchName);
    }
    view.busy = false;
    view.wait = view.group ? kOutsideGroupSeconds : kOutsideListSeconds;
    std::string const room = view.roomId;
    controller.outside = std::move(view);
    snapshotBattleLevelStats(source->m_levelID.value());
    pauseBattleMods();
    controller.constructing = true;
    auto* scene = PlayLayer::scene(copy, false, false);
    controller.constructing = false;
    if (!scene || !controller.play.lock()) {
        restoreBattleLevelStats();
        controller.reset();
        return false;
    }
    log::info("Versus: LIVE view of map {} (room {}, battle {})", source->m_levelID.value(), room, controller.battleID);
    CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.5f, scene));
    return true;
}

// LIVE 보기를 열 수 없는 상태인 이유(방/큐/다른 레벨 열림)를 돌려준다. 가능하면 빈 문자열.
std::string liveRefusal() {
    auto& service = Service::get();
    auto const* ranked = service.rankedState();
    if (service.room() || service.group() || (ranked && ranked->queuedAt > 0))
        return "Leave your room or the Ranked queue before spectating from Live.";
    auto* controller = Controller::peek();
    if (isMatchLaunchActive() || replay::viewing() || replay::preparing() ||
        (controller && (controller->constructing || controller->play.lock())))
        return "Another level is open - leave it first.";
    return {};
}

// LIVE 보기를 여는 과정(맵 다운로드 -> 장면 정리 대기 -> startLive)을 진행하는 싱글턴 노드.
class Opener final : public CCNode {
public:
    // Idle 대기 없음 / Map 맵 다운로드 중 / Waiting 다른 장면 전환이 끝나기를 기다림.
    enum class Phase { Idle, Map, Waiting };
    Phase phase = Phase::Idle;
    std::optional<Controller::Outside> view;
    std::string watchUid, watchName;
    int64_t level = 0;
    std::uint64_t generation = 0;
    Ref<Notification> notice;
    float waited = 0.f, tick = 0.f;
    int shownPercent = -1;

    inline static Opener* instance = nullptr;
    static Opener* peek() { return instance; }
    static Opener& get() {
        static auto* opener = [] {
            auto* node = new Opener;
            node->init();
            instance = node;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 4, false);
            node->release();
            return node;
        }();
        return *opener;
    }
    // 진행 중인지.
    bool busy() const { return phase != Phase::Idle; }
    // 상단 알림을 만들거나 문구를 갱신한다.
    void say(std::string const& text) {
        if (notice) {
            notice->setString(text);
            return;
        }
        notice = Notification::create(text, NotificationIcon::Loading, 0.f);
        notice->show();
    }
    // 작업을 끝내고 알림을 닫는다.
    void end() {
        ++generation;
        phase = Phase::Idle;
        view.reset();
        watchUid.clear();
        watchName.clear();
        if (notice) {
            notice->hide();
            notice = nullptr;
        }
    }
    // 실패 알림을 띄우고 끝낸다.
    void fail(std::string const& text) {
        end();
        Notification::create(text, NotificationIcon::Error, 3.f)->show();
    }
    // free play/lounge가 아닌 다른 레벨이 열려 있는지.
    static bool otherLevel() {
        auto* layer = PlayLayer::get();
        return layer && !lounge::owns(layer);
    }
    // 시작: 맵을 내려받는다. 방이 Ranked 방이면 Ranked용 오디오 규칙을 적용한다.
    void begin(Controller::Outside value, std::string uid, std::string name) {
        view = std::move(value);
        watchUid = std::move(uid);
        watchName = std::move(name);
        level = view->battle.level.id;
        phase = Phase::Map;
        ++generation;
        waited = 0.f;
        tick = 0.f;
        shownPercent = -1;
        say("Opening the live view - downloading the map...");
        auto const epoch = generation;
        bool const ranked = view->roomId.starts_with("ranked-");
        ensureMapCached(
            level,
            [epoch](bool ok, std::string detail) {
                auto* self = Opener::peek();
                if (!self || epoch != self->generation || self->phase != Phase::Map) return;
                if (!ok) {
                    self->fail(detail.empty() ? "The map could not be downloaded." : detail);
                    return;
                }
                self->phase = Phase::Waiting;
                self->waited = 0.f;
            },
            ranked ? AudioNeed::Ranked : AudioNeed::All);
    }
    // 맵을 받는 동안 0.25초마다 진행률을 알리고, 조건이 어긋나면 취소한다.
    // 다운로드가 끝나면 다른 장면이 모두 정리될 때까지(최대 8초) 기다렸다가 startLive를 호출한다.
    void update(float dt) override {
        if (phase == Phase::Map) {
            tick -= dt;
            if (tick > 0.f) return;
            tick = .25f;
            if (auto why = liveRefusal(); !why.empty() || otherLevel()) {
                fail(why.empty() ? std::string("The live view was canceled.") : why);
                return;
            }
            int const percent =
                std::clamp(mapDownloadProgress(level, view && view->roomId.starts_with("ranked-")).percent, 0, 100);
            if (percent != shownPercent) {
                shownPercent = percent;
                say(fmt::format("Opening the live view - downloading the map... {}%", percent));
            }
            return;
        }
        if (phase != Phase::Waiting) return;
        waited += dt;
        if (auto why = liveRefusal(); !why.empty() || otherLevel()) {
            fail(why.empty() ? std::string("The live view was canceled.") : why);
            return;
        }
        auto* director = CCDirector::sharedDirector();
        bool const clear = !PlayLayer::get() && !GJBaseGameLayer::get() && !director->getNextScene() &&
                           !typeinfo_cast<CCTransitionScene*>(director->getRunningScene());
        if (!clear) {
            if (waited > 8.f) fail("The map could not be opened right now - try again.");
            return;
        }
        Ref<GJGameLevel> source(cachedLevel(level));
        auto value = std::move(*view);
        auto uid = std::move(watchUid), name = std::move(watchName);
        end();
        if (!source) {
            Notification::create("The map could not be loaded.", NotificationIcon::Error, 3.f)->show();
            return;
        }
        if (!startLive(source.data(), std::move(value), std::move(uid), std::move(name)))
            Notification::create("The map could not be opened.", NotificationIcon::Error, 3.f)->show();
    }
};

// 방 밖에서 배틀을 열기 전 검사(이미 열리는 중, 배틀 ID, 맵 ID 범위, Platformer)를 하고 Opener를 시작한다.
bool openOutside(Controller::Outside view, std::string const& watchUid, std::string const& watchName,
                 std::string& why) {
    if (auto* opener = Opener::peek(); opener && opener->busy()) {
        why = "The live view is opening...";
        return false;
    }
    why = liveRefusal();
    if (!why.empty()) return false;
    auto const& battle = view.battle;
    if (battle.id.empty()) {
        why = "This battle cannot be watched (update the server).";
        return false;
    }
    if (battle.level.id <= 0 || battle.level.id > std::numeric_limits<int>::max()) {
        why = "This map cannot be opened.";
        return false;
    }
    if (battle.level.platformer) {
        why = kPlatformerMapMessage;
        return false;
    }
    Opener::get().begin(std::move(view), watchUid, watchName);
    return true;
}
}

// LIVE 매치를 GroupBattle 형태로 변환한다.
GroupBattle battleOf(LiveMatch const& match) {
    GroupBattle battle;
    battle.id = match.battleId;
    battle.releasedAt = match.releasedAt;
    battle.level = match.level;
    battle.rules = match.rules;
    for (auto const& player : match.players) {
        GroupSeat seat;
        seat.profile = player.profile;
        seat.state = player.state;
        seat.loaded = true;
        battle.seats.push_back(std::move(seat));
    }
    return battle;
}
// 관전할 만한 플레이어를 고른다. 아직 로딩 중이거나 연습 규칙이면 불가, 진행 중인 사람이 없으면 불가.
std::string firstToWatch(GroupBattle const& battle, std::string& name, std::string& why) {
    if (battle.releasedAt == 0) {
        why = "This battle is still loading - try again in a moment.";
        return {};
    }
    if (!battle::watchableRules(battle.rules)) {
        why = "Practice battles cannot be watched live (their runs restart from checkpoints).";
        return {};
    }
    int best = -1;
    for (int i = 0; i < static_cast<int>(battle.seats.size()); ++i) {
        auto const& seat = battle.seats[i];
        if (seat.out || seat.disqualified || battle::resultFinal(seat.state, battle.rules) ||
            seat.profile.uid.empty() || seat.profile.uid == Service::get().profile().uid)
            continue;
        auto const reach = [](GroupSeat const& x) { return std::max(x.state.bestPercent, x.state.currentPercent); };
        if (best < 0 || reach(seat) > reach(battle.seats[best])) best = i;
    }
    if (best < 0) {
        why = "Nobody in this battle is still playing.";
        return {};
    }
    name = battle.seats[best].profile.name.empty() ? std::string("Player") : battle.seats[best].profile.name;
    return battle.seats[best].profile.uid;
}
// uid 플레이어를 지금 관전할 수 있는지 검사하고 불가능하면 이유를 why에 채운다.
bool canWatch(GroupBattle const& battle, std::string const& uid, std::string& why) {
    if (battle.releasedAt == 0) {
        why = "This battle is still loading - try again in a moment.";
        return false;
    }
    if (!battle::watchableRules(battle.rules)) {
        why = "Practice battles cannot be watched live (their runs restart from checkpoints).";
        return false;
    }
    if (uid.empty() || uid == Service::get().profile().uid) {
        why = "You cannot watch yourself.";
        return false;
    }
    for (auto const& seat : battle.seats) {
        if (seat.profile.uid != uid) continue;
        if (seat.out || seat.disqualified || battle::resultFinal(seat.state, battle.rules)) {
            why = "This player has finished - nothing left to watch live.";
            return false;
        }
        return true;
    }
    why = "This player is no longer in the battle.";
    return false;
}
// 1v1 LIVE 매치를 방 밖에서 연다.
bool openLive(LiveMatch const& match, std::string const& watchUid, std::string const& watchName, std::string& why) {
    Controller::Outside view;
    view.roomId = match.roomId;
    view.roomName = match.name;
    view.battle = battleOf(match);
    return openOutside(std::move(view), watchUid, watchName, why);
}
// 그룹 배틀을 방 밖에서 연다.
bool openLive(GroupBattle const& battle, std::string const& roomId, std::string const& roomName,
              std::string const& watchUid, std::string const& watchName, std::string& why) {
    Controller::Outside view;
    view.roomId = roomId;
    view.roomName = roomName;
    view.group = true;
    view.battle = battle;
    return openOutside(std::move(view), watchUid, watchName, why);
}
// LIVE로 보고 있는 그룹 방 ID.
std::string liveRoom() {
    auto* controller = Controller::peek();
    return controller && controller->play.lock() && controller->outside && controller->outside->group
               ? controller->outside->roomId
               : std::string();
}
// LIVE로 보고 있는 방 이름.
std::string liveRoomName() {
    auto* controller = Controller::peek();
    return controller && controller->play.lock() && controller->outside ? controller->outside->roomName : std::string();
}

// 그룹 멤버용 free play 시작. 방 밖 LIVE와 달리 서비스에 그룹 관전 중임을 알린다.
bool start(GJGameLevel* source, std::string battleID, std::string watchUid, std::string watchName) {
    auto& controller = Controller::get();
    if (!source || controller.constructing || controller.play.lock() || PlayLayer::get()) return false;
    std::uint64_t hash = 0;
    auto* copy = createBattleLevel(source, source->m_lowDetailModeToggled, hash);
    if (!copy) return false;
    controller.reset();
    controller.battleID = std::move(battleID);
    if (watchUid != Service::get().profile().uid) {
        controller.watchUid = std::move(watchUid);
        controller.watchName = watchName.empty() ? std::string("Player") : std::move(watchName);
    }
    snapshotBattleLevelStats(source->m_levelID.value());
    pauseBattleMods();
    controller.constructing = true;
    auto* scene = PlayLayer::scene(copy, false, false);
    controller.constructing = false;
    if (!scene || !controller.play.lock()) {
        restoreBattleLevelStats();
        controller.reset();
        return false;
    }
    log::info("Versus: Group spectator play of map {} (battle {})", source->m_levelID.value(), controller.battleID);
    Service::get().setGroupSpectating(true);
    CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.5f, scene));
    return true;
}
// free play가 열려 있거나 열리는 중인지.
bool active() {
    auto* controller = Controller::peek();
    return controller && (controller->constructing || controller->play.lock());
}
// LIVE 보기를 여는 중인지.
bool opening() {
    auto* opener = Opener::peek();
    return opener && opener->busy();
}
// 이 레이어가 free play의 PlayLayer인지.
bool owns(GJBaseGameLayer* layer) {
    auto* controller = Controller::peek();
    return controller && controller->owns(layer);
}
// 관전 대상을 지정한다(본인 제외).
bool spectate(std::string const& uid, std::string const& name) {
    auto* controller = Controller::peek();
    if (!controller || !controller->play.lock() || controller->leaving || uid.empty() ||
        uid == Service::get().profile().uid)
        return false;
    controller->watchUid = uid;
    controller->watchName = name.empty() ? std::string("Player") : name;
    controller->watchOver = 0.f;
    return true;
}
// 현재 관전 대상의 uid.
std::string spectatedUid() {
    auto* controller = Controller::peek();
    return controller && controller->play.lock() ? controller->watchUid : std::string();
}
// 관전을 지정할 수 있는 상태인지.
bool canSpectate() {
    auto* controller = Controller::peek();
    return controller && controller->play.lock() && !controller->leaving;
}
// free play를 끝내고 이전 화면으로 돌아간다. 저장해 둔 레벨 통계를 복원한다.
bool leave() {
    auto* controller = Controller::peek();
    if (!controller || !controller->play.lock()) return false;
    bool const member = !controller->outside;
    controller->reset();
    if (member) Service::get().setGroupSpectating(false);
    restoreBattleLevelStats();
    CCDirector::sharedDirector()->popSceneWithTransition(.5f, kPopTransitionFade);
    GameManager::sharedState()->fadeInMenuMusic();
    return true;
}
}

using versus::freeplay::Controller;

// free play PlayLayer 후킹. 생성 중이면 컨트롤러에 연결하고, 클리어하면 테스트 모드로 처리해
// 기록을 남기지 않으며 일정 시간 뒤 자동 재시작한다. 종료 화면은 띄우지 않는다.
class $modify(VersusGroupFreePlayLayer, PlayLayer){
    bool init(GJGameLevel * level, bool useReplay, bool dontCreateObjects){auto* controller = Controller::peek();
if (controller && controller->constructing) {
    versus::rebind(controller->play, static_cast<PlayLayer*>(this));
    versus::attachPausedMods(this);
}
return PlayLayer::init(level, useReplay, dontCreateObjects);
}
void onEnterTransitionDidFinish() {
    PlayLayer::onEnterTransitionDidFinish();
    if (auto* controller = Controller::peek(); controller && controller->owns(this)) controller->entered(this);
}
// 클리어 시 테스트 모드를 잠시 켜 GD 기록/보상 반영을 막고, 레벨 통계를 복원한 뒤 재시작 타이머를 건다.
void levelComplete() {
    auto* controller = Controller::peek();
    if (!controller || !controller->owns(this)) {
        PlayLayer::levelComplete();
        return;
    }
    bool const previous = m_isTestMode;
    m_isTestMode = true;
    PlayLayer::levelComplete();
    m_isTestMode = previous;
    versus::restoreBattleLevelStats();
    controller->restart = versus::freeplay::kClearRestartSeconds;
}
// 종료 화면은 free play에서는 보여 주지 않는다.
void showEndLayer() {
    if (auto* controller = Controller::peek(); controller && controller->owns(this)) return;
    PlayLayer::showEndLayer();
}
void resetLevel() {
    PlayLayer::resetLevel();
    if (auto* controller = Controller::peek(); controller && controller->owns(this)) controller->restart = 0.f;
}
}
;
