// 배틀 세션 구현. Session 싱글턴이 한 판의 1v1/그룹 배틀 동안 PlayLayer를 감시하며
// 내 진행(시도, 퍼센트, 일시 정지)을 서버에 보고하고 상대/다른 플레이어 상태를 받아 HUD에 반영한다.
// 결과가 정해지면 결과 연출을 보여 주고, 끝난 뒤에는 free play와 관전을 제공한다.
// 또한 일시 정지 예산, 안티치트 실격, 워치독(멈춤 복구), 리롤 투표, 배속 처리를 담당한다.
#include "ExecutionEffects.hpp"
#include "BattleSession.hpp"
#include "Particles.hpp"
#include "BattleProgress.hpp"
#include "IntegrityMonitor.hpp"
#include "IntegrityCodes.hpp"
#include "IntegrityFeatures.hpp"
#include "BattleHUD.hpp"
#include "GroupResult.hpp"
#include "Edition.hpp"
#include "MenuCursor.hpp"
#include "MenuCursorScope.hpp"
#include "MapCache.hpp"
#include "MapRecord.hpp"
#include "MatchLaunch.hpp"
#include "SpriteFrames.hpp"
#include "VersusAudio.hpp"
#include "VersusService.hpp"
#include "Replay.hpp"
#include "PercentGuard.hpp"
#include "VersusUI.hpp"
#include "WeakRefs.hpp"
#include "DebugMode.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <cmath>
#include <map>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJRobotSprite.hpp>
#include <Geode/binding/GJSpiderSprite.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/binding/CCCircleWave.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

