// 배틀 리플레이의 녹화, 재생(Viewer), 라이브 중계/관전(Live)을 구현한 파일.
// - Recorder: 매치 중 내 시도(attempt)의 입력과 위치 샘플을 모아 가장 높은 %의 시도를 서버에 업로드
// - Viewer: 저장된 리플레이를 받아 같은 맵을 열고 입력/위치를 주입해 재생
// - Live: 지금 진행 중인 시도를 조각(LivePart)으로 실시간 전송하고, 상대 조각을 받아 이어서 재생
// - Seeding/SeedScope: 랜덤 트리거가 같은 결과가 나오도록 GD 난수를 시도별로 고정
// 포맷 자체는 ReplayFormat.hpp에 있다.
#include "Replay.hpp"
#include "ExternalCheatMonitor.hpp"
#include "compat/Compat.hpp"
#include "compat/GameHooks.hpp"
#include "MapCache.hpp"
#include "MatchLaunch.hpp"
#include "PercentGuard.hpp"
#include "VersusService.hpp"
#include "WeakRefs.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/AdvancedFollowInstance.hpp>
#include <Geode/binding/AdvancedFollowTriggerObject.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/modify/SpawnTriggerGameObject.hpp>
#include <Geode/utils/base64.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

using namespace geode::prelude;

namespace versus::replay {
namespace {
using Clock = std::chrono::steady_clock;

// 64비트 정수 해시 믹서 (splitmix64 계열). 시드 값을 섞는 데 쓴다.
std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

// 한 시도 동안 난수를 결정적으로 만들기 위한 상태. 같은 매치/시도/트리거 위치에서는
// 녹화 때와 재생 때 같은 난수열이 나오게 한다. layer가 해당 레이어일 때만 켜진다(on).
struct Seeding {
    GJBaseGameLayer const* layer = nullptr;
    std::uint64_t attempt = 0;
    int run = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> draws;
    // base(매치 시드)가 0이면 꺼 둔다. 아니면 매치 시드와 시도 번호를 섞어 이 시도의 시드를 만든다.
    void begin(GJBaseGameLayer const* owner, std::uint64_t base, int attemptRun) {
        draws.clear();
        layer = base ? owner : nullptr;
        run = attemptRun;
        attempt = base ? (mix(base ^ mix(static_cast<std::uint64_t>(attemptRun) + 0x5eedull)) | 1) : 0;
    }
    void end() {
        layer = nullptr;
        attempt = 0;
        run = 0;
        draws.clear();
    }
    bool on(GJBaseGameLayer const* owner) const { return attempt && owner && owner == layer; }
};
// 전역 Seeding 하나.
Seeding& seeding() {
    static Seeding value;
    return value;
}
// layer에서 run번째 시도의 난수 고정을 시작한다 (hook 쪽에서 resetLevel 직전에 호출).
void seedAttempt(GJBaseGameLayer const* layer, std::uint64_t base, int run) {
    seeding().begin(layer, base, run);
}
// matchId 문자열에서 매치 전체 시드를 만든다. 0이 되지 않도록 최하위 비트를 켠다.
std::uint64_t matchSeed(std::string_view matchId) {
    return mix(hashLevel(matchId)) | 1;
}
// 트리거 한 곳을 식별하는 키. 종류(kind), 오브젝트 ID, 배치 위치(startPosition)를 섞는다.
std::uint64_t siteKey(std::uint64_t kind, GameObject const* object, std::uint64_t extra = 0) {
    std::uint64_t key = mix(kind << 56 ^ extra);
    if (object) {
        key ^= mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(object->m_objectID)) << 1 | 1);
        key ^= mix((static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(object->m_startPosition.x)) << 32) |
                   std::bit_cast<std::uint32_t>(object->m_startPosition.y));
    }
    return key;
}
// 트리거가 실행되는 동안만 GD의 fast_srand 상태를 이 위치 전용 값으로 바꿨다가 되돌리는 RAII.
// 같은 위치에서 몇 번째 호출인지(draws)도 시드에 섞어, 호출 순서가 같으면 결과가 같다.
// counts가 false이면 호출 횟수를 올리지 않는다.
class SeedScope {
    bool active = false;
    std::uint64_t saved = 0, key = 0;

public:
    bool counts = true;
    SeedScope(GJBaseGameLayer const* layer, std::uint64_t site) {
        auto& s = seeding();
        if (!s.on(layer)) return;
        key = site;
        auto const used = s.draws[site];
        saved = GameToolbox::getfast_srand();
        GameToolbox::fast_srand(mix(s.attempt ^ site) ^ mix(static_cast<std::uint64_t>(used) + 1));
        active = true;
    }
    SeedScope(SeedScope const&) = delete;
    SeedScope& operator=(SeedScope const&) = delete;
    ~SeedScope() {
        if (!active) return;
        GameToolbox::fast_srand(saved);
        if (counts && seeding().attempt) ++seeding().draws[key];
    }
};
}

namespace {
// 플레이어가 점프 버튼(1번)을 누르고 있는지.
bool holdingJump(PlayerObject* player) {
    auto const& held = player->m_holdingButtons;
    auto const found = held.find(1);
    return found != held.end() && found->second;
}

// 플레이어의 현재 상태를 Sample 하나로 담는다 (위치, 속도, 회전, 중력/지면/사망/버튼 플래그).
Sample capture(PlayerObject* player, std::int32_t step, std::uint8_t index, bool postInput, double time) {
    Sample sample;
    sample.step = step;
    sample.time = time;
    sample.player = index;
    sample.x = player->m_position.x;
    sample.y = player->m_position.y;
    sample.yVelocity = player->m_yVelocity;
    sample.xVelocity = player->m_platformerXVelocity;
    sample.rotation = player->getRotation();
    sample.lastX = player->m_lastPosition.x;
    sample.lastY = player->m_lastPosition.y;
    sample.flags = static_cast<std::uint8_t>(
        (player->m_isUpsideDown ? UpsideDown : 0) | (player->m_isOnGround ? OnGround : 0) |
        (postInput ? PostInput : 0) | (player->m_isDead ? Dead : 0) | (holdingJump(player) ? HoldJump : 0) |
        (player->m_holdingLeft ? HoldLeft : 0) | (player->m_holdingRight ? HoldRight : 0));
    return sample;
}

// 매치 한 판 동안의 녹화 상태. enabled는 녹화 대상 여부, upload는 서버 업로드 여부(라이브 중계만이면 false),
// attempt는 지금 시도 녹화 중인지. current는 진행 중인 시도, best는 지금까지 가장 높은 %의 시도.
// clock/stepTime은 step 길이를 누적한 시간(초)으로, 샘플과 입력에 시각을 달아 준다.
struct Recorder {
    bool enabled = false, attempt = false, finished = false, haveBest = false;
    bool upload = false;
    GJBaseGameLayer* layer = nullptr;
    std::string matchId;
    std::int64_t levelId = 0;
    int levelVersion = 0;
    std::uint64_t levelHash = 0;
    bool lowDetail = false;
    std::int32_t step = -1, postInputStep = -1;
    std::uint64_t seedBase = 0;
    double clock = 0, stepTime = 0;
    float lastDelta = -1.f, lastWarp = -1.f;
    Recording current, best;
};
Recorder& recorder() {
    static Recorder value;
    return value;
}
void streamBegan();
void streamEnded();
// 이번 배틀에서 쓰는 레벨 정보 (id, 버전, 데이터 해시). 매치 시작 때 Recorder로 복사된다.
struct BattleLevel {
    std::int64_t id = 0;
    int version = 0;
    std::uint64_t hash = 0;
    bool lowDetail = false;
};
BattleLevel& battleLevel() {
    static BattleLevel value;
    return value;
}
}

// 배틀 레벨 정보를 기억해 둔다. 이후 beginMatch가 녹화 헤더에 쓴다.
void noteBattleLevel(std::int64_t levelId, int levelVersion, std::uint64_t hash, bool lowDetail) {
    battleLevel() = {levelId, levelVersion, hash, lowDetail};
}

// 매치 시작 때 녹화기를 초기화한다. enabled면 업로드용 녹화, live면 업로드 없이 라이브 전송용으로만 녹화한다.
// 난수 시드는 matchId에서 만들어 모든 클라이언트가 같은 값을 쓴다.
void beginMatch(PlayLayer* layer, std::string matchId, bool enabled, bool live) {
    auto& r = recorder();
    auto const& level = battleLevel();
    r.enabled = (enabled || live) && layer && !matchId.empty();
    r.upload = enabled && r.enabled;
    r.attempt = false;
    r.finished = false;
    r.haveBest = false;
    r.layer = layer;
    r.matchId = std::move(matchId);
    r.levelId = level.id;
    r.levelVersion = level.version;
    r.levelHash = level.hash;
    r.lowDetail = level.lowDetail;
    r.step = -1;
    r.postInputStep = -1;
    r.seedBase = r.enabled ? matchSeed(r.matchId) : 0;
    seeding().end();
    for (auto* recording : {&r.current, &r.best}) {
        recording->inputs.clear();
        recording->samples.clear();
        if (r.enabled) {
            recording->inputs.reserve(8192);
            recording->samples.reserve(1 << 15);
        }
    }
}

// 시도 시작 전에 난수 고정을 미리 걸어 둔다 (resetLevel 도중 트리거가 실행될 수 있기 때문).
void prepareAttempt(PlayLayer* layer, int run) {
    auto& r = recorder();
    if (!r.enabled || r.finished || static_cast<GJBaseGameLayer*>(layer) != r.layer) return;
    seeding().begin(layer, r.seedBase, run);
}

// 새 시도 녹화 시작. current를 비우고 헤더(레벨 정보, run 번호)를 채운 뒤 라이브 전송에 알린다.
void beginAttempt(PlayLayer* layer, int run) {
    auto& r = recorder();
    if (!r.enabled || r.finished || static_cast<GJBaseGameLayer*>(layer) != r.layer) return;
    r.current.inputs.clear();
    r.current.samples.clear();
    r.current.levelId = r.levelId;
    r.current.levelVersion = r.levelVersion;
    r.current.levelHash = r.levelHash;
    r.current.lowDetail = r.lowDetail;
    r.current.run = run;
    r.current.percent = 0;
    r.current.endStep = 0;
    r.current.end = End::Cut;
    r.current.coins = -1;
    r.current.seedBase = r.seedBase;
    r.current.endTime = 0;
    r.current.uneven = false;
    r.step = -1;
    r.postInputStep = -1;
    r.clock = r.stepTime = 0;
    r.lastDelta = r.lastWarp = -1.f;
    r.attempt = true;
    auto& s = seeding();
    if (!s.on(layer) || s.run != run) s.begin(layer, r.seedBase, run);
    streamBegan();
}

// 물리 step이 시작될 때마다 호출된다. step 번호와 누적 시간을 갱신하고, kSampleEvery step마다
// (또는 입력 직후 step에) P1/P2 상태를 샘플로 저장한다. step 0에서는 GD의 난수 시드도 기록한다.
void stepStart(GJBaseGameLayer* layer, float stepDelta) {
    auto& r = recorder();
    if (!r.attempt || layer != r.layer) return;
    std::int32_t const step = ++r.step;
    if (step >= kMaxSteps) {
        r.attempt = false;
        return;
    }
    float const warp = layer->m_gameState.m_timeWarp;
    // time warp가 그대로인데 step 길이가 바뀌면 step이 고르지 않은 환경으로 표시한다.
    if (r.lastDelta >= 0.f && stepDelta != r.lastDelta && warp == r.lastWarp) r.current.uneven = true;
    r.lastDelta = stepDelta;
    r.lastWarp = warp;
    r.stepTime = r.clock;
    r.clock += static_cast<double>(stepDelta);
    if (step == 0) {
        r.current.stepDelta = stepDelta;
        r.current.randomSeed = layer->m_randomSeed;
        r.current.replaySeed = layer->m_replayRandSeed;
    }
    bool const post = step == r.postInputStep;
    if (step % kSampleEvery != 0 && !post) return;
    if (layer->m_player1) r.current.samples.push_back(capture(layer->m_player1, step, 0, post, r.stepTime));
    if (layer->m_gameState.m_isDualMode && layer->m_player2)
        r.current.samples.push_back(capture(layer->m_player2, step, 1, post, r.stepTime));
}

