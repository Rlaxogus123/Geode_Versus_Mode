// 매치 시작 절차와 배틀 중 GD 훅 모음. LaunchController가 맵 다운로드 -> 노래/효과음 준비 ->
// PlayLayer 진입 -> 양쪽 로딩 동기화 -> 카운트다운 후 시작 순서를 진행하고, 끝나면 원래 화면으로 돌려보낸다.
// 또한 배틀 중 GD 통계/레벨 기록이 바뀌지 않게 막는 훅, 시작 전 입력 차단, 일시 정지/리롤 UI,
// 치트 모드 감지용 훅 순서 지정을 담고 있다.
#include "MatchLaunch.hpp"
#include "GroupFreePlay.hpp"
#include "Season2Lounge.hpp"
#include "KeyDefer.hpp"
#include "MapCache.hpp"
#include "BattleSession.hpp"
#include "PercentGuard.hpp"
#include "ExternalCheatMonitor.hpp"
#include "IntegrityMonitor.hpp"
#include "VersusService.hpp"
#include "VersusUI.hpp"
#include "BattleLevelData.hpp"
#include "MenuCursor.hpp"
#include "MenuCursorScope.hpp"
#include "Replay.hpp"
#include "WeakRefs.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/MusicDownloadDelegate.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJRobotSprite.hpp>
#include <Geode/binding/GJSpiderSprite.hpp>
#include <Geode/binding/SpriteAnimationManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/CurrencyRewardLayer.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/UILayer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
// 단조 시계.
using Clock = std::chrono::steady_clock;
// 배틀 레벨의 유저 코인을 읽을 때 GD의 기존 획득 기록을 무시하게 하는 플래그.
bool s_readBattleCoins = false;
// GD의 원래 levelComplete를 실행 중인지.
bool s_nativeBattleComplete = false;
// 한 프레임에 시뮬레이션할 최대 시간(초).
constexpr float kMaxBattleFrameDelta = .25f;
// 로딩 단계: 대기 없음, 맵 다운로드, 리소스 준비, PlayLayer 생성, 상대 대기, 플레이 중, 복귀.
enum class Phase { Idle, Download, Resources, Entering, Waiting, Playing, Returning };

// 레벨의 GD 통계 스냅샷. 배틀이 끝나면 restore로 되돌려 실제 기록이 바뀌지 않게 한다.
struct LevelStats {
    geode::SeedValueRSV attempts, jumps, clicks, attemptTime;
    int chk;
    bool chkValid, legitimate;
    geode::SeedValueVSR normalPercent;
    geode::SeedValueRSV orbCompletion, newNormalPercent2;
    int practicePercent, bestTime, clicksTime, bestPoints, clicksPoints;
    gd::string personalBests;
    explicit LevelStats(GJGameLevel* level)
        : attempts(level->m_attempts), jumps(level->m_jumps), clicks(level->m_clicks),
          attemptTime(level->m_attemptTime), chk(level->m_chk), chkValid(level->m_isChkValid),
          legitimate(level->m_isCompletionLegitimate), normalPercent(level->m_normalPercent),
          orbCompletion(level->m_orbCompletion), newNormalPercent2(level->m_newNormalPercent2),
          practicePercent(level->m_practicePercent), bestTime(level->m_bestTime), clicksTime(level->m_clicksTime),
          bestPoints(level->m_bestPoints), clicksPoints(level->m_clicksPoints), personalBests(level->m_personalBests) {}
    void restore(GJGameLevel* level) const {
        level->m_attempts = attempts;
        level->m_jumps = jumps;
        level->m_clicks = clicks;
        level->m_attemptTime = attemptTime;
        level->m_chk = chk;
        level->m_isChkValid = chkValid;
        level->m_isCompletionLegitimate = legitimate;
        level->m_normalPercent = normalPercent;
        level->m_orbCompletion = orbCompletion;
        level->m_newNormalPercent2 = newNormalPercent2;
        level->m_practicePercent = practicePercent;
        level->m_bestTime = bestTime;
        level->m_clicksTime = clicksTime;
        level->m_bestPoints = bestPoints;
        level->m_clicksPoints = clicksPoints;
        level->m_personalBests = personalBests;
    }
};
// 스냅샷을 가진 레벨 목록.
std::vector<std::pair<Ref<GJGameLevel>, LevelStats>> s_savedStats;
// 캐시된 레벨과 GD에 저장된 레벨의 통계를 저장한다.
void snapshotLevelStats(int levelID) {
    s_savedStats.clear();
    auto keep = [](GJGameLevel* level) {
        if (!level) return;
        for (auto const& entry : s_savedStats)
            if (entry.first.data() == level) return;
        s_savedStats.emplace_back(Ref<GJGameLevel>(level), LevelStats(level));
    };
    keep(versus::cachedLevel(levelID));
    if (auto* manager = GameLevelManager::sharedState()) keep(manager->getSavedLevel(levelID));
}
// 저장해 둔 통계를 레벨에 복원한다.
void restoreLevelStats() {
    for (auto const& [level, stats] : s_savedStats)
        if (level) stats.restore(level.data());
}

// 배틀 플레이 진행을 실제 맵에 저장할지 여부(꺼져 있음).
constexpr bool kSaveMapProgress = false;
// 배틀 중 호출된 GD 통계 변경 한 건의 기록.
struct ProgressCall {
    enum class Kind {
        Stat,
        Challenge,
        Completed,
        StarLevel,
        DemonLevel,
        Currency,
        Diamonds,
        Claimed,
        UserCoin,
        CoinAchievement,
        Percentage,
        Achievement
    };
    Kind kind;
    std::string key;
    int value = 0;
    int extra = 0;
    bool flag = false;
};
// 진행 기록 장부. 배틀 복사본의 통계와 호출 기록을 모아 두었다가 확정되면 커밋하고 아니면 버린다.
struct ProgressLedger {
    bool armed = false, began = false, sealed = false, discarded = false;
    int levelID = 0;
    Ref<GJGameLevel> copy;
    std::optional<LevelStats> sealedStats;
    std::array<int, 3> coinFlags{0, 0, 0};
    std::vector<ProgressCall> calls;
    std::vector<std::string> pendingCoins;
    std::vector<std::string> claimedPending;
};
// 기록할 호출 수의 상한.
constexpr size_t kProgressCallLimit = 20000;
ProgressLedger s_progress;
// 진행 기록 처리 함수 선언.
void armProgress(GJGameLevel* copy, int levelID, std::string const& roomID);
bool progressRecording();
void recordProgress(ProgressCall call);
void sealProgress(PlayLayer* layer, char const* how);
void restorePendingCoins(std::vector<std::string> const& keys);
void discardProgress(std::string const& why, bool onlyOpen = false);
void commitProgress();

// 별/오브 보상 및 실제 맵 사용 관련 함수 선언.
bool rankedRewardsWanted();
std::string realMapRefusal(GJGameLevel* level, std::string const& roomID, std::uint64_t& hash);
std::array<int, 6> rewardStats();
void showBattleRewards(GJGameLevel* level, std::array<int, 6> const& before, int bestBefore);

// 배틀 중 훅을 일시 정지해야 하는 모드 ID 목록(진행 기록/퍼센트를 바꾸는 모드).
constexpr std::array<char const*, 3> kBattleIncompatibleMods = {
    "kevadroz.practicecheckpointpermanence",
    "billgd.death_counter_popup",
    versus::percent::kPercentageToggle,
};
// pauseBattleMods(true)처럼 임베디드로 요청할 때만 추가로 정지하는 모드: Versus 화면 안 PlayLayer(시즌 2 라운지,
// 리플레이, 자유 플레이)에서 PlayLayer::onEnter에 람다를 큐에 넣고 그 안에서 보통 플레이 씬에만 있는 것을 읽어
// 튕기는 모드들.
// - fracturefx.quickprofile: 11-02 01:25 크래시 로그(컵 들어갈 때/돌아올 때).
// - mishpro.comments_in_pause_menu: 11-09 크래시 로그(랭크 로비 입장, FixedPlayLayer::onEnterH 람다가 0x0 읽음).
constexpr std::array<char const*, 2> kEmbeddedIncompatibleMods = {
    "fracturefx.quickprofile",
    "mishpro.comments_in_pause_menu",
};
// 정지한 훅 목록과 그 훅이 속한 PlayLayer.
std::vector<Hook*> s_pausedHooks;
Weak<PlayLayer> s_pausedFor;
bool s_modsPaused = false;
bool s_embeddedPaused = false;
// 모드의 켜진 훅을 모두 끄고 기록한다.
void pauseHooksOf(char const* id) {
    auto* mod = Loader::get()->getLoadedMod(id);
    if (!mod) return;
    size_t paused = 0;
    for (auto* hook : mod->getHooks()) {
        if (!hook || !hook->isEnabled()) continue;
        if (auto result = hook->disable(); result.isOk()) {
            s_pausedHooks.push_back(hook);
            ++paused;
        } else
            log::warn("Versus: could not pause a hook of {}: {}", id, result.unwrapErr());
    }
    log::info("Versus: paused {} hook(s) of {} for the battle", paused, id);
}
// 충돌 모드의 훅을 정지한다(한 번만).
void pauseIncompatibleMods(bool embedded = false) {
    if (!s_modsPaused) {
        s_modsPaused = true;
        for (auto const* id : kBattleIncompatibleMods) pauseHooksOf(id);
    }
    if (embedded && !s_embeddedPaused) {
        s_embeddedPaused = true;
        for (auto const* id : kEmbeddedIncompatibleMods) pauseHooksOf(id);
    }
}
// 정지했던 훅을 모두 다시 켠다.
void resumeIncompatibleMods() {
    if (!s_modsPaused) return;
    s_modsPaused = false;
    s_embeddedPaused = false;
    for (auto* hook : s_pausedHooks) {
        if (!hook || hook->isEnabled()) continue;
        if (auto result = hook->enable(); result.isErr())
            log::warn("Versus: could not restore a paused hook: {}", result.unwrapErr());
    }
    if (!s_pausedHooks.empty()) log::info("Versus: restored {} paused hook(s)", s_pausedHooks.size());
    s_pausedHooks.clear();
    s_pausedFor = nullptr;
}

// 마지막으로 만든 배틀 레벨에서 Start Pos를 제거했는지.
bool s_lastBattleStripped = false;
// 배틀용 레벨을 만든다. 레벨 데이터를 풀어 Start Pos를 제거하고(원래 압축 여부 유지)
// 해시를 계산하며, 저장되지 않도록(m_dontSave) 하고 코인 검증 플래그를 초기화한다.
GJGameLevel* makeBattleLevel(GJGameLevel* source, bool lowDetail, std::uint64_t& hash) {
    auto const original = source->m_levelString;
    bool const compressed = std::string_view(original.c_str()).find(';') == std::string_view::npos;
    auto decoded = compressed ? ZipUtils::decompressString(original, false, 11) : original;
    if (decoded.empty()) return nullptr;
    auto* battleLevel = GJGameLevel::create();
    battleLevel->copyLevelInfo(source);
    battleLevel->m_levelID = source->m_levelID.value();
    battleLevel->m_levelType = source->m_levelType;
    battleLevel->m_audioTrack = source->m_audioTrack;
    battleLevel->m_songID = source->m_songID;
    battleLevel->m_songIDs = source->m_songIDs;
    battleLevel->m_sfxIDs = source->m_sfxIDs;
    battleLevel->m_lowDetailModeToggled = lowDetail;
    battleLevel->m_disableShakeToggled = source->m_disableShakeToggled;
    battleLevel->m_dontSave = true;
    battleLevel->m_firstCoinVerified = 0;
    battleLevel->m_secondCoinVerified = 0;
    battleLevel->m_thirdCoinVerified = 0;
    std::string const decodedText(decoded);
    std::string stripped = versus::withoutStartPositions(decodedText);
    s_lastBattleStripped = stripped != decodedText;
    hash = versus::replay::hashLevel(stripped);
    if (!s_lastBattleStripped)
        battleLevel->m_levelString = original;
    else
        battleLevel->m_levelString =
            compressed ? ZipUtils::compressString(gd::string(stripped), false, 11) : gd::string(stripped);
    versus::percent::prepareLevel(battleLevel);
    return battleLevel;
}