using namespace geode::prelude;
namespace versus::battle {
namespace {
using Clock = std::chrono::steady_clock;
// 결과 화면 단계별 시간(초): 진행도 표시 -> 승패 발표 -> 처형 연출 -> 확인 팝업(자동 복귀까지 20초).
constexpr float RESULT_REVEAL_TIME_SCALE = 1.3f;
constexpr float RESULT_PROGRESS_TIME = 1.4f * RESULT_REVEAL_TIME_SCALE;
constexpr float RESULT_DECISION_TIME = .875f * RESULT_REVEAL_TIME_SCALE;
constexpr float RESULT_SUMMARY_TIME = RESULT_PROGRESS_TIME + RESULT_DECISION_TIME;
constexpr float RESULT_EXECUTION_TIME = 4.2f;
constexpr float RESULT_CONFIRM_TIME = 20.f;
constexpr float RESULT_CONFIRM_START = RESULT_SUMMARY_TIME + RESULT_EXECUTION_TIME;
// 일시 정지를 얻기 위해 일시 정지 버튼/Esc를 눌러야 하는 시간(초).
constexpr float kPauseHoldSeconds = 5.f;
// Esc 키 반복 입력이 일시 정지 직후 다시 처리되지 않도록 막는 플래그.
bool escapeRepeatLatch = false;
// 현재 적용된 오디오 속도. 규칙의 배속과 맞추려고 FMOD 마스터 채널의 피치를 바꾼다.
float s_audioSpeed = 1.f;
// 오디오 피치를 바꾼다(차이가 작으면 무시).
void setAudioSpeed(float speed) {
    if (std::abs(speed - s_audioSpeed) < .001f) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    FMOD::ChannelGroup* master = nullptr;
    if (!engine || !engine->m_system || engine->m_system->getMasterChannelGroup(&master) != FMOD_OK || !master) return;
    master->setPitch(speed);
    s_audioSpeed = speed;
}
// 프로필 아이콘(색, 오라 포함)을 만든다.
ccColor3B color(int rgb) {
    return ccc3((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}
SimplePlayer* icon(PlayerProfile const& profile, float scale = .85f) {
    auto* manager = GameManager::sharedState();
    int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
    auto* result = SimplePlayer::create(std::clamp(profile.icon, 1, count));
    result->setColors(color(profile.color1), color(profile.color2));
    result->setGlowOutline(ccWHITE);
    addMembershipAura(result, profile.membership, profile.cosmetics);
    result->setScale(scale);
    return result;
}

// 파티클 프리셋으로 폭발 입자를 만든다.
CCParticleSystemQuad* nativeBurst(CCNode* parent, CCPoint position, char const* preset, ccColor3B tint, unsigned count,
                                  float duration, float life, float speed, float size) {
    auto* particles = versus::particleSystem(preset);
    if (!particles) return nullptr;
    particles->setTotalParticles(count);
    particles->setEmitterMode(kCCParticleModeGravity);
    particles->setPositionType(kCCPositionTypeGrouped);
    particles->setSourcePosition({0.f, 0.f});
    particles->setPosition(position);
    particles->setPosVar({4.f, 4.f});
    particles->setDuration(duration);
    particles->setLife(life);
    particles->setLifeVar(life * .25f);
    particles->setEmissionRate(static_cast<float>(count) / duration);
    particles->setAngle(90.f);
    particles->setAngleVar(180.f);
    particles->setSpeed(speed);
    particles->setSpeedVar(speed * .3f);
    particles->setGravity({0.f, -35.f});
    particles->setStartSize(size);
    particles->setStartSizeVar(size * .3f);
    particles->setEndSize(0.f);
    particles->setEndSizeVar(0.f);
    particles->setStartColor({tint.r / 255.f, tint.g / 255.f, tint.b / 255.f, 1.f});
    particles->setStartColorVar({.08f, .08f, .08f, 0.f});
    particles->setEndColor({tint.r / 255.f, tint.g / 255.f, tint.b / 255.f, 0.f});
    particles->setEndColorVar({0.f, 0.f, 0.f, 0.f});
    particles->setBlendAdditive(true);
    particles->setAutoRemoveOnFinish(true);
    parent->addChild(particles, 16);
    particles->resetSystem();
    return particles;
}

// 원형 파동 연출.
void nativeWave(CCNode* parent, CCPoint position, ccColor3B tint, float radius) {
    if (auto* wave = CCCircleWave::create(8.f, radius, .4f, false, true)) {
        wave->m_color = tint;
        wave->setPosition(position);
        parent->addChild(wave, 15);
    }
}

// 승자 연출: 파동 + 폭발 + 반짝임.
void winnerEffect(CCNode* parent, CCPoint position, PlayerProfile const& player) {
    auto tint = color(player.color1);
    tint = ccc3((tint.r + 255) / 2, (tint.g + 255) / 2, (tint.b + 255) / 2);
    nativeWave(parent, position, tint, 66.f);
    nativeBurst(parent, position, "explodeEffect.plist", tint, 52, .06f, .5f, 105.f, 5.f);
    nativeBurst(parent, position, "glitterEffectIcon.plist", ccc3(255, 228, 135), 20, .16f, .65f, 45.f, 10.f);
}

// 처형 연출 등에 쓰는 충돌 효과.
void impactEffect(CCNode* parent, CCPoint position, ccColor3B tint) {
    nativeWave(parent, position, ccWHITE, 80.f);
    nativeBurst(parent, position, "explodeEffect.plist", tint, 64, .06f, .48f, 185.f, 7.f);
    nativeBurst(parent, position, "glitterEffectIcon.plist", ccWHITE, 20, .08f, .4f, 110.f, 9.f);
    parent->runAction(CCSequence::create(
        CCDelayTime::create(.09f),
        CallFuncExt::create([parent, position, tint] { nativeWave(parent, position, tint, 58.f); }), nullptr));
}

// 발사체에 불꽃 꼬리 입자를 붙인다.
void projectileTrail(CCNode* shot, float direction, ccColor3B tint) {
    if (auto* particles = nativeBurst(shot, {0.f, 0.f}, "fireballEffect.plist", tint, 32, .44f, .2f, 35.f, 5.f)) {
        particles->setPositionType(kCCPositionTypeFree);
        particles->setAngle(direction > 0.f ? 180.f : 0.f);
        particles->setAngleVar(25.f);
        particles->setGravity({0.f, 0.f});
        particles->setZOrder(-1);
    }
}

// 발사체 스프라이트(프레임이 없으면 사각형).
CCSprite* projectileSprite(char const* frame, ccColor3B tint) {
    auto* sprite = loadedFrameSprite(frame);
    if (!sprite) sprite = CCSprite::create("square.png");
    sprite->setColor(tint);
    auto const size = sprite->getContentSize();
    sprite->setScale(26.f / std::max(1.f, std::max(size.width, size.height)));
    return sprite;
}
}

namespace {
// 관전(보라)/free play(회색) 프레임 색.
constexpr ccColor3B kWatchFrame = {176, 120, 255}, kWatchTitle = {214, 186, 255};
constexpr ccColor3B kFreeFrame = {150, 156, 168}, kFreeTitle = {205, 210, 222};
}
// 프레임을 관전 중이면 보라색, 아니면 회색으로 바꾼다.
void tintSpectatorFrame(CCNode* frame, bool watching) {
    if (!frame) return;
    int const state = watching ? 2 : 1;
    if (frame->getTag() == state) return;
    frame->setTag(state);
    ccColor3B const tint = watching ? kWatchFrame : kFreeFrame;
    for (auto* child : CCArrayExt<CCNode*>(frame->getChildren())) {
        auto* layer = typeinfo_cast<CCLayerColor*>(child);
        if (!layer) continue;
        if (auto* fade = typeinfo_cast<CCLayerGradient*>(child)) {
            fade->setStartColor(tint);
            fade->setEndColor(tint);
        } else
            layer->setColor(tint);
        layer->setOpacity(layer->getOpacity());
    }
    if (auto* title = typeinfo_cast<CCLabelBMFont*>(frame->getChildByID("spectator-mode-title"_spr)))
        title->setColor(watching ? kWatchTitle : kFreeTitle);
}
// 관전/free play 화면의 테두리 프레임(가장자리 글로우, 실선, SPECTATOR MODE 제목, 안내 문구)을 만든다.
CCNodeRGBA* addSpectatorFrame(CCNode* parent, int z, char const* caption) {
    auto const window = CCDirector::sharedDirector()->getWinSize();
    ccColor3B const tint = kFreeFrame;
    auto* frame = CCNodeRGBA::create();
    frame->setTag(1);
    frame->setID("spectator-mode"_spr);
    frame->setCascadeOpacityEnabled(true);
    float const glow = 22.f, line = 3.f;
    auto edge = [&](CCPoint at, CCSize size, CCPoint toward) {
        auto* fade =
            CCLayerGradient::create(ccc4(tint.r, tint.g, tint.b, 120), ccc4(tint.r, tint.g, tint.b, 0), toward);
        fade->setContentSize(size);
        fade->setPosition(at);
        frame->addChild(fade);
    };
    edge({0.f, 0.f}, {glow, window.height}, {1.f, 0.f});
    edge({window.width - glow, 0.f}, {glow, window.height}, {-1.f, 0.f});
    edge({0.f, 0.f}, {window.width, glow}, {0.f, 1.f});
    edge({0.f, window.height - glow}, {window.width, glow}, {0.f, -1.f});
    auto solid = [&](CCPoint at, CCSize size) {
        auto* bar = CCLayerColor::create(ccc4(tint.r, tint.g, tint.b, 230), size.width, size.height);
        bar->setPosition(at);
        frame->addChild(bar);
    };
    solid({0.f, 0.f}, {line, window.height});
    solid({window.width - line, 0.f}, {line, window.height});
    solid({0.f, 0.f}, {window.width, line});
    solid({0.f, window.height - line}, {window.width, line});
    auto* title = createCosmeticLabel("SPECTATOR MODE", "bigFont.fnt");
    title->setID("spectator-mode-title"_spr);
    title->setScale(.5f);
    title->setColor(kFreeTitle);
    title->setPosition({window.width / 2.f, 30.f});
    frame->addChild(title);
    auto* note = createCosmeticLabel(caption, "chatFont.fnt");
    note->setID("spectator-mode-note"_spr);
    note->limitLabelWidth(window.width - 60.f, .55f, .3f);
    note->setPosition({window.width / 2.f, 14.f});
    frame->addChild(note);
    parent->addChild(frame, z);
    frame->setOpacity(0);
    frame->runAction(CCSequence::create(CCFadeIn::create(.35f), CallFuncExt::create([frame] {
                                            frame->runAction(CCRepeatForever::create(CCSequence::create(
                                                CCEaseSineInOut::create(CCFadeTo::create(1.3f, 150)),
                                                CCEaseSineInOut::create(CCFadeTo::create(1.3f, 255)), nullptr)));
                                        }),
                                        nullptr));
    return frame;
}
// 라이브 관전 중이면 관전용 문구와 경고를 보여 주고, 아니면 평소 문구로 되돌린다.
void showLiveState(CCNode* frame, GJBaseGameLayer* layer, std::string const& normal) {
    if (!frame) return;
    bool const live = replay::liveWatching(layer);
    tintSpectatorFrame(frame, live);
    auto const window = CCDirector::sharedDirector()->getWinSize();
    if (auto* note = typeinfo_cast<CCLabelBMFont*>(frame->getChildByID("spectator-mode-note"_spr))) {
        auto const caption = live ? replay::liveCaption() : normal;
        float const room = std::max(80.f, std::min(window.width - 60.f, window.width - 2.f * kSpectateButtonsReserve));
        if (!caption.empty() && caption != note->getString()) {
            note->setString(caption.c_str());
            note->limitLabelWidth(room, .55f, .3f);
        } else if (note->getScaledContentSize().width > room + .5f)
            note->limitLabelWidth(room, .55f, .3f);
    }
    auto const warning = live ? replay::liveWarning() : std::string();
    auto* line = typeinfo_cast<CCLabelBMFont*>(frame->getChildByIDRecursive("spectator-live-warning"_spr));
    if (!line) {
        if (warning.empty()) return;
        auto* holder = CCNode::create();
        holder->setID("spectator-live-warning-holder"_spr);
        frame->addChild(holder, 2);
        line = createCosmeticLabel("", "chatFont.fnt");
        line->setID("spectator-live-warning"_spr);
        line->setColor(ccc3(255, 190, 90));
        auto const place = battleTextPlace(window);
        float const step = 19.f * place.scale / .65f;
        line->setAnchorPoint(place.anchor);
        line->setPosition({place.at.x, place.at.y < 40.f ? place.at.y + step : place.at.y - step});
        holder->addChild(line);
    }
    line->setVisible(!warning.empty());
    if (!warning.empty() && warning != line->getString()) {
        line->setString(warning.c_str());
        line->limitLabelWidth(window.width * (line->getAnchorPoint().x == .5f ? 1.f : .5f) - 80.f, .55f, .3f);
    }
}
// 일시 정지 메뉴에 "SPECTATOR MODE - NO PAUSE LIMIT" 문구를 붙인다.
void markSpectatorPause(PlayLayer* owner) {
    if (!owner || !owner->getParent()) return;
    for (auto* child : CCArrayExt<CCNode*>(owner->getParent()->getChildren())) {
        auto* menu = typeinfo_cast<PauseLayer*>(child);
        if (!menu || menu->getChildByID("versus-spectator-pause"_spr)) continue;
        auto size = CCDirector::sharedDirector()->getWinSize();
        auto* text = createCosmeticLabel("SPECTATOR MODE - NO PAUSE LIMIT", "goldFont.fnt");
        text->setID("versus-spectator-pause"_spr);
        text->limitLabelWidth(220.f, .5f, .25f);
        text->setPosition({size.width - 120.f, size.height - 23.f});
        menu->addChild(text, 100);
    }
}

namespace {
// 배틀 세션 싱글턴. 필드는 대략 방/전투 식별자, 규칙, 내/상대 상태, 보고 상태,
// free play와 관전 상태, 결과 연출 노드, 일시 정지 길게 누르기 상태로 나뉜다.
class Session final : public CCNode {
public:
    Weak<PlayLayer> play;
    std::string roomID, battleID, uid;
    // Ranked Season 2 관전 테스트 기능을 서버가 지원하는지.
    bool spectateServer = false;
    PlayerProfile hostProfile, guestProfile;
    GameRules rules;
    BattlePlayerState local;
    BattlePlayerState other;
    BattleInfo finalResult;
    bool host = false, active = false, releasing = false;
    bool group = false;
    Weak<GroupHUD> groupHUD;
    bool reporting = false, dirty = false, forcingQuit = false;
    bool quitDialog = false, leaving = false, timedOut = false;
    bool finished = false, spectating = false, returning = false, resultVisible = false;
    bool freePlay = false;
    float freePlayRestart = 0.f;
    Clock::time_point lockedAt{};
    bool lateCheatLogged = false;
    Weak<CCNode> freePlayFrame;
    Weak<CCNode> spectateHUD;
    bool spectateShown = false;
    bool cursorShown = false, cursorUnlocked = false;
    float cursorTimer = 0.f;
    std::string watchUid, watchName, freePlayCaption;
    float watchOver = 0.f;
    bool watchAuto = false;
    bool winnerRevealed = false, executionRevealed = false, confirmationVisible = false;
    int runBest = 0, lastRecordedRun = 0;
    int levelUserCoins = 0;
    int reportFailures = 0;
    std::string reportError;
    Clock::time_point lastSync{};
    int ackFailures = 0;
    // 서버에 닿지 못했다고 보는 시간(초)과 결과 확인(ack)에 연속 실패했을 때 그냥 나가는 횟수.
    static constexpr int kUnreachableSeconds = 15, kAckFailuresToLeave = 8;
    // 서버는 한 자리에서 진행 보고도 heartbeat도 30초(BATTLE_STALE_MS) 동안 없으면 연결 끊김 패배로 끝낸다.
    // 15초가 넘으면 화면 가운데에 크게 경고한다.
    static constexpr float kSeatStaleSeconds = 30.f, kLinkWarnSeconds = 15.f;
    // 서버가 확인하기 전까지 다시 보낼 사망 상태와 재전송 횟수.
    std::optional<BattlePlayerState> pendingDeath;
    int pendingDeathSends = 0;
    Clock::time_point runStarted;
    float runSimulated = 0.f;
    std::string integrityReason;
    std::string shownIntegrityCode, shownIntegrityFeatures;
    float returnRetry = 0.f;
    uint64_t generation = 0;
    float reportTimer = 0.f, uiTimer = 0.f;
    float keepAlive = 0.f;
    Clock::time_point resultBegan;
    Weak<CCNode> hud;
    Weak<BattleHUD> playerHUD;
    Weak<CCLabelBMFont> status, returnStatus, antiCheatAlert;
    Weak<CCNode> linkWarning;
    Weak<CCLabelBMFont> linkWarningText;
    float statusScale = .65f;
    Weak<CCLabelBMFont> hostPercent, guestPercent;
    Weak<CCLayerColor> hostGauge, guestGauge;
    Weak<CCLayerColor> transitionShade, resultTimerFill;
    Weak<CCNodeRGBA> progressSection, decisionBanner, actorSection, resultPopup;
    Weak<CCNode> resultRoot;
    float resultTimerWidth = 0.f;
    float displayedHostPercent = 0.f;
    float displayedGuestPercent = 0.f;
    float deadClock = 0.f, haltClock = 0.f, resultWaitClock = 0.f, animationClock = 0.f;
    bool animationRevivedLogged = false;
    int64_t shownHostReroll = 0, shownGuestReroll = 0;
    bool rerollSending = false, rerolledExit = false;
    int hostBefore = -2, guestBefore = -2;
    std::map<std::string, int> groupBefore;
    bool escapeHeld = false, holdGranted = false, holdConsumed = false;
    float holdTime = 0.f, holdShown = 0.f, holdHint = 0.f;
    float holdBaseScale = 0.f;
    ccColor3B holdBaseColor{255, 255, 255};
    Weak<CCNode> holdImage, holdNode;
    Weak<CCDrawNode> holdRing;
    Weak<CCLabelBMFont> holdLabel;
    double lastLevelTime = -1.0;
    int stillFrames = 0, movingFrames = 0;
    float resultHold = 0.f;

    inline static Session* instance = nullptr;
    static Session* peek() { return instance; }
    static Session& get() {
        static auto* session = [] {
            auto* node = new Session;
            node->init();
            instance = node;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 2, false);
            node->release();
            return node;
        }();
        return *session;
    }
    // 이 레이어가 세션의 PlayLayer인지.
    bool owns(GJBaseGameLayer* layer) const {
        if (!active || !layer) return false;
        auto owner = play.lock();
        return owner && static_cast<GJBaseGameLayer*>(owner.data()) == layer;
    }
    // 방의 배틀이 이 세션의 배틀과 같은지.
    bool sameBattle() const {
        auto const& room = Service::get().room();
        return room && room->id == roomID && room->battle && room->battle->id == battleID;
    }
    // 내가 더 이상 시도할 수 없는 상태인지.
    bool done() const { return terminal(local, localRules()); }
    // Season 2 Ranked 1v1에서 시도 소진자의 관전 기능이 켜지는 조건.
    bool spectateTest() const {
        return spectateServer && !group && roomID.starts_with("ranked-s2-") && rules.mode == 0 && !rules.practice &&
               !rules.allowHacks;
    }
    // 시도를 모두 쓴 좌석을 관전 좌석으로 정하고, 로컬/상대가 관전자인지 판단한다.
    SpectatorSeat spectatorNow() const {
        if (!spectateTest()) return SpectatorSeat::None;
        return spectatorSeat(host ? local : other, host ? other : local, rules);
    }
    bool localSpectator() const {
        auto const seat = spectatorNow();
        return seat != SpectatorSeat::None && (seat == SpectatorSeat::Host) == host;
    }
    bool otherSpectator() const {
        auto const seat = spectatorNow();
        return seat != SpectatorSeat::None && (seat == SpectatorSeat::Host) != host;
    }
    // 관전 좌석은 시도 횟수를 무제한으로 본 규칙을 쓴다.
    GameRules localRules() const { return seatRules(rules, host, spectatorNow()); }
    GameRules otherRules() const { return seatRules(rules, !host, spectatorNow()); }
    std::string otherName() const { return host ? guestProfile.name : hostProfile.name; }
    // 게임 진행과 입력을 막아야 하는 상태(종료, 퇴장, 관전, 실격).
    bool blocked() const { return finished || leaving || (spectating && !freePlay) || !integrityReason.empty(); }
    // 지금 일시 정지가 허용되는지. 사망한 시도 사이에만 허용된다(free play는 자유).
    bool canPause(PlayLayer* layer) const {
        if (freePlay) return !releasing && !finished && !leaving;
        bool const dead = layer->m_playerDied || (layer->m_player1 && layer->m_player1->m_isDead) ||
                          (layer->m_gameState.m_isDualMode && layer->m_player2 && layer->m_player2->m_isDead);
        return !releasing && !blocked() && !layer->m_hasCompletedLevel && canPauseBetweenAttempts(local, dead);
    }
    // 문구가 바뀐 경우에만 라벨을 갱신하고 폭에 맞춘다.
    void label(Weak<CCLabelBMFont> const& ref, std::string const& value) {
        if (auto target = ref.lock(); target && value != target->getString()) {
            target->setString(value.c_str());
            bool const placed = &ref == &status;
            target->limitLabelWidth(CCDirector::sharedDirector()->getWinSize().width *
                                            (!placed || target->getAnchorPoint().x == .5f ? 1.f : .5f) -
                                        32.f,
                                    placed ? statusScale : .65f, .2f);
            target->setOpacity(target->getOpacity());
        }
    }
    // 안티치트 감지 경고 문구를 화면 왼쪽 아래에 표시한다.
    void showIntegrityAlert(std::string const& code, std::string const& features, bool opponent) {
        if (!integrity::validFeatureDetails(code, features)) return;
        auto root = hud.lock();
        if (!play.lock() || !root) return;
        auto const text = integrity::detectionText(code, features, opponent);
        if (text.empty()) return;
        if (auto existing = antiCheatAlert.lock())
            existing->setString(text.c_str());
        else {
            auto* alert = createCosmeticLabel(text.c_str(), "chatFont.fnt");
            alert->setID("anti-cheat-detecting"_spr);
            alert->setAnchorPoint({0.f, 0.f});
            alert->setAlignment(kCCTextAlignmentLeft);
            alert->setColor(ccc3(255, 92, 74));
            alert->setScale(.48f);
            alert->setPosition({10.f, 10.f});
            alert->limitLabelWidth(CCDirector::sharedDirector()->getWinSize().width * .48f, .48f, .28f);
            root->addChild(alert, 1000);
            rebind(antiCheatAlert, alert);
        }
        shownIntegrityCode = code;
        shownIntegrityFeatures = features;
    }
    // HUD 구성: 일시 정지 버튼 위치 조정, 1v1/그룹 카드 HUD, 상태 문구 라벨.
    void hudSetup(PlayLayer* layer) {
        auto window = CCDirector::sharedDirector()->getWinSize();
        if (auto* ui = layer->m_uiLayer) {
            if (auto* pause = ui->m_pauseBtn) {
                if (auto* parent = pause->getParent())
#ifdef GEODE_IS_DESKTOP
                    pause->setPosition(parent->convertToNodeSpace({window.width / 2.f, window.height - 22.f}));
#else
                    pause->setPosition(parent->convertToNodeSpace({window.width - 22.f, window.height - 22.f}));
#endif
            }
        }
        auto* root = CCNode::create();
        root->setID("battle-hud"_spr);
        if (group) {
            if (auto* cards = GroupHUD::create(guestProfile, rules)) {
                root->addChild(cards);
                rebind(groupHUD, cards);
                cards->setCardOpacity(battleCardAlpha(true), battleCardAlpha(false));
                if (auto const& room = Service::get().group(); room && room->battle && room->battle->id == battleID)
                    cards->updateGroup(local, *room->battle, uid);
            }
        } else if (auto* cards =
                       BattleHUD::create(host ? hostProfile : guestProfile, host ? guestProfile : hostProfile, rules)) {
            root->addChild(cards);
            rebind(playerHUD, cards);
            cards->updatePlayers(local, other);
            cards->setCardOpacity(battleCardAlpha(true), battleCardAlpha(false));
        }
        auto make = [root](std::string const& text, CCPoint position, float scale) {
            auto* label = createCosmeticLabel(text.c_str(), "chatFont.fnt");
            label->setScale(scale);
            label->setPosition(position);
            root->addChild(label);
            return label;
        };
        auto const place = battleTextPlace(window);
        statusScale = place.scale;
        auto* info = make("", place.at, place.scale);
        info->setAnchorPoint(place.anchor);
        info->setOpacity(battleTextAlpha());
        info->setAlignment(place.anchor.x < .25f   ? kCCTextAlignmentLeft
                           : place.anchor.x > .75f ? kCCTextAlignmentRight
                                                   : kCCTextAlignmentCenter);
        rebind(status, info);
        layer->addChild(root, 100005);
        rebind(hud, root);
    }
    // 배틀 세션 시작. 방 정보로 모든 상태를 초기화하고 첫 시도를 시작하며,
    // 규칙의 연습 모드를 맞추고 HUD와 리플레이 기록을 시작한다.
    void beginSession(PlayLayer* layer) {
        auto const& room = Service::get().room();
        if (!room || !room->battle || !room->guest) return;
        audio::stopResultMusic();
        ++generation;
        rebind(play, layer);
        active = true;
        finished = false;
        leaving = false;
        timedOut = false;
        forcingQuit = false;
        reporting = false;
        dirty = true;
        quitDialog = false;
        returning = false;
        resultVisible = false;
        winnerRevealed = false;
        executionRevealed = false;
        confirmationVisible = false;
        progressSection = nullptr;
        decisionBanner = nullptr;
        actorSection = nullptr;
        transitionShade = nullptr;
        resultTimerFill = nullptr;
        hostPercent = nullptr;
        guestPercent = nullptr;
        hostGauge = nullptr;
        guestGauge = nullptr;
        resultPopup = nullptr;
        resultRoot = nullptr;
        displayedHostPercent = 0.f;
        displayedGuestPercent = 0.f;
        returnRetry = 0.f;
        runBest = 0;
        lastRecordedRun = 0;
        freePlay = false;
        freePlayRestart = 0.f;
        freePlayFrame = nullptr;
        spectateHUD = nullptr;
        spectateShown = false;
        resetCursor();
        watchUid.clear();
        watchName.clear();
        freePlayCaption.clear();
        watchOver = 0.f;
        watchAuto = false;
        lockedAt = {};
        lateCheatLogged = false;
        resultHold = 0.f;
        replay::liveStop();
        reportFailures = 0;
        reportError.clear();
        integrityReason.clear();
        shownIntegrityCode.clear();
        lastSync = Clock::now();
        ackFailures = 0;
        pendingDeath.reset();
        pendingDeathSends = 0;
        runStarted = Clock::now();
        runSimulated = 0.f;
        tpsCounts.clear();
        tpsSteps = 0;
        shownIntegrityFeatures.clear();
        antiCheatAlert = nullptr;
        host = Service::get().isHost();
        uid = Service::get().profile().uid;
        group = Service::get().inGroup();
        groupHUD = nullptr;
        roomID = room->id;
        battleID = room->battle->id;
        rules = room->rules;
        spectateServer = !debug::newerThan(debug::wireVersion(), kSpectateTestLastServer);
        hostProfile = room->host;
        guestProfile = *room->guest;
        levelUserCoins = room->level.userCoins;
        // 각 플레이어의 이 맵 이전 기록(최고 퍼센트)을 모아 결과 화면에 쓴다.
        {
            int64_t const levelId = room->level.id;
            int const own = mapLocalBest(room->level.id);
            auto reported = [levelId](auto const& download) {
                return download.levelId == levelId ? download.best : -2;
            };
            hostBefore = host ? own : reported(room->hostDownload);
            guestBefore = host ? reported(room->guestDownload) : own;
            groupBefore.clear();
            if (group)
                if (auto const& members = Service::get().group())
                    for (auto const& member : members->members)
                        groupBefore[member.profile.uid] = member.profile.uid == uid ? own : member.best;
        }
        local = host ? room->battle->host : room->battle->guest;
        other = host ? room->battle->guest : room->battle->host;
        shownHostReroll = room->battle->hostReroll;
        shownGuestReroll = room->battle->guestReroll;
        rerollSending = false;
        rerolledExit = false;
        reportTimer = 0.f;
        uiTimer = 0.f;
        keepAlive = 5.f;
        deadClock = 0.f;
        haltClock = 0.f;
        resultWaitClock = 0.f;
        animationClock = 0.f;
        animationRevivedLogged = false;
        spectating = false;
        local.spectating = false;
        local.inAttempt = true;
        local.runNumber = std::max(1, local.runNumber);
        releasing = true;
        if (layer->m_isPracticeMode != rules.practice) layer->togglePracticeMode(rules.practice);
        releasing = false;
        preloadBattleAssets();
        hudSetup(layer);
        replay::beginMatch(layer, battleID, !rules.practice && !group, !rules.practice);
        Service::get().forgetLiveWatched();
        replay::beginAttempt(layer, local.runNumber);
    }
    // 현재 퍼센트(0~100).
    int percent(PlayLayer* layer) const { return std::clamp(layer->getCurrentPercentInt(), 0, 100); }
    // 프레임마다 진행도를 갱신한다. 사망을 놓쳤으면 사망 처리하고,
    // 레이스 모드에서 목표 퍼센트에 도달하면 시도를 마치고 free play로 들어간다.
    void sampleState(PlayLayer* layer) {
        if (!owns(layer) || releasing || blocked() || local.paused || !local.inAttempt) return;
        if (layer->m_isTestMode || layer->m_isPracticeMode != rules.practice) return;
        if ((layer->m_player1 && layer->m_player1->m_isDead) ||
            (layer->m_gameState.m_isDualMode && layer->m_player2 && layer->m_player2->m_isDead)) {
            death(layer, "death seen by the frame check");
            return;
        }
        int current = percent(layer);
        if (current != local.currentPercent) {
            local.currentPercent = current;
            dirty = true;
        }
        local.bestPercent = std::max(local.bestPercent, current);
        runBest = std::max(runBest, current);
        if (rules.mode == 1 && current >= rules.targetPercent) {
            saveAttempt();
            replay::endAttempt(current, replay::End::Cut);
            local.inAttempt = false;
            local.spectating = true;
            spectating = true;
            dirty = true;
            logRun("target reached");
            enterFreePlay();
        }
    }
    // 현재 시도의 최고 퍼센트를 서버 기록으로 보낸다(시도당 한 번).
    void saveAttempt() {
        if (local.runNumber <= lastRecordedRun || local.runNumber <= 0) return;
        Service::get().recordAttempt(local.runNumber, runBest);
        lastRecordedRun = local.runNumber;
    }
    // 결과가 정해진 뒤 free play로 진입할 수 있는 상태인지.
    bool freePlayAllowed() const {
        return active && !finished && !leaving && !timedOut && integrityReason.empty() &&
               spectatorPlay(localRules(), local, false);
    }
    // 실제 맵 진행 기록 집계를 끝낸다.
    void endCountedPlay() {
        if (auto owner = play.lock(); owner && active) versus::progress::endCountedPlay(owner.data());
    }
    // free play 진입: 일시 정지 상태를 정산하고 관전 프레임을 띄우며 리플레이 기록을 마감한다.
    void enterFreePlay() {
        endCountedPlay();
        if (freePlay || !freePlayAllowed()) return;
        if (local.paused) {
            auto const now = Service::get().serverNow();
            ++local.pauseSeq;
            local.pauseChangedAt = now;
            consumePause(local, now);
            dirty = true;
            reportTimer = 0.f;
        }
        freePlay = true;
        spectating = true;
        local.spectating = true;
        if (lockedAt == Clock::time_point{}) lockedAt = Clock::now();
        showFreePlayFrame();
        if (auto owner = play.lock()) replay::endAttempt(percent(owner.data()), replay::End::Cut);
        replay::finishMatch();
    }
    // free play 종료: 프레임과 관전 버튼을 지운다.
    void exitFreePlay() {
        if (!freePlay) return;
        freePlay = false;
        freePlayRestart = 0.f;
        if (auto frame = freePlayFrame.lock()) frame->removeFromParent();
        freePlayFrame = nullptr;
        if (auto button = spectateHUD.lock()) {
            button->removeFromParent();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
            if (!finished && !leaving)
                if (auto owner = play.lock(); owner && !owner->m_isPaused) {
                    versus::cursor::hideForPlay();
                    resetCursor();
                }
#endif
        }
        spectateHUD = nullptr;
        spectateShown = false;
    }
    // free play 중에만 연습 모드를 바꿀 수 있다.
    bool practiceSwitchable() const { return freePlay && active && !releasing && !finished && !leaving; }
    // 규칙의 연습 모드 설정으로 되돌린다.
    void restorePractice() {
        auto owner = play.lock();
        if (!owner || owner->m_isPracticeMode == rules.practice) return;
        releasing = true;
        owner->togglePracticeMode(rules.practice);
        releasing = false;
    }
    // free play 프레임과 안내 문구를 만든다.
    void showFreePlayFrame() {
        auto root = hud.lock();
        if (!root || freePlayFrame.lock()) return;
        auto owner = play.lock();
        bool const real = owner && versus::progress::realMap(owner.data());
        freePlayCaption =
            real ? "Your result is locked. Play freely - Stars & Orbs is on: GD records this play like normal play."
                 : "Your result is locked. Play freely - practice mode works too, nothing here counts.";
        rebind(freePlayFrame, addSpectatorFrame(root.data(), 50, freePlayCaption.c_str()));
        showSpectateHUD(root.data());
    }
    static constexpr float kSpectateY = kSpectateButtonY, kSpectateScale = kSpectateButtonScale,
                           kSpectateGap = kSpectateButtonGap, kSpectateMargin = kSpectateButtonMargin;
    // free play의 Spectate / Stop Spectate / Switch 버튼을 만든다.
    void showSpectateHUD(CCNode* root) {
        if (spectateHUD.lock()) return;
        auto* holder = CCNode::create();
        holder->setID("group-spectate-hud"_spr);
        auto* menu = CCMenu::create();
        menu->setID("group-spectate-hud-menu"_spr);
        menu->setPosition({0.f, 0.f});
        auto add = [&](char const* text, char const* id, SEL_MenuHandler handler) {
            auto* sprite = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_05.png", 1.f);
            sprite->setScale(kSpectateScale);
            auto* item = CCMenuItemSpriteExtra::create(sprite, this, handler);
            item->setID(id);
            menu->addChild(item);
        };
        add("Spectate", "group-spectate-hud-button"_spr, menu_selector(Session::onSpectateHUD));
        add("Stop Spectate", "spectate-stop-button"_spr, menu_selector(Session::onStopSpectate));
        add("Switch", "spectate-switch-button"_spr, menu_selector(Session::onSpectateSwitch));
        holder->addChild(menu);
        holder->setVisible(false);
        root->addChild(holder, 51);
        rebind(spectateHUD, holder);
        spectateShown = false;
    }
    // 관전할 만한 플레이어가 남아 있는지(1v1은 상대, 그룹은 진행 중인 다른 좌석).
    bool someoneToWatch() const {
        if (!group) return !rules.practice && !other.forfeited && !terminal(other, otherRules());
        auto const& room = Service::get().group();
        if (!room || !room->battle || room->battle->id != battleID || room->battle->finishedAt > 0) return false;
        if (!watchableRules(room->battle->rules)) return false;
        for (auto const& seat : room->battle->seats)
            if (seat.profile.uid != uid && !seat.out && !seat.disqualified &&
                !resultFinal(seat.state, room->battle->rules))
                return true;
        return false;
    }
    // 관전 버튼 표시 여부와 위치를 갱신하고, 보일 때 데스크톱에서는 커서를 푼다.
    void updateSpectateHUD(PlayLayer* owner) {
        auto holder = spectateHUD.lock();
        if (!holder) return;
        bool const watching = !watchUid.empty();
        bool const shown = freePlay && !finished && !leaving && !owner->m_isPaused && (watching || someoneToWatch());
        holder->setVisible(shown);
        if (!shown) {
            spectateShown = false;
            releaseMenuCursor(holder.data());
            return;
        }
        keepMenuCursorVisible(holder.data());
        auto const window = CCDirector::sharedDirector()->getWinSize();
        auto* start = holder->getChildByIDRecursive("group-spectate-hud-button"_spr);
        auto* stop = holder->getChildByIDRecursive("spectate-stop-button"_spr);
        auto* swap = holder->getChildByIDRecursive("spectate-switch-button"_spr);
        if (start && stop && swap) {
            start->setVisible(!watching);
            stop->setVisible(watching);
            swap->setVisible(watching && group);
            float right = window.width - kSpectateMargin;
            for (auto* item : {watching ? stop : start, swap}) {
                if (!item->isVisible()) continue;
                float const width = item->getScaledContentSize().width;
                item->setPosition({right - width / 2.f, kSpectateY});
                right -= width + kSpectateGap;
            }
        }
        if (spectateShown) return;
        spectateShown = true;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
    }
    // 관전 버튼을 눌러도 되는 상태인지.
    bool spectateUsable(PlayLayer* owner) const {
        return owner && freePlay && !finished && !leaving && !owner->m_isPaused && !local.forfeited;
    }
    void onSpectateHUD(CCObject*) {
        auto owner = play.lock();
        if (!spectateUsable(owner.data())) return;
        if (group) {
            versus::showGroupStandings();
            return;
        }
        if (someoneToWatch()) startWatching(host ? guestProfile.uid : hostProfile.uid, otherName());
    }
    void onStopSpectate(CCObject*) {
        auto owner = play.lock();
        if (owner && !watchUid.empty()) stopWatching(owner.data(), true);
    }
    void onSpectateSwitch(CCObject*) {
        auto owner = play.lock();
        if (group && spectateUsable(owner.data())) versus::showGroupStandings();
    }
    // 해당 플레이어의 실시간 관전을 시작한다.
    bool startWatching(std::string const& target, std::string const& name) {
        auto owner = play.lock();
        if (!owner || !freePlay || finished || leaving || local.forfeited || target.empty() || target == uid)
            return false;
        if (target != watchUid) log::info("Versus: spectating {} live", name);
        watchUid = target;
        watchName = name.empty() ? std::string("Player") : name;
        watchOver = 0.f;
        return true;
    }
    // 관전을 끝내고 필요하면 내 맵을 처음부터 다시 시작한다.
    void stopWatching(PlayLayer* owner, bool restart) {
        bool const was = owner && replay::liveWatching(owner);
        watchUid.clear();
        watchName.clear();
        watchOver = 0.f;
        replay::liveWatch(nullptr, {}, {});
        if (was && restart) owner->resetLevelFromStart();
    }
    // 관전 갱신: Season 2 관전 좌석이면 자동으로 상대를 관전하고, 대상의 시도가 끝나면 3초 뒤 free play로 돌아간다.
    // 대상이 플레이 중이면 replay::liveWatch로 실시간 진행을 따라 본다.
    void updateWatch(PlayLayer* owner, float dt) {
        if (spectateTest() && localSpectator() && !watchAuto && watchUid.empty() && freePlay && !local.forfeited) {
            watchAuto = true;
            startWatching(host ? guestProfile.uid : hostProfile.uid, otherName());
        }
        if (watchUid.empty()) return;
        if (!freePlay || finished || leaving || local.forfeited) {
            stopWatching(owner, false);
            return;
        }
        bool active = false, over = false;
        if (group) {
            auto const& room = Service::get().group();
            GroupSeat const* seat = nullptr;
            if (room && room->battle && room->battle->id == battleID)
                if (int const index = room->battle->seatOf(watchUid); index >= 0) seat = &room->battle->seats[index];
            if (seat) {
                active = seat->state.inAttempt && !seat->state.paused;
                over = seat->out || seat->disqualified || resultFinal(seat->state, room->battle->rules);
            } else
                over = true;
        } else {
            active = other.inAttempt && !other.paused;
            over = other.forfeited || terminal(other, otherRules());
        }
        replay::liveTargetActive(active);
        if (over && !replay::livePlaying()) {
            watchOver += dt;
            if (watchOver >= 3.f) {
                if (group)
                    Notification::create(fmt::format("{} is done - back to free play.", watchName),
                                         NotificationIcon::Info, 2.f)
                        ->show();
                stopWatching(owner, true);
                return;
            }
        } else
            watchOver = 0.f;
        replay::liveWatch(owner, LiveTarget{roomID, battleID, group, watchUid}, watchName);
    }
    // 경고창 등 FLAlertLayer가 열려 있는지.
    static bool alertOpen(CCNode* scene) {
        for (auto* child : CCArrayExt<CCNode*>(scene->getChildren()))
            if (child->isVisible() && typeinfo_cast<FLAlertLayer*>(child)) return true;
        return false;
    }
    void resetCursor() {
        cursorShown = cursorUnlocked = false;
        cursorTimer = 0.f;
    }
    // 데스크톱에서 일시 정지/알림/관전 버튼이 있을 때 커서를 보이고 풀고, 평소에는 숨긴다.
    void updateCursor(PlayLayer* owner, float dt) {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        if (!inRunningScene(owner)) return;
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        bool const overlay = pauseMenuState(owner) != 0 || alertOpen(scene);
        bool const unlock = overlay || spectateShown;
        cursorTimer -= dt;
        if (unlock || versus::cursor::gameShowsCursor()) {
#ifdef GEODE_IS_WINDOWS
            if (!cursorShown || unlock != cursorUnlocked || (unlock && cursorTimer <= 0.f)) {
                cursorTimer = .5f;
                versus::cursor::show(unlock);
            }
#else
            if (!cursorShown) {
                if (unlock) versus::cursor::show(true);
            } else if (unlock != cursorUnlocked)
                versus::cursor::relock(unlock);
#endif
            cursorShown = true;
            cursorUnlocked = unlock;
            return;
        }
        if (cursorShown) versus::cursor::hideForPlay();
        cursorShown = cursorUnlocked = false;
#else
        (void)owner;
        (void)dt;
#endif
    }
    // free play 중 일시 정지 메뉴 표시와 프레임/관전 버튼 갱신.
    void updateFreePlayPause(PlayLayer* owner) {
        if (freePlay) markSpectatorPause(owner);
        if (auto frame = freePlayFrame.lock()) frame->setVisible(!(group && owner->m_isPaused));
        updateSpectateHUD(owner);
    }
    // PlayLayer에서 세션을 분리한다.
    void detachSession(PlayLayer* layer) {
        if (!active || !layer) return;
        auto owner = play.lock();
        if (!owner || owner.data() != layer) return;
        replay::liveStop();
        versus::progress::endCountedPlay(layer);
        active = false;
        Service::get().wantBattleReport(false);
        reporting = false;
        play = nullptr;
        audio::stopResultMusic();
    }
    // 현재 시도가 실제로 흐른 시간(초).
    float runRealSeconds() const { return std::chrono::duration<float>(Clock::now() - runStarted).count(); }
    // 시도 종료 로그. 게임 시간이 실제 시간보다 비정상적으로 빠르면 경고를 남긴다.
    void logRun(char const* how) const {
        float const real = runRealSeconds();
        float const scale = CCDirector::sharedDirector()->getScheduler()->getTimeScale();
        log::info(
            "Versus run {} ended at {}% after {:.2f} s ({:.2f} s of game time, time scale {:.2f}; {}; {} of {} lives used)",
            local.runNumber, local.currentPercent, real, runSimulated, scale, how, local.attemptsUsed,
            localRules().attempts);
        if (real < 1.f && runSimulated > real * 2.f + .1f)
            log::warn("[Versus] game time raced in run {}: {:.2f} s simulated in {:.2f} s real (time scale {:.2f})",
                      local.runNumber, runSimulated, real, scale);
    }
    // 시뮬레이션된 시간을 시도에 누적한다.
    void simulate(GJBaseGameLayer* layer, float dt) {
        if (owns(layer) && local.inAttempt && dt > 0.f) runSimulated += dt;
    }
    // 사망 처리: 시도를 마감하고 서버에 보낼 사망 상태를 보관한다. 더 이상 시도할 수 없으면 free play로 들어간다.
    void death(PlayLayer* layer, char const* how = "death") {
        if (!owns(layer) || releasing || !local.inAttempt || blocked()) return;
        if (!finishAttempt(local, localRules(), percent(layer))) return;
        replay::endAttempt(local.currentPercent, replay::End::Death);
        runBest = std::max(runBest, local.currentPercent);
        saveAttempt();
        logRun(how);
        pendingDeath = local;
        pendingDeathSends = 0;
        dirty = true;
        reportTimer = 0.f;
        if (done()) {
            spectating = true;
            local.spectating = true;
            enterFreePlay();
        }
    }
    // 클리어 처리: 퍼센트 100과 유저 코인을 기록하고 free play로 들어가 3초 뒤 다시 시작한다.
    void clear(PlayLayer* layer) {
        if (owns(layer) && freePlay) {
            if (!replay::liveWatching(layer)) freePlayRestart = 3.f;
            return;
        }
        if (!owns(layer) || local.cleared || finished || leaving || spectating || !integrityReason.empty()) return;
        local.currentPercent = 100;
        local.bestPercent = 100;
        local.cleared = true;
        replay::endAttempt(100, replay::End::Clear);
        local.userCoins = std::clamp(layer->countCollectedUserCoins(), 0, 3);
        runBest = 100;
        saveAttempt();
        local.inAttempt = false;
        local.spectating = true;
        spectating = true;
        dirty = true;
        reportTimer = 0.f;
        logRun("clear");
        enterFreePlay();
        freePlayRestart = 3.f;
    }
    // 리셋 전: free play는 허용, 종료 상태면 막고, 진행 중인 시도는 사망으로 처리한 뒤 다음 시도를 준비한다.
    bool resetBefore(PlayLayer* layer) {
        if (!owns(layer) || releasing) return true;
        if (freePlay) return true;
        if (blocked() || done()) return false;
        if (local.inAttempt) death(layer, "restart");
        if (freePlay) return true;
        if (done()) return false;
        replay::prepareAttempt(layer, local.runNumber + 1);
        return true;
    }
    // 리셋 후: 새 시도를 시작(시도 번호, 퍼센트 초기화)한다.
    void resetAfter(PlayLayer* layer) {
        if (!owns(layer) || releasing) return;
        if (freePlay) {
            freePlayRestart = 0.f;
            return;
        }
        pause(layer, false);
        if (done()) return;
        local.currentPercent = 0;
        local.inAttempt = true;
        local.spectating = false;
        spectating = false;
        ++local.runNumber;
        runBest = 0;
        dirty = true;
        reportTimer = 0.f;
        runStarted = Clock::now();
        runSimulated = 0.f;
        replay::beginAttempt(layer, local.runNumber);
    }
    // 일시 정지 상태 변화를 기록하고 서버에 알린다. 예산을 다 쓰면 포기 처리한다.
    void pause(PlayLayer* layer, bool value) {
        if (!owns(layer) || finished || freePlay || local.paused == value) return;
        auto now = Service::get().serverNow();
        ++local.pauseSeq;
        local.pauseChangedAt = now;
        if (value) {
            local.paused = true;
            local.pausedAt = now;
        } else
            consumePause(local, now);
        if (pauseRemaining(local, now) == 0) {
            forfeit(false);
            return;
        }
        dirty = true;
        reportTimer = 0.f;
        report(true);
    }
    // 일시 정지 메뉴에 남은 일시 정지 시간 시계를 표시한다.
    void updatePauseClock(PlayLayer* owner, int64_t now) {
        if (!local.paused || !owner->getParent()) return;
        for (auto* child : CCArrayExt<CCNode*>(owner->getParent()->getChildren())) {
            auto* pause = typeinfo_cast<PauseLayer*>(child);
            if (!pause) continue;
            auto* timer = static_cast<CCLabelBMFont*>(pause->getChildByID("versus-pause-clock"_spr));
            if (!timer) {
                auto size = CCDirector::sharedDirector()->getWinSize();
                auto* icon = CCSprite::createWithSpriteFrameName("GJ_timeIcon_001.png");
                icon->setScale(17.f / icon->getContentSize().height);
                icon->setPosition({size.width - 123.f, size.height - 23.f});
                pause->addChild(icon, 100);
                timer = createCosmeticLabel("20.0s", "bigFont.fnt");
                timer->setID("versus-pause-clock"_spr);
                timer->setPosition({size.width - 69.f, size.height - 23.f});
                timer->setScale(.45f);
                pause->addChild(timer, 100);
            }
            auto remaining = pauseRemaining(local, now);
            timer->setString(fmt::format("{:.1f}s", remaining / 1000.).c_str());
            timer->setColor(remaining <= 10000 ? ccc3(255, 95, 80) : ccWHITE);
        }
    }
    // 일시 정지 메뉴 상태: 1 열림, 0 없음, -1 판단 보류(레벨 시간이 흐르는지로 구분).
    int pauseMenuState(PlayLayer* owner) const {
        if (!owner->getParent()) return 0;
        bool hidden = false;
        for (auto* child : CCArrayExt<CCNode*>(owner->getParent()->getChildren())) {
            if (auto* menu = typeinfo_cast<PauseLayer*>(child)) {
                if (menu->isVisible()) return 1;
                hidden = true;
            }
        }
        if (!hidden) return 0;
        if (stillFrames >= 3) return 1;
        if (movingFrames >= 3) return 0;
        return -1;
    }
    // 레벨 시간이 멈췄는지 움직이는지 프레임 수를 센다.
    void trackLevelTime(PlayLayer* owner) {
        double const time = owner->m_gameState.m_levelTime;
        bool const still = time == lastLevelTime;
        stillFrames = still ? std::min(stillFrames + 1, 1000) : 0;
        movingFrames = still ? 0 : std::min(movingFrames + 1, 1000);
        lastLevelTime = time;
    }
    // 일시 정지 버튼을 길게 눌러 일시 정지를 얻을 수 있는 상태인지(시도 중이고 지금은 일시 정지가 막혀 있을 때).
    bool canHoldPause(PlayLayer* layer) const {
        return !freePlay && !releasing && !blocked() && !layer->m_hasCompletedLevel && !layer->m_isPaused &&
               local.inAttempt && !local.paused && !canPause(layer) &&
               pauseRemaining(local, Service::get().serverNow()) > 0;
    }
    // 길게 누르기 표시(버튼 확대/색)를 되돌린다.
    void endHoldVisual() {
        if (auto image = holdImage.lock()) {
            image->setScale(holdBaseScale);
            if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(image.data())) rgba->setColor(holdBaseColor);
        }
        holdImage = nullptr;
        if (auto node = holdNode.lock()) node->setVisible(false);
    }
    // 일시 정지 버튼/Esc를 5초 누르면 일시 정지를 허용한다. 진행 링과 안내 문구를 그린다.
    void updatePauseHold(PlayLayer* owner, float dt) {
        auto* ui = owner->m_uiLayer;
        auto* button = ui ? ui->m_pauseBtn : nullptr;
        bool const allowed = canHoldPause(owner);
        if (!allowed) escapeHeld = false;
        bool const pressed = escapeHeld || (button && button->isVisible() && button->isSelected());
        if (!pressed) holdConsumed = false;
        bool const holding = allowed && pressed && !holdConsumed;
        if (holding)
            holdTime += dt;
        else {
            if (holdTime > 0.f && allowed) holdHint = 2.5f;
            holdTime = 0.f;
        }
        holdHint = std::max(0.f, holdHint - dt);
        if (!allowed) holdHint = 0.f;
        if (holding && holdTime >= kPauseHoldSeconds) {
            holdConsumed = true;
            holdTime = 0.f;
            holdShown = 0.f;
            holdHint = 0.f;
            endHoldVisual();
            if (escapeHeld) escapeRepeatLatch = true;
            holdGranted = true;
            owner->pauseGame(false);
            holdGranted = false;
            escapeHeld = false;
            return;
        }
        float const target = holding ? std::min(1.f, holdTime / kPauseHoldSeconds) : 0.f;
        holdShown = holding ? target : std::max(0.f, holdShown - dt * 4.f);
        if (holdShown <= 0.f && holdHint <= 0.f) {
            endHoldVisual();
            return;
        }
        if (!ui) return;
        auto const window = CCDirector::sharedDirector()->getWinSize();
        CCPoint center = ui->convertToNodeSpace(CCPoint{window.width - 22.f, window.height - 22.f});
        if (button && button->getParent())
            center = ui->convertToNodeSpace(button->getParent()->convertToWorldSpace(button->getPosition()));
        auto* image = button ? button->getNormalImage() : nullptr;
        if (image && holdShown > 0.f) {
            if (!holdImage.lock()) {
                holdImage = image;
                holdBaseScale = image->getScale();
                if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(image)) holdBaseColor = rgba->getColor();
            }
            float const eased = 1.f - (1.f - holdShown) * (1.f - holdShown);
            image->setScale(holdBaseScale * (1.f + .9f * eased));
            if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(image))
                rgba->setColor(
                    ccc3(255, static_cast<GLubyte>(255 - 150 * eased), static_cast<GLubyte>(255 - 185 * eased)));
        } else if (holdShown <= 0.f) {
            if (auto old = holdImage.lock()) {
                old->setScale(holdBaseScale);
                if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(old.data())) rgba->setColor(holdBaseColor);
                holdImage = nullptr;
            }
        }
        auto node = holdNode.lock();
        if (!node || node->getParent() != ui) {
            if (node) node->removeFromParent();
            auto* fresh = CCNode::create();
            fresh->setID("versus-pause-hold"_spr);
            auto* ring = CCDrawNode::create();
            auto* text = CCLabelBMFont::create("", "bigFont.fnt");
            text->setAnchorPoint({1.f, .5f});
            fresh->addChild(ring);
            fresh->addChild(text);
            ui->addChild(fresh, 1000);
            holdNode = fresh;
            holdRing = ring;
            holdLabel = text;
            node = holdNode.lock();
        }
        node->setVisible(true);
        if (auto ring = holdRing.lock()) {
            ring->clear();
            float const size = button ? button->getScaledContentSize().width : 30.f;
            float const radius = size * (.62f + .55f * holdShown) + 4.f;
            constexpr int kSegments = 48;
            auto point = [&](float turn) {
                constexpr float kPi = 3.14159265f;
                float const angle = kPi / 2.f - turn * 2.f * kPi;
                return center + CCPoint{std::cos(angle) * radius, std::sin(angle) * radius};
            };
            if (holdShown > 0.f) {
                for (int i = 0; i < kSegments; ++i)
                    ring->drawSegment(point(i / float(kSegments)), point((i + 1) / float(kSegments)), 1.6f,
                                      ccc4f(0.f, 0.f, 0.f, .45f));
                int const filled = std::max(1, static_cast<int>(std::ceil(holdShown * kSegments)));
                for (int i = 0; i < filled; ++i)
                    ring->drawSegment(point(i / float(kSegments)),
                                      point(std::min(holdShown, (i + 1) / float(kSegments))), 2.4f,
                                      ccc4f(1.f, .55f - .35f * holdShown, .25f, 1.f));
            }
        }
        if (auto text = holdLabel.lock()) {
            bool const counting = holding && holdShown > 0.f;
            text->setString(counting
                                ? fmt::format("PAUSE IN {:.1f}", std::max(0.f, kPauseHoldSeconds - holdTime)).c_str()
                                : "HOLD 5 S TO PAUSE");
            text->setScale(counting ? .42f : .36f);
            text->setColor(counting ? ccc3(255, 190, 120) : ccWHITE);
            text->setOpacity(counting ? 255 : static_cast<GLubyte>(255 * std::min(1.f, holdHint)));
            float const size = button ? button->getScaledContentSize().height : 30.f;
            text->setPosition(center + CCPoint{14.f, -size * (.62f + .55f * holdShown) - 14.f});
        }
    }
    // Esc 키: 길게 누르기 대상이면 누름 상태를 기록하고 소비한다.
    bool escapeKey(PlayLayer* layer, bool down) {
        if (!down) {
            escapeHeld = false;
            return false;
        }
        if (!canHoldPause(layer)) return false;
        escapeHeld = true;
        return true;
    }
    // 뒤로 가기 키: 안내만 보여 주고 소비한다.
    bool backKey(PlayLayer* layer) {
        if (!canHoldPause(layer)) return false;
        if (!escapeHeld) holdHint = 2.5f;
        return true;
    }
    // 내 진행 상태를 서버에 보고한다. 죽은 직후의 사망 상태를 먼저(최대 2번) 보내고,
    // 성공/실패에 따라 재전송과 오류 표시를 결정하며 측정한 TPS도 함께 보낸다.
    void report(bool force = false) {
        if (auto layer = play.lock(); layer && integrity::framePending(layer.data())) return;
        if (!active || finished || !sameBattle() || reporting || (!dirty && !force)) return;
        if (!Service::get().battleReportAvailable()) return;
        reportTimer = 1.f;
        bool position = false;
        if (pendingDeath) {
            auto const& room = Service::get().room();
            auto const* confirmed =
                room && room->battle ? (host ? &room->battle->host : &room->battle->guest) : nullptr;
            if (pendingDeathSends >= 2 ||
                (confirmed && (confirmed->runNumber > pendingDeath->runNumber ||
                               (confirmed->runNumber == pendingDeath->runNumber && !confirmed->inAttempt &&
                                confirmed->attemptsUsed >= pendingDeath->attemptsUsed))))
                pendingDeath.reset();
        }
        bool const deathFirst = pendingDeath.has_value();
        if (int const tps = measuredTps(); tps > 0) {
            local.tps = tps;
            if (pendingDeath) pendingDeath->tps = tps;
        }
        auto const progress = deathFirst ? *pendingDeath : local;
        if (deathFirst) ++pendingDeathSends;
        reporting = true;
        dirty = false;
        auto epoch = generation;
        Service::get().reportBattle(
            progress, position,
            [this, epoch, pauseSeq = progress.pauseSeq, deathFirst, sent = progress](bool success, std::string detail) {
                if (epoch != generation) return;
                reporting = false;
                if (deathFirst) {
                    if (success && pendingDeath && pendingDeath->runNumber == sent.runNumber) pendingDeath.reset();
                    bool const newer = pendingDeath || local.runNumber != sent.runNumber ||
                                       local.inAttempt != sent.inAttempt || local.attemptsUsed != sent.attemptsUsed ||
                                       local.forfeited != sent.forfeited || local.cleared != sent.cleared ||
                                       local.spectating != sent.spectating || local.integrityCode != sent.integrityCode;
                    if (newer) {
                        dirty = true;
                        if (success) reportTimer = 0.f;
                    }
                }
                if (!success) {
                    dirty = true;
                    if (detail != "Room update in progress.") {
                        reportError = std::move(detail);
                        if (++reportFailures == 3) log::warn("Versus progress update failed: {}", reportError);
                    }
                } else {
                    reportFailures = 0;
                    reportError.clear();
                    lastSync = Clock::now();
                }
                if (local.pauseSeq != pauseSeq) {
                    dirty = true;
                    reportTimer = 0.f;
                }
            });
    }
    // 포기 처리. leaveRoom이면 방을 떠나고 아니면 일시 정지 한도 초과 등으로 서버 결과를 기다린다.
    void forfeit(bool leaveRoom) {
        if (!active || finished || leaving) return;
        endCountedPlay();
        saveAttempt();
        if (local.inAttempt)
            if (auto owner = play.lock()) replay::endAttempt(percent(owner.data()), replay::End::Cut);
        if (leaveRoom) replay::finishMatch();
        if (local.paused) {
            ++local.pauseSeq;
            local.pauseChangedAt = Service::get().serverNow();
        }
        consumePause(local, Service::get().serverNow());
        local.forfeited = true;
        local.inAttempt = false;
        local.paused = false;
        local.pausedAt = 0;
        local.spectating = true;
        spectating = true;
        exitFreePlay();
        dirty = true;
        reportTimer = 0.f;
        leaving = leaveRoom;
        timedOut = !leaveRoom;
        if (leaveRoom) resultBegan = Clock::now();
        report(true);
        if (auto owner = play.lock()) dismissPause(owner.data());
        label(status,
              leaveRoom ? "Forfeiting. Returning to the room..." : "Pause limit reached. Waiting for result...");
    }
    // 안티치트로 실격 처리: 결과가 이미 확정된 뒤의 늦은 감지는 무시하고,
    // 그 외에는 이유와 코드를 기록해 강제로 패배(포기) 처리한다.
    void disqualify(PlayLayer* layer, std::string const& reason, std::string const& code, std::string const& features) {
        if (!owns(layer) || !Service::get().antiCheatEnforced() || finished || leaving || !sameBattle() ||
            Service::get().room()->battle->finishedAt > 0 || !integrityReason.empty())
            return;
        if (!integrity::externalCode(code) || !integrity::validFeatureDetails(code, features)) return;
        if (lockedAt != Clock::time_point{} && !local.forfeited && resultLocked(local, localRules()) &&
            Clock::now() - lockedAt > std::chrono::seconds(2)) {
            if (!lateCheatLogged)
                log::info("[Versus anti-cheat] {} seen in spectator mode after the result was final: ignored", reason);
            lateCheatLogged = true;
            versus::progress::discard(reason, true);
            return;
        }
        integrityReason = reason;
        versus::progress::discard(reason);
        local.integrityCode = code;
        local.integrityFeatures = features;
        showIntegrityAlert(code, features, false);
        log::warn("[Versus anti-cheat] Cheat detected: {}; forced defeat; match={}, run={}", reason, battleID,
                  local.runNumber);
        forfeit(false);
        timedOut = false;
        FMODAudioEngine::sharedEngine()->pauseAllMusic(true);
        label(status, "Cheat detected - forced defeat. Waiting for match sync...");
    }
    // 나가기 요청 처리. 종료 후면 복귀, 상황에 따라 확인 창(그룹 free play, 서버 연결 불가, 관전 좌석 등)을 띄우고
    // 일반 배틀에서 나가면 포기로 처리한다.
    bool askQuit(PlayLayer* layer) {
        if (!owns(layer) || forcingQuit) return false;
        if (finished) {
            if (confirmationVisible) exitAfterResult();
            return true;
        }
        if (quitDialog || leaving) return true;
        if (freePlay && group) {
            quitDialog = true;
            auto epoch = generation;
            createQuickPopup("Leave the Map?",
                             "Your result is <cg>locked</c> and still counts.\nWait for the final result in the room?",
                             "Stay", "Leave", [this, epoch](FLAlertLayer*, bool yes) {
                                 if (epoch != generation || !active) return;
                                 quitDialog = false;
                                 if (yes) leaveLocked();
                             });
            return true;
        }
        if (freePlay && serverUnreachable()) {
            quitDialog = true;
            auto epoch = generation;
            createQuickPopup(
                "Leave the Map?",
                "The server can't be reached right now (maintenance or connection).\n"
                "You can leave. If this match is still running when the server is back, it may count as a loss.",
                "Stay", "Leave", [this, epoch](FLAlertLayer*, bool yes) {
                    if (epoch != generation || !active) return;
                    quitDialog = false;
                    if (yes) leaveUnreachable();
                });
            return true;
        }
        if (spectateTest() && (localSpectator() || otherSpectator())) {
            quitDialog = true;
            auto epoch = generation;
            createQuickPopup("Leave Versus?",
                             "Leaving <cy>ends this match</c>.\nThe result goes by how far each of you got.", "Stay",
                             "Leave", [this, epoch](FLAlertLayer*, bool yes) {
                                 if (epoch != generation || !active) return;
                                 quitDialog = false;
                                 if (yes) forfeit(true);
                             });
            return true;
        }
        if (freePlay) {
            FLAlertLayer::create(
                "Spectator Mode",
                "Your result is <cg>locked</c>. You can keep playing the map,\nbut you can't leave until the match ends.",
                "OK")
                ->show();
            return true;
        }
        quitDialog = true;
        auto epoch = generation;
        std::string const text =
            group ? "Leaving this battle counts as a <cr>forfeit</c>.\nThe other players play on."
            : spectating
                ? "Leaving this match counts as a <cr>forfeit</c>.\n<cy>If your opponent disconnected, you win automatically within 30 seconds.</c>"
                : "Leaving this match counts as a <cr>forfeit</c>.";
        createQuickPopup("Leave Versus?", text, "Stay", "Leave", [this, epoch](FLAlertLayer*, bool yes) {
            if (epoch != generation || !active) return;
            quitDialog = false;
            if (yes) forfeit(true);
        });
        return true;
    }
    // 열려 있는 일시 정지 메뉴를 닫고 오디오를 재개한다.
    void dismissPause(PlayLayer* owner) {
        auto* parent = owner->getParent();
        auto* children = parent ? parent->getChildren() : nullptr;
        if (children) {
            for (int i = static_cast<int>(children->count()) - 1; i >= 0; --i) {
                if (auto* pause = typeinfo_cast<PauseLayer*>(children->objectAtIndex(i)))
                    pause->removeFromParentAndCleanup(true);
            }
        }
        if (owner->m_isPaused) owner->resume();
        if (auto* engine = FMODAudioEngine::sharedEngine()) {
            engine->resumeAllAudio();
            engine->resumeAllEffects();
        }
    }
    // 리롤 투표 알림(HUD 줄과 효과음)을 표시한다.
    void noteReroll(int64_t at, int64_t& shown, bool hostSide) {
        if (at <= 0 || at == shown) return;
        shown = at;
        if (auto cards = playerHUD.lock()) cards->showReroll(hostSide == host);
        audio::play(audio::Cue::Reroll);
    }
    void noteRerolls(BattleInfo const& battle) {
        noteReroll(battle.hostReroll, shownHostReroll, true);
        noteReroll(battle.guestReroll, shownGuestReroll, false);
    }
    // 투표가 아직 유효한 시간 안인지.
    static bool voteCounts(int64_t at) {
        auto const age = Service::get().serverNow() - at;
        return at > 0 && age >= -5'000 && age < kRerollVoteWindowMs;
    }
    // 리롤 가능 조건: Season 2가 아닌 Ranked 1v1에서 출발한 뒤 결과가 정해지지 않았을 때.
    bool rerollAvailable() const {
        if (!active || group || finished || leaving || !Service::get().isRankedMatch() || !sameBattle() ||
            Service::get().botBattle())
            return false;
        if (Service::get().season2()) return false;
        auto const& room = Service::get().room();
        return room->launch && room->launch->releasedAt > 0 && room->battle->finishedAt == 0 &&
               !resultLocked(local, rules) && !resultLocked(other, rules);
    }
    bool ownRerollVoted() const { return voteCounts(host ? shownHostReroll : shownGuestReroll); }
    bool opponentRerollVoted() const { return voteCounts(host ? shownGuestReroll : shownHostReroll); }
    // 서버에 리롤 투표를 보낸다.
    void voteReroll() {
        if (!rerollAvailable() || ownRerollVoted() || rerollSending) return;
        rerollSending = true;
        auto const epoch = generation;
        Service::get().voteRankedReroll(battleID, "",
                                        [this, epoch](std::optional<RankedState> state, std::string detail) {
                                            if (epoch != generation) return;
                                            rerollSending = false;
                                            if (!state) {
                                                Notification::create(detail.empty() ? "Reroll unavailable." : detail,
                                                                     NotificationIcon::Warning, 2.5f)
                                                    ->show();
                                                return;
                                            }
                                            if (sameBattle()) noteRerolls(*Service::get().room()->battle);
                                        });
    }
    // 양쪽이 리롤하면 이 맵을 버리고 나간다.
    void rerollExit(PlayLayer* owner) {
        replay::liveStop();
        exitFreePlay();
        replay::abandonMatch();
        endCountedPlay();
        finished = true;
        rerolledExit = true;
        resultBegan = Clock::now();
        dismissPause(owner);
        FMODAudioEngine::sharedEngine()->pauseAllMusic(true);
        audio::play(audio::Cue::Reroll);
        label(status, "Both players chose Reroll. Loading a new map...");
    }
    void onResultExit(CCObject*) {
        if (finished && confirmationVisible) exitAfterResult();
    }
    // 그룹에서 결과가 확정된 플레이어가 맵을 나간다.
    void leaveLocked() {
        if (!active || finished || forcingQuit || !group) return;
        quitMap();
    }
    // 마지막 동기화 후 6초 이상 지났는지.
    bool syncBehind() const {
        return lastSync != Clock::time_point{} && Clock::now() - lastSync >= std::chrono::seconds(6);
    }
    // 서버가 내 자리를 마지막으로 확인한 뒤 지난 시간(초). 진행 보고와 heartbeat 성공을 모두 본다.
    float seatSilence() const {
        auto const ack = std::max(lastSync, Service::get().lastSeatAck());
        return std::chrono::duration<float>(Clock::now() - ack).count();
    }
    // 연결 끊김 경고. 서버에 닿지 않는 동안 뛴 판은 기록되지 않고, 30초가 지나면 서버가 이 자리를 연결 끊김
    // 패배로 끝낸다(server/battle_rules.py seat_stale). 서버가 이미 플레이로 결과를 확정한 자리(클리어,
    // 목숨 소진)는 끊겨도 지지 않으므로 띄우지 않는다. 점검 중에는 서버가 끊김을 세지 않으므로 띄우지 않는다.
    void updateLinkWarning(BattleInfo const& snapshot) {
        auto const& room = Service::get().room();
        auto const& confirmed = host ? snapshot.host : snapshot.guest;
        float const silent = seatSilence();
        bool const show = !group && !finished && !leaving && room && room->launch && room->launch->releasedAt > 0 &&
                          !Service::get().botBattle() && !Service::get().debugRoom() &&
                          !Service::get().maintenance() && !local.forfeited && !confirmed.forfeited &&
                          !finalByPlay(confirmed, localRules()) && silent >= kLinkWarnSeconds;
        auto box = linkWarning.lock();
        if (!show) {
            if (box) box->setVisible(false);
            return;
        }
        auto root = hud.lock();
        if (!root) return;
        auto const window = CCDirector::sharedDirector()->getWinSize();
        float const width = std::min(window.width - 40.f, 340.f);
        if (!box || box->getParent() != root.data()) {
            if (box) box->removeFromParent();
            auto* node = CCNode::create();
            node->setID("connection-warning"_spr);
            node->setPosition({window.width / 2.f, window.height * .64f});
            auto* back = CCScale9Sprite::create("square02_small.png");
            back->setContentSize({width, 64.f});
            back->setColor(ccc3(60, 0, 8));
            back->setOpacity(200);
            node->addChild(back);
            auto* title = CCLabelBMFont::create("CONNECTION LOST", "bigFont.fnt");
            title->setColor(ccc3(255, 96, 96));
            title->setPosition({0.f, 14.f});
            title->limitLabelWidth(width - 24.f, .55f, .2f);
            node->addChild(title);
            auto* detail = CCLabelBMFont::create("", "chatFont.fnt");
            detail->setAlignment(kCCTextAlignmentCenter);
            detail->setPosition({0.f, -12.f});
            node->addChild(detail);
            root->addChild(node, 1001);
            rebind(linkWarning, node);
            rebind(linkWarningText, detail);
            box = linkWarning.lock();
        }
        box->setVisible(true);
        int const left = static_cast<int>(std::ceil(kSeatStaleSeconds - silent));
        auto const text = left > 0 ? fmt::format("Your runs are not reaching the server.\n"
                                                 "This match is lost in {} s if the connection does not return.",
                                                 left)
                                   : std::string("No connection for 30 s.\nThis match is probably lost already.");
        if (auto detail = linkWarningText.lock(); detail && text != detail->getString()) {
            detail->setString(text.c_str());
            detail->limitLabelWidth(width - 24.f, .6f, .2f);
        }
    }
    void hideLinkWarning() {
        if (auto box = linkWarning.lock()) box->setVisible(false);
    }
    // 점검 중이거나 보고가 3번 이상 실패하고 15초 이상 동기화되지 않았으면 서버에 닿지 않는 것으로 본다.
    bool serverUnreachable() const {
        if (Service::get().maintenance()) return true;
        return reportFailures >= 3 && lastSync != Clock::time_point{} &&
               Clock::now() - lastSync >= std::chrono::seconds(kUnreachableSeconds);
    }
    // 서버에 닿지 않는 상태에서 free play를 떠난다.
    void leaveUnreachable() {
        if (!active || finished || forcingQuit || !freePlay) return;
        log::warn("[Versus] left spectator mode while the server could not be reached (battle {})", battleID);
        endCountedPlay();
        quitMap();
    }
    // 서버 처리 없이 맵을 닫는다.
    void quitMap() {
        replay::liveStop();
        replay::finishMatch();
        forcingQuit = true;
        audio::stopResultMusic();
        if (auto owner = play.lock()) owner->onQuit();
        active = false;
        Service::get().wantBattleReport(false);
    }
    // 그룹 배틀 종료 연출: 그룹 결과 화면을 띄우고 30초 뒤 자동 복귀한다.
    void groupEndVisual() {
        exitFreePlay();
        if (local.inAttempt)
            if (auto owner = play.lock()) replay::endAttempt(percent(owner.data()), replay::End::Cut);
        replay::finishMatch();
        endCountedPlay();
        finished = true;
        resultBegan = Clock::now();
        auto owner = play.lock();
        if (!owner) return;
        dismissPause(owner.data());
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
        FMODAudioEngine::sharedEngine()->pauseAllMusic(true);
        GroupBattle battle;
        if (auto const& room = Service::get().group(); room && room->battle && room->battle->id == battleID)
            battle = *room->battle;
        auto* root =
            createGroupResult(battle, uid, "Return to Room", this, menu_selector(Session::onResultExit), &groupBefore);
        updateGroupResult(root, kGroupResultSeconds, kGroupResultSeconds);
        if (auto* scene = owner->getParent())
            scene->addChild(root, 100010);
        else
            owner->addChild(root, 100010);
        rebind(resultRoot, root);
        resultVisible = true;
        confirmationVisible = true;
        root->setScale(.94f);
        root->runAction(CCEaseBackOut::create(CCScaleTo::create(.3f, 1.f)));
        Service::get().markGroupResultShown(battleID);
        audio::play(audio::Cue::Finish);
        audio::startResultMusic();
    }
    // 플레이가 아닌 이유(안티치트, 연결 끊김, 기권, 일시정지 초과)로 끝났을 때 그 사유. 아니면 빈 문구.
    // 연결 끊김 = 진 쪽이 기권으로 기록됐고 안티치트가 아니며, 마지막 보고가 결과보다 25초 이상 앞섬.
    struct ResultReason {
        std::string text;
        bool disconnected = false;
    };
    ResultReason resultReason(BattleInfo const& info) const {
        if (info.draw) return {};
        auto const& loserState = info.winnerUid == hostProfile.uid ? info.guest : info.host;
        if (!loserState.forfeited && loserState.pausedAt <= 0) return {};
        bool const won = info.winnerUid == uid;
        bool const disconnected = loserState.forfeited && loserState.integrityCode.empty() &&
                                  info.finishedAt - loserState.updatedAt >= 25'000;
        if (!loserState.integrityCode.empty())
            return {integrity::resultReason(loserState.integrityCode, won, loserState.integrityFeatures)};
        if (disconnected) return {won ? "Opponent disconnected" : "Connection lost", true};
        if (won) return {loserState.forfeited ? "Opponent forfeited" : "Opponent timed out"};
        return {loserState.forfeited ? "Match forfeited" : "Pause timeout"};
    }
    // 1v1 결과 화면을 만든다. 진행도 게이지, 승패 문구, 포기/연결 끊김 사유, 처형 연출 준비, 결과 확인 팝업과 타이머 바를 구성한다.
    void endVisual(BattleInfo const& info) {
        if (finished || leaving) return;
        if (group) {
            groupEndVisual();
            return;
        }
        replay::liveStop();
        hideLinkWarning();
        exitFreePlay();
        if (local.inAttempt) {
            auto const& final = host ? info.host : info.guest;
            runBest = std::min(runBest, final.bestPercent);
            saveAttempt();
            if (auto owner = play.lock()) replay::endAttempt(percent(owner.data()), replay::End::Cut);
        }
        replay::finishMatch();
        endCountedPlay();
        finished = true;
        finalResult = info;
        resultBegan = Clock::now();
        auto owner = play.lock();
        if (!owner) return;
        dismissPause(owner.data());
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
        FMODAudioEngine::sharedEngine()->pauseAllMusic(true);
        auto window = CCDirector::sharedDirector()->getWinSize();
        auto* root = CCNode::create();
        root->setID("battle-result"_spr);
        keepMenuCursorVisible(root);
        auto* shade = CCLayerColor::create(ccc4(5, 20, 50, 175));
        root->addChild(shade);
        auto section = [root](int z) {
            auto* node = CCNodeRGBA::create();
            node->setCascadeOpacityEnabled(true);
            node->setOpacity(0);
            root->addChild(node, z);
            return node;
        };
        auto* progress = section(5);
        rebind(progressSection, progress);
        auto* curtain = CCLayerColor::create(ccc4(0, 0, 0, 0));
        root->addChild(curtain, 10);
        rebind(transitionShade, curtain);
        auto* decision = CCNodeRGBA::create();
        decision->setCascadeOpacityEnabled(true);
        decision->setOpacity(0);
        progress->addChild(decision, 15);
        rebind(decisionBanner, decision);
        auto* actors = section(20);
        rebind(actorSection, actors);
        auto const& winnerProfile = info.winnerUid == hostProfile.uid ? hostProfile : guestProfile;
        auto const winnerName = winnerProfile.name;
        auto* title = createCosmeticLabel(info.draw ? "DRAW" : (winnerName + " WINS!").c_str(), "bigFont.fnt");
        title->setPosition({window.width / 2.f, window.height / 2.f + 94.f});
        title->limitLabelWidth(window.width - 40.f, .75f, .3f);
        if (!info.draw) styleNickname(title, winnerProfile.membership, winnerProfile.cosmetics);
        decision->addChild(title, 10);
        auto resultLabel = [window](CCNode* parent, std::string text, float x, float y, float width, float scale,
                                    char const* font = "chatFont.fnt") {
            auto* value = createCosmeticLabel(text.c_str(), font);
            value->setScale(std::min(scale, width / std::max(1.f, value->getContentSize().width)));
            value->setPosition({window.width / 2.f + x, window.height / 2.f + y});
            parent->addChild(value, 5);
            return value;
        };
        resultLabel(decision,
                    info.draw               ? "Equal result"
                    : info.winnerUid == uid ? "You won!"
                                            : "You lost",
                    0.f, 65.f, window.width - 40.f, .95f);
        auto* hostPreview = icon(hostProfile, 1.8f);
        auto* guestPreview = icon(guestProfile, 1.8f);
        guestPreview->setFlipX(true);
        hostPreview->setPosition({window.width / 2.f - 95.f, window.height / 2.f - 10.f});
        guestPreview->setPosition({window.width / 2.f + 95.f, window.height / 2.f - 10.f});
        progress->addChild(hostPreview);
        progress->addChild(guestPreview);
        if (levelUserCoins > 0) {
            auto coinCount = [progress, window](float x, int coins) {
                auto* group = CCNode::create();
                group->setPosition({window.width / 2.f + x, window.height / 2.f + 28.f});
                auto* coin = CCSprite::createWithSpriteFrameName("usercoin_small01_001.png");
                if (coin) {
                    coin->setScale(.42f);
                    coin->setPosition({-14.f, 0.f});
                    group->addChild(coin);
                }
                auto* value = createCosmeticLabel(fmt::format("{}/3", std::clamp(coins, 0, 3)).c_str(), "goldFont.fnt");
                value->setScale(.45f);
                value->setPosition({8.f, 0.f});
                group->addChild(value);
                progress->addChild(group, 8);
            };
            coinCount(-95.f, info.host.userCoins);
            coinCount(95.f, info.guest.userCoins);
        }
        styleNickname(resultLabel(progress, hostProfile.name, -95.f, -56.f, 170.f, .5f, "bigFont.fnt"),
                      hostProfile.membership, hostProfile.cosmetics);
        styleNickname(resultLabel(progress, guestProfile.name, 95.f, -56.f, 170.f, .5f, "bigFont.fnt"),
                      guestProfile.membership, guestProfile.cosmetics);
        rebind(hostPercent, resultLabel(progress, "0%", -95.f, -79.f, 140.f, .5f, "bigFont.fnt"));
        if (auto value = hostPercent.lock())
            value->setColor(info.draw || info.winnerUid == hostProfile.uid ? ccc3(179, 237, 255) : ccc3(235, 235, 235));
        resultLabel(progress, "VS", 0.f, -79.f, 35.f, .55f, "bigFont.fnt");
        rebind(guestPercent, resultLabel(progress, "0%", 95.f, -79.f, 140.f, .5f, "bigFont.fnt"));
        if (auto value = guestPercent.lock())
            value->setColor(info.draw || info.winnerUid == guestProfile.uid ? ccc3(179, 237, 255)
                                                                            : ccc3(235, 235, 235));
        auto gauge = [progress, window](float x, ccColor4B tint) {
            constexpr float width = 145.f;
            auto* track = CCLayerColor::create(ccc4(28, 49, 67, 255));
            track->setContentSize({width, 7.f});
            track->setPosition({window.width / 2.f + x - width / 2.f, window.height / 2.f - 99.f});
            progress->addChild(track, 5);
            auto* fill = CCLayerColor::create(tint);
            fill->setContentSize({0.f, 7.f});
            track->addChild(fill);
            return fill;
        };
        rebind(hostGauge, gauge(-95.f, ccc4(80, 205, 255, 255)));
        rebind(guestGauge, gauge(95.f, ccc4(255, 139, 178, 255)));
        if (rules.practice && rules.mode == 0) {
            resultLabel(progress, fmt::format("{} attempts", info.host.attemptsUsed + 1), -95.f, -113.f, 170.f, .6f);
            resultLabel(progress, fmt::format("{} attempts", info.guest.attemptsUsed + 1), 95.f, -113.f, 170.f, .6f);
        } else {
            for (auto [x, best] : {std::pair{-95.f, hostBefore}, std::pair{95.f, guestBefore}}) {
                auto [text, color] = mapRecordText(best);
                if (!text.empty()) resultLabel(progress, text, x, -113.f, 170.f, .55f)->setColor(color);
            }
        }
        rebind(returnStatus, resultLabel(root, "", 0.f, -133.f, window.width - 40.f, .65f));
        if (auto footer = returnStatus.lock()) footer->setZOrder(40);
        if (auto const why = resultReason(info); !why.text.empty()) {
            auto* reason = createCosmeticLabel(why.text.c_str(), "chatFont.fnt");
            reason->setScale(.75f);
            reason->setPosition({window.width / 2.f, window.height / 2.f + 36.f});
            decision->addChild(reason);
        }
        auto* a = icon(hostProfile, 1.8f);
        auto* b = icon(guestProfile, 1.8f);
        b->setFlipX(true);
        actors->setContentSize(window);
        actors->setAnchorPoint({.5f, .5f});
        actors->setPosition(window / 2.f);
        actors->setVisible(false);
        a->setPosition({window.width / 2.f - 95.f, window.height / 2.f - 10.f});
        b->setPosition({window.width / 2.f + 95.f, window.height / 2.f - 10.f});
        actors->addChild(a);
        actors->addChild(b);
        if (!info.draw) {
            auto* winner = info.winnerUid == hostProfile.uid ? a : b;
            auto* loser = info.winnerUid == hostProfile.uid ? b : a;
            int const choice = resolveExecution(winnerProfile.cosmetics.execution, winnerProfile.membership,
                                                battleID + info.winnerUid);
            execution::play(actors, winner, loser, choice, RESULT_SUMMARY_TIME + .55f);
        }
        auto* popup = CCNodeRGBA::create();
        popup->setCascadeOpacityEnabled(true);
        popup->setOpacity(0);
        popup->setVisible(false);
        popup->setID("result-confirmation"_spr);
        float const panelWidth = std::min(390.f, window.width - 24.f);
        float const panelHeight = std::min(190.f, window.height - 24.f);
        auto* panel = CCLayerColor::create(ccc4(8, 29, 56, 248));
        panel->setContentSize({panelWidth, panelHeight});
        panel->setPosition({(window.width - panelWidth) / 2.f, (window.height - panelHeight) / 2.f});
        popup->addChild(panel);
        auto popupLabel = [popup, window](std::string const& value, float x, float y, float width, float scale,
                                          char const* font = "bigFont.fnt") {
            auto* text = createCosmeticLabel(value.c_str(), font);
            text->setPosition({window.width / 2.f + x, window.height / 2.f + y});
            text->limitLabelWidth(width, scale, .2f);
            popup->addChild(text, 2);
            return text;
        };
        popupLabel("MATCH RESULT", 0.f, 76.f, panelWidth - 24.f, .48f);
        auto* resultTitle = popupLabel(info.draw ? "DRAW" : winnerName + " WINS!", 0.f, 55.f, panelWidth - 24.f, .55f);
        if (!info.draw) styleNickname(resultTitle, winnerProfile.membership, winnerProfile.cosmetics);
        // 기권·연결 끊김 등으로 끝났으면 카드에도 그 사유를 적는다(위의 발표 문구는 카드에 가려진다).
        // 내가 연결 끊김으로 졌으면, 끊긴 뒤 뛴 판이 기록되지 않았다는 것도 함께 알린다.
        if (auto const why = resultReason(info); !why.text.empty()) {
            bool const won = info.winnerUid == uid;
            auto* reason = popupLabel(why.disconnected && !won ? "Connection lost - runs played while offline did not count"
                                                               : why.text,
                                      0.f, 38.f, panelWidth - 24.f, .55f, "chatFont.fnt");
            reason->setColor(won ? ccc3(150, 235, 170) : ccc3(255, 150, 150));
        }

        constexpr float playerX = 82.f;
        auto* hostResultIcon = icon(hostProfile, !info.draw && info.winnerUid == hostProfile.uid ? 1.35f : 1.05f);
        auto* guestResultIcon = icon(guestProfile, !info.draw && info.winnerUid == guestProfile.uid ? 1.35f : 1.05f);
        guestResultIcon->setFlipX(true);
        hostResultIcon->setPosition({window.width / 2.f - playerX, window.height / 2.f + 10.f});
        guestResultIcon->setPosition({window.width / 2.f + playerX, window.height / 2.f + 10.f});
        if (!info.draw && info.winnerUid != hostProfile.uid) hostResultIcon->setOpacity(115);
        if (!info.draw && info.winnerUid != guestProfile.uid) guestResultIcon->setOpacity(115);
        popup->addChild(hostResultIcon, 3);
        popup->addChild(guestResultIcon, 3);
        popupLabel("VS", 0.f, 8.f, 38.f, .42f);
        styleNickname(popupLabel(hostProfile.name, -playerX, -25.f, 125.f, .36f, "bigFont.fnt"), hostProfile.membership,
                      hostProfile.cosmetics);
        styleNickname(popupLabel(guestProfile.name, playerX, -25.f, 125.f, .36f, "bigFont.fnt"),
                      guestProfile.membership, guestProfile.cosmetics);
        auto* hostScore = popupLabel(fmt::format("{}%", info.host.bestPercent), -playerX, -44.f, 100.f, .42f);
        auto* guestScore = popupLabel(fmt::format("{}%", info.guest.bestPercent), playerX, -44.f, 100.f, .42f);
        if (info.draw || info.winnerUid == hostProfile.uid) hostScore->setColor(ccc3(179, 237, 255));
        if (info.draw || info.winnerUid == guestProfile.uid) guestScore->setColor(ccc3(179, 237, 255));
        for (auto [x, best] : {std::pair{-playerX, hostBefore}, std::pair{playerX, guestBefore}}) {
            auto [text, color] = mapRecordText(best);
            if (!text.empty()) popupLabel(text, x, -58.f, 150.f, .5f, "chatFont.fnt")->setColor(color);
        }

        auto* menu = CCMenu::create();
        menu->setPosition({window.width / 2.f, window.height / 2.f - 78.f});
        auto* button = ButtonSprite::create(Service::get().isRankedMatch() ? "View Rank Update" : "Return to Room",
                                            "goldFont.fnt", "GJ_button_01.png");
        button->setScale(.72f);
        menu->addChild(CCMenuItemSpriteExtra::create(button, this, menu_selector(Session::onResultExit)));
        popup->addChild(menu, 3);
        root->addChild(popup, 50);
        rebind(resultPopup, popup);
        resultTimerWidth = std::min(440.f, window.width - 48.f);
        auto* timerTrack = CCLayerColor::create(ccc4(255, 255, 255, 48));
        timerTrack->setContentSize({resultTimerWidth, 2.f});
        timerTrack->setPosition({(window.width - resultTimerWidth) / 2.f, 8.f});
        root->addChild(timerTrack, 60);
        auto* timerFill = CCLayerColor::create(ccc4(255, 255, 255, 240));
        timerFill->setContentSize({resultTimerWidth, 2.f});
        timerFill->setPosition(timerTrack->getPosition());
        root->addChild(timerFill, 61);
        rebind(resultTimerFill, timerFill);
        if (auto* scene = owner->getParent())
            scene->addChild(root, 100010);
        else
            owner->addChild(root, 100010);
        rebind(resultRoot, root);
        resultVisible = true;
        audio::play(audio::Cue::Finish);
        audio::startResultMusic();
        progress->runAction(CCFadeIn::create(.42f));
    }
    // 결과 화면이 시작된 뒤 지난 시간(초).
    float resultElapsed() const { return std::chrono::duration<float>(Clock::now() - resultBegan).count(); }
    // 결과 연출 진행: 퍼센트가 올라가는 애니메이션 -> 승자 발표 -> 처형 연출 -> 확인 팝업과 자동 복귀 카운트다운.
    void updateResultVisual(float elapsed, float dt) {
        if (!resultVisible) return;
        float remaining = 0.f;
        if (elapsed < RESULT_SUMMARY_TIME)
            remaining = 1.f - elapsed / RESULT_SUMMARY_TIME;
        else if (elapsed < RESULT_CONFIRM_START)
            remaining = 1.f - (elapsed - RESULT_SUMMARY_TIME) / RESULT_EXECUTION_TIME;
        else
            remaining = 1.f - (elapsed - RESULT_CONFIRM_START) / RESULT_CONFIRM_TIME;
        if (auto bar = resultTimerFill.lock())
            bar->setContentSize({resultTimerWidth * std::clamp(remaining, 0.f, 1.f), 2.f});
        float const lerpAmount = std::clamp(-std::expm1(-8.f * std::max(0.f, dt) / RESULT_REVEAL_TIME_SCALE), 0.f, 1.f);
        displayedHostPercent =
            std::lerp(displayedHostPercent, static_cast<float>(finalResult.host.bestPercent), lerpAmount);
        displayedGuestPercent =
            std::lerp(displayedGuestPercent, static_cast<float>(finalResult.guest.bestPercent), lerpAmount);
        if (elapsed >= RESULT_PROGRESS_TIME || std::abs(displayedHostPercent - finalResult.host.bestPercent) < .01f)
            displayedHostPercent = static_cast<float>(finalResult.host.bestPercent);
        if (elapsed >= RESULT_PROGRESS_TIME || std::abs(displayedGuestPercent - finalResult.guest.bestPercent) < .01f)
            displayedGuestPercent = static_cast<float>(finalResult.guest.bestPercent);
        int const hostValue = static_cast<int>(std::round(displayedHostPercent));
        int const guestValue = static_cast<int>(std::round(displayedGuestPercent));
        auto setPercent = [](Weak<CCLabelBMFont> const& ref, int value) {
            if (auto item = ref.lock()) {
                auto display = fmt::format("{}%", value);
                if (display != item->getString()) {
                    item->setString(display.c_str());
                    item->limitLabelWidth(140.f, .5f, .2f);
                }
            }
        };
        setPercent(hostPercent, hostValue);
        setPercent(guestPercent, guestValue);
        if (auto bar = hostGauge.lock()) bar->setContentSize({145.f * displayedHostPercent / 100.f, 7.f});
        if (auto bar = guestGauge.lock()) bar->setContentSize({145.f * displayedGuestPercent / 100.f, 7.f});
        if (elapsed < RESULT_PROGRESS_TIME) return;
        if (!winnerRevealed) {
            winnerRevealed = true;
            if (auto curtain = transitionShade.lock())
                curtain->runAction(CCFadeTo::create(.21f * RESULT_REVEAL_TIME_SCALE, 65));
            if (auto banner = decisionBanner.lock())
                banner->runAction(CCFadeIn::create(.385f * RESULT_REVEAL_TIME_SCALE));
            if (!finalResult.draw)
                if (auto result = resultRoot.lock()) {
                    auto window = CCDirector::sharedDirector()->getWinSize();
                    float const winnerX = finalResult.winnerUid == hostProfile.uid ? -95.f : 95.f;
                    winnerEffect(result.data(), {window.width / 2.f + winnerX, window.height / 2.f - 10.f},
                                 finalResult.winnerUid == hostProfile.uid ? hostProfile : guestProfile);
                }
        }
        if (elapsed < RESULT_SUMMARY_TIME) {
            return;
        }
        if (elapsed < RESULT_CONFIRM_START) {
            if (!executionRevealed) {
                executionRevealed = true;
                if (!finalResult.draw) {
                    if (auto section = progressSection.lock())
                        section->runAction(CCSequence::create(CCFadeOut::create(.22f), CCHide::create(), nullptr));
                    if (auto section = actorSection.lock()) {
                        section->setVisible(true);
                        section->setOpacity(0);
                        section->runAction(
                            CCSequence::create(CCDelayTime::create(.22f), CCFadeIn::create(.35f), nullptr));
                    }
                }
            }
            return;
        }
        if (!confirmationVisible) {
            confirmationVisible = true;
            if (auto section = actorSection.lock())
                section->runAction(CCSequence::create(CCFadeOut::create(.28f), CCHide::create(), nullptr));
            if (auto section = progressSection.lock())
                section->runAction(CCSequence::create(CCFadeOut::create(.28f), CCHide::create(), nullptr));
            if (auto curtain = transitionShade.lock()) curtain->runAction(CCFadeTo::create(.35f, 120));
            if (auto popup = resultPopup.lock()) {
                popup->setVisible(true);
                popup->runAction(CCSequence::create(CCDelayTime::create(.35f), CCFadeIn::create(.7f), nullptr));
            }
        }
        auto seconds = std::max(0, static_cast<int>(std::ceil(RESULT_CONFIRM_START + RESULT_CONFIRM_TIME - elapsed)));
        label(returnStatus, fmt::format("Review result  |  Auto-return in {}s", seconds));
    }
    // 결과 화면에서 나간다. 서버가 아직 결과를 확정하지 않았으면 잠시(8초) 기다린 뒤 포기를 대기열에 올리고,
    // 확정되었으면 결과 확인(ack)을 서버에 보낸 뒤 맵을 나간다.
    void exitAfterResult() {
        if (!active || forcingQuit || returning) return;
        bool const unconfirmed = leaving && sameBattle() && Service::get().room()->battle->finishedAt <= 0 &&
                                 !(group && Service::get().room()->battle->host.forfeited);
        if (unconfirmed && resultElapsed() < 8.f) {
            report(true);
            returnRetry = 1.f;
            return;
        }
        if (unconfirmed) {
            Service::get().queueForfeit(local);
        }
        auto quit = [this] {
            replay::finishMatch();
            forcingQuit = true;
            audio::stopResultMusic();
            auto owner = play.lock();
            if (owner) owner->onQuit();
            active = false;
            Service::get().wantBattleReport(false);
        };
        if (!sameBattle() || unconfirmed) {
            quit();
            return;
        }
        returning = true;
        auto epoch = generation;
        Service::get().acknowledgeResult([this, epoch, quit](bool ok, std::string detail) {
            if (epoch != generation) return;
            returning = false;
            if (ok) {
                ackFailures = 0;
                quit();
                return;
            }
            if (++ackFailures >= kAckFailuresToLeave || Service::get().maintenance()) {
                quit();
                return;
            }
            returnRetry = 1.f;
            label(returnStatus, detail);
        });
    }
    // 노드가 실행 중인 장면에 속하는지.
    static bool inRunningScene(CCNode* node) {
        if (!node) return false;
        while (node->getParent()) node = node->getParent();
        return node == CCDirector::sharedDirector()->getRunningScene();
    }
    // 일시 정지 메뉴가 장면에 있는지.
    static bool pauseLayerPresent(PlayLayer* owner) {
        auto* parent = owner->getParent();
        if (!parent) return false;
        for (auto* child : CCArrayExt<CCNode*>(parent->getChildren()))
            if (typeinfo_cast<PauseLayer*>(child)) return true;
        return false;
    }
    // 플레이어가 죽은 상태인지.
    static bool playerDead(PlayLayer* layer) {
        return (layer->m_player1 && layer->m_player1->m_isDead) ||
               (layer->m_gameState.m_isDualMode && layer->m_player2 && layer->m_player2->m_isDead);
    }
    // 노드 트리의 스케줄러와 액션을 모두 재개한다.
    static void resumeTree(CCNode* node) {
        if (!node) return;
        node->resumeSchedulerAndActions();
        if (auto* children = node->getChildren())
            for (auto* child : CCArrayExt<CCNode*>(children)) resumeTree(child);
    }
    // 멈춘 로봇/스파이더 애니메이션을 다시 실행시킨다.
    static bool reviveAnimation(CCNode* sprite) {
        if (!sprite || !sprite->getParent() || !sprite->getParent()->isRunning()) return false;
        bool revived = false;
        if (!sprite->isRunning()) {
            sprite->onEnter();
            revived = true;
        }
        if (CCDirector::sharedDirector()->getScheduler()->isTargetPaused(sprite)) revived = true;
        resumeTree(sprite);
        return revived;
    }
    // 워치독: 결과를 30초 넘게 못 받거나, 일시 정지 메뉴 없이 게임이 멈추거나(2.5초),
    // 죽은 채 4초간 리스폰이 없거나, 로봇/스파이더 애니메이션이 멈췄으면 복구한다.
    void watchdog(PlayLayer* owner, float dt) {
        dt = std::max(0.f, dt);
        if (blocked()) {
            resultWaitClock += dt;
            if (resultWaitClock >= 30.f) {
                resultWaitClock = 0.f;
                if (local.forfeited) {
                    log::warn(
                        "[Versus watchdog] no match result 30 s after this seat's forfeit; returning to the room");
                    leaving = true;
                    resultBegan = Clock::now() - std::chrono::seconds(9);
                    label(status, "No result from the server yet. Returning to the room...");
                } else {
                    log::warn("[Versus watchdog] gameplay blocked 30 s without a result "
                              "(spectating={}, freePlay={}, integrity={})",
                              spectating, freePlay, !integrityReason.empty());
                    if (freePlayAllowed()) enterFreePlay();
                }
            }
        } else
            resultWaitClock = 0.f;
        if (!inRunningScene(owner) || owner->m_hasCompletedLevel) {
            deadClock = haltClock = 0.f;
            return;
        }
        auto* scheduler = CCDirector::sharedDirector()->getScheduler();
        bool const menu = pauseLayerPresent(owner);
        bool const halted = owner->m_isPaused || !owner->isRunning() || scheduler->isTargetPaused(owner);
        if (halted && !menu) {
            haltClock += dt;
            if (haltClock >= 2.5f) {
                haltClock = 0.f;
                log::warn("[Versus watchdog] gameplay halted without a pause menu "
                          "(paused={}, running={}, scheduler-paused={}); resuming",
                          owner->m_isPaused, owner->isRunning(), scheduler->isTargetPaused(owner));
                if (owner->m_isPaused) owner->resume();
                if (!owner->isRunning()) owner->onEnter();
                owner->resumeSchedulerAndActions();
            }
        } else
            haltClock = 0.f;
        bool const dead = playerDead(owner);
        if (dead && !menu && !halted && !replay::liveWatching(owner)) {
            deadClock += dt;
            if (deadClock >= 4.f) {
                deadClock = 0.f;
                bool const respawn = freePlay || (!blocked() && !done());
                log::warn("[Versus watchdog] player dead 4 s without a respawn "
                          "(inAttempt={}, done={}, blocked={}, freePlay={}, actions={}); {}",
                          local.inAttempt, done(), blocked(), freePlay,
                          CCDirector::sharedDirector()->getActionManager()->numberOfRunningActionsInTarget(owner),
                          respawn ? "respawning" : "waiting for the result");
                if (respawn) {
                    owner->stopAllActions();
                    owner->resetLevel();
                }
            }
        } else
            deadClock = 0.f;
        if (!dead && !menu && !halted) {
            animationClock += dt;
            if (animationClock >= .5f) {
                animationClock = 0.f;
                for (auto* player : {owner->m_player1, owner->m_gameState.m_isDualMode ? owner->m_player2 : nullptr}) {
                    if (!player || !player->isRunning()) continue;
                    bool revived = false;
                    if (player->m_isRobot && reviveAnimation(player->m_robotSprite)) revived = true;
                    if (player->m_isSpider && reviveAnimation(player->m_spiderSprite)) revived = true;
                    if (revived && !animationRevivedLogged) {
                        animationRevivedLogged = true;
                        log::warn("[Versus watchdog] a stalled robot/spider animation was restarted");
                    }
                }
            }
        } else
            animationClock = 0.f;
    }
    // TPS 측정: 스텝당 시간으로 초당 스텝 수를 세어 가장 많이 나온 값을 TPS로 보고한다.
    std::map<int, int> tpsCounts;
    int tpsSteps = 0;
    void noteStep(float stepDelta, float warp) {
        if (finished || leaving || freePlay || !(stepDelta > 0.f)) return;
        double const tps = std::clamp(std::min(1.f, warp), .05f, 1.f) / static_cast<double>(stepDelta);
        if (!(tps >= 1. && tps <= 100000.)) return;
        int const key = static_cast<int>(std::lround(tps));
        if (auto found = tpsCounts.find(key); found != tpsCounts.end())
            ++found->second;
        else if (tpsCounts.size() < 32)
            tpsCounts.emplace(key, 1);
        ++tpsSteps;
    }
    int measuredTps() const {
        if (tpsSteps < 120) return 0;
        int best = 0, count = 0;
        for (auto const& [tps, steps] : tpsCounts)
            if (steps > count) {
                best = tps;
                count = steps;
            }
        return best;
    }
    // 규칙의 배속(%)을 시간 배율로 바꾼다(일시 정지 중에는 1).
    float mapSpeed() const {
        if (!active || finished || leaving || rules.speed == 100 || !validRuleSpeed(rules.speed)) return 1.f;
        auto owner = play.lock();
        return owner && !owner->m_isPaused ? rules.speed / 100.f : 1.f;
    }
    // 매 프레임 진행. 방 상태가 바뀌었는지 확인하고 상대 상태를 받아 반영하며, 결과가 정해지면 결과 연출을 시작한다.
    // 그 외에는 free play, 관전, 커서, 일시 정지 감지, 워치독, 길게 누르기, 서버 보고, HUD 갱신을 한다.
    void update(float dt) override {
        setAudioSpeed(mapSpeed());
        if (!active || finished || leaving) replay::liveStop();
        if (!active) return;
        auto owner = play.lock();
        if (!owner) {
            audio::stopResultMusic();
            active = false;
            Service::get().wantBattleReport(false);
            return;
        }
        versus::percent::enforce(owner.data());
        returnRetry -= dt;
        if (finished || leaving) {
            hideLinkWarning();
            resetCursor();
            if (holdImage.lock() || holdNode.lock()) {
                endHoldVisual();
                escapeHeld = false;
                holdTime = holdShown = holdHint = 0.f;
            }
            auto elapsed = resultElapsed();
            float const exitTime = rerolledExit     ? .9f
                                   : !resultVisible ? 3.f
                                   : group          ? kGroupResultSeconds
                                                    : RESULT_CONFIRM_START + RESULT_CONFIRM_TIME;
            if (finished && group)
                updateGroupResult(resultRoot.lock().data(), exitTime - elapsed, kGroupResultSeconds);
            else if (finished && !rerolledExit)
                updateResultVisual(elapsed, dt);
            if (elapsed >= exitTime && returnRetry <= 0.f) exitAfterResult();
            return;
        }
        // 방의 배틀이 사라졌으면 결과 없이 끝난 것으로 보고 방으로 돌아간다.
        if (!sameBattle()) {
            if (!finished && !leaving) {
                endCountedPlay();
                replay::liveStop();
                finished = true;
                resultBegan = Clock::now();
                if (local.inAttempt) replay::endAttempt(percent(owner.data()), replay::End::Cut);
                replay::finishMatch();
                exitFreePlay();
                dismissPause(owner.data());
                FMODAudioEngine::sharedEngine()->pauseAllMusic(true);
                label(status, "Match ended. Returning to the room...");
            }
            return;
        }
        // 서버가 알려 준 상태에서 상대 상태와 감지 알림을 받고, 내 진행은 서버 확인값과 합친다.
        auto const snapshot = *Service::get().room()->battle;
        other = host ? snapshot.guest : snapshot.host;
        if (!other.integrityCode.empty() &&
            (shownIntegrityCode != other.integrityCode || shownIntegrityFeatures != other.integrityFeatures))
            showIntegrityAlert(other.integrityCode, other.integrityFeatures, true);
        reconcileProgress(local, host ? snapshot.host : snapshot.guest, localRules());
        if (done()) spectating = true;
        if (!group) {
            noteRerolls(snapshot);
            if (snapshot.finishedAt && snapshot.rerolled && !finished && !leaving) {
                rerollExit(owner.data());
                return;
            }
        }
        // 서버가 배틀 종료를 알리면 결과 연출을 시작한다(라이브 관전 중이면 최대 6초 보류).
        if (snapshot.finishedAt && !finished && !leaving) {
            bool const watchingClear = replay::liveWatching(owner.data()) && other.cleared && replay::livePlaying();
            if ((replay::liveHolding() || watchingClear) && resultHold < 6.f)
                resultHold += dt;
            else
                endVisual(snapshot);
        }
        if (group && snapshot.host.forfeited && !finished && !leaving) {
            leaving = true;
            resultBegan = Clock::now();
            exitFreePlay();
            dismissPause(owner.data());
            label(status, local.integrityCode.empty()
                              ? "You are out of this battle. Returning to the room..."
                              : "Cheat detected - you are out of this battle. Returning to the room...");
            return;
        }
        // 진행 중 처리: free play 전환, 실시간 스트림/관전, 일시 정지 감지와 시간 소진 확인, 워치독.
        auto now = Service::get().serverNow();
        if (!finished && !leaving) {
            if (freePlay != freePlayAllowed()) {
                if (freePlay) {
                    exitFreePlay();
                    restorePractice();
                } else
                    enterFreePlay();
            }
            replay::liveStream(!freePlay && !local.forfeited && Service::get().liveWatched()
                                   ? LiveTarget{roomID, battleID, group, {}}
                                   : LiveTarget{});
            updateWatch(owner.data(), dt);
            if (freePlay && freePlayRestart > 0.f && !owner->m_isPaused && !replay::liveWatching(owner.data())) {
                freePlayRestart -= dt;
                if (freePlayRestart <= 0.f) {
                    freePlayRestart = 0.f;
                    owner->resetLevelFromStart();
                }
            }
            updateFreePlayPause(owner.data());
            updateCursor(owner.data(), dt);
            trackLevelTime(owner.data());
            int const pauseMenu = pauseMenuState(owner.data());
            bool actuallyPaused = nativePauseActive(owner->m_isPaused, pauseMenu > 0);
            bool const measuring = owner->m_isPaused && pauseMenu < 0;
            if (!measuring && local.paused != actuallyPaused) pause(owner.data(), actuallyPaused);
            sampleState(owner.data());
            updatePauseClock(owner.data(), now);
            if (!local.forfeited && !freePlay && pauseRemaining(local, now) == 0) forfeit(false);
            if (pauseRemaining(other, now) == 0) dirty = true;
            if (!finished && !leaving) watchdog(owner.data(), dt);
            if (!finished && !leaving) updatePauseHold(owner.data(), dt);
        }
        // 5초마다 상태를 다시 보내 연결이 살아 있음을 알린다.
        keepAlive -= dt;
        if (keepAlive <= 0.f) {
            keepAlive = 5.f;
            dirty = true;
        }
        reportTimer -= dt;
        bool const due = !finished && !leaving && !reporting && dirty && reportTimer <= 0.f && sameBattle();
        Service::get().wantBattleReport(due);
        if (progressReportDue(reportTimer, reporting, dirty, Service::get().battleReportAvailable())) {
            report();
        }
        // 0.1초마다 HUD와 상태 문구를 갱신한다.
        uiTimer -= dt;
        if (uiTimer <= 0.f) {
            uiTimer = .1f;
            if (auto cards = groupHUD.lock()) {
                if (auto const& room = Service::get().group(); room && room->battle && room->battle->id == battleID)
                    cards->updateGroup(local, *room->battle, uid);
            }
            if (auto cards = playerHUD.lock()) {
                cards->setSeatRules(localRules(), otherRules());
                cards->updatePlayers(local, other);
                if (auto const& room = Service::get().room(); room && room->rules.allowHacks) {
                    auto text = [&room](std::string const& uid) {
                        auto it = room->hacks.find(uid);
                        return it == room->hacks.end() ? std::string("No hacks") : integrity::hackListText(it->second);
                    };
                    cards->setHacks(text(host ? hostProfile.uid : guestProfile.uid),
                                    text(host ? guestProfile.uid : hostProfile.uid));
                }
            }
            if (!finished && !leaving && group)
                label(status, !integrityReason.empty() ? "Cheat detected - forced defeat. Waiting for match sync..."
                              : reportFailures >= 3    ? "Retrying match sync... " + reportError
                              : timedOut               ? "Pause limit reached."
                              : local.forfeited        ? "Forfeited."
                              : local.paused           ? "Paused"
                              : local.cleared          ? "Cleared! Waiting for the other players..."
                              : spectating             ? "Out of lives - waiting for the other players..."
                              : syncBehind()           ? "Syncing with the server..."
                                                       : "");
            else if (!finished && !leaving)
                label(status, !integrityReason.empty() ? "Cheat detected - forced defeat. Waiting for match sync..."
                              : reportFailures >= 3    ? "Retrying match sync... " + reportError
                              : timedOut               ? "Pause limit reached. Waiting for result..."
                              : local.forfeited        ? "Match forfeited. Waiting for result..."
                              : local.paused           ? "Paused"
                              : other.paused           ? "Opponent paused"
                              : spectating && terminal(other, otherRules()) ? "Confirming match result..."
                              : syncBehind()                                ? "Syncing with the server..."
                                                                            : "");
            updateLinkWarning(snapshot);
            if (auto frame = freePlayFrame.lock()) showLiveState(frame.data(), owner.data(), freePlayCaption);
        }
    }
};
}
// Esc 키 반복 입력 처리: 일시 정지를 얻은 직후의 반복 입력은 막고, 키를 떼면 누름 상태를 해제한다.
$execute {
    KeyboardInputEvent(KEY_Escape)
        .listen([](KeyboardInputData& data) {
            if (data.action == KeyboardInputData::Action::Repeat)
                return escapeRepeatLatch ? ListenerResult::Stop : ListenerResult::Propagate;
            escapeRepeatLatch = false;
            if (data.action == KeyboardInputData::Action::Release)
                if (auto* s = Session::peek()) s->escapeHeld = false;
            return ListenerResult::Propagate;
        })
        .leak();
}
// 외부 인터페이스: 세션이 없거나 대상 레이어가 아니면 허용/무시 기본값을 돌려주는 얇은 래퍼들.
void begin(PlayLayer* layer) {
    Session::get().beginSession(layer);
}
void detach(PlayLayer* layer) {
    if (auto* s = Session::peek()) s->detachSession(layer);
}
bool activeFor(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer);
}
float speedFor(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) ? s->mapSpeed() : 1.f;
}
void noteStep(GJBaseGameLayer* layer, float stepDelta) {
    auto* s = Session::peek();
    if (s && s->owns(layer)) s->noteStep(stepDelta, layer->m_gameState.m_timeWarp);
}
bool spectatePlayer(std::string const& uid, std::string const& name) {
    auto* s = Session::peek();
    return s && s->active && s->startWatching(uid, name);
}
std::string spectatedUid() {
    auto* s = Session::peek();
    return s && s->active ? s->watchUid : std::string();
}
bool canSpectate() {
    auto* s = Session::peek();
    if (!s || !s->active) return false;
    auto owner = s->play.lock();
    return owner && s->freePlay && !s->finished && !s->leaving && !s->local.forfeited;
}
bool groupSpectatorPause(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->group && !s->finished && !s->leaving && resultLocked(s->local, s->rules);
}
void resetAudioSpeed() {
    setAudioSpeed(1.f);
}
void disqualify(PlayLayer* layer, std::string const& reason, std::string const& integrityCode,
                std::string const& integrityFeatures) {
    if (auto* s = Session::peek()) s->disqualify(layer, reason, integrityCode, integrityFeatures);
}
void flushIntegrityReport(PlayLayer* layer) {
    auto* s = Session::peek();
    if (s && s->owns(layer) && s->reportTimer <= 0.f && s->dirty) s->report();
}
bool blocksGameplay(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->blocked();
}
bool blocksInput(GJBaseGameLayer* layer) {
    return blocksGameplay(layer);
}
bool blocksPause(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    if (!s || !s->owns(layer)) return false;
    auto owner = s->play.lock();
    return !owner || !s->canPause(owner.data());
}
void sample(PlayLayer* layer) {
    if (auto* s = Session::peek()) s->sampleState(layer);
}
void simulated(GJBaseGameLayer* layer, float dt) {
    if (auto* s = Session::peek()) s->simulate(layer, dt);
}
void died(PlayLayer* layer) {
    if (auto* s = Session::peek()) s->death(layer);
}
void completed(PlayLayer* layer) {
    if (auto* s = Session::peek()) s->clear(layer);
}
bool beforeReset(PlayLayer* layer) {
    auto* s = Session::peek();
    return !s || s->resetBefore(layer);
}
void afterReset(PlayLayer* layer) {
    if (auto* s = Session::peek()) s->resetAfter(layer);
}
void paused(PlayLayer* layer, bool value) {
    if (auto* s = Session::peek()) s->pause(layer, value);
}
bool escapeKey(GJBaseGameLayer* layer, bool down) {
    auto* s = Session::peek();
    if (!s || !s->owns(layer)) return false;
    auto owner = s->play.lock();
    return owner && s->escapeKey(owner.data(), down);
}
bool backKey(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    if (!s || !s->owns(layer)) return false;
    auto owner = s->play.lock();
    return owner && s->backKey(owner.data());
}
bool pauseHoldGranted(GJBaseGameLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->holdGranted;
}
bool pauseMenuOpen(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && layer->m_isPaused && s->pauseLayerPresent(layer);
}
bool allowPracticeToggle(PlayLayer* layer, bool practice) {
    auto* s = Session::peek();
    return !s || !s->owns(layer) || s->releasing || practice == s->rules.practice || s->practiceSwitchable();
}
bool practiceSwitchable(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->practiceSwitchable();
}
bool requestQuit(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->askQuit(layer);
}
bool canVoteReroll(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->rerollAvailable();
}
bool rerollVoted(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->ownRerollVoted();
}
bool opponentRerollVoted(PlayLayer* layer) {
    auto* s = Session::peek();
    return s && s->owns(layer) && s->opponentRerollVoted();
}
void voteReroll(PlayLayer* layer) {
    if (auto* s = Session::peek(); s && s->owns(layer)) s->voteReroll();
}
}