// 버튼 입력 기록. bits: 1비트 눌림/뗌, 1~2비트 버튼 번호(1~3), 8은 P2 입력 표시. 다음 step은 입력 직후 샘플로 남긴다.
void input(GJBaseGameLayer* layer, bool down, int button, bool player1) {
    auto& r = recorder();
    if (!r.attempt || layer != r.layer || r.step < 0 || button < 1 || button > 3) return;
    r.current.inputs.push_back(
        {r.step, static_cast<std::uint8_t>((down ? 1 : 0) | (button << 1) | (player1 ? 0 : 8)), r.stepTime});
    r.postInputStep = r.step + 1;
}

// 시도 종료. 결과(%, 종료 방식, 코인 수)를 채우고 라이브 전송에 마지막 조각을 알린다.
// 지금까지의 best보다 %가 높을 때만 best로 교체한다.
void endAttempt(int percent, End end) {
    auto& r = recorder();
    if (!r.attempt) return;
    r.attempt = false;
    if (seeding().on(r.layer)) seeding().end();
    r.current.percent = std::clamp(percent, 0, 100);
    r.current.end = end;
    r.current.endStep = std::max<std::int32_t>(0, r.step);
    r.current.endTime = r.stepTime;
    auto* play = PlayLayer::get();
    r.current.coins =
        play && static_cast<GJBaseGameLayer*>(play) == r.layer ? std::clamp(play->countCollectedUserCoins(), 0, 3) : -1;
    streamEnded();
    if (r.current.samples.empty()) return;
    if (!r.haveBest || r.current.percent > r.best.percent) {
        std::swap(r.current, r.best);
        r.haveBest = true;
    }
}

// 매치 중단 시 녹화를 폐기하고 난수 고정도 푼다.
void abandonMatch() {
    auto& r = recorder();
    r.enabled = false;
    r.attempt = false;
    r.finished = true;
    r.haveBest = false;
    r.upload = false;
    if (seeding().on(r.layer)) seeding().end();
}

// 매치 종료. best 시도를 백그라운드 스레드에서 인코딩해 업로드한다.
// 크기가 kMaxUploadChars를 넘으면 샘플 간격을 늘려 가며 다시 인코딩하고, 끝내 크면 업로드를 포기한다.
// 업로드 호출은 메인 스레드로 넘겨 처리한다.
void finishMatch() {
    auto& r = recorder();
    if (!r.enabled || r.finished) return;
    r.finished = true;
    r.attempt = false;
    if (seeding().on(r.layer)) seeding().end();
    if (!r.upload) {
        r.haveBest = false;
        return;
    }
    if (!r.haveBest || r.best.samples.empty()) {
        log::info("Versus replay: no recorded attempt for this match");
        return;
    }
    auto recording = std::make_shared<Recording>(std::move(r.best));
    r.haveBest = false;
    // x 좌표가 전혀 변하지 않은 기록이면 경고만 남긴다 (기록 이상 점검용).
    if (recording->endStep > 240 && recording->samples.front().x == recording->samples.back().x)
        log::warn("Versus replay: the player's x position never changed in attempt {} ({} steps)", recording->run,
                  recording->endStep);
    auto matchId = r.matchId;
    std::thread([recording, matchId = std::move(matchId)]() mutable {
        std::string data;
        int used = 0;
        for (int interval : kSampleIntervals) {
            auto raw = encode(*recording, interval);
            data = utils::base64::encode(std::span<std::uint8_t const>(raw), utils::base64::Base64Variant::Url);
            used = interval;
            if (data.size() <= kMaxUploadChars) break;
        }
        if (data.empty() || data.size() > kMaxUploadChars) {
            auto const size = data.size();
            Loader::get()->queueInMainThread(
                [size] { log::warn("Versus replay: too large to upload ({} chars), skipped", size); });
            return;
        }
        ReplayUpload upload;
        upload.matchId = std::move(matchId);
        upload.run = recording->run;
        upload.percent = recording->percent;
        upload.coins = recording->coins;
        upload.end = recording->end == End::Clear ? "clear" : recording->end == End::Death ? "death" : "cut";
        upload.steps = std::max<std::int64_t>(1, recording->endStep + 1);
        upload.inputs = static_cast<std::int64_t>(recording->inputs.size());
        upload.level = recording->levelId;
        upload.data = std::move(data);
        Loader::get()->queueInMainThread([upload = std::move(upload), used]() mutable {
            log::info("Versus replay: attempt {} ({}%) ready, {} chars, samples every {} steps", upload.run,
                      upload.percent, upload.data.size(), used);
            Service::get().uploadReplay(std::move(upload));
        });
    }).detach();
}

namespace {
// 쉼표로 구분된 ID 문자열(노래/효과음 목록)을 양수 ID 집합으로 파싱한다.
void collectIDs(std::set<int>& result, std::string const& encoded) {
    std::string text(encoded);
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        int value = 0;
        auto parsed = std::from_chars(text.data() + start, text.data() + end, value);
        if (parsed.ec == std::errc() && value > 0) result.insert(value);
        start = end + 1;
    }
}

// 샘플의 위치/속도/회전/중력 방향을 플레이어에 그대로 덮어쓴다.
void applySample(PlayerObject* player, Sample const& sample) {
    bool const upsideDown = sample.flags & UpsideDown;
    if (player->m_isUpsideDown != upsideDown) player->flipGravity(upsideDown, true);
    CCPoint const point{static_cast<float>(sample.x), static_cast<float>(sample.y)};
    player->setPosition(point);
    player->m_position = point;
    player->m_lastPosition = CCPoint(sample.lastX, sample.lastY);
    player->m_yVelocity = sample.yVelocity;
    player->m_platformerXVelocity = sample.xVelocity;
    player->setRotation(sample.rotation);
}

// 저장된 리플레이를 재생하는 컨트롤러 (싱글턴 노드, 매 프레임 update).
// Phase 순서: Fetching(서버에서 받기) -> Map(맵 캐시) -> Resources(노래/효과음 다운로드)
// -> Entering(PlayLayer 생성) -> Ready -> Playing -> Ended.
// 재생 방식은 세 가지다: 같은 step 길이면 step 그대로(exact), 다른 TPS이면 비율로 대응(mapped),
// 고르지 않은 step이면 시간 기준(timedMode). 재생 중 실제 입력은 막고 기록된 입력만 주입(injecting)한다.
class Viewer final : public CCNode {
public:
    enum class Phase { Idle, Fetching, Map, Resources, Entering, Ready, Playing, Ended };
    Phase phase = Phase::Idle;
    ReplayInfo info;
    Recording recording;
    Ref<GJGameLevel> level;
    Weak<PlayLayer> play;
    Weak<CCNode> hud;
    Ref<Notification> notice;
    std::set<int> songs, effects, requestedSongs, requestedEffects;
    // 상태 플래그: constructing은 PlayLayer 생성 중, entered는 씬 진입 완료, armed는 재생 시작됨,
    // inputsOnly는 위치 없이 입력만 있는 오래된 포맷, levelChanged는 맵이 녹화 후 바뀜, tpsDiffers는 TPS 차이.
    bool constructing = false, entered = false, armed = false, injecting = false, starting = false;
    bool ended = false, endShown = false, restart = false, frozen = false, levelChanged = false, tpsDiffers = false;
    bool audioMissing = false;
    bool contentRequested = false;
    bool mapped = false;
    double ratio = 1.0;
    float viewerDelta = 0.f;
    bool timedMode = false;
    double viewerStart = 0, viewerNext = 0;
    float viewerLastDelta = -1.f, viewerLastWarp = -1.f;
    TimedCursor timed[2];
    std::vector<std::uint32_t> perPlayer[2];
    std::size_t cursor[2] = {0, 0};
    std::int32_t lastApplied[2] = {-1, -1};
    Weak<CCLabelBMFont> notesLabel;
    bool inputsOnly = false;
    int driftedAt = -1;
    float largestCorrection = 0.f;
    std::int32_t step = -1;
    std::size_t nextInput = 0, nextSample = 0;
    float timer = 0.f, endTimer = 0.f;
    Clock::time_point began;
    std::uint64_t generation = 0;