// 로봇/스파이더 모드로 시작하는 맵에서 스프라이트 애니메이션이 깨지지 않도록 모드를 껐다 켜서 다시 만든다.
void refreshStartModeSprites(PlayLayer* layer) {
    if (!layer) return;
    auto* manager = GameManager::sharedState();
    for (auto* player : {layer->m_player1, layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr}) {
        if (!player || (!player->m_isRobot && !player->m_isSpider)) continue;
        bool const robot = player->m_isRobot;
        CCAnimatedSprite* sprite =
            robot ? static_cast<CCAnimatedSprite*>(player->m_robotSprite) : player->m_spiderSprite;
        auto* animation = sprite ? sprite->m_animationManager : nullptr;
        log::info("Versus: the map starts as a {} (sprite {}, running {}, animation '{}'); rebuilding it",
                  robot ? "robot" : "spider", sprite != nullptr, sprite && sprite->isRunning(),
                  animation ? std::string(animation->m_currentAnimation) : std::string("-"));
        if (robot) {
            if (manager) player->updatePlayerRobotFrame(manager->getPlayerRobot());
            player->toggleRobotMode(false, true);
            player->toggleRobotMode(true, true);
        } else {
            if (manager) player->updatePlayerSpiderFrame(manager->getPlayerSpider());
            player->toggleSpiderMode(false, true);
            player->toggleSpiderMode(true, true);
        }
    }
}

// 노드가 현재 실행 중인 장면에 속하는지.
bool currentSceneContains(CCNode* node) {
    if (!node) return false;
    while (node->getParent()) node = node->getParent();
    return node == CCDirector::sharedDirector()->getRunningScene();
}

// "1,2,3" 형태의 ID 목록에서 양수 ID를 모은다.
void collectIDs(std::set<int>& result, gd::string const& encoded) {
    std::string text(encoded);
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        int value = 0;
        auto parsed = std::from_chars(text.data() + start, text.data() + end, value);
        if (parsed.ec == std::errc() && parsed.ptr == text.data() + end && value > 0) result.insert(value);
        start = end + 1;
    }
}

// 매치 로딩을 총괄하는 싱글턴 노드. 매 프레임 update에서 단계를 진행한다.
class LaunchController final : public CCNode, public LevelDownloadDelegate, public MusicDownloadDelegate {
public:
    // 현재 단계, launch/방/맵 ID, 처리한 launch 기록.
    Phase phase = Phase::Idle;
    std::string launchID;
    std::string roomID;
    std::string handledID;
    std::string handledRoom;
    int levelID = 0;
    int64_t requestedAt = 0;
    uint64_t generation = 0;
    // constructing은 PlayLayer 생성 중, nativeReady는 GD의 setup이 끝났는지.
    bool constructing = false;
    bool nativeReady = false;
    bool redirectQuit = false;
    // 레벨 다운로드/오디오/취소 요청의 진행 상태 플래그들.
    bool downloadPending = false;
    uint64_t downloadGeneration = 0;
    int downloadID = 0;
    bool arrivalPending = false;
    bool arrivalAcknowledged = false;
    bool cancelPending = false;
    bool musicWatching = false;
    // realMap은 배틀용 복사본 대신 실제 맵을 쓰는 중인지(보상 기능, 현재 비활성).
    bool realMap = false;
    bool realLdm = false;
    Ref<GJGameLevel> realLevel;
    std::string rewardsNote;
    std::array<int, 6> realStatsBefore{};
    int realBestBefore = 0;
    bool contentURLRequested = false;
    unsigned int cancelAttempts = 0;
    unsigned int entryFrames = 0;
    // 출발 카운트다운(ms)과 릴리스가 늦었을 때 보장하는 최소 카운트다운.
    static constexpr int64_t kStartCountdownMs = 3000, kMinStartCountdownMs = 2000;
    int64_t startDeadline = 0;
    // 다운로드 기록이 서버에 올라가기를 기다리는 시간.
    static constexpr auto kRecordWaitMs = std::chrono::milliseconds(3000);
    std::optional<Clock::time_point> recordAskedAt;
    std::string cancelID;
    std::string cancelRoom;
    std::string notice;
    float timer = 0.f;
    float arrivalTimer = 0.f;
    float cancelTimer = 0.f;
    float quitRetry = 0.f;
    Clock::time_point began;
    Ref<GJGameLevel> level;
    Weak<PlayLayer> play;
    Weak<CCLabelBMFont> label;
    Weak<CCLabelBMFont> waitingNames;
    Weak<CCScale9Sprite> waitingPanel;
    Weak<CCScene> staging;
    std::set<int> songs;
    std::set<int> effects;
    std::set<int> requestedSongs;
    std::set<int> requestedEffects;

    // 싱글턴 접근자. 스케줄러에 등록해 매 프레임 update를 받는다.
    inline static LaunchController* instance = nullptr;
    static LaunchController* peek() { return instance; }
    static bool running() { return instance && instance->active(); }
    static LaunchController& get() {
        static auto* controller = [] {
            auto* result = new LaunchController;
            result->init();
            instance = result;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(result, 1, false);
            result->release();
            return result;
        }();
        return *controller;
    }

    // 로딩 절차가 진행 중인지.
    bool active() const { return phase != Phase::Idle; }
    // 이 레이어가 로딩 대기 중(플레이 전)인 배틀 레이어인지. 이 동안 게임 진행과 입력을 막는다.
    bool gated(GJBaseGameLayer* candidate) const {
        if (!active() || phase == Phase::Playing || !candidate) return false;
        auto owner = play.lock();
        return owner && static_cast<GJBaseGameLayer*>(owner.data()) == candidate;
    }
    // 이 PlayLayer가 현재 매치의 레이어인지.
    bool belongs(PlayLayer* candidate) const {
        if (!active() || !candidate) return false;
        auto owner = play.lock();
        return owner && owner.data() == candidate;
    }
    // 방의 launch가 아직 이 컨트롤러가 처리 중인 것과 같은지.
    bool currentLaunch() const {
        auto const& room = versus::Service::get().room();
        return room && room->id == roomID && room->started && room->launch && room->launch->id == launchID &&
               room->level.id == levelID;
    }
    // 상태 문구를 갱신한다.
    void text(std::string const& message) {
        if (auto target = label.lock(); target && message != target->getString()) target->setString(message.c_str());
    }
    static constexpr size_t kWaitingNamesShown = 10;
    // 아직 로딩 중인 플레이어 이름 목록을 상자에 표시한다(줄바꿈, 최대 10명).
    void showWaiting(std::vector<std::string> const& names) {
        auto target = waitingNames.lock();
        auto box = waitingPanel.lock();
        if (!target || !box) return;
        std::string message;
        if (!names.empty()) {
            message =
                names.size() == 1 ? std::string("Waiting for:") : fmt::format("Waiting for {} players:", names.size());
            std::string line;
            for (size_t i = 0; i < names.size(); ++i) {
                auto const piece = names[i] + (i + 1 < names.size() ? "," : "");
                if (!line.empty() && line.size() + 1 + piece.size() > 44) {
                    message += "\n" + line;
                    line.clear();
                }
                line += (line.empty() ? "" : " ") + piece;
            }
            if (!line.empty()) message += "\n" + line;
        }
        if (message == target->getString()) return;
        target->setString(message.c_str());
        target->setVisible(!message.empty());
        box->setVisible(!message.empty());
        if (message.empty()) return;
        auto const window = CCDirector::sharedDirector()->getWinSize();
        float const height = target->getContentSize().height * target->getScale() + 10.f;
        float const top = window.height / 2.f - 51.f;
        box->setContentSize({340.f, height});
        box->setPosition({window.width / 2.f, top - height / 2.f});
        target->setPosition({window.width / 2.f, top - height / 2.f});
    }
    // 서버에 launch 취소를 보내는 재시도를 시작한다.
    void startCancel() {
        cancelID = launchID;
        cancelRoom = roomID;
        cancelAttempts = 0;
        cancelTimer = 0.f;
    }
    // 취소 요청을 최대 5번까지 재시도한다. Ranked면 cancelRanked, 아니면 cancelLaunch를 쓴다.
    void retryCancel(float dt) {
        if (cancelID.empty() || cancelPending || cancelAttempts >= 5) return;
        auto& service = versus::Service::get();
        auto const& room = service.room();
        if (!room || room->id != cancelRoom || !room->launch || room->launch->id != cancelID) {
            cancelID.clear();
            return;
        }
        cancelTimer -= dt;
        if (cancelTimer > 0.f || service.busy()) return;
        cancelPending = true;
        ++cancelAttempts;
        auto id = cancelID;
        if (service.isRankedMatch()) {
            service.cancelRanked([this, id](std::optional<versus::RankedState> state, std::string) {
                cancelPending = false;
                if (cancelID != id) return;
                if (state)
                    cancelID.clear();
                else
                    cancelTimer = 1.f;
            });
            return;
        }
        service.cancelLaunch(id, [this, id](bool success, std::string) {
            cancelPending = false;
            if (cancelID != id) return;
            if (success)
                cancelID.clear();
            else
                cancelTimer = 1.f;
        });
    }
    // 로딩 실패/취소 처리: 안내 문구를 저장하고 복귀 단계로 넘긴다.
    void fail(std::string message, bool cancel = true) {
        if (!active() || phase == Phase::Returning) return;
        notice = std::move(message);
        if (cancel) startCancel();
        phase = Phase::Returning;
        quitRetry = 0.f;
    }
    bool cancelDialog = false;
    // 로딩 화면의 Cancel 버튼. Ranked는 취소하면 패배 처리이므로 확인 창을 먼저 띄운다.
    void cancelButton(CCObject*) {
        if (cancelDialog || !active() || phase == Phase::Returning) return;
        if (!versus::Service::get().isRankedMatch()) {
            fail("Match loading was canceled.");
            return;
        }
        cancelDialog = true;
        auto epoch = generation;
        createQuickPopup("Forfeit Ranked?", "Canceling synchronization counts as a <cr>loss</c>.", "Stay", "Forfeit",
                         [this, epoch](auto*, bool yes) {
                             cancelDialog = false;
                             if (yes && epoch == generation && phase != Phase::Playing) fail("Ranked match forfeited.");
                         });
    }