    inline static Viewer* instance = nullptr;
    static Viewer& get() {
        static auto* viewer = [] {
            auto* node = new Viewer;
            node->init();
            instance = node;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 3, false);
            return node;
        }();
        return *viewer;
    }
    static Viewer* peek() { return instance; }
    bool owns(GJBaseGameLayer const* layer) const {
        if (phase == Phase::Idle || !layer) return false;
        auto owner = play.lock();
        return owner && static_cast<GJBaseGameLayer const*>(owner.data()) == layer;
    }
    bool busy() const { return phase != Phase::Idle; }
    bool preparing() const {
        return phase == Phase::Fetching || phase == Phase::Map || phase == Phase::Resources || phase == Phase::Entering;
    }

    // 화면 알림을 띄운다 (Loading 아이콘이면 계속 표시, 아니면 3초).
    void say(std::string const& text, NotificationIcon icon = NotificationIcon::Loading) {
        if (notice) notice->hide();
        notice = Notification::create(text, icon, icon == NotificationIcon::Loading ? 0.f : 3.f);
        notice->show();
    }
    // 재생 준비 실패 처리. 씬 밖이면 알림창으로, 씬 안이면 로그로 알리고 상태를 초기화한다.
    void fail(std::string const& text) {
        if (notice) {
            notice->hide();
            notice = nullptr;
        }
        bool const inScene = play.lock() != nullptr;
        reset();
        if (!inScene)
            FLAlertLayer::create("Replay", text, "OK")->show();
        else
            log::warn("Versus replay: {}", text);
    }
    // 모든 상태를 초기화한다. generation을 올려 진행 중이던 비동기 콜백이 무시되게 한다.
    void reset() {
        ++generation;
        phase = Phase::Idle;
        recording = {};
        level = nullptr;
        play = nullptr;
        hud = nullptr;
        songs.clear();
        effects.clear();
        requestedSongs.clear();
        requestedEffects.clear();
        constructing = entered = armed = injecting = starting = ended = endShown = restart = frozen = false;
        levelChanged = tpsDiffers = contentRequested = inputsOnly = mapped = timedMode = audioMissing = false;
        ratio = 1.0;
        viewerDelta = 0.f;
        viewerStart = viewerNext = 0;
        viewerLastDelta = viewerLastWarp = -1.f;
        timed[0] = timed[1] = {};
        perPlayer[0].clear();
        perPlayer[1].clear();
        notesLabel = nullptr;
        driftedAt = -1;
        step = -1;
        nextInput = nextSample = 0;
        seeding().end();
    }
    // 지금 재생 위치를 기록 기준 step 번호로 환산한다.
    std::int32_t recordedNow() const {
        if (timedMode) return stepAtTime(recording, viewerStart);
        return mapped ? static_cast<std::int32_t>(std::floor(step * ratio + 1e-6)) : step;
    }

    // 리플레이 재생 시작. 매치 진행 중이거나 방에 있으면 거부하고, 서버에서 리플레이 파일을 받아 loaded()로 넘긴다.
    // 콜백은 generation(epoch)이 같을 때만 처리한다.
    void start(ReplayInfo value) {
        if (busy()) return;
        if (versus::isMatchLaunchActive() || Service::get().room()) {
            FLAlertLayer::create("Replay", "Leave the room before watching a replay.", "OK")->show();
            return;
        }
        reset();
        info = std::move(value);
        phase = Phase::Fetching;
        say("Loading replay...");
        auto const epoch = generation;
        Service::get().fetchReplay(info.matchId, info.uid,
                                   [this, epoch](std::optional<ReplayFile> file, std::string error) {
                                       if (epoch != generation) return;
                                       if (!file) {
                                           fail(error.empty() ? "Replay unavailable." : error);
                                           return;
                                       }
                                       loaded(std::move(*file));
                                   });
    }
    // 받은 리플레이를 base64 -> Recording으로 해석하고 플레이어별 샘플 인덱스를 만든다.
    // 이어서 맵을 캐시에서 준비하고 필요한 노래/효과음 ID를 모은다. 오디오를 못 받으면 없이 재생한다.
    void loaded(ReplayFile file) {
        auto decoded = utils::base64::decode(file.data, utils::base64::Base64Variant::Url);
        if (decoded.isErr()) {
            fail("The replay data is damaged.");
            return;
        }
        auto raw = std::move(decoded).unwrap();
        auto parsed = decode(std::span<std::uint8_t const>(raw));
        if (!parsed || parsed->samples.empty()) {
            fail("This replay was made by a newer Versus or is damaged.");
            return;
        }
        recording = std::move(*parsed);
        recording.coins = file.coins >= 0                                        ? file.coins
                          : recording.end == End::Clear && file.serverCoins >= 0 ? file.serverCoins
                                                                                 : -1;
        inputsOnly = recording.format < 2;
        for (std::uint32_t i = 0; i < recording.samples.size(); ++i)
            perPlayer[recording.samples[i].player ? 1 : 0].push_back(i);
        if (info.name.empty()) info.name = file.name;
        phase = Phase::Map;
        say("Downloading the map...");
        auto const epoch = generation;
        versus::ensureMapCached(
            recording.levelId,
            [this, epoch](bool ok, std::string detail) {
                if (epoch != generation) return;
                if (!ok) {
                    fail(detail.empty() ? "The map could not be downloaded." : detail);
                    return;
                }
                level = versus::cachedLevel(recording.levelId);
                if (!level) {
                    fail("The map could not be loaded.");
                    return;
                }
                if (level->m_songID > 0) songs.insert(level->m_songID);
                collectIDs(songs, level->m_songIDs);
                collectIDs(effects, level->m_sfxIDs);
                if (!detail.empty()) {
                    audioMissing = true;
                    auto* music = MusicDownloadManager::sharedState();
                    std::erase_if(songs, [music](int id) { return !music->isSongDownloaded(id); });
                    std::erase_if(effects, [music](int id) { return !music->isSFXDownloaded(id); });
                }
                phase = Phase::Resources;
                began = Clock::now();
                timer = 0.f;
                say("Downloading the music...");
            },
            versus::AudioNeed::Ranked);
    }
    // 필요한 노래/효과음이 모두 다운로드됐는지. 안 된 것은 한 번씩 다운로드를 요청한다.
    // 커스텀 콘텐츠 URL이 필요한 경우 먼저 그 URL부터 받아 온다.
    bool resourcesReady() {
        auto* music = MusicDownloadManager::sharedState();
        bool needsURL =
            std::any_of(effects.begin(), effects.end(), [music](int id) { return !music->isSFXDownloaded(id); }) ||
            std::any_of(songs.begin(), songs.end(),
                        [music](int id) { return id > 10000000 && !music->isSongDownloaded(id); });
        if (needsURL && music->m_customContentURL.empty()) {
            if (!contentRequested) {
                contentRequested = true;
                music->getCustomContentURL();
            }
            return false;
        }
        bool ready = true;
        for (int id : songs) {
            if (music->isSongDownloaded(id)) continue;
            ready = false;
            if (requestedSongs.insert(id).second) music->downloadSong(id);
        }
        for (int id : effects) {
            if (music->isSFXDownloaded(id)) continue;
            ready = false;
            if (requestedEffects.insert(id).second) music->downloadSFX(id);
        }
        return ready;
    }
    // 녹화한 레벨과 같은 방식으로 배틀용 레벨 복사본을 만들어 PlayLayer 씬으로 들어간다.
    // 레벨 데이터 해시가 녹화 때와 다르면 levelChanged로 표시한다. 진입 중에는 배틀 모드/통계를 따로 보관한다.
    void enter() {
        if (!level) {
            fail("The map could not be loaded.");
            return;
        }
        std::uint64_t hash = 0;
        auto* copy = versus::createBattleLevel(level.data(), recording.lowDetail, hash);
        if (!copy) {
            fail("The map could not be decoded.");
            return;
        }
        levelChanged = recording.levelHash != 0 && hash != recording.levelHash;
        phase = Phase::Entering;
        versus::snapshotBattleLevelStats(static_cast<int>(recording.levelId));
        versus::pauseBattleMods();
        constructing = true;
        auto* scene = versus::compat::playScene(copy);
        constructing = false;
        if (!scene || !play.lock()) {
            versus::restoreBattleLevelStats();
            fail("The map could not be opened.");
            return;
        }
        if (notice) {
            notice->hide();
            notice = nullptr;
        }
        level = nullptr;
        CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.5f, scene));
    }
    // HUD에 표시할 코인 문구 (레벨에 코인이 없으면 빈 문자열).
    std::string coinsText(PlayLayer* layer, char const* before) const {
        int const total = layer && layer->m_level ? std::clamp(layer->m_level->m_coins, 0, 3) : 0;
        if (recording.coins < 0 || (total == 0 && recording.coins == 0)) return {};
        return total > 0 ? fmt::format("{}Coins {}/{}", before, recording.coins, total)
                         : fmt::format("{}Coins {}", before, recording.coins);
    }
    // 상단에 리플레이 제목(플레이어, 시도 번호, %)과 안내 문구 라벨을 붙인다.
    void installHud(PlayLayer* layer) {
        auto const window = CCDirector::sharedDirector()->getWinSize();
        auto* root = CCNode::create();
        root->setID("versus-replay-hud"_spr);
        auto* title = CCLabelBMFont::create(
            fmt::format("REPLAY  -  {}  -  Attempt {}  -  {}%{}{}", info.name, recording.run, recording.percent,
                        recording.end == End::Clear ? " (clear)" : "", coinsText(layer, "  -  "))
                .c_str(),
            "goldFont.fnt");
        title->setScale(.5f);
        title->limitLabelWidth(window.width - 120.f, .5f, .2f);
        title->setPosition({window.width / 2.f, window.height - 52.f});
        root->addChild(title);
        auto* warning = CCLabelBMFont::create("", "chatFont.fnt");
        warning->setColor(ccc3(255, 150, 120));
        warning->setPosition({window.width / 2.f, window.height - 70.f});
        root->addChild(warning);
        rebind(notesLabel, warning);
        layer->addChild(root, 100005);
        rebind(hud, root);
        refreshNotes();
    }
    // 상황에 맞는 안내 문구 갱신: 맵 변경, 오디오 없음, TPS 차이, 위치 정보 없는 오래된 기록 등.
    void refreshNotes() {
        auto label = notesLabel.lock();
        if (!label) return;
        std::string notes;
        if (levelChanged) notes += "The map was updated after this match - it may not match. ";
        if (audioMissing) notes += "The map's song or sounds could not be downloaded: played without them. ";
        if (mapped && viewerDelta > 0.f && recording.stepDelta > 0.f)
            notes += fmt::format("Recorded at {:.0f} TPS, shown at your {:.0f} TPS (smoothed). ",
                                 1.f / recording.stepDelta, 1.f / viewerDelta);
        if (timedMode && viewerDelta > 0.f) {
            double const rate = recording.endTime > .5      ? recording.endStep / recording.endTime
                                : recording.stepDelta > 0.f ? 1.0 / recording.stepDelta
                                                            : 0.0;
            notes += fmt::format("Recorded at {:.0f} TPS{}, shown at your {:.0f} TPS in real time. ", rate,
                                 recording.uneven ? " (uneven steps)" : "", 1.f / viewerDelta);
        }
        if (inputsOnly)
            notes += "Recorded by an old version without positions: played from the inputs only, it may drift. ";
        if (recording.format >= 2 && !recording.seedBase)
            notes += "Recorded by an old version: random triggers may differ. ";
        label->setString(notes.c_str());
        label->limitLabelWidth(CCDirector::sharedDirector()->getWinSize().width - 40.f, .6f, .25f);
    }
    // 재생 시작. 난수 고정 후 게임을 시작시키고(startGame hook은 이때만 통과), 두 번째 이후 시도면 resetLevel로 그 시도까지 넘긴다.
    void go(PlayLayer* layer) {
        if (armed) return;
        seeding().begin(layer, recording.seedBase, 1);
        starting = true;
        layer->startGame();
        starting = false;
        refreshBattleStartMode(layer);
        armed = true;
        rearm();
        if (recording.run > 1) layer->resetLevel();
        phase = Phase::Playing;
    }
    // 재생 위치/커서를 처음으로 되돌린다 (재시작 때도 사용).
    void rearm() {
        step = -1;
        nextInput = nextSample = 0;
        cursor[0] = cursor[1] = 0;
        lastApplied[0] = lastApplied[1] = -1;
        ended = endShown = frozen = false;
        endTimer = 0.f;
        driftedAt = -1;
        largestCorrection = 0.f;
        viewerStart = viewerNext = 0;
        viewerLastDelta = viewerLastWarp = -1.f;
        timed[0] = timed[1] = {};
    }
    // 재생 중 매 물리 step 시작에 호출. step 0에서 기록된 시드를 복원하고 재생 방식을 결정한다:
    // TPS가 1% 넘게 다르거나 step이 고르지 않으면 timedMode, 정수에 가까운 비율이면 mapped.
    // 그 외에는 해당 step의 샘플을 플레이어에 적용하고 그 step의 입력을 주입한다.
    void stepStart(GJBaseGameLayer* layer, float stepDelta) {
        std::int32_t const now = ++step;
        float const warp = layer->m_gameState.m_timeWarp;
        if (now == 0) {
            layer->m_randomSeed = recording.randomSeed;
            layer->m_replayRandSeed = recording.replaySeed;
            bool const otherRate =
                recording.stepDelta > 0.f && std::abs(stepDelta - recording.stepDelta) > recording.stepDelta * .01f;
            if (recording.timed() && !inputsOnly && (otherRate || recording.uneven)) {
                if (otherRate && !tpsDiffers)
                    log::warn("Versus replay: recorded step {} s, this game {} s (played by time)", recording.stepDelta,
                              stepDelta);
                tpsDiffers = otherRate;
                timedMode = true;
                viewerDelta = stepDelta;
                refreshNotes();
            } else if (otherRate) {
                if (!tpsDiffers)
                    log::warn("Versus replay: recorded step {} s, this game {} s", recording.stepDelta, stepDelta);
                tpsDiffers = true;
                double value = static_cast<double>(stepDelta) / recording.stepDelta;
                if (std::abs(value - std::round(value)) < .002)
                    value = std::round(value);
                else if (value < 1.0 && std::abs(1.0 / value - std::round(1.0 / value)) < .002)
                    value = 1.0 / std::round(1.0 / value);
                mapped = !inputsOnly && value > 0.0 && std::isfinite(value);
                ratio = mapped ? value : 1.0;
                viewerDelta = stepDelta;
                refreshNotes();
            }
        }
        // 재생 도중 step 길이가 갑자기 달라지면(예: 프레임 변동) 그 지점부터 시간 기준으로 바꾼다.
        else if (!timedMode && !mapped && recording.timed() && !inputsOnly && viewerLastDelta >= 0.f &&
                 stepDelta != viewerLastDelta && warp == viewerLastWarp) {
            timedMode = true;
            viewerDelta = stepDelta;
            viewerNext = timeAtStep(recording, now);
            log::info("Versus replay: this game's steps are uneven from step {}: played by time", now);
            refreshNotes();
        }
        viewerLastDelta = stepDelta;
        viewerLastWarp = warp;
        if (timedMode) {
            viewerStart = now == 0 ? 0.0 : viewerNext;
            viewerNext = viewerStart + static_cast<double>(stepDelta);
            stepTimed(layer, now, stepDelta);
            return;
        }
        if (mapped) {
            stepMapped(layer, now);
            return;
        }
        auto& samples = recording.samples;
        while (nextSample < samples.size() && samples[nextSample].step < now) ++nextSample;
        while (nextSample < samples.size() && samples[nextSample].step == now) {
            auto const& sample = samples[nextSample++];
            auto* player = sample.player ? layer->m_player2 : layer->m_player1;
            if (!player || (sample.player && !layer->m_gameState.m_isDualMode)) continue;
            // 죽은 플레이어에는 적용하지 않는다. 기록 위치와의 차이는 largestCorrection으로 모아 로그에 남긴다.
            if (!player->m_isDead && !inputsOnly) {
                if (now > 0)
                    largestCorrection =
                        std::max(largestCorrection, static_cast<float>(std::abs(player->m_position.x - sample.x) +
                                                                       std::abs(player->m_position.y - sample.y)));
                applySample(player, sample);
            }
            // step 0에서는 샘플 시점에 눌려 있던 버튼을 먼저 눌러 둔다 (홀드 상태 복원).
            if (now == 0) {
                bool const p1 = sample.player == 0;
                injecting = true;
                if ((sample.flags & HoldJump) && !holdingJump(player)) layer->handleButton(true, 1, p1);
                if ((sample.flags & HoldLeft) && !player->m_holdingLeft) layer->handleButton(true, 2, p1);
                if ((sample.flags & HoldRight) && !player->m_holdingRight) layer->handleButton(true, 3, p1);
                injecting = false;
            }
        }
        // 이 step에 실제로 들어온 입력은 버리고, 기록된 입력만 주입한다.
        layer->m_queuedButtons.clear();
        auto& inputs = recording.inputs;
        while (nextInput < inputs.size() && inputs[nextInput].step < now) ++nextInput;
        injecting = true;
        while (nextInput < inputs.size() && inputs[nextInput].step == now) {
            auto const bits = inputs[nextInput++].bits;
            layer->handleButton(bits & 1, (bits >> 1) & 3, !(bits & 8));
        }
        injecting = false;
    }
    // 다른 TPS로 재생할 때의 step 처리. 이 step이 기록의 어느 지점(now*ratio)에 해당하는지 구해
    // 앞뒤 샘플 사이를 보간해 적용하고, 그 시점까지의 입력을 주입한다.
    void stepMapped(GJBaseGameLayer* layer, std::int32_t now) {
        auto const& samples = recording.samples;
        double const at = now * ratio;
        for (int p = 0; p < 2; ++p) {
            auto* player = p ? layer->m_player2 : layer->m_player1;
            auto const& list = perPlayer[p];
            if (!player || list.empty() || (p && !layer->m_gameState.m_isDualMode)) continue;
            auto& c = cursor[p];
            while (c + 1 < list.size() && samples[list[c + 1]].step <= at + 1e-6) ++c;
            auto const& a = samples[list[c]];
            if (a.step > at + 1e-6) continue;
            Sample state = a;
            if (c + 1 < list.size()) {
                auto const& b = samples[list[c + 1]];
                double const t = (at - a.step) / std::max(1, b.step - a.step);
                if (t > 1e-9 && continuous(a, b, recording.stepDelta))
                    state = blend(a, b, std::min(t, 1.0));
                else if (lastApplied[p] == a.step)
                    continue;
            } else if (lastApplied[p] == a.step)
                continue;
            if (!player->m_isDead) applySample(player, state);
            lastApplied[p] = a.step;
            if (now == 0) {
                bool const p1 = p == 0;
                injecting = true;
                if ((a.flags & HoldJump) && !holdingJump(player)) layer->handleButton(true, 1, p1);
                if ((a.flags & HoldLeft) && !player->m_holdingLeft) layer->handleButton(true, 2, p1);
                if ((a.flags & HoldRight) && !player->m_holdingRight) layer->handleButton(true, 3, p1);
                injecting = false;
            }
        }
        layer->m_queuedButtons.clear();
        auto const& inputs = recording.inputs;
        injecting = true;
        while (nextInput < inputs.size() &&
               static_cast<std::int32_t>(std::floor(inputs[nextInput].step / ratio + 1e-6)) <= now) {
            auto const bits = inputs[nextInput++].bits;
            layer->handleButton(bits & 1, (bits >> 1) & 3, !(bits & 8));
        }
        injecting = false;
    }
    // 시간 기준 재생. 이 step의 시작 시각(viewerStart)에 맞는 샘플 상태를 구해 적용하고,
    // 다음 step 시작 전까지의 입력을 주입한다.
    void stepTimed(GJBaseGameLayer* layer, std::int32_t now, float stepDelta) {
        double const at = viewerStart;
        for (int p = 0; p < 2; ++p) {
            auto* player = p ? layer->m_player2 : layer->m_player1;
            if (!player || (p && !layer->m_gameState.m_isDualMode)) continue;
            auto const state = timedState(recording, perPlayer[p], timed[p], at);
            if (!state) continue;
            if (!player->m_isDead) {
                if (now > 0)
                    largestCorrection =
                        std::max(largestCorrection, static_cast<float>(std::abs(player->m_position.x - state->x) +
                                                                       std::abs(player->m_position.y - state->y)));
                applySample(player, *state);
            }
            if (now == 0) {
                bool const p1 = p == 0;
                injecting = true;
                if ((state->flags & HoldJump) && !holdingJump(player)) layer->handleButton(true, 1, p1);
                if ((state->flags & HoldLeft) && !player->m_holdingLeft) layer->handleButton(true, 2, p1);
                if ((state->flags & HoldRight) && !player->m_holdingRight) layer->handleButton(true, 3, p1);
                injecting = false;
            }
        }
        layer->m_queuedButtons.clear();
        auto const& inputs = recording.inputs;
        auto const last = timedInputsUntil(recording, nextInput, at + static_cast<double>(stepDelta));
        injecting = true;
        while (nextInput < last) {
            auto const bits = inputs[nextInput++].bits;
            layer->handleButton(bits & 1, (bits >> 1) & 3, !(bits & 8));
        }
        injecting = false;
    }
    // 기록된 시도의 끝에 도달했을 때 처리. Death: 기록 종료 2step 후에도 안 죽었으면 강제로 죽인다.
    // Clear: 약 1초(스텝 수는 30~2000으로 제한) 기다려 클리어를 기다리고, 안 되면 화면을 멈춘다.
    // Cut: 매치가 그 시도 도중 끝난 것이므로 그대로 멈춘다.
    void stepEnd(PlayLayer* layer) {
        auto const at = recordedNow();
        if (ended || at < recording.endStep) return;
        bool const dead = (layer->m_player1 && layer->m_player1->m_isDead) ||
                          (layer->m_gameState.m_isDualMode && layer->m_player2 && layer->m_player2->m_isDead);
        if (recording.end == End::Death) {
            if (!dead && at >= recording.endStep + 2 && layer->m_player1) {
                layer->destroyPlayer(layer->m_player1, nullptr);
            }
            if (dead || at >= recording.endStep + 2) ended = true;
        } else if (recording.end == End::Clear) {
            std::int32_t const wait =
                recording.stepDelta > 0.f
                    ? std::clamp(static_cast<std::int32_t>(std::lround(1.0 / recording.stepDelta)), 30, 2000)
                    : 240;
            if (layer->m_hasCompletedLevel || at >= recording.endStep + wait) ended = true;
            if (ended && !layer->m_hasCompletedLevel) frozen = true;
        } else {
            ended = true;
            frozen = true;
        }
    }
    // 재생이 끝나면 결과 팝업(나가기 / 다시 보기)을 띄운다. 데스크톱에서는 커서를 풀어 준다.
    void showEnd(PlayLayer* layer) {
        if (endShown) return;
        endShown = true;
        log::info("Versus replay: attempt {} played to step {} of {}; largest position correction {:.3f}{}{}",
                  recording.run, recordedNow(), recording.endStep, largestCorrection,
                  inputsOnly ? " (inputs only)" : "",
                  mapped      ? fmt::format(" (other TPS: {:.3f} recorded steps per step)", ratio)
                  : timedMode ? fmt::format(" (by time: {:.3f} s of {:.3f} s{})", viewerStart, recording.endTime,
                                            recording.uneven ? ", uneven steps" : "")
                              : std::string());
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
        auto const epoch = generation;
        auto text =
            fmt::format("<cy>{}</c> reached <cg>{}%</c> on attempt {}{}.", info.name, recording.percent, recording.run,
                        recording.end == End::Clear ? " (level cleared)"
                        : recording.end == End::Cut ? " (the match ended during this attempt)"
                                                    : "");
        if (auto const coins = coinsText(layer, ""); !coins.empty()) text += fmt::format("\n<cy>{}</c>", coins);
        if (driftedAt >= 0)
            text += fmt::format("\n<co>This old replay has no positions and drifted: it died at {}%.</c>", driftedAt);
        createQuickPopup("Replay finished", text, "Exit", "Watch again",
                         [this, epoch, layer = Weak<PlayLayer>(layer)](FLAlertLayer*, bool again) {
                             auto owner = layer.lock();
                             if (epoch != generation || !owner) return;
                             if (again) {
                                 restart = true;
                                 owner->resetLevel();
                             } else
                                 owner->onQuit();
                         });
    }
    // 매 프레임: 리소스 단계는 0.25초마다 확인하고 20초가 지나면 오디오 없이 진행한다.
    // Ready 단계에서 1초 뒤 재생을 시작하고, 끝난 뒤 1.2초 지나면 결과 팝업을 띄운다.
    void update(float dt) override {
        if (phase == Phase::Resources) {
            timer -= dt;
            if (timer > 0.f) return;
            timer = .25f;
            if (resourcesReady())
                enter();
            else if (Clock::now() - began > std::chrono::seconds(20)) {
                audioMissing = true;
                enter();
            }
            return;
        }
        if (phase != Phase::Ready && phase != Phase::Playing && phase != Phase::Ended) return;
        auto owner = play.lock();
        if (!owner) {
            reset();
            return;
        }
        if (phase == Phase::Ready) {
            timer -= dt;
            if (timer <= 0.f && entered) go(owner.data());
            return;
        }
        if (ended && !endShown && !owner->m_isPaused) {
            endTimer += dt;
            if (endTimer >= 1.2f) showEnd(owner.data());
        }
    }
};
}

// 공개 API: 리플레이 보기 시작 / 보는 중인지 / 준비 중인지 / 뷰어에서 나가기.
void watch(ReplayInfo info) {
    Viewer::get().start(std::move(info));
}
bool viewing() {
    auto* viewer = Viewer::peek();
    return viewer && (viewer->constructing || viewer->play.lock());
}
bool preparing() {
    auto* viewer = Viewer::peek();
    return viewer && viewer->preparing();
}
// 뷰어를 끝내고 이전 화면으로 복귀한다. 보관해 둔 레벨 통계를 되돌리고 메뉴 음악을 다시 튼다.
bool leaveViewer() {
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->play.lock()) return false;
    viewer->reset();
    versus::restoreBattleLevelStats();
    CCDirector::sharedDirector()->popSceneWithTransition(.5f, kPopTransitionFade);
    GameManager::sharedState()->fadeInMenuMusic();
    return true;
}

namespace {
// 라이브 설정값. kLiveSendEvery/kLivePollEvery는 전송/폴링 주기(초), kLiveSampleInterval은 라이브 샘플 간격(step),
// kLivePartSamples/kLivePartInputs는 한 조각의 최대 샘플/입력 수, kLiveStuck은 같은 조각 응답이 반복될 때 포기하는 횟수.
constexpr float kLiveSendEvery = .4f;
constexpr float kLivePollEvery = .5f;
constexpr int kLiveSampleInterval = 8;
constexpr std::size_t kLivePartSamples = 600, kLivePartInputs = 2000;
constexpr int kLiveStuck = 3;
// kLiveDrain: 중계가 끝난 뒤에도 남은 조각을 보내 주는 시간. kLiveNoParts: 조각 요청 없이 상태만 묻는 인덱스.
// Lag 값들(초): 보는 쪽이 유지하려는 버퍼 길이(시작 2.0, 1.5~5.0), kLiveBehind는 이만큼 더 뒤처지면 빨리 감기.
// kLiveChunk 계열은 빨리 감기 한 번에 돌리는 시간 조각(초), kLiveBudget은 프레임당 빨리 감기에 쓰는 시간 상한,
// kLiveStaleSeconds는 더 새 시도가 있는데 조각이 안 올 때 낡은 시도로 보는 기준.
constexpr auto kLiveDrain = std::chrono::seconds(5);
constexpr int kLiveNoParts = 1'000'000;
constexpr double kLiveLagStart = 2.0, kLiveLagMin = 1.5, kLiveLagMax = 5.0;
constexpr double kLiveBehind = 2.5;
constexpr float kLiveChunk = 1.f / 15.f;
constexpr float kLiveChunkMin = 1.f / 60.f, kLiveChunkMax = .25f;
constexpr auto kLiveBudget = std::chrono::milliseconds(12);
constexpr float kLiveStaleSeconds = 3.f;

// 내가 보내는 시도 하나의 전송 큐. parts는 base64로 인코딩한 조각들, acked는 서버가 받았다고 확인한 개수,
// stuck은 같은 조각을 서버가 계속 다시 요구한 횟수.
struct Outgoing {
    int run = 0;
    std::vector<std::string> parts;
    std::size_t acked = 0;
    LiveCursor cursor;
    int stuck = 0;
    bool ended() const { return cursor.ended; }
};

// 녹화 중인 시도에서 bound step 직전까지를 새 조각으로 잘라 큐에 넣는다. 새 내용이 없으면 false.
bool cutPart(Recording const& source, Outgoing& out, std::int32_t bound, double boundTime, bool ending) {
    auto part = cutLivePart(source, out.cursor, bound, boundTime, ending, kLiveSampleInterval, kLivePartSamples,
                            kLivePartInputs);
    if (!part) return false;
    auto raw = encodeLive(*part);
    out.parts.push_back(utils::base64::encode(std::span<std::uint8_t const>(raw), utils::base64::Base64Variant::Url));
    return true;
}

// P1 또는 듀얼 모드의 P2가 죽었는지.
bool deadPlayer(GJBaseGameLayer* layer) {
    return (layer->m_player1 && layer->m_player1->m_isDead) ||
           (layer->m_gameState.m_isDualMode && layer->m_player2 && layer->m_player2->m_isDead);
}

// 라이브 컨트롤러 (싱글턴 노드). 두 가지 역할: 내 시도를 서버로 중계(stream*)하고,
// 다른 사람의 중계를 받아 같은 맵 위에서 재생(watch*, step*)한다. 재생은 서버에서 받은 데이터의 약간 뒤를
// 따라가며(lagTarget 만큼 버퍼) 데이터가 모자라면 멈추고(buffering), 많이 뒤처지면 빨리 감는다.
class Live final : public CCNode {
public:
    inline static Live* instance = nullptr;
    static Live& get() {
        static auto* live = [] {
            auto* node = new Live;
            node->init();
            instance = node;
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 3, false);
            return node;
        }();
        return *live;
    }
    static Live* peek() { return instance; }