    // 로딩 오버레이(상태 문구, 대기자 이름 상자, 보상 안내, Cancel 버튼)를 만든다.
    void installLabel(CCNode* parent, bool cancelButtonVisible) {
        auto window = CCDirector::sharedDirector()->getWinSize();
        auto* panel = CCScale9Sprite::create("square02b_001.png");
        panel->setContentSize({340.f, 94.f});
        panel->setColor(ccc3(11, 48, 88));
        panel->setOpacity(235);
        panel->setPosition(window / 2.f);
        auto* root = CCNode::create();
        root->setID("versus-launch-wait"_spr);
        root->addChild(panel);
        versus::keepMenuCursorVisible(root);
        auto* title = CCLabelBMFont::create("VERSUS", "bigFont.fnt");
        title->setScale(.55f);
        title->setPosition({window.width / 2.f, window.height / 2.f + 25.f});
        root->addChild(title);
        auto* status = CCLabelBMFont::create("Loading selected map...", "chatFont.fnt");
        status->setScale(.85f);
        status->setAlignment(kCCTextAlignmentCenter);
        status->setPosition({window.width / 2.f, window.height / 2.f - 2.f});
        root->addChild(status);
        versus::rebind(label, status);
        auto* namesPanel = CCScale9Sprite::create("square02b_001.png");
        namesPanel->setContentSize({340.f, 20.f});
        namesPanel->setColor(ccc3(8, 36, 68));
        namesPanel->setOpacity(225);
        namesPanel->setVisible(false);
        root->addChild(namesPanel);
        versus::rebind(waitingPanel, namesPanel);
        auto* names = CCLabelBMFont::create("", "chatFont.fnt");
        names->setScale(.62f);
        names->setAlignment(kCCTextAlignmentCenter);
        names->setColor(ccc3(255, 214, 140));
        names->setVisible(false);
        root->addChild(names);
        versus::rebind(waitingNames, names);
        if (!rewardsNote.empty()) {
            auto* note = CCLabelBMFont::create(rewardsNote.c_str(), "chatFont.fnt");
            note->setScale(.62f);
            note->setColor(realMap ? ccc3(255, 222, 90) : ccc3(170, 190, 215));
            note->setPosition({window.width / 2.f, window.height / 2.f - 58.f});
            note->limitLabelWidth(420.f, .62f, .3f);
            root->addChild(note);
        }
        if (cancelButtonVisible) {
            auto* menu = CCMenu::create();
            menu->setPosition({window.width / 2.f, window.height / 2.f - 29.f});
            auto* caption = CCLabelBMFont::create("Cancel", "chatFont.fnt");
            caption->setScale(.75f);
            caption->setColor(ccc3(145, 210, 255));
            menu->addChild(CCMenuItemSpriteExtra::create(caption, this, menu_selector(LaunchController::cancelButton)));
            root->addChild(menu);
        }
        parent->addChild(root, 100000);
    }

    // 매치 로딩 시작. 상태를 초기화하고 로딩 장면으로 바꾸며 GD 음악/효과음을 멈춘다.
    void begin() {
        auto const& room = versus::Service::get().room();
        if (!room || !room->launch || !room->started || room->level.id <= 0 ||
            room->level.id > std::numeric_limits<int>::max()) {
            if (versus::Service::get().isRankedMatch())
                versus::showRanked();
            else
                versus::showRoom();
            return;
        }
        if (active()) return;
        ++generation;
        s_savedStats.clear();
        realMap = false;
        realLevel = nullptr;
        rewardsNote.clear();
        launchID = room->launch->id;
        roomID = room->id;
        handledID = launchID;
        handledRoom = roomID;
        levelID = static_cast<int>(room->level.id);
        requestedAt = room->launch->requestedAt;
        began = Clock::now();
        phase = Phase::Download;
        quitRetry = 0.f;
        arrivalPending = false;
        arrivalAcknowledged = false;
        arrivalTimer = 0.f;
        entryFrames = 0;
        startDeadline = 0;
        recordAskedAt.reset();
        nativeReady = false;
        downloadPending = false;
        timer = 0.f;
        notice.clear();
        contentURLRequested = false;
        level = nullptr;
        play = nullptr;
        songs.clear();
        effects.clear();
        requestedSongs.clear();
        requestedEffects.clear();
        auto* scene = CCScene::create();
        auto* bg = CCLayerColor::create(ccc4(5, 25, 62, 255));
        scene->addChild(bg);
        installLabel(scene, true);
        versus::rebind(staging, scene);
        CCDirector::sharedDirector()->replaceScene(scene);
        FMODAudioEngine::sharedEngine()->stopAllMusic(true);
        FMODAudioEngine::sharedEngine()->stopAllEffects();
    }

    // Ranked 매치 로딩인지.
    bool rankedLaunch() const { return versus::Service::get().isRankedMatch() || roomID.starts_with("ranked-"); }
    // 맵 준비(MapCache) 요청. 완료되면 리소스 단계로 넘어가고 실패하면 로딩을 취소한다.
    void requestDownload() {
        if (downloadPending) return;
        downloadPending = true;
        auto epoch = generation;
        bool const ranked = rankedLaunch();
        versus::ensureMapCached(
            levelID,
            [this, epoch](bool success, std::string detail) {
                if (epoch != generation || phase != Phase::Download) return;
                downloadPending = false;
                if (!success) {
                    fail(std::move(detail));
                    return;
                }
                level = versus::cachedLevel(levelID);
                phase = Phase::Resources;
                timer = 0.f;
            },
            ranked ? versus::rankedAudioNeed() : versus::AudioNeed::All);
    }
    // 레벨 다운로드 델리게이트를 해제한다.
    void finishDownload() {
        downloadPending = false;
        auto* manager = GameLevelManager::sharedState();
        if (manager && manager->m_levelDownloadDelegate == this) manager->m_levelDownloadDelegate = nullptr;
    }
    // 직접 레벨을 받았을 때의 완료 처리: 노래/효과음 ID를 모아 리소스 단계로 넘어간다.
    void levelDownloadFinished(GJGameLevel* downloaded) override {
        if (!downloadPending || !downloaded || downloaded->m_levelID.value() != downloadID) return;
        auto valid = downloadGeneration == generation && phase == Phase::Download;
        finishDownload();
        if (!valid) return;
        if (downloaded->m_levelString.empty()) {
            fail("The selected map contains no level data.");
            return;
        }
        level = downloaded;
        if (downloaded->m_songID > 0) songs.insert(downloaded->m_songID);
        collectIDs(songs, downloaded->m_songIDs);
        collectIDs(effects, downloaded->m_sfxIDs);
        phase = Phase::Resources;
        auto* music = MusicDownloadManager::sharedState();
        music->tryLoadLibraries();
        music->addMusicDownloadDelegate(this);
        musicWatching = true;
        timer = 0.f;
    }
    void levelDownloadFailed(int) override {
        if (!downloadPending) return;
        auto valid = downloadGeneration == generation && phase == Phase::Download;
        finishDownload();
        if (valid) fail("The selected map could not be downloaded.");
    }

    // 필요한 노래와 효과음을 동시에 최대 3개씩 내려받는다. 모두 있으면 true.
    bool resourcesReady() {
        if (versus::mapAudioSkipped(levelID)) return true;
        auto* music = MusicDownloadManager::sharedState();
        bool needsContentURL =
            std::any_of(effects.begin(), effects.end(), [music](int id) { return !music->isSFXDownloaded(id); }) ||
            std::any_of(songs.begin(), songs.end(),
                        [music](int id) { return id > 10000000 && !music->isSongDownloaded(id); });
        if (needsContentURL && music->m_customContentURL.empty()) {
            if (!contentURLRequested) {
                contentURLRequested = true;
                music->getCustomContentURL();
            }
            return false;
        }
        bool ready = true;
        int outstanding = 0;
        for (int id : requestedSongs)
            if (!music->isSongDownloaded(id)) ++outstanding;
        for (int id : requestedEffects)
            if (!music->isSFXDownloaded(id)) ++outstanding;
        for (int id : songs) {
            if (music->isSongDownloaded(id)) continue;
            ready = false;
            if (requestedSongs.contains(id) || outstanding >= 3 || music->isRunningActionForSongID(id)) continue;
            requestedSongs.insert(id);
            ++outstanding;
            music->downloadSong(id);
        }
        for (int id : effects) {
            if (music->isSFXDownloaded(id)) continue;
            ready = false;
            if (requestedEffects.contains(id) || outstanding >= 3 || music->isDLActive(music->getSFXDownloadKey(id)))
                continue;
            requestedEffects.insert(id);
            ++outstanding;
            music->downloadSFX(id);
        }
        return ready;
    }
    // 음악 다운로드 델리게이트를 해제한다.
    void stopWatchingMusic() {
        if (!musicWatching) return;
        MusicDownloadManager::sharedState()->removeMusicDownloadDelegate(this);
        musicWatching = false;
    }
    void downloadSongFailed(int id, GJSongError) override {
        if (phase == Phase::Resources && requestedSongs.contains(id))
            fail("The selected map's music could not be downloaded.");
    }
    void downloadSFXFailed(int id, GJSongError) override {
        if (phase == Phase::Resources && requestedEffects.contains(id))
            fail("The selected map's sounds could not be downloaded.");
    }
    // 배틀용 레벨을 만들고(또는 실제 맵 사용 조건이 맞으면 실제 레벨) PlayLayer 장면으로 진입한다.
    // 호환되지 않는 모드는 이때 일시 정지한다.
    void enterLevel() {
        if (!level || !currentLaunch()) {
            fail("The match was canceled.", false);
            return;
        }
        stopWatchingMusic();
        constructing = true;
        phase = Phase::Entering;
        pauseIncompatibleMods();
        std::uint64_t hash = 0;
        bool const lowDetail =
            versus::Service::get().isRankedMatch()
                ? versus::Service::get().rankedLdm()
                : level->m_lowDetailModeToggled || Mod::get()->getSavedValue<bool>("room-ldm", false);
        GJGameLevel* battleLevel = nullptr;
        rewardsNote.clear();
        if (rankedRewardsWanted()) {
            auto const why = realMapRefusal(level.data(), roomID, hash);
            if (why.empty()) {
                realMap = true;
                realLevel = level;
                realStatsBefore = rewardStats();
                realBestBefore = level->m_normalPercent.value();
                realLdm = level->m_lowDetailModeToggled;
                level->m_lowDetailModeToggled = lowDetail;
                battleLevel = level.data();
                rewardsNote = "Stars & orbs ON: the real map - GD records this battle like normal play";
                log::info("Versus stars & orbs: map {} is played as the real map (best {}%, {} attempts)", levelID,
                          level->m_normalPercent.value(), level->m_attempts.value());
            } else {
                rewardsNote = "Stars & orbs OFF for this battle: " + why;
                log::info("Versus stars & orbs: off for this battle ({})", why);
            }
        }
        if (realMap) {
            s_savedStats.clear();
            armProgress(nullptr, levelID, roomID);
        } else {
            battleLevel = makeBattleLevel(level.data(), lowDetail, hash);
            if (!battleLevel) {
                constructing = false;
                fail("Unable to decode battle map.");
                return;
            }
            snapshotLevelStats(levelID);
            armProgress(battleLevel, levelID, roomID);
        }
        versus::replay::noteBattleLevel(levelID, level->m_levelVersion, hash, lowDetail);
        versus::percent::prepareLevel(battleLevel);
        auto* scene = PlayLayer::scene(battleLevel, false, false);
        constructing = false;
        if (realMap && realLevel) realLevel->m_lowDetailModeToggled = realLdm;
        if (!scene) {
            fail("Unable to open the selected map.");
            return;
        }
        phase = Phase::Waiting;
        CCDirector::sharedDirector()->replaceScene(scene);
        level = nullptr;
    }
    // 생성 중인 PlayLayer를 컨트롤러에 연결한다.
    void attach(PlayLayer* layer) {
        versus::rebind(play, layer);
        versus::rebind(s_pausedFor, layer);
    }
    // PlayLayer 진입 후: 로딩 오버레이를 붙이고 Percentage Toggle 안내를 띄우며, 눌려 있던 입력을 비운다.
    void entered(PlayLayer* layer) {
        if (!belongs(layer)) return;
        installLabel(layer, true);
        if (versus::percent::toggleWouldApply(layer->m_level))
            Notification::create("Percentage Toggle is off in Versus: this battle uses GD 2.2's percentage",
                                 NotificationIcon::Info, 4.f)
                ->show();
        if (layer->m_uiLayer) layer->m_uiLayer->resetAllButtons();
        layer->m_queuedButtons.clear();
    }
    // 출발 신호: 오버레이를 지우고 Playing 단계로 바꾸며 배틀 세션을 시작한 뒤 게임을 시작한다.
    void releaseGame(PlayLayer* layer) {
        if (phase != Phase::Waiting) return;
        layer->m_queuedButtons.clear();
        if (layer->m_uiLayer) layer->m_uiLayer->resetAllButtons();
        if (auto* overlay = layer->getChildByID("versus-launch-wait"_spr)) overlay->removeFromParentAndCleanup(true);
        label = nullptr;
        waitingNames = nullptr;
        waitingPanel = nullptr;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        versus::cursor::hideForPlay();
#endif
        phase = Phase::Playing;
        versus::percent::enforce(layer);
        versus::battle::begin(layer);
        if (s_progress.armed && versus::battle::activeFor(layer)) s_progress.began = true;
        if (!versus::battle::blocksGameplay(layer)) layer->startGame();
        refreshStartModeSprites(layer);
    }
    // 플레이어가 나가려 할 때: 복귀 단계로 바꾸고 필요하면 launch 취소를 시작한다.
    void prepareQuit() {
        if (!active()) return;
        if (phase != Phase::Returning) {
            notice.clear();
            auto owner = play.lock();
            if (!owner || !versus::battle::activeFor(owner.data())) startCancel();
            phase = Phase::Returning;
        }
        redirectQuit = true;
    }
    // 원래 화면(Ranked, 방, 로비 중 해당하는 곳)으로 돌아가고 상태를 정리한다.
    void returnScene() {
        stopWatchingMusic();
        versus::battle::resetAudioSpeed();
        restoreLevelStats();
        s_savedStats.clear();
        if (realMap && realLevel) {
            realLevel->m_lowDetailModeToggled = realLdm;
            showBattleRewards(realLevel.data(), realStatsBefore, realBestBefore);
        }
        realMap = false;
        realLevel = nullptr;
        auto nativeQuit = redirectQuit;
        auto message = notice;
        phase = Phase::Idle;
        redirectQuit = false;
        constructing = false;
        play = nullptr;
        label = nullptr;
        waitingNames = nullptr;
        waitingPanel = nullptr;
        staging = nullptr;
        level = nullptr;
        commitProgress();
        auto& service = versus::Service::get();
        bool const bot = service.botBattle() || roomID.starts_with("ranked-debug-bot-");
        if (bot) {
            service.endBotBattle();
            if (auto result = service.takeBotResult(); !result.empty() && message.empty()) message = result;
        }
        auto const& room = service.room();
        if (bot || service.isRankedMatch() || roomID.starts_with("ranked-")) {
            service.takeNotice();
            CCDirector::sharedDirector()->replaceScene(versus::rankedScene(message, true));
            message.clear();
        } else if (room && room->id == roomID) {
            auto* scene = versus::roomScene();
            CCDirector::sharedDirector()->replaceScene(scene);
        } else {
            auto fromService = service.takeNotice();
            if (!fromService.empty()) message = fromService;
            CCDirector::sharedDirector()->replaceScene(versus::lobbyScene(message));
            message.clear();
        }
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
        if (!message.empty())
            Loader::get()->queueInMainThread([message] { versus::alertServerError("Versus", message); });
        if (!nativeQuit) GameManager::sharedState()->fadeInMenuMusic();
    }