    LiveTarget streamTarget;
    std::string streamRoom;
    std::deque<Outgoing> outgoing;
    bool sending = false, wanted = false;
    float sendTimer = 0.f;
    int failures = 0;
    Clock::time_point drainUntil{};
    std::uint64_t streamEpoch = 0;

    // 중계 대상을 설정한다. 비어 있으면 중계를 끝내고 남은 조각만 잠시 더 보낸다(drain).
    // 방/전투가 바뀌면 큐를 비우고 새로 시작한다.
    void stream(LiveTarget const& me) {
        if (me.empty()) {
            if (wanted) {
                wanted = false;
                drainUntil = Clock::now() + kLiveDrain;
            }
            return;
        }
        auto room = me.key();
        if (room == streamRoom) {
            if (!wanted) {
                wanted = true;
                for (auto& out : outgoing) out.stuck = 0;
                attemptBegan();
            }
            return;
        }
        ++streamEpoch;
        streamRoom = std::move(room);
        streamTarget = me;
        streamTarget.of.clear();
        outgoing.clear();
        sending = false;
        sendTimer = 0.f;
        failures = 0;
        wanted = true;
        log::info("Versus live: streaming my attempts (room {}, battle {})", streamTarget.roomId,
                  streamTarget.battleId);
        attemptBegan();
    }
    // 새 시도가 시작되면 전송 큐에 그 시도를 추가한다. 큐는 최근 2개 시도만 유지한다.
    void attemptBegan() {
        auto& r = recorder();
        if (streamRoom.empty() || !wanted || !r.enabled || !r.attempt) return;
        if (!outgoing.empty() && outgoing.back().run == r.current.run) return;
        if (!outgoing.empty() && !outgoing.back().ended() && outgoing.back().acked == outgoing.back().parts.size())
            outgoing.pop_back();
        outgoing.emplace_back();
        outgoing.back().run = outgoing.back().cursor.run = r.current.run;
        while (outgoing.size() > 2) outgoing.pop_front();
        sendTimer = 0.f;
    }
    // 시도가 끝나면 남은 구간을 마지막 조각(종료 정보 포함)까지 모두 잘라 큐에 넣는다.
    void attemptEnded() {
        auto& r = recorder();
        if (streamRoom.empty() || !wanted || outgoing.empty() || outgoing.back().run != r.current.run ||
            outgoing.back().ended())
            return;
        auto& out = outgoing.back();
        double const step = r.current.stepDelta > 0.f ? r.current.stepDelta : 1.0 / 240.0;
        for (int guard = 0; guard < 4096 && !out.ended(); ++guard)
            if (!cutPart(r.current, out, r.current.endStep + 1, r.current.endTime + step, true)) break;
        sendTimer = 0.f;
    }
    // 전송 주기 처리. 보낼 차례가 되면 진행 중인 시도의 새 구간을 조각으로 자르고, 아직 확인되지 않은
    // 가장 오래된 조각 하나를 서버에 보낸다. 실패하면 1, 2, 4, 8초로 간격을 늘려 다시 시도한다.
    // 응답에는 서버가 가진 조각 수가 있어 그에 맞춰 acked를 갱신한다 (같은 조각을 3번 요구하면 그 시도는 포기).
    void tickStream(float dt) {
        if (streamRoom.empty()) return;
        if (!wanted && Clock::now() > drainUntil) return;
        sendTimer -= dt;
        if (sending || sendTimer > 0.f) return;
        auto& r = recorder();
        if (wanted && r.attempt && !outgoing.empty() && outgoing.back().run == r.current.run &&
            !outgoing.back().ended() && outgoing.back().stuck < kLiveStuck) {
            auto& out = outgoing.back();
            if (out.acked == out.parts.size() && r.step > 0) cutPart(r.current, out, r.step, r.stepTime, false);
        }
        while (outgoing.size() > 1 && outgoing.front().acked >= outgoing.front().parts.size()) outgoing.pop_front();
        Outgoing* next = nullptr;
        for (auto& out : outgoing)
            if (out.stuck < kLiveStuck && out.acked < out.parts.size()) {
                next = &out;
                break;
            }
        if (!next) {
            sendTimer = kLiveSendEvery;
            return;
        }
        sending = true;
        int const run = next->run;
        auto const index = next->acked;
        auto const epoch = streamEpoch;
        Service::get().exchangeLive(
            streamTarget, LiveSend{run, static_cast<int>(index), next->parts[index]}, std::nullopt,
            [this, epoch, run, index](std::optional<LiveAnswer> answer, std::string) {
                if (epoch != streamEpoch) return;
                sending = false;
                if (!answer) {
                    failures = std::min(failures + 1, 4);
                    sendTimer = static_cast<float>(1 << (failures - 1));
                    return;
                }
                failures = 0;
                auto found = std::find_if(outgoing.begin(), outgoing.end(),
                                          [run](Outgoing const& out) { return out.run == run; });
                if (found == outgoing.end()) {
                    sendTimer = 0.f;
                    return;
                }
                if (!answer->watchedMe) {
                    if (answer->run == run)
                        found->acked =
                            std::min<std::size_t>(static_cast<std::size_t>(answer->next), found->parts.size());
                    drainUntil = Clock::now();
                    sendTimer = kLiveSendEvery;
                    return;
                }
                if (answer->run == run) {
                    auto const next =
                        std::min<std::size_t>(static_cast<std::size_t>(answer->next), found->parts.size());
                    found->stuck = next == index ? found->stuck + 1 : 0;
                    found->acked = found->stuck >= kLiveStuck ? found->parts.size() : next;
                    if (found->stuck == kLiveStuck)
                        log::warn("Versus live: attempt {} no longer taken (part {})", run, index);
                } else
                    found->acked = index + 1;
                bool const more = std::any_of(outgoing.begin(), outgoing.end(), [](Outgoing const& out) {
                    return out.stuck < kLiveStuck && out.acked < out.parts.size();
                });
                sendTimer = more ? 0.f : kLiveSendEvery;
            });
    }

    // 관전 쪽 상태. stage: Waiting(재생 전) / Running / Done(시도 끝, 다음 시도 대기).
    // rec은 지금까지 받은 시도 기록, have는 받은 조각 수, latest는 서버가 아는 가장 최근 시도 번호,
    // complete/completeTime은 받은 데이터가 확정된 지점(step/초), skip은 포기한 시도.
    enum class Play { Waiting, Running, Done };
    LiveTarget watchTarget;
    std::string watchRoom, name;
    Weak<PlayLayer> view;
    int pollFailures = 0;
    Clock::time_point watchStarted{}, lostAt{};
    std::deque<Clock::time_point> stalls;
    bool targetActive = false;
    std::uint64_t watchEpoch = 0;
    bool polling = false;
    float pollTimer = 0.f;
    Clock::time_point notBefore{}, lastPart{};
    Recording rec;
    int have = 0, latest = 0;
    int skip = 0;
    bool recEnded = false, stale = false;
    std::int32_t complete = 0;
    double completeTime = 0;
    std::vector<std::uint32_t> perPlayer[2];
    Play stage = Play::Waiting;
    bool ownReset = false, injecting = false, frozen = false, fastForward = false, timedMode = false;
    bool ended = false, hold = false, muted = false;
    double lagTarget = kLiveLagStart;
    float steady = 0.f;
    float savedVolume = 1.f;
    std::int32_t step = -1;
    std::size_t nextInput = 0, nextSample = 0;
    TimedCursor timed[2];
    double viewerStart = 0, viewerNext = 0;
    float viewerLastDelta = -1.f, viewerLastWarp = -1.f;
    Clock::time_point frameStart{}, sliceAt{}, fastSince{};
    float fastSlice = kLiveChunk;
    float largestCorrection = 0.f;

    // 이 레이어가 라이브 관전 중인 레이어인지.
    bool owns(GJBaseGameLayer const* layer) const {
        if (watchRoom.empty() || !layer) return false;
        auto owner = view.lock();
        return owner && static_cast<GJBaseGameLayer const*>(owner.data()) == layer;
    }
    // 관전 시작. 같은 대상이면 이름만 갱신하고, 아니면 이전 관전을 정리한 뒤 새로 설정한다.
    // 시작 후 1.5초(notBefore)가 지나기 전에는 재생을 시작하지 않는다.
    void watch(PlayLayer* layer, LiveTarget const& target, std::string who) {
        if (!layer || target.empty() || target.of.empty()) {
            stopWatch();
            return;
        }
        auto room = target.key();
        if (room == watchRoom && owns(layer)) {
            name = std::move(who);
            return;
        }
        stopWatch();
        watchRoom = std::move(room);
        watchTarget = target;
        name = std::move(who);
        rebind(view, layer);
        notBefore = Clock::now() + std::chrono::milliseconds(1500);
        watchStarted = Clock::now();
        pollFailures = 0;
        lostAt = {};
        stalls.clear();
        log::info("Versus live: watching {} (room {}, battle {})", name, watchTarget.roomId, watchTarget.battleId);
    }
    // 화면에 보여 줄 연결 상태 경고 문구를 고른다: 폴링 실패 반복, 조각 유실, 데이터 지연, 최근 20초 동안 버퍼링 3회 이상.
    std::string warning() const {
        if (watchRoom.empty()) return {};
        auto const now = Clock::now();
        auto const since = [&now](Clock::time_point at) { return std::chrono::duration<float>(now - at).count(); };
        if (pollFailures >= 3) return "Your connection is unstable - the live view is delayed.";
        if (lostAt != Clock::time_point{} && since(lostAt) < 8.f)
            return fmt::format(
                "Some of {}'s live data was lost (unstable connection) - going on with the next attempt.", name);
        if (targetActive && stage != Play::Done && !recEnded) {
            if (have == 0 && since(watchStarted) > 6.f && since(lastPart) > 6.f)
                return fmt::format(
                    "{}'s live data is not arriving - their connection may be slow, or their game out of date.", name);
            if (have > 0 && since(lastPart) > 4.f)
                return fmt::format("{}'s connection is slow or unstable - waiting for their live data.", name);
        }
        int recent = 0;
        for (auto const& at : stalls) recent += since(at) < 20.f ? 1 : 0;
        if (recent >= 3) return fmt::format("{}'s connection is slow - the live view keeps buffering.", name);
        return {};
    }
    // 관전 종료. 소리를 되돌리고 진행 중인 요청을 무효화(epoch)하며 상태를 초기화한다.
    void stopWatch() {
        unmute();
        if (watchRoom.empty() && !view.lock()) return;
        ++watchEpoch;
        if (auto layer = view.lock(); layer && seeding().on(layer.data())) seeding().end();
        if (!watchRoom.empty())
            log::info("Versus live: stopped watching (attempt {}, largest correction {:.3f})", rec.run,
                      largestCorrection);
        view = nullptr;
        watchRoom.clear();
        watchTarget = {};
        targetActive = false;
        polling = false;
        pollTimer = 0.f;
        clearRun(0);
        lagTarget = kLiveLagStart;
        steady = 0.f;
    }
    // 시도 하나의 수신 상태를 비운다. run은 이제 따라갈 시도 번호(0이면 아직 없음).
    void clearRun(int run) {
        rec = {};
        rec.run = run;
        rec.format = kFormat;
        have = 0;
        recEnded = stale = false;
        complete = 0;
        completeTime = 0;
        perPlayer[0].clear();
        perPlayer[1].clear();
        lastPart = Clock::now();
        stage = Play::Waiting;
        frozen = fastForward = ended = hold = false;
        unmute();
    }
    // 재생 위치를 처음으로 되돌린다.
    void rearm() {
        step = -1;
        nextInput = nextSample = 0;
        timed[0] = timed[1] = {};
        viewerStart = viewerNext = 0;
        viewerLastDelta = viewerLastWarp = -1.f;
        timedMode = false;
        ended = false;
        hold = false;
        frozen = false;
        fastForward = false;
    }
    std::int32_t recordedNow() const { return timedMode ? stepAtTime(rec, viewerStart) : step; }
    // 현재 재생 위치(초). 버퍼에 남은 길이를 계산하는 기준이다.
    double playhead() const {
        if (timedMode) return viewerNext;
        return step < 0 ? 0.0 : timeAtStep(rec, step + 1);
    }

    // 버퍼링/빨리 감기 동안 FMOD 마스터 볼륨을 0으로 하고 원래 볼륨을 저장해 둔다. unmute가 복원한다.
    void mute() {
        if (muted) return;
        auto* engine = FMODAudioEngine::sharedEngine();
        FMOD::ChannelGroup* master = nullptr;
        if (!engine || !engine->m_system || engine->m_system->getMasterChannelGroup(&master) != FMOD_OK || !master)
            return;
        float volume = 1.f;
        if (master->getVolume(&volume) == FMOD_OK) savedVolume = volume;
        master->setVolume(0.f);
        muted = true;
    }
    void unmute() {
        if (!muted) return;
        muted = false;
        auto* engine = FMODAudioEngine::sharedEngine();
        FMOD::ChannelGroup* master = nullptr;
        if (!engine || !engine->m_system || engine->m_system->getMasterChannelGroup(&master) != FMOD_OK || !master)
            return;
        master->setVolume(savedVolume);
    }
    // 노래를 레벨 시간(songOffset + levelTime)에 맞춘다. 80ms 넘게 어긋난 경우에만 이동한다.
    void syncMusic(PlayLayer* layer) {
        auto* engine = FMODAudioEngine::sharedEngine();
        if (!engine || !layer->m_levelSettings || !engine->isMusicPlaying(0)) return;
        double const target =
            (static_cast<double>(layer->m_levelSettings->m_songOffset) + layer->m_gameState.m_levelTime) * 1000.0;
        if (!(target >= 0.0 && target < 36'000'000.0)) return;
        double const now = engine->getMusicTimeMS(0);
        if (std::abs(now - target) > 80.0) engine->setMusicTimeMS(static_cast<unsigned>(target), true, 0);
    }
    void resume(PlayLayer* layer) {
        unmute();
        syncMusic(layer);
    }

    // 매 프레임 관전 처리: 일정 주기로 폴링하고, 더 새 시도가 있는데 3초 넘게 조각이 없으면 낡은 시도로 처리한다.
    void tickWatch(float dt) {
        if (watchRoom.empty()) return;
        auto layer = view.lock();
        if (!layer) {
            stopWatch();
            return;
        }
        pollTimer -= dt;
        if (!polling && pollTimer <= 0.f) poll();
        if (stage != Play::Done && !recEnded && !stale && rec.run > 0 && latest > rec.run &&
            Clock::now() - lastPart > std::chrono::duration<float>(kLiveStaleSeconds)) {
            stale = true;
            log::info("Versus live: attempt {} stopped arriving (attempt {} is newer)", rec.run, latest);
            if (stage != Play::Running) {
                skip = rec.run;
                clearRun(0);
            }
        }
        if (stage == Play::Waiting) tryStart(layer.data());
    }
    // 서버에 조각을 요청한다. 아직 시도가 없으면 최신 시도부터, 진행 중이면 이어서(have부터),
    // 끝났거나 낡았으면 더 새 시도가 있을 때 그것을 처음부터 받는다.
    void poll() {
        int run = rec.run, from = have;
        if (rec.run == 0) {
            if (skip > 0 && latest <= skip) {
                run = skip;
                from = kLiveNoParts;
            } else {
                run = latest > skip ? latest : 0;
                from = 0;
            }
        } else if (stage == Play::Done || stale) {
            if (latest > rec.run) {
                run = latest;
                from = 0;
            } else
                from = kLiveNoParts;
        } else if (recEnded) {
            pollTimer = kLivePollEvery;
            return;
        }
        polling = true;
        auto const epoch = watchEpoch;
        Service::get().exchangeLive(watchTarget, std::nullopt, LiveWatch{run, from},
                                    [this, epoch](std::optional<LiveAnswer> answer, std::string error) {
                                        if (epoch != watchEpoch) return;
                                        polling = false;
                                        pollTimer = kLivePollEvery;
                                        if (!answer) {
                                            if (error != kLiveRefused) pollFailures = std::min(pollFailures + 1, 100);
                                            pollTimer = 1.f;
                                            return;
                                        }
                                        pollFailures = 0;
                                        if (!answer->watched) {
                                            pollTimer = 1.f;
                                            return;
                                        }
                                        received(*answer);
                                    });
    }
    // 이상한 조각을 받으면 그 시도를 포기하고 다음 시도로 넘어간다.
    void giveUp(char const* why) {
        log::warn("Versus live: {} part ({} of attempt {}): this attempt is given up", why, have, rec.run);
        skip = rec.run;
        stale = true;
        lostAt = Clock::now();
    }
    // 서버 응답 처리. 조각을 순서대로 디코드해 rec에 붙인다 (head 조각 여부, 번호, 순서 검사).
    // 하나라도 어긋나면 giveUp. 아직 조각이 더 남았으면 바로 다시 폴링한다.
    void received(LiveAnswer const& answer) {
        latest = answer.latest;
        if (answer.watchRun <= 0 || answer.watchRun == skip) return;
        if (answer.watchRun != rec.run) {
            if (answer.watchFrom != 0) return;
            if (stage == Play::Running && !ended && recEnded && !stale) return;
            log::info("Versus live: attempt {} ({} part(s) so far)", answer.watchRun, answer.watchNext);
            clearRun(answer.watchRun);
        }
        if (answer.watchFrom != have || stale) return;
        bool whole = true;
        for (auto const& text : answer.parts) {
            auto decoded = utils::base64::decode(text, utils::base64::Base64Variant::Url);
            if (decoded.isErr()) {
                giveUp("a damaged");
                whole = false;
                break;
            }
            auto raw = std::move(decoded).unwrap();
            auto part = decodeLive(std::span<std::uint8_t const>(raw));
            if (!part || part->run != rec.run || part->index != have || (have == 0 && !part->head)) {
                giveUp("an unreadable");
                whole = false;
                break;
            }
            auto const before = rec.samples.size();
            if (!appendLive(rec, *part)) {
                giveUp("an out of order");
                whole = false;
                break;
            }
            for (auto k = before; k < rec.samples.size(); ++k)
                perPlayer[rec.samples[k].player ? 1 : 0].push_back(static_cast<std::uint32_t>(k));
            complete = std::max(complete, part->complete);
            completeTime = std::max(completeTime, part->completeTime);
            if (part->ended) recEnded = true;
            ++have;
            lastPart = Clock::now();
        }
        if (whole && have < answer.watchNext) pollTimer = 0.f;
    }
    // 버퍼가 lagTarget만큼 쌓이면 재생을 시작한다: 연습 모드를 끄고 난수를 고정한 뒤 resetLevel로 새 시도를 시작한다.
    void tryStart(PlayLayer* layer) {
        if (rec.run <= 0 || have == 0 || stale || Clock::now() < notBefore || layer->m_isPaused) return;
        if (!recEnded && completeTime < lagTarget) return;
        if (rec.samples.empty()) {
            if (recEnded) {
                stage = Play::Done;
                hold = true;
            }
            return;
        }
        unmute();
        if (layer->m_isPracticeMode) layer->togglePracticeMode(false);
        layer->stopAllActions();
        ownReset = true;
        seeding().begin(layer, rec.seedBase, rec.run);
        layer->resetLevel();
        ownReset = false;
        rearm();
        stage = Play::Running;
        log::info("Versus live: playing attempt {} of {} ({:.1f} s received{})", rec.run, name, completeTime,
                  recEnded ? ", complete" : "");
    }
    // 데이터가 모자라 재생을 멈춘다 (buffering). 소리를 끄고, 버퍼 목표를 0.5초 늘린다 (최대 kLiveLagMax).
    void stall() {
        if (frozen) return;
        frozen = true;
        mute();
        stalls.push_back(Clock::now());
        while (stalls.size() > 8) stalls.pop_front();
        lagTarget = std::min(kLiveLagMax, lagTarget + .5);
        steady = 0.f;
    }
    // 시도 재생이 끝남. 컷으로 끝난 시도는 다음 시도가 올 때까지 멈춰 둔다(hold).
    void finishRun() {
        ended = true;
        stage = Play::Done;
        hold = rec.end == End::Cut;
        resumeIfMuted();
    }
    void resumeIfMuted() {
        if (!muted) return;
        if (auto layer = view.lock())
            resume(layer.data());
        else
            unmute();
    }