    // 매 프레임 진행. 복귀 중이면 PlayLayer를 나가도록 반복해서 요청하고, 아니면 launch가 유효한지,
    // 60초 안에 시작했는지 확인한 뒤 단계별로 진행한다(다운로드 -> 리소스 -> 상대 대기).
    void update(float dt) override {
        retryCancel(dt);
        if (s_modsPaused && !active() && !s_pausedFor.lock()) resumeIncompatibleMods();
        if (!active() || phase == Phase::Playing) return;
        if (phase == Phase::Returning) {
            if (auto owner = play.lock(); owner && currentSceneContains(owner.data())) {
                quitRetry -= dt;
                if (quitRetry > 0.f) return;
                quitRetry = 1.f;
                redirectQuit = true;
                owner->onQuit();
            } else
                returnScene();
            return;
        }
        if (!currentLaunch()) {
            fail(versus::Service::get().inGroup() ? "The Group battle was canceled or started without you."
                                                  : "The match was canceled or a player left.",
                 false);
            return;
        }
        auto& service = versus::Service::get();
        auto launch = *service.room()->launch;
        auto now = service.serverNow();
        // launch가 60초 안에 출발하지 못하면 실패 처리한다(Ranked는 무효 경기).
        if (!launch.releasedAt &&
            ((requestedAt > 0 && now >= requestedAt + 60000) || Clock::now() - began >= std::chrono::seconds(60))) {
            fail(versus::Service::get().isRankedMatch()
                     ? "The battle did not start within 60 seconds.\nThe match was voided - ratings did not change."
                     : "Both players did not enter within 60 seconds.\nPlease try again.");
            return;
        }
        timer -= dt;
        if (phase == Phase::Download) {
            if (timer <= 0.f) {
                timer = .5f;
                auto const progress = versus::mapDownloadProgress(levelID, rankedLaunch());
                text(fmt::format("{}\n{}", progress.caption(), progress.detail()));
                requestDownload();
            }
            return;
        }
        if (phase == Phase::Resources) {
            if (timer <= 0.f) {
                timer = .25f;
                auto const progress = versus::mapDownloadProgress(levelID, rankedLaunch());
                text(fmt::format("{}\n{}", progress.caption(), progress.detail()));
                if (resourcesReady()) enterLevel();
            }
            return;
        }
        // 상대 대기 단계: 내 로딩 완료를 서버에 알리고(markLoaded) 다운로드 기록이 올라가기를 잠시 기다린다.
        if (phase != Phase::Waiting) return;
        auto owner = play.lock();
        if (!owner || !nativeReady || !currentSceneContains(owner.data()) || !owner->m_player1) return;
        if (++entryFrames < 2) return;
        bool ownLoaded = service.isHost() ? launch.hostLoaded : launch.guestLoaded;
        arrivalAcknowledged = arrivalAcknowledged || ownLoaded;
        arrivalTimer -= dt;
        bool recordIn = true;
        if (!arrivalAcknowledged &&
            !(service.group() && service.group()->battle && service.group()->battle->id == launchID)) {
            if (!recordAskedAt) {
                recordAskedAt = Clock::now();
                service.reportDownloadNow();
            }
            recordIn = service.downloadReported(levelID) || Clock::now() - *recordAskedAt >= kRecordWaitMs;
        }
        if (!arrivalAcknowledged && !arrivalPending && arrivalTimer <= 0.f && !service.busy() && recordIn) {
            arrivalPending = true;
            auto currentGeneration = generation;
            service.markLoaded(launchID, [this, currentGeneration](bool success, std::string) {
                if (currentGeneration != generation) return;
                arrivalPending = false;
                arrivalAcknowledged = arrivalAcknowledged || success;
                arrivalTimer = 1.f;
            });
        }
        // 그룹 배틀이면 좌석별 로딩 상태를 세어 아직 안 온 사람 이름을 표시한다.
        auto const& group = service.group();
        auto const* groupBattle = group && group->battle && group->battle->id == launchID ? &*group->battle : nullptr;
        size_t arrived = 0, playing = 0, waiting = 0;
        std::vector<std::string> missing;
        if (groupBattle)
            for (auto const& seat : groupBattle->seats) {
                if (seat.out) continue;
                ++playing;
                if (seat.loaded) {
                    ++arrived;
                    continue;
                }
                if (seat.profile.uid == service.profile().uid) continue;
                if (++waiting <= kWaitingNamesShown)
                    missing.push_back(seat.profile.name.empty() ? "Player" : seat.profile.name);
            }
        // 서버가 릴리스했고 모두 로딩됐으면 카운트다운 후 시작한다. 릴리스가 늦으면 최소 카운트다운을 보장한다.
        if (launch.releasedAt && launch.hostLoaded && launch.guestLoaded && arrivalAcknowledged) {
            if (startDeadline == 0) {
                startDeadline = std::max(launch.releasedAt + kStartCountdownMs, now + kMinStartCountdownMs);
                if (startDeadline > launch.releasedAt + kStartCountdownMs)
                    log::info("Versus: the release came {} ms late - the countdown runs {} ms", now - launch.releasedAt,
                              kMinStartCountdownMs);
            }
            auto remaining = startDeadline - now;
            if (remaining <= 0) {
                releaseGame(owner.data());
                return;
            }
            text(groupBattle ? fmt::format("{} players connected\nStarting in {}...", arrived, (remaining + 999) / 1000)
                             : fmt::format("Both players connected\nStarting in {}...", (remaining + 999) / 1000));
            showWaiting({});
        }
        // 그룹: 접속한 인원 현황과 남은 시간(30초 기준)을 표시한다.
        else if (groupBattle) {
            auto remaining = std::max<int64_t>(0, requestedAt + 30000 - now);
            text(fmt::format("Waiting for players... {} / {}\n{} seconds remaining", arrived, playing,
                             (remaining + 999) / 1000));
            showWaiting(waiting <= kWaitingNamesShown ? missing : std::vector<std::string>{});
        }
        // 1v1: 상대를 기다리는 문구와 남은 시간(60초 기준).
        else {
            auto remaining = std::max<int64_t>(0, requestedAt + 60000 - now);
            text(fmt::format("Waiting for the other player...\n{} seconds remaining", (remaining + 999) / 1000));
        }
    }
};
}

namespace versus {
// 매치 로딩 시작.
void launchMatch() {
    LaunchController::get().begin();
}
// 배틀용 레벨 복사본 생성(원본이 없으면 nullptr).
GJGameLevel* createBattleLevel(GJGameLevel* source, bool lowDetail, std::uint64_t& hash) {
    return source ? makeBattleLevel(source, lowDetail, hash) : nullptr;
}
// 배틀과 충돌하는 모드의 훅을 정지한다(임베디드 대상 포함).
void pauseBattleMods() {
    LaunchController::get();
    pauseIncompatibleMods(true);
}
void attachPausedMods(PlayLayer* layer) {
    rebind(s_pausedFor, layer);
}
// 등록된 PlayLayer가 끝났으면 정지한 훅을 다시 켠다(로딩이 진행 중이면 보류).
void releasePausedMods(PlayLayer* layer) {
    if (!layer || !s_modsPaused || s_pausedFor.peek() != layer) return;
    s_pausedFor = nullptr;
    if (auto* launch = LaunchController::peek(); launch && launch->active()) return;
    resumeIncompatibleMods();
}
void snapshotBattleLevelStats(int levelID) {
    snapshotLevelStats(levelID);
}
void refreshBattleStartMode(PlayLayer* layer) {
    refreshStartModeSprites(layer);
}
void restoreBattleLevelStats() {
    restoreLevelStats();
}
// 로딩이 진행 중이거나, 방의 launch가 이미 이 컨트롤러가 처리한 것과 같은지.
bool isMatchLaunchActive() {
    if (!LaunchController::peek()) return false;
    auto& controller = LaunchController::get();
    auto const& room = Service::get().room();
    return controller.active() ||
           (room && room->id == controller.handledRoom && room->launch && room->launch->id == controller.handledID);
}
}