    // GJBaseGameLayer::update 앞에서 이번 프레임에 진행할 시간(dt)을 정한다. 0이면 이번 프레임은 정지.
    // lag은 받아 둔 데이터가 재생 위치보다 얼마나 앞서 있는지(초)이다.
    // - 버퍼가 다 떨어지면 stall, 목표보다 kLiveBehind 넘게 쌓이면 빨리 감기(fastForward) 시작
    // - 20초 동안 안정적이면 버퍼 목표를 0.25초 줄인다.
    float frameBegin(PlayLayer* layer, float dt) {
        frameStart = Clock::now();
        if (stage == Play::Waiting) return dt;
        if (stage == Play::Done) return hold ? 0.f : dt;
        double const head = playhead();
        double const lag = (recEnded ? rec.endTime : completeTime) - head;
        if (frozen) {
            if (!recEnded && !stale && lag < lagTarget) return 0.f;
            frozen = false;
            if (!fastForward) resume(layer);
        }
        if (!recEnded && lag <= 1e-4) {
            if (stale) {
                finishRun();
                hold = true;
                return 0.f;
            }
            stall();
            return 0.f;
        }
        if (!fastForward && lag > lagTarget + kLiveBehind) {
            fastForward = true;
            fastSlice = kLiveChunk;
            fastSince = Clock::now();
            mute();
            log::info("Versus live: {:.1f} s behind: fast-forward", lag);
        }
        if (fastForward) return slice(layer);
        steady += dt;
        if (steady >= 20.f) {
            steady = 0.f;
            lagTarget = std::max(kLiveLagMin, lagTarget - .25);
        }
        return recEnded ? dt : static_cast<float>(std::min<double>(dt, lag));
    }
    // 빨리 감기 한 조각의 길이를 정한다. 따라잡았으면 빨리 감기를 끝내고 소리를 복원한다.
    float slice(PlayLayer* layer) {
        double const lag = (recEnded ? rec.endTime : completeTime) - playhead();
        if (lag <= lagTarget || (!recEnded && lag <= 1e-4)) {
            fastForward = false;
            resume(layer);
            log::info("Versus live: caught up in {:.2f} s",
                      std::chrono::duration<float>(Clock::now() - fastSince).count());
            return 0.f;
        }
        sliceAt = Clock::now();
        return static_cast<float>(std::min<double>(fastSlice, lag - lagTarget + 1e-3));
    }
    // 한 프레임에 이어서 더 돌릴 조각. 이전 조각의 처리 시간에 따라 조각 길이를 늘리거나 줄이고,
    // 프레임 시간 예산(kLiveBudget)을 넘으면 중단한다.
    float frameMore(PlayLayer* layer) {
        if (stage != Play::Running || !fastForward || frozen) return 0.f;
        auto const now = Clock::now();
        auto const cost = now - sliceAt;
        if (cost < std::chrono::milliseconds(3))
            fastSlice = std::min(kLiveChunkMax, fastSlice * 2.f);
        else if (cost > std::chrono::milliseconds(8))
            fastSlice = std::max(kLiveChunkMin, fastSlice * .5f);
        if (now - frameStart >= kLiveBudget) return 0.f;
        return slice(layer);
    }

    // 물리 step 시작 전 호출. false이면 이번 step을 건너뛴다 (데이터 부족 등).
    // 첫 step에서 재생 방식(exact/timed)을 정하고, 받은 데이터 범위를 넘어서면 stall한다.
    // 녹화 데이터를 적용하는 동안은 integrity의 native process scope를 열어 둔다.
    bool stepBegin(GJBaseGameLayer* layer, float stepDelta) {
        layer->m_queuedButtons.clear();
        if (stage != Play::Running || ended) return stage != Play::Done || !hold;
        std::int32_t const now = step + 1;
        float const warp = layer->m_gameState.m_timeWarp;
        if (now == 0) {
            bool const otherRate = rec.stepDelta > 0.f && std::abs(stepDelta - rec.stepDelta) > rec.stepDelta * .01f;
            timedMode = otherRate || rec.uneven;
            if (timedMode)
                log::info("Versus live: played by time (recorded step {} s, here {} s{})", rec.stepDelta, stepDelta,
                          rec.uneven ? ", uneven steps" : "");
        } else if (!timedMode &&
                   (rec.uneven || (viewerLastDelta >= 0.f && stepDelta != viewerLastDelta && warp == viewerLastWarp))) {
            timedMode = true;
            viewerNext = timeAtStep(rec, now);
            log::info("Versus live: uneven steps from step {}: played by time", now);
        }
        double const start = timedMode && now > 0 ? viewerNext : 0.0;
        if (!recEnded && !stale) {
            bool const beyond = timedMode ? start + stepDelta > completeTime + 1e-7 : now >= complete;
            if (beyond) {
                stall();
                return false;
            }
        }
        if (stale && (timedMode ? start + stepDelta > completeTime + 1e-7 : now >= complete)) {
            finishRun();
            hold = true;
            return false;
        }
        step = now;
        viewerLastDelta = stepDelta;
        viewerLastWarp = warp;
        if (now == 0) {
            layer->m_randomSeed = rec.randomSeed;
            layer->m_replayRandSeed = rec.replaySeed;
        }
        integrity::beginNativeProcessScope();
        if (timedMode) {
            viewerStart = start;
            viewerNext = start + static_cast<double>(stepDelta);
            stepTimed(layer, now, stepDelta);
        } else
            stepExact(layer, now);
        integrity::endNativeProcessScope();
        return true;
    }
    // 기록된 입력을 주입 허용 상태에서 실행한다.
    void press(GJBaseGameLayer* layer, std::uint8_t bits) {
        injecting = true;
        layer->handleButton(bits & 1, (bits >> 1) & 3, !(bits & 8));
        injecting = false;
    }
    // step 0에서 샘플 시점에 눌려 있던 버튼(점프/좌/우)을 먼저 눌러 홀드 상태를 복원한다.
    void holdThrough(GJBaseGameLayer* layer, PlayerObject* player, Sample const& state, bool p1) {
        injecting = true;
        if ((state.flags & HoldJump) && !holdingJump(player)) layer->handleButton(true, 1, p1);
        if ((state.flags & HoldLeft) && !player->m_holdingLeft) layer->handleButton(true, 2, p1);
        if ((state.flags & HoldRight) && !player->m_holdingRight) layer->handleButton(true, 3, p1);
        injecting = false;
    }
    // step 번호 기준으로 샘플과 입력을 적용한다 (녹화와 같은 step 길이일 때).
    void stepExact(GJBaseGameLayer* layer, std::int32_t now) {
        auto const& samples = rec.samples;
        while (nextSample < samples.size() && samples[nextSample].step < now) ++nextSample;
        while (nextSample < samples.size() && samples[nextSample].step == now) {
            auto const& sample = samples[nextSample++];
            auto* player = sample.player ? layer->m_player2 : layer->m_player1;
            if (!player || (sample.player && !layer->m_gameState.m_isDualMode)) continue;
            if (!player->m_isDead) {
                if (now > 0)
                    largestCorrection =
                        std::max(largestCorrection, static_cast<float>(std::abs(player->m_position.x - sample.x) +
                                                                       std::abs(player->m_position.y - sample.y)));
                applySample(player, sample);
            }
            if (now == 0) holdThrough(layer, player, sample, sample.player == 0);
        }
        auto const& inputs = rec.inputs;
        while (nextInput < inputs.size() && inputs[nextInput].step < now) ++nextInput;
        while (nextInput < inputs.size() && inputs[nextInput].step == now) press(layer, inputs[nextInput++].bits);
    }
    // 시간 기준으로 샘플과 입력을 적용한다 (step 길이가 다를 때).
    void stepTimed(GJBaseGameLayer* layer, std::int32_t now, float stepDelta) {
        double const at = viewerStart;
        for (int p = 0; p < 2; ++p) {
            auto* player = p ? layer->m_player2 : layer->m_player1;
            if (!player || (p && !layer->m_gameState.m_isDualMode)) continue;
            auto const state = timedState(rec, perPlayer[p], timed[p], at);
            if (!state) continue;
            if (!player->m_isDead) {
                if (now > 0)
                    largestCorrection =
                        std::max(largestCorrection, static_cast<float>(std::abs(player->m_position.x - state->x) +
                                                                       std::abs(player->m_position.y - state->y)));
                applySample(player, *state);
            }
            if (now == 0) holdThrough(layer, player, *state, p == 0);
        }
        auto const last = timedInputsUntil(rec, nextInput, at + static_cast<double>(stepDelta));
        while (nextInput < last) press(layer, rec.inputs[nextInput++].bits);
    }
    // 기록된 종료 지점에 도달했을 때의 처리. Viewer::stepEnd와 같은 규칙(Death/Clear/Cut).
    void stepEnd(PlayLayer* layer) {
        if (stage != Play::Running || ended || !recEnded) return;
        auto const at = recordedNow();
        if (at < rec.endStep) return;
        bool const dead = deadPlayer(layer);
        if (rec.end == End::Death) {
            if (!dead && at >= rec.endStep + 2 && layer->m_player1) layer->destroyPlayer(layer->m_player1, nullptr);
            if (dead || at >= rec.endStep + 2) finishRun();
        } else if (rec.end == End::Clear) {
            std::int32_t const wait =
                rec.stepDelta > 0.f ? std::clamp(static_cast<std::int32_t>(std::lround(1.0 / rec.stepDelta)), 30, 2000)
                                    : 240;
            if (layer->m_hasCompletedLevel || at >= rec.endStep + wait) finishRun();
        } else
            finishRun();
    }
    // 관전 중 PlayLayer의 사망 처리를 허용할지. 기록된 종료 지점 전의 사망은 무시한다.
    bool deathAllowed() const { return stage != Play::Running || ended || (recEnded && recordedNow() >= rec.endStep); }
    // 클리어로 끝난 시도의 끝을 재생하는 중인지 / 지금 재생 중인지.
    bool holding() const {
        return !watchRoom.empty() && stage == Play::Running && !ended && recEnded && rec.end == End::Clear;
    }
    bool playing() const { return !watchRoom.empty() && stage == Play::Running && !ended; }
    // 관전 화면 상단에 쓸 캡션 (누구의 몇 번째 시도, 버퍼링/따라잡기 상태).
    std::string caption() const {
        if (watchRoom.empty()) return {};
        std::string state;
        if (stage == Play::Waiting)
            state = rec.run > 0 && have > 0 ? "Buffering..." : fmt::format("Waiting for {}'s attempt...", name);
        else if (stage == Play::Done)
            state = fmt::format("Waiting for {}'s next attempt...", name);
        else if (frozen)
            state = "Buffering...";
        else if (fastForward)
            state = "Catching up...";
        std::string text = fmt::format("LIVE - watching {}", name);
        if (rec.run > 0) text += fmt::format(" - attempt {}", rec.run);
        if (!state.empty()) text += "  |  " + state;
        return text;
    }