namespace {

// GD에 저장된 해당 레벨(ID 일치 확인).
GJGameLevel* savedMapLevel(int levelID) {
    auto* manager = GameLevelManager::sharedState();
    auto* level = manager && levelID > 0 ? manager->getSavedLevel(levelID) : nullptr;
    return level && level->m_levelID.value() == levelID ? level : nullptr;
}
// 배틀 진행을 맵에 기록하면 안 되는 이유(연습, 핵 허용, 디버그, 리플레이, Platformer 등). 가능하면 빈 문자열.
std::string progressRefusal(GJGameLevel* saved, std::string const& roomID) {
    auto& service = versus::Service::get();
    auto const& room = service.room();
    if (!room) return "no room";
    if (room->rules.practice) return "practice rules";
    if (!service.antiCheatEnforced()) return "this room allows hacks";
    if (service.botBattle() || roomID.starts_with("ranked-debug-bot-")) return "debug bot battle";
    if (service.debugRoom()) return "debug room";
    if (versus::replay::viewing()) return "replay";
    if (!saved) return "the map is not in the saved levels";
    if (saved->m_levelType != GJLevelType::Saved) return "not an online level";
    if (saved->isPlatformer()) return "platformer level";
    if (s_lastBattleStripped) return "the map contains start positions (removed for the battle)";
    return {};
}
// Ranked에서 실제 맵으로 보상을 받을지(기능 플래그와 사용자 설정 모두 켜져 있어야 함).
bool rankedRewardsWanted() {
    if (!versus::progress::kStarsOrbsEnabled) return false;
    return versus::Service::get().isRankedMatch() && Mod::get()->getSavedValue<bool>("ranked-rewards", false);
}
// 실제 맵으로 플레이할 수 없는 이유를 돌려준다. 가능하면 빈 문자열이며, 그때 Start Pos 없는 데이터의 해시를 hash에 채운다.
std::string realMapRefusal(GJGameLevel* level, std::string const& roomID, std::uint64_t& hash) {
    auto& service = versus::Service::get();
    auto const& room = service.room();
    if (!room) return "no match room";
    if (service.botBattle() || roomID.starts_with("ranked-debug-bot-")) return "a debug bot battle";
    if (room->rules.practice) return "practice rules";
    if (!service.antiCheatEnforced()) return "hacks are allowed";
    if (versus::replay::viewing()) return "a replay";
    if (!level || level != savedMapLevel(level->m_levelID.value())) return "the map is not in GD's saved levels";
    if (level->m_levelType != GJLevelType::Saved) return "not an online level";
    if (level->isPlatformer()) return "a platformer level";
    auto const& original = level->m_levelString;
    bool const compressed = std::string_view(original.c_str()).find(';') == std::string_view::npos;
    auto const decoded = compressed ? ZipUtils::decompressString(original, false, 11) : original;
    if (decoded.empty()) return "the map data could not be read";
    std::string const text(decoded);
    std::string const stripped = versus::withoutStartPositions(text);
    if (stripped != text) return "the map has start positions";
    hash = versus::replay::hashLevel(stripped);
    s_lastBattleStripped = false;
    return {};
}
bool realMapCheat(GJGameLevel* level);
// 진행 기록 장부를 새로 준비한다. 기록 기능이 꺼져 있거나 조건을 어기면 기록하지 않는다.
void armProgress(GJGameLevel* copy, int levelID, std::string const& roomID) {
    if (s_progress.armed && !s_progress.discarded) discardProgress("an earlier battle's progress was never committed");
    s_progress = {};
    if (!copy) return;
    if (!kSaveMapProgress) {
        log::info("Versus map progress: off (a battle never changes the real map or GD stats)");
        return;
    }
    auto* saved = savedMapLevel(levelID);
    if (auto why = progressRefusal(saved, roomID); !why.empty()) {
        log::info("Versus map progress: not saved for this battle ({})", why);
        return;
    }
    LevelStats(saved).restore(copy);
    s_progress.armed = true;
    s_progress.levelID = levelID;
    s_progress.copy = copy;
    log::info("Versus map progress: recording map {} (best {}%, {} attempts)", levelID, saved->m_normalPercent.value(),
              saved->m_attempts.value());
}
// 지금 호출을 기록해도 되는 상태인지(배틀 복사본의 집계 대상 플레이 중).
bool progressRecording() {
    if (!s_progress.armed || s_progress.sealed || s_progress.discarded) return false;
    auto* launch = LaunchController::peek();
    if (!launch || !launch->active()) return false;
    if (launch->constructing) return true;
    if (!s_progress.began) return false;
    auto* layer = PlayLayer::get();
    return layer && launch->belongs(layer) && layer->m_level == s_progress.copy.data() && !layer->m_isPracticeMode;
}
// 안티치트가 켜진 방에서 외부 치트가 감지되었는지.
bool progressCheatActive() {
    return versus::Service::get().antiCheatEnforced() && versus::integrity::activeExternalCheat(false).detected;
}
// 통계 호출을 장부에 기록한다. 같은 통계 키가 연속이면 합치고 상한을 넘으면 기록을 버린다.
void recordProgress(ProgressCall call) {
    if (!progressRecording()) return;
    auto& calls = s_progress.calls;
    if (call.kind == ProgressCall::Kind::Stat && !calls.empty() && calls.back().kind == ProgressCall::Kind::Stat &&
        calls.back().key == call.key) {
        calls.back().value += call.value;
        return;
    }
    if (calls.size() >= kProgressCallLimit) {
        discardProgress("too many recorded changes");
        return;
    }
    calls.push_back(std::move(call));
}
bool progressLevel(GJGameLevel* level) {
    return level && (level == s_progress.copy.data() || level->m_levelID.value() == s_progress.levelID);
}
// 기록한 진행을 버린다. 보류 중이던 유저 코인은 원래대로 되돌린다.
void discardProgress(std::string const& why, bool onlyOpen) {
    if (!s_progress.armed || s_progress.discarded) return;
    if (onlyOpen && s_progress.sealed) return;
    s_progress.discarded = true;
    if (s_progress.copy) s_progress.copy->m_dontSave = true;
    restorePendingCoins(s_progress.claimedPending);
    s_progress.calls.clear();
    s_progress.pendingCoins.clear();
    s_progress.claimedPending.clear();
    s_progress.sealedStats.reset();
    log::info("Versus map progress: dropped ({})", why);
}
// 집계 대상 플레이를 확정한다. 시작하지 않았거나 치트가 켜져 있으면 대신 버린다.
void sealProgress(PlayLayer* layer, char const* how) {
    if (!s_progress.armed || s_progress.sealed || s_progress.discarded) return;
    if (!s_progress.began) {
        discardProgress("the battle never started");
        return;
    }
    if (!s_progress.copy) {
        discardProgress("the battle map is gone");
        return;
    }
    if (layer && layer == PlayLayer::get() && progressCheatActive()) {
        discardProgress("a cheat is enabled");
        return;
    }
    s_progress.sealedStats.emplace(s_progress.copy.data());
    s_progress.copy->m_dontSave = true;
    s_progress.sealed = true;
    log::info("Versus map progress: counted play ended ({}): best {}%, {} attempts, {} jumps, {} changes", how,
              s_progress.sealedStats->normalPercent.value(), s_progress.sealedStats->attempts.value(),
              s_progress.sealedStats->jumps.value(), s_progress.calls.size());
}
// 보상 비교에 쓰는 GD 통계 키(오브, 별, 문, 다이아, 키, 유저 코인).
constexpr std::array<char const*, 6> kRewardStats{"14", "6", "28", "13", "21", "12"};
std::array<int, 6> rewardStats() {
    std::array<int, 6> values{};
    if (auto* stats = GameStatsManager::sharedState())
        for (size_t i = 0; i < kRewardStats.size(); ++i) values[i] = stats->getStat(kRewardStats[i]);
    return values;
}
// 배틀로 얻은 보상 변화량.
struct BattleRewards {
    int orbs = 0, stars = 0, moons = 0, diamonds = 0, keys = 0, coins = 0;
    int bestBefore = 0, bestAfter = 0;
    bool any() const { return orbs > 0 || stars > 0 || moons > 0 || diamonds > 0 || keys > 0; }
};
// 배틀 보상 연출(CurrencyRewardLayer와 알림)을 장면이 안정되면 재생한다.
void playBattleRewards(BattleRewards rewards, int tries) {
    Loader::get()->queueInMainThread([rewards, tries] {
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene || versus::lounge::realPlayLayer() || typeinfo_cast<CCTransitionScene*>(scene)) {
            if (tries < 300) playBattleRewards(rewards, tries + 1);
            return;
        }
        Ref<CCScene> target(scene);
        scene->runAction(CCSequence::create(
            CCDelayTime::create(.6f), CallFuncExt::create([rewards, target] {
                if (CCDirector::sharedDirector()->getRunningScene() != target.data()) return;
                auto const size = CCDirector::sharedDirector()->getWinSize();
                if (rewards.any()) {
                    auto const shownFrom = rewardStats();
                    target->runAction(CCSequence::create(
                        CCDelayTime::create(6.f), CallFuncExt::create([shownFrom] {
                            if (rewardStats() != shownFrom)
                                log::warn("Versus stars & orbs: stats changed while the reward effect played");
                        }),
                        nullptr));
                    auto* layer = CurrencyRewardLayer::create(
                        rewards.orbs, rewards.stars, rewards.moons, rewards.diamonds,
                        rewards.keys > 0 ? CurrencySpriteType::DemonKey : CurrencySpriteType::Icon,
                        std::max(0, rewards.keys), CurrencySpriteType::Icon, 0, size / 2.f, CurrencyRewardType::Default,
                        0.f, 1.f);
                    if (layer) target->addChild(layer, 100000);
                }
                std::string text = rewards.bestAfter >= 100 && rewards.bestBefore < 100 ? std::string("Level complete!")
                                   : rewards.bestAfter > rewards.bestBefore
                                       ? fmt::format("New best {}%!", rewards.bestAfter)
                                       : std::string();
                if (rewards.coins > 0)
                    text += fmt::format("{}+{} coin{}", text.empty() ? "" : " ", rewards.coins,
                                        rewards.coins > 1 ? "s" : "");
                if (!text.empty()) Notification::create(text, NotificationIcon::Success, 3.f)->show();
            }),
            nullptr));
    });
}
// 배틀 전후 통계를 비교해 보상 연출을 띄운다.
void showBattleRewards(GJGameLevel* level, std::array<int, 6> const& before, int bestBefore) {
    auto const after = rewardStats();
    BattleRewards rewards;
    rewards.orbs = after[0] - before[0];
    rewards.stars = after[1] - before[1];
    rewards.moons = after[2] - before[2];
    rewards.diamonds = after[3] - before[3];
    rewards.keys = after[4] - before[4];
    rewards.coins = after[5] - before[5];
    rewards.bestBefore = bestBefore;
    rewards.bestAfter = level ? level->m_normalPercent.value() : bestBefore;
    log::info(
        "Versus stars & orbs: the battle gave +{} orbs, +{} stars, +{} moons, +{} diamonds, +{} keys, +{} coins; best {}% -> {}%",
        rewards.orbs, rewards.stars, rewards.moons, rewards.diamonds, rewards.keys, rewards.coins, rewards.bestBefore,
        rewards.bestAfter);
    if (rewards.any() || rewards.bestAfter > rewards.bestBefore || rewards.coins > 0) playBattleRewards(rewards, 0);
}
// 보류 중이던 유저 코인 키를 다시 보류 상태로 복원한다.
void restorePendingCoins(std::vector<std::string> const& keys) {
    auto* stats = GameStatsManager::sharedState();
    if (!stats) return;
    for (auto const& key : keys)
        if (!stats->hasUserCoin(key.c_str()) && !stats->hasPendingUserCoin(key.c_str()))
            stats->storePendingUserCoin(key.c_str());
}
constexpr std::array<char const*, 2> kProgressStats{"1", "2"};
// 확정된 진행을 실제 맵에 반영한다(시도/점프/클릭/시도 시간과 지정된 통계만). 보상과 기록 변경은 반영하지 않는다.
void commitProgress() {
    if (s_progress.armed && (!s_progress.sealed || !s_progress.sealedStats))
        discardProgress("the battle ended without a final state");
    auto ledger = std::exchange(s_progress, ProgressLedger{});
    if (!ledger.armed || ledger.discarded) return;
    restorePendingCoins(ledger.claimedPending);
    auto* saved = savedMapLevel(ledger.levelID);
    if (!saved) {
        log::warn("Versus map progress: map {} is no longer in the saved levels; nothing written", ledger.levelID);
        return;
    }
    int const attemptsBefore = saved->m_attempts.value();
    std::vector<GJGameLevel*> targets{saved};
    if (auto* cached = versus::cachedLevel(ledger.levelID);
        cached && cached != saved && cached->m_levelID.value() == ledger.levelID)
        targets.push_back(cached);
    auto const& played = *ledger.sealedStats;
    for (auto* target : targets) {
        target->m_attempts = played.attempts;
        target->m_jumps = played.jumps;
        target->m_clicks = played.clicks;
        target->m_attemptTime = played.attemptTime;
    }
    auto const statsBefore = rewardStats();
    auto* stats = GameStatsManager::sharedState();
    int kept = 0, left = 0;
    for (auto const& call : ledger.calls) {
        bool const counted = call.kind == ProgressCall::Kind::Stat &&
                             std::find(kProgressStats.begin(), kProgressStats.end(), call.key) != kProgressStats.end();
        if (!counted) {
            ++left;
            continue;
        }
        if (stats) stats->incrementStat(call.key.c_str(), call.value);
        ++kept;
    }
    if (rewardStats() != statsBefore) log::warn("Versus map progress: a reward stat changed in the commit");
    log::info("Versus map progress: saved to map {}: attempts {} -> {} ({} stat change(s); {} reward/record change(s) "
              "left out, best stays {}%)",
              ledger.levelID, attemptsBefore, saved->m_attempts.value(), kept, left, saved->m_normalPercent.value());
}
// 클리어 처리 동안 GD의 코인/저장 플래그를 임시로 바꾸고, 끝나면 복원하며 새로 생긴 보류 코인을 장부로 옮긴다.
class ProgressCompletion {
public:
    explicit ProgressCompletion(GJGameLevel* copy) {
        auto* saved = copy ? savedMapLevel(s_progress.levelID) : nullptr;
        if (!saved) return;
        level = copy;
        battleFlags = {copy->m_firstCoinVerified.value(), copy->m_secondCoinVerified.value(),
                       copy->m_thirdCoinVerified.value()};
        copy->m_firstCoinVerified = saved->m_firstCoinVerified.value();
        copy->m_secondCoinVerified = saved->m_secondCoinVerified.value();
        copy->m_thirdCoinVerified = saved->m_thirdCoinVerified.value();
        dontSave = copy->m_dontSave;
        copy->m_dontSave = false;
        if (auto* stats = GameStatsManager::sharedState(); stats && stats->m_pendingUserCoins) {
            for (auto [key, value] : CCDictionaryExt<std::string, CCObject*>(stats->m_pendingUserCoins))
                pendingBefore.insert(key);
            completedBefore = stats->m_completedLevels ? stats->m_completedLevels->count() : 0;
            coinsBefore = stats->m_verifiedUserCoins ? stats->m_verifiedUserCoins->count() : 0;
        }
        log::info("Versus map progress: map {} cleared in counted play; completing it like a normal clear",
                  s_progress.levelID);
    }
    void finish() {
        if (!level) return;
        auto* copy = level.data();
        copy->m_dontSave = dontSave;
        s_progress.coinFlags = {copy->m_firstCoinVerified.value(), copy->m_secondCoinVerified.value(),
                                copy->m_thirdCoinVerified.value()};
        copy->m_firstCoinVerified = battleFlags[0];
        copy->m_secondCoinVerified = battleFlags[1];
        copy->m_thirdCoinVerified = battleFlags[2];
        if (auto* stats = GameStatsManager::sharedState(); stats && stats->m_pendingUserCoins) {
            std::vector<std::string> added;
            std::set<std::string> after;
            for (auto [key, value] : CCDictionaryExt<std::string, CCObject*>(stats->m_pendingUserCoins)) {
                after.insert(key);
                if (!pendingBefore.contains(key)) added.emplace_back(key);
            }
            bool const kept = s_progress.armed && !s_progress.discarded;
            std::vector<std::string> claimed;
            for (auto const& key : pendingBefore)
                if (!after.contains(key)) claimed.push_back(key);
            if (kept) {
                for (auto const& key : added) {
                    stats->m_pendingUserCoins->removeObjectForKey(key);
                    s_progress.pendingCoins.push_back(key);
                }
                s_progress.claimedPending.insert(s_progress.claimedPending.end(), claimed.begin(), claimed.end());
            } else {
                for (auto const& key : added) stats->m_pendingUserCoins->removeObjectForKey(key);
                restorePendingCoins(claimed);
            }
            if (stats->m_completedLevels && stats->m_completedLevels->count() != completedBefore)
                log::warn("Versus map progress: GD's clear changed the completed levels outside the recorded calls");
            if (stats->m_verifiedUserCoins && stats->m_verifiedUserCoins->count() != coinsBefore)
                log::warn("Versus map progress: GD's clear changed the stored coins outside the recorded calls");
        }
        level = nullptr;
    }
    ~ProgressCompletion() { finish(); }

private:
    Ref<GJGameLevel> level;
    std::array<int, 3> battleFlags{0, 0, 0};
    bool dontSave = true;
    std::set<std::string> pendingBefore;
    unsigned int completedBefore = 0, coinsBefore = 0;
};
// 실제 맵 클리어 후 GD가 유저 코인을 실제 획득보다 많이 올렸으면 원래대로 되돌린다.
class RealCoinGuard {
public:
    explicit RealCoinGuard(GJGameLevel* real) {
        auto* stats = GameStatsManager::sharedState();
        if (!real || !stats) return;
        level = real;
        owned = ownedCoins(stats);
        userCoins = stats->getStat("12");
    }
    void finish() {
        auto* stats = GameStatsManager::sharedState();
        if (level && stats) {
            int const stored = std::max(0, ownedCoins(stats) - owned);
            int const added = stats->getStat("12") - userCoins;
            if (added > stored) {
                log::warn("Versus stars & orbs: the clear added {} user coin(s) for {} stored; set back", added,
                          stored);
                stats->setStat("12", userCoins + stored);
            }
        }
        level = nullptr;
    }
    ~RealCoinGuard() { finish(); }

private:
    int ownedCoins(GameStatsManager* stats) {
        int count = 0;
        for (int i = 1; i <= std::clamp(level->m_coins, 0, 3); ++i)
            if (stats->hasUserCoin(level->getCoinKey(i))) ++count;
        return count;
    }
    Ref<GJGameLevel> level;
    int owned = 0, userCoins = 0;
};
}
namespace versus::progress {
// 집계 대상 플레이의 종료를 확정한다.
void endCountedPlay(PlayLayer* layer) {
    if (!s_progress.armed || s_progress.sealed || s_progress.discarded) return;
    if (!layer || layer->m_level != s_progress.copy.data()) return;
    sealProgress(layer, "counted play ended");
}
void discard(std::string const& why, bool onlyOpen) {
    discardProgress(why, onlyOpen);
}
// 이 레이어가 실제 맵으로 열린 배틀 레이어인지.
bool realMap(PlayLayer* layer) {
    auto* launch = LaunchController::peek();
    return launch && launch->realMap && launch->belongs(layer) && layer->m_level == launch->realLevel.data();
}
}