    // 매 프레임: 내 시도 전송과 상대 관전을 각각 처리한다.
    void update(float dt) override {
        tickStream(dt);
        tickWatch(dt);
    }
};

// Recorder가 시도 시작/종료를 Live에 알리는 연결 함수 (Live가 아직 없으면 무시).
void streamBegan() {
    if (auto* live = Live::peek()) live->attemptBegan();
}
void streamEnded() {
    if (auto* live = Live::peek()) live->attemptEnded();
}
}

// 공개 API: 중계 시작/중지, 관전 시작/중지, 관전 상태 조회.
void liveStream(LiveTarget const& me) {
    if (me.empty() && !Live::peek()) return;
    Live::get().stream(me);
}
void liveWatch(PlayLayer* layer, LiveTarget const& target, std::string name) {
    if ((!layer || target.empty() || target.of.empty()) && !Live::peek()) return;
    Live::get().watch(layer, target, std::move(name));
}
void liveTargetActive(bool active) {
    if (auto* live = Live::peek()) live->targetActive = active;
}
std::string liveWarning() {
    auto* live = Live::peek();
    return live ? live->warning() : std::string();
}
std::string liveWatchedUid() {
    auto* live = Live::peek();
    return live && !live->watchRoom.empty() ? live->watchTarget.of : std::string();
}
void liveStop() {
    auto* live = Live::peek();
    if (!live) return;
    live->stream({});
    live->stopWatch();
}
bool liveWatching(GJBaseGameLayer const* layer) {
    auto* live = Live::peek();
    return live && live->owns(layer);
}
bool liveHolding() {
    auto* live = Live::peek();
    return live && live->holding();
}
bool livePlaying() {
    auto* live = Live::peek();
    return live && live->playing();
}
std::string liveCaption() {
    auto* live = Live::peek();
    return live ? live->caption() : std::string();
}
}

// 아래는 GD 클래스 후킹. 리플레이 재생/관전 중인 레이어에서는 실제 입력, 사망, 클리어 UI, 연습 모드를
// 막고 기록된 내용만 반영한다. 그렇지 않은 레이어는 원래 동작 그대로 둔다.
using versus::replay::Viewer;
using versus::replay::Live;

// PlayLayer 후킹. init에서 뷰어 레이어를 연결하고, 진입이 끝나면 HUD를 붙여 Ready로 넘긴다.
// startGame과 GD의 지연 시작(versus::hooks::holdReplayStart)은 뷰어 제어 중 막고, resetLevel/destroyPlayer/levelComplete는
// 재생 규칙에 맞게 처리한다.
// 뷰어/라이브의 사망은 GD 재시도 화면 대신 자동 재시도로 처리해, 다시 시작할지는 resetLevel 훅이 정한다.
class $modify(VersusReplayPlayLayer, PlayLayer){
    bool init(VERSUS_PLAY_INIT_PARAMS){auto* viewer = Viewer::peek();
if (viewer && viewer->constructing) {
    versus::rebind(viewer->play, static_cast<PlayLayer*>(this));
    versus::attachPausedMods(this);
}
return PlayLayer::init(VERSUS_PLAY_INIT_ARGS);
}
void onEnterTransitionDidFinish() {
    PlayLayer::onEnterTransitionDidFinish();
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->owns(this) || viewer->entered) return;
    versus::percent::enforce(this);
    viewer->entered = true;
    viewer->phase = Viewer::Phase::Ready;
    viewer->timer = 1.f;
    viewer->installHud(this);
}
void startGame() {
    auto* viewer = Viewer::peek();
    if (viewer && viewer->owns(this) && !viewer->starting) return;
    PlayLayer::startGame();
}
void resetLevel() {
    if (auto* live = Live::peek(); live && live->owns(this)) {
        if (live->ownReset) PlayLayer::resetLevel();
        return;
    }
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->owns(this)) {
        PlayLayer::resetLevel();
        return;
    }
    if (viewer->ended && !viewer->restart) return;
    viewer->restart = false;
    versus::compat::clearStartPos(this);
    versus::replay::seedAttempt(this, viewer->recording.seedBase, viewer->recording.run);
    PlayLayer::resetLevel();
    viewer->rearm();
}
void destroyPlayer(PlayerObject* player, GameObject* object) {
    if (auto* live = Live::peek(); live && live->owns(this)) {
        if (live->deathAllowed()) {
            versus::compat::AutoRetryScope retry;
            PlayLayer::destroyPlayer(player, object);
        }
        return;
    }
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->owns(this)) {
        PlayLayer::destroyPlayer(player, object);
        return;
    }
    if (!viewer->armed || (!viewer->inputsOnly && viewer->recordedNow() < viewer->recording.endStep)) return;
    {
        versus::compat::AutoRetryScope retry;
        PlayLayer::destroyPlayer(player, object);
    }
    bool const early = viewer->recordedNow() < viewer->recording.endStep;
    if (early && !viewer->ended &&
        ((m_player1 && m_player1->m_isDead) || (m_gameState.m_isDualMode && m_player2 && m_player2->m_isDead))) {
        viewer->driftedAt = std::clamp(getCurrentPercentInt(), 0, 100);
        viewer->ended = true;
    }
}
// 클리어 처리 중에는 테스트 모드로 바꿔 실제 기록이 남지 않게 하고, 끝나면 레벨 통계를 되돌린다.
void levelComplete() {
    if (auto* live = Live::peek(); live && live->owns(this)) {
        PlayLayer::levelComplete();
        if (live->owns(this) && live->stage == Live::Play::Running && !live->ended) live->finishRun();
        return;
    }
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->owns(this)) {
        PlayLayer::levelComplete();
        return;
    }
    bool const previous = m_isTestMode;
    m_isTestMode = true;
    PlayLayer::levelComplete();
    m_isTestMode = previous;
    versus::restoreBattleLevelStats();
    viewer->ended = true;
}
void showEndLayer() {
    auto* viewer = Viewer::peek();
    if (viewer && viewer->owns(this)) return;
    PlayLayer::showEndLayer();
}
void togglePracticeMode(bool practice) {
    auto* viewer = Viewer::peek();
    if (viewer && viewer->owns(this) && practice) return;
    if (auto* live = Live::peek(); live && live->owns(this) && practice) return;
    PlayLayer::togglePracticeMode(practice);
}
}
;

// GJBaseGameLayer 후킹. update에서 라이브 관전은 프레임 시간을 쪼개 여러 번 돌리고(빨리 감기),
// processCommands 앞뒤로 step 시작/끝 처리를 끼워 넣는다. handleButton은 주입한 입력만 통과시킨다.
class $modify(VersusReplayGameLayer, GJBaseGameLayer){void update(float dt){auto* viewer = Viewer::peek();
if (viewer && viewer->owns(this) && (!viewer->armed || viewer->frozen)) return;
if (auto* live = Live::peek(); live && live->owns(this)) {
    auto* play = static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
    float slice = live->frameBegin(play, dt);
    for (int calls = 0; slice > 0.f && calls < 64 && live->owns(this); ++calls) {
        GJBaseGameLayer::update(slice);
        slice = live->owns(this) ? live->frameMore(play) : 0.f;
    }
    return;
}
GJBaseGameLayer::update(dt);
}
void processCommands(float dt, bool isHalfTick, bool isLastTick) {
    if (auto* live = Live::peek(); live && live->owns(this)) {
        if (!live->stepBegin(this, dt)) return;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (live->owns(this)) live->stepEnd(static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this)));
        return;
    }
    auto* viewer = Viewer::peek();
    if (!viewer || !viewer->owns(this) || !viewer->armed || viewer->frozen) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        return;
    }
    viewer->stepStart(this, dt);
    GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    if (auto* play = typeinfo_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this))) viewer->stepEnd(play);
}
void handleButton(bool down, int button, bool isPlayer1) {
    auto* viewer = Viewer::peek();
    if (viewer && viewer->owns(this) && !viewer->injecting) return;
    if (auto* live = Live::peek(); live && live->owns(this) && !live->injecting) return;
    GJBaseGameLayer::handleButton(down, button, isPlayer1);
}
}
;

// 뷰어가 맡은 레이어는 GD의 지연 시작도 막는다(시작은 뷰어가 startGame으로 한다).
bool versus::hooks::holdReplayStart(PlayLayer* layer) {
    auto* viewer = Viewer::peek();
    return viewer && viewer->owns(layer);
}

// 아래는 난수 고정용 후킹. 트리거(효과/랜덤/스폰)와 그룹 조회, Advanced Follow 처리가 도는 동안
// SeedScope를 걸어 그 위치의 난수가 시도마다 같게 나오도록 한다. siteKey의 첫 인자가 위치 종류다.
namespace {
using versus::replay::SeedScope;
using versus::replay::siteKey;
}

class $modify(VersusSeededEffect, EffectGameObject){
    void triggerObject(GJBaseGameLayer * layer, int uniqueID,
                       versus::compat::Vector<int> const* remapKeys){SeedScope scope(layer, siteKey(1, this));
EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
}
}
;

class $modify(VersusSeededRandom, RandTriggerGameObject){
    void triggerObject(GJBaseGameLayer * layer, int uniqueID,
                       versus::compat::Vector<int> const* remapKeys){SeedScope scope(layer, siteKey(2, this));
RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
}
}
;

class $modify(VersusSeededSpawn, SpawnTriggerGameObject){
    void triggerObject(GJBaseGameLayer * layer, int uniqueID,
                       versus::compat::Vector<int> const* remapKeys){SeedScope scope(layer, siteKey(3, this));
SpawnTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
}
}
;

class $modify(VersusSeededLayer, GJBaseGameLayer){
    GameObject *
    tryGetObject(int groupID){SeedScope scope(this, siteKey(4, nullptr, static_cast<std::uint32_t>(groupID)));
return GJBaseGameLayer::tryGetObject(groupID);
}
// 이미 처리된 인스턴스는 그냥 통과시키고, 처음 처리될 때만 호출 횟수를 센다.
void processAdvancedFollowAction(AdvancedFollowInstance& instance, bool started, float dt) {
    if (instance.m_processed) {
        GJBaseGameLayer::processAdvancedFollowAction(instance, started, dt);
        return;
    }
    SeedScope scope(this, siteKey(5, instance.m_gameObject, static_cast<std::uint32_t>(instance.m_group)));
    GJBaseGameLayer::processAdvancedFollowAction(instance, started, dt);
    scope.counts = instance.m_processed;
}
}
;