// 배틀 중 PlayLayer 훅. 로딩 중에는 게임 시작, 일시 정지, 리셋을 막고 Start Pos 오브젝트를 건너뛰며,
// 사망/클리어/종료를 BattleSession으로 전달한다. 치트 모드보다 먼저 사망을 보도록 훅 우선순위를 지정한다.
class $modify(VersusMatchPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First);
        (void)self.setHookPriorityBeforePre("PlayLayer::destroyPlayer", "absolllute.megahack");
        (void)self.setHookPriorityBeforePre("PlayLayer::destroyPlayer", "tipp7.chameleon");
        (void)self.setHookPriorityPre("PlayLayer::resetLevel", Priority::First);
    }
    struct Fields {
        bool initialized = false;
    };
    // 배틀 레벨에서는 Start Pos 오브젝트(ID 31)를 추가하지 않고 유저 코인을 매번 새 코인으로 취급한다.
    void addObject(GameObject * object) {
        if (!LaunchController::running() || !LaunchController::get().belongs(this)) {
            PlayLayer::addObject(object);
            return;
        }
        if (object && object->m_objectID == 31) return;
        bool const previousCoinRead = s_readBattleCoins;
        s_readBattleCoins = true;
        PlayLayer::addObject(object);
        s_readBattleCoins = previousCoinRead;
        if (object && object->getType() == GameObjectType::UserCoin)
            if (auto* coin = typeinfo_cast<EnhancedGameObject*>(object)) coin->m_hasUniqueCoin = false;
    }
    // 배틀 레벨을 만들 때 컨트롤러에 연결하고 Percentage 보호를 적용한다.
    bool init(GJGameLevel * level, bool useReplay, bool dontCreateObjects) {
        if (!LaunchController::running()) return PlayLayer::init(level, useReplay, dontCreateObjects);
        auto& controller = LaunchController::get();
        if (controller.constructing && level && level->m_levelID.value() == controller.levelID) controller.attach(this);
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields->initialized = true;
        versus::percent::enforce(this);
        return true;
    }
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        if (LaunchController::running()) LaunchController::get().entered(this);
    }
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (!LaunchController::running()) return;
        auto& controller = LaunchController::get();
        if (controller.belongs(this)) controller.nativeReady = true;
    }
    // 로딩 중이거나 시작이 막힌 상태에서는 게임/음악 시작을 막는다.
    void startGame() {
        if (!LaunchController::running()) {
            PlayLayer::startGame();
            return;
        }
        if (LaunchController::get().gated(this) || versus::battle::blocksGameplay(this)) return;
        PlayLayer::startGame();
    }
    void startGameDelayed() {
        if (!LaunchController::running()) {
            PlayLayer::startGameDelayed();
            return;
        }
        if (LaunchController::get().gated(this) || versus::battle::blocksGameplay(this)) return;
        PlayLayer::startGameDelayed();
    }
    void startMusic() {
        if (!LaunchController::running()) {
            PlayLayer::startMusic();
            return;
        }
        if (LaunchController::get().gated(this) || versus::battle::blocksGameplay(this)) return;
        PlayLayer::startMusic();
    }
    // 일시 정지: 배틀 규칙(일시 정지 예산 등)에 따라 허용/차단하고 상태를 세션에 알린다.
    void pauseGame(bool unfocused) {
        if (!LaunchController::running()) {
            PlayLayer::pauseGame(unfocused);
            return;
        }
        if (LaunchController::get().gated(this)) return;
        if (unfocused) versus::battle::escapeKey(this, false);
        if (versus::battle::pauseMenuOpen(this)) return;
        if (versus::battle::pauseHoldGranted(this)) {
            PlayLayer::pauseGame(unfocused);
            versus::battle::paused(this, m_isPaused);
            return;
        }
        if (versus::battle::blocksPause(this)) {
            if (!unfocused && versus::battle::blocksGameplay(this)) versus::battle::requestQuit(this);
            return;
        }
        PlayLayer::pauseGame(unfocused);
        versus::battle::paused(this, m_isPaused);
    }
    void resume() {
        PlayLayer::resume();
        if (LaunchController::running()) versus::battle::paused(this, false);
    }
    // 리셋: 로딩 중이면 막고, 세션의 beforeReset/afterReset으로 시도 시작/종료를 처리한다.
    void resetLevel() {
        if (!LaunchController::running()) {
            PlayLayer::resetLevel();
            return;
        }
        auto& controller = LaunchController::get();
        if (m_fields->initialized && controller.nativeReady && controller.gated(this)) return;
        if (!versus::battle::beforeReset(this)) return;
        bool const previousCoinRead = s_readBattleCoins;
        if (controller.belongs(this)) {
            s_readBattleCoins = true;
            setStartPosObject(nullptr);
        }
        PlayLayer::resetLevel();
        s_readBattleCoins = previousCoinRead;
        versus::battle::afterReset(this);
    }
    // 사망: 한 번만 처리하고 사망 후 세션에 알린다.
    void destroyPlayer(PlayerObject * player, GameObject * object) {
        if (!versus::battle::activeFor(this)) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        Ref<PlayLayer> keepAlive(this);
        auto* effective = player ? player : m_player1;
        if (effective && effective->m_isDead) return;
        PlayLayer::destroyPlayer(player, object);
        if (effective && (effective == m_player1 || (m_gameState.m_isDualMode && effective == m_player2))) {
            if (effective->m_isDead) versus::battle::died(this);
        }
    }
    // 클리어: 라이브 관전 중이면 테스트 모드로 처리하고, 배틀이면 통계가 실제 기록에 남지 않게 테스트 모드와 복원을 적용한다.
    // 무결성 검사를 한 뒤 세션에 클리어를 알린다.
    void levelComplete() {
        if (!LaunchController::running()) {
            PlayLayer::levelComplete();
            return;
        }
        if (versus::replay::liveWatching(this)) {
            bool const previousTest = m_isTestMode;
            m_isTestMode = true;
            PlayLayer::levelComplete();
            m_isTestMode = previousTest;
            if (LaunchController::get().belongs(this)) restoreLevelStats();
            return;
        }
        bool const previousNative = s_nativeBattleComplete;
        s_nativeBattleComplete = true;
        bool const battle = LaunchController::get().belongs(this);
        bool saveProgress = false;
        if (battle && progressRecording() && m_level == s_progress.copy.data()) {
            versus::integrity::checkNow(this);
            saveProgress = progressRecording() && !progressCheatActive();
        }
        auto& controller = LaunchController::get();
        bool const real = battle && controller.realMap && m_level == controller.realLevel.data();
        if (real) {
            versus::integrity::checkNow(this);
            saveProgress = !realMapCheat(m_level);
        }
        bool const previousTest = m_isTestMode;
        if (battle && !saveProgress) m_isTestMode = true;
        ProgressCompletion completion(saveProgress && !real ? m_level : nullptr);
        RealCoinGuard coins(saveProgress && real ? m_level : nullptr);
        PlayLayer::levelComplete();
        coins.finish();
        completion.finish();
        if (battle) {
            m_isTestMode = previousTest;
            restoreLevelStats();
        }
        s_nativeBattleComplete = previousNative;
        bool const previousCoinRead = s_readBattleCoins;
        if (LaunchController::get().belongs(this)) s_readBattleCoins = true;
        versus::integrity::checkNow(this);
        versus::battle::completed(this);
        s_readBattleCoins = previousCoinRead;
    }
    // 배틀 중에는 GD의 종료 화면을 띄우지 않는다.
    void showEndLayer() {
        if (versus::battle::activeFor(this)) return;
        PlayLayer::showEndLayer();
    }
    // 실제 맵 배틀에서는 New Best 연출을 보류한다.
    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        if (versus::progress::realMap(this)) {
            log::info("Versus stars & orbs: New Best held back (reward {}, {} orbs, {} diamonds, key {})", newReward,
                      orbs, diamonds, demonKey);
            return;
        }
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }
    // 배틀 중 재시도 화면 대신 바로 리셋한다.
    void showRetryLayer() {
        if (versus::battle::activeFor(this)) {
            if (!versus::battle::blocksGameplay(this)) resetLevel();
            return;
        }
        PlayLayer::showRetryLayer();
    }
    // 연습 모드 전환은 배틀 규칙이 허용할 때만.
    void togglePracticeMode(bool practice) {
        if (!versus::battle::allowPracticeToggle(this, practice)) return;
        PlayLayer::togglePracticeMode(practice);
    }
    // 나가기: 세션이 먼저 처리하고(확인 창 등), 배틀이면 컨트롤러를 복귀 단계로 바꾼 뒤 기본 동작을 호출한다.
    void onQuit() {
        if (!LaunchController::running()) {
            PlayLayer::onQuit();
            return;
        }
        if (versus::battle::requestQuit(this)) return;
        auto& controller = LaunchController::get();
        bool const versusMatch = controller.belongs(this);
        if (versusMatch) {
            controller.prepareQuit();
            versus::battle::detach(this);
        }
        PlayLayer::onQuit();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        if (versusMatch) {
            PlatformToolbox::toggleLockCursor(false);
            PlatformToolbox::showCursor();
        }
#endif
    }
};

// 배틀 레벨 생성/리셋 중에는 이미 획득한 유저 코인을 기록에서 숨겨 매번 코인이 보이게 한다.
class $modify(VersusBattleCoins,
              GameStatsManager){bool hasUserCoin(char const* key){auto* launch = LaunchController::peek();
if ((launch && launch->constructing) || s_readBattleCoins) return false;
return GameStatsManager::hasUserCoin(key);
}
bool hasPendingUserCoin(char const* key) {
    auto* launch = LaunchController::peek();
    if ((launch && launch->constructing) || s_readBattleCoins) return false;
    return GameStatsManager::hasPendingUserCoin(key);
}
}
;

// GD 통계 변경을 막아야 하는 배틀/리플레이/free play/라운지 상태인지.
namespace {
bool battleSafeMode() {
    if (versus::replay::viewing()) return true;
    if (versus::freeplay::active()) return true;
    if (versus::lounge::active()) return true;
    auto* launch = LaunchController::peek();
    if (!launch || !launch->active()) return false;
    if (launch->realMap) return false;
    if (launch->constructing) return true;
    auto* layer = PlayLayer::get();
    return layer && launch->belongs(layer);
}
// 실제 맵에서 치트가 감지된 상태인지.
bool realMapCheat(GJGameLevel* level) {
    auto* launch = LaunchController::peek();
    return launch && launch->active() && launch->realMap && level && level == launch->realLevel.data() &&
           progressCheatActive();
}
}
using ProgressKind = ProgressCall::Kind;
// 배틀 중에는 GD 통계/보상/완료 기록 변경 호출을 막고(필요하면 장부에만 기록한다).
class $modify(VersusSafeStats, GameStatsManager){void incrementStat(char const* key, int amount){
    if (battleSafeMode()){if (key) recordProgress({ProgressKind::Stat, key, amount});
return;
}
GameStatsManager::incrementStat(key, amount);
}
void incrementChallenge(GJChallengeType type, int amount) {
    if (battleSafeMode()) {
        recordProgress({ProgressKind::Challenge, {}, static_cast<int>(type), amount});
        return;
    }
    GameStatsManager::incrementChallenge(type, amount);
}
void completedLevel(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::Completed});
        return;
    }
    GameStatsManager::completedLevel(level);
}
void completedStarLevel(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::StarLevel});
        return;
    }
    GameStatsManager::completedStarLevel(level);
}
void completedDemonLevel(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::DemonLevel});
        return;
    }
    GameStatsManager::completedDemonLevel(level);
}
GJRewardItem* completedDailyLevel(GJGameLevel* level) {
    if (battleSafeMode()) return nullptr;
    return GameStatsManager::completedDailyLevel(level);
}
void awardCurrencyForLevel(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::Currency});
        return;
    }
    GameStatsManager::awardCurrencyForLevel(level);
}
void awardDiamondsForLevel(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::Diamonds});
        return;
    }
    GameStatsManager::awardDiamondsForLevel(level);
}
void markLevelAsCompletedAndClaimed(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::Claimed});
        return;
    }
    GameStatsManager::markLevelAsCompletedAndClaimed(level);
}
void storeUserCoin(char const* key) {
    if (battleSafeMode()) {
        if (key) recordProgress({ProgressKind::UserCoin, key});
        return;
    }
    GameStatsManager::storeUserCoin(key);
}
void checkCoinAchievement(GJGameLevel* level) {
    if (battleSafeMode()) {
        if (progressLevel(level)) recordProgress({ProgressKind::CoinAchievement});
        return;
    }
    GameStatsManager::checkCoinAchievement(level);
}
}
;
// 퍼센트와 업적 보고도 같은 방식으로 막는다.
class $modify(VersusSafeGameManager, GameManager){
    void reportPercentageForLevel(int levelID, int percentage, bool isPlatformer){if (battleSafeMode()){
        if (levelID == s_progress.levelID) recordProgress({ProgressKind::Percentage, {}, percentage, 0, isPlatformer});
return;
}
GameManager::reportPercentageForLevel(levelID, percentage, isPlatformer);
}
void reportAchievementWithID(char const* key, int percent, bool dontNotify) {
    if (battleSafeMode()) {
        if (key) recordProgress({ProgressKind::Achievement, key, percent, 0, dontNotify});
        return;
    }
    GameManager::reportAchievementWithID(key, percent, dontNotify);
}
}
;
// 배틀 레벨의 퍼센트 저장은 메모리상 값만 갱신하고 실제 저장은 하지 않는다.
class $modify(VersusSafeLevel,
              GJGameLevel){void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts,
                                               bool isChkValid){if (battleSafeMode()){auto* play = PlayLayer::get();
if (play && play->m_level == this && this == s_progress.copy.data() && !isPracticeMode && progressRecording()) {
    bool const dontSave = m_dontSave;
    m_dontSave = false;
    GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    m_dontSave = dontSave;
    return;
}
auto* layer = PlayLayer::get();
if (layer && layer->m_level == this && m_dontSave && percent >= 0 && percent <= 100) {
    if (isPracticeMode) {
        if (percent > m_practicePercent) m_practicePercent = percent;
    } else if (percent > m_normalPercent.value())
        m_normalPercent = percent;
}
return;
}
if (realMapCheat(this)) return;
GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
}
}
;
// 배틀 중인 레벨이 GD 저장 목록에 저장되지 않게 한다.
class $modify(VersusSafeLevelManager, GameLevelManager){void saveLevel(GJGameLevel * level){
    if (battleSafeMode() && level && PlayLayer::get() && level == PlayLayer::get()->m_level) return;
GameLevelManager::saveLevel(level);
}
}
;

// Prism 모드의 PlayLayer::init 전후에 범위 표시를 걸어 무결성 검사가 Prism의 초기화를 구분하게 한다.
class $modify(VersusPrismInitScope, PlayLayer){
    static void onModify(auto& self){(void)self.setHookPriorityBeforePre("PlayLayer::init", "firee.prism");
}
bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
    versus::integrity::beginPrismInitScope();
    bool const result = PlayLayer::init(level, useReplay, dontCreateObjects);
    versus::integrity::endPrismInitScope();
    return result;
}
}
;

class $modify(VersusAfterPrismInit, PlayLayer){
    static void onModify(auto& self){(void)self.setHookPriorityAfterPre("PlayLayer::init", "firee.prism");
}
bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
    bool const result = PlayLayer::init(level, useReplay, dontCreateObjects);
    if (result) versus::integrity::markPrismInitBaseReturned();
    return result;
}
}
;

// Prism/MegaHack의 즉시 클리어를 관찰해 무결성 검사에 반영한다.
class $modify(VersusPrismInstantComplete, PlayLayer){
    void playEndAnimationToPos(CCPoint position){versus::integrity::observePrismInstantComplete(this);
versus::integrity::observeMegaHackInstantComplete(this);
PlayLayer::playEndAnimationToPos(position);
versus::integrity::checkNow(this);
}
void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
    versus::integrity::observeMegaHackInstantComplete(this);
    PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    versus::integrity::checkNow(this);
}
}
;

// MegaHack postUpdate 전후에 범위 표시를 건다.
class $modify(VersusMegaHackPostScope, PlayLayer){static void onModify(auto& self){
    (void)self.setHookPriorityBeforePre("PlayLayer::postUpdate", "absolllute.megahack");
}
void postUpdate(float dt) {
    versus::integrity::beginMegaHackPostUpdateScope();
    PlayLayer::postUpdate(dt);
    versus::integrity::endMegaHackPostUpdateScope();
}
}
;

class $modify(VersusAfterMegaHackPost, PlayLayer){
    static void onModify(auto& self){(void)self.setHookPriorityAfterPre("PlayLayer::postUpdate", "absolllute.megahack");
}
void postUpdate(float dt) {
    PlayLayer::postUpdate(dt);
    versus::integrity::markMegaHackPostUpdateBaseReturned();
}
}
;

// Prism의 processCommands 전후 범위 표시를 건다.
class $modify(VersusPrismProcessScope, GJBaseGameLayer){static void onModify(auto& self){
    (void)self.setHookPriorityBeforePre("GJBaseGameLayer::processCommands", "firee.prism");
}
void processCommands(float dt, bool halfTick, bool lastTick) {
    versus::integrity::beginPrismProcessScope();
    GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    versus::integrity::endPrismProcessScope();
}
}
;

// 배틀 중 GJBaseGameLayer 훅: 로딩 중 업데이트/입력 차단, 프레임 시간 상한과 배틀 속도 적용,
// 입력 리플레이 기록, 치트로 주입된 입력 감지, 유저 코인 수집 처리.
class $modify(VersusMatchGameLayer, GJBaseGameLayer){static void onModify(auto& self){
    (void)self.setHookPriorityAfterPre("GJBaseGameLayer::processCommands", "firee.prism");
(void)self.setHookPriorityAfterPre("GJBaseGameLayer::handleButton", "firee.prism");
}
// 로딩 중이면 업데이트를 멈추고, 아니면 프레임 시간(최대 0.25초)과 배틀 속도를 적용해 진행한 뒤 세션에 알린다.
void update(float dt) {
    if (!LaunchController::running()) {
        GJBaseGameLayer::update(dt);
        return;
    }
    if (LaunchController::get().gated(this) || versus::battle::blocksGameplay(this)) return;
    float const simulated = battleLayer() ? std::min(dt, kMaxBattleFrameDelta) : dt;
    GJBaseGameLayer::update(battleLayer() ? simulated * versus::battle::speedFor(this) : simulated);
    versus::battle::simulated(this, simulated);
    if (auto* play = PlayLayer::get()) versus::battle::sample(play);
}
// 입력: Prism이 주입한 입력이 감지되면 실격 처리하고, 로딩 중이거나 차단 상태면 무시한다.
void handleButton(bool down, int button, bool player1) {
    if (!LaunchController::running()) {
        GJBaseGameLayer::handleButton(down, button, player1);
        return;
    }
    if (auto status = versus::integrity::observePrismInjectedInput(); status.detected)
        versus::battle::disqualify(PlayLayer::get(), status.detail, status.code, status.features);
    if (LaunchController::get().gated(this) || versus::battle::blocksInput(this)) return;
    GJBaseGameLayer::handleButton(down, button, player1);
    versus::replay::input(this, down, button, player1);
}
bool battleLayer() {
    if (!LaunchController::running()) return false;
    auto* play = typeinfo_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
    return play && LaunchController::get().belongs(play);
}
bool hasUniqueCoin(EffectGameObject* object) {
    if (!s_nativeBattleComplete && battleLayer()) return false;
    return GJBaseGameLayer::hasUniqueCoin(object);
}
void pickupItem(EffectGameObject* object) {
    GJBaseGameLayer::pickupItem(object);
    if (!object || object->getType() != GameObjectType::UserCoin || !battleLayer() || !m_collectedItems) return;
    CCDictElement* element = nullptr;
    CCDictElement* next = nullptr;
    HASH_ITER(hh, m_collectedItems->m_pElements, element, next) if (element->getObject() == object) return;
    m_collectedItems->setObject(object, fmt::format("versus-coin-{}", object->m_uniqueID));
}
// 입력 처리 단계의 시작/끝을 무결성 모니터에 알리고 리플레이 스텝을 기록한다.
void processCommands(float dt, bool halfTick, bool lastTick) {
    if (!LaunchController::running()) {
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
        return;
    }
    if (LaunchController::get().gated(this) || versus::battle::blocksInput(this)) return;
    versus::replay::stepStart(this, dt);
    versus::battle::noteStep(this, dt);
    versus::integrity::beginNativeProcessScope();
    GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    versus::integrity::endNativeProcessScope();
}
}
;

// UILayer 훅: 로딩 중이거나 차단 상태이면 터치/키 입력을 막고 Esc/뒤로 가기를 배틀 규칙에 맞게 처리한다.
class $modify(VersusMatchUILayer, UILayer){bool ccTouchBegan(CCTouch * touch, CCEvent* event){
    if (!LaunchController::running()) return UILayer::ccTouchBegan(touch, event);
if (LaunchController::get().gated(m_gameLayer) || versus::battle::blocksInput(m_gameLayer)) return false;
return UILayer::ccTouchBegan(touch, event);
}
void keyDown(enumKeyCodes key, double timestamp) {
    if (!LaunchController::running()) {
        UILayer::keyDown(key, timestamp);
        return;
    }
    if (LaunchController::get().gated(m_gameLayer)) return;
    if (key == KEY_Escape && versus::battle::escapeKey(m_gameLayer, true)) return;
    if (key == KEY_Escape && versus::battle::blocksPause(m_gameLayer)) {
        deferLeave();
        return;
    }
    if (versus::battle::blocksInput(m_gameLayer) && key != KEY_Escape) return;
    UILayer::keyDown(key, timestamp);
}
void keyBackClicked() {
    if (!LaunchController::running()) {
        UILayer::keyBackClicked();
        return;
    }
    auto& controller = LaunchController::get();
    if (controller.gated(m_gameLayer)) {
        versus::deferBack(this, [] { LaunchController::get().cancelButton(nullptr); });
        return;
    }
    if (versus::battle::backKey(m_gameLayer)) return;
    if (versus::battle::blocksPause(m_gameLayer)) {
        deferLeave();
        return;
    }
    UILayer::keyBackClicked();
}
void deferLeave() {
    versus::deferBack(this,
                      [self = Ref<UILayer>(this)] { static_cast<VersusMatchUILayer*>(self.data())->offerLeave(); });
}
void offerLeave() {
    if (!versus::battle::blocksGameplay(m_gameLayer)) return;
    if (auto* play = typeinfo_cast<PlayLayer*>(m_gameLayer)) versus::battle::requestQuit(play);
}
}
;

// 일시 정지 메뉴 훅: 리롤 투표 버튼(양쪽 합의)과 그룹 관전 버튼을 추가하고,
// 재시작/나가기/연습 전환을 배틀 규칙에 맞게 제한한다.
class $modify(VersusBattlePauseLayer, PauseLayer){void customSetup(){PauseLayer::customSetup();
auto* layer = PlayLayer::get();
if (layer && versus::battle::groupSpectatorPause(layer)) {
    addSpectateButton();
    return;
}
if (!layer || !versus::battle::canVoteReroll(layer)) return;
bool const voted = versus::battle::rerollVoted(layer);
bool const asked = versus::battle::opponentRerollVoted(layer);
auto* sprite = ButtonSprite::create(voted   ? "Reroll Sent"
                                    : asked ? "Reroll 1/2"
                                            : "Reroll",
                                    "goldFont.fnt", "GJ_button_06.png", 1.f);
sprite->setScale(1.05f);
auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(VersusBattlePauseLayer::onVersusReroll));
item->setID("versus-reroll"_spr);
if (voted) {
    item->setEnabled(false);
    sprite->setCascadeOpacityEnabled(true);
    sprite->setOpacity(130);
}
auto const window = CCDirector::sharedDirector()->getWinSize();
auto* menu = CCMenu::create();
menu->setID("versus-reroll-menu"_spr);
menu->setPosition({0.f, 0.f});
item->setPosition({window.width / 2.f, 38.f});
menu->addChild(item);
addChild(menu, 50);
auto* hint = CCLabelBMFont::create(voted   ? "Waiting for your opponent to press Reroll too"
                                   : asked ? "Your opponent wants a new map: press Reroll to agree"
                                           : "Both players: a new map with 40% fewer lives",
                                   "chatFont.fnt");
hint->setID("versus-reroll-hint"_spr);
hint->setScale(.55f);
hint->setColor(asked && !voted ? ccc3(255, 215, 120) : ccc3(255, 200, 190));
hint->setPosition({window.width / 2.f, 12.f});
addChild(hint, 50);
}
void addSpectateButton() {
    auto* sprite = ButtonSprite::create("Spectate", "goldFont.fnt", "GJ_button_05.png", 1.f);
    sprite->setScale(1.05f);
    auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(VersusBattlePauseLayer::onVersusSpectate));
    item->setID("versus-group-spectate-button"_spr);
    auto const window = CCDirector::sharedDirector()->getWinSize();
    auto* menu = CCMenu::create();
    menu->setID("versus-group-spectate-menu"_spr);
    menu->setPosition({0.f, 0.f});
    item->setPosition({window.width / 2.f, 38.f});
    menu->addChild(item);
    addChild(menu, 50);
    auto* hint = CCLabelBMFont::create("Everyone's live progress in this battle", "chatFont.fnt");
    hint->setID("versus-group-spectate-hint"_spr);
    hint->setScale(.55f);
    hint->setColor(ccc3(200, 225, 255));
    hint->setPosition({window.width / 2.f, 12.f});
    addChild(hint, 50);
}
void onVersusSpectate(CCObject*) {
    if (auto* layer = PlayLayer::get(); layer && versus::battle::groupSpectatorPause(layer))
        versus::showGroupStandings();
}
void onVersusReroll(CCObject*) {
    auto* layer = PlayLayer::get();
    if (!layer || !versus::battle::canVoteReroll(layer) || versus::battle::rerollVoted(layer)) return;
    PauseLayer::onResume(nullptr);
    versus::battle::voteReroll(layer);
}
static void finishRestart(Weak<PlayLayer> game, Weak<PauseLayer> overlay) {
    auto layer = game.lock();
    if (!layer || !versus::battle::activeFor(layer.data())) return;
    auto pause = overlay.lock();
    if (pause && pause->getParent() && pause->isVisible()) return;
    if (layer->m_isPaused) layer->resume();
    versus::battle::paused(layer.data(), false);
}
void onRestart(CCObject* sender) {
    if (!versus::battle::activeFor(PlayLayer::get())) {
        PauseLayer::onRestart(sender);
        return;
    }
    auto game = Weak<PlayLayer>(PlayLayer::get());
    auto overlay = Weak<PauseLayer>(this);
    PauseLayer::onRestart(sender);
    finishRestart(game, overlay);
}
void onRestartFull(CCObject* sender) {
    if (!versus::battle::activeFor(PlayLayer::get())) {
        PauseLayer::onRestartFull(sender);
        return;
    }
    auto game = Weak<PlayLayer>(PlayLayer::get());
    auto overlay = Weak<PauseLayer>(this);
    PauseLayer::onRestartFull(sender);
    finishRestart(game, overlay);
}
void onQuit(CCObject* sender) {
    auto* layer = PlayLayer::get();
    if (layer && versus::battle::requestQuit(layer)) return;
    PauseLayer::onQuit(sender);
}
void onNormalMode(CCObject* sender) {
    auto* layer = PlayLayer::get();
    if (versus::battle::activeFor(layer) && !versus::battle::practiceSwitchable(layer)) return;
    PauseLayer::onNormalMode(sender);
}
void onPracticeMode(CCObject* sender) {
    auto* layer = PlayLayer::get();
    if (versus::battle::activeFor(layer) && !versus::battle::practiceSwitchable(layer)) return;
    PauseLayer::onPracticeMode(sender);
}
}
;

// 리플레이 뷰어/free play/매치 로딩에서 나갈 때 GD의 복귀 장면 대신 알맞은 화면으로 돌아가게 한다.
class $modify(VersusMatchGameManager,
              GameManager){void returnToLastScene(GJGameLevel * level){if (versus::replay::leaveViewer()) return;
if (versus::freeplay::leave()) return;
if (!LaunchController::running()) {
    GameManager::returnToLastScene(level);
    return;
}
auto& controller = LaunchController::get();
if (controller.redirectQuit) {
    controller.returnScene();
    return;
}
GameManager::returnToLastScene(level);
}
}
;
