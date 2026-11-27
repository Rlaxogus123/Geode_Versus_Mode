// Season 2 lounge / plaza 장면 구현. 실제 GD PlayLayer(플랫포머 레벨)를 Host 레이어 안에
// 배경 장면으로 띄우고, 그 위에 다른 플레이어 군중 / 배지 / 안내판을 올리며 카메라와 입력을 제어한다.
// LoungeMap은 lounge 맵 레벨을 GD 서버에서 받아 캐시하고, LoungeAudio는 맵에 쓰이는 노래 / 효과음을 내려받는다.
// 파일 끝의 $modify 훅들은 lounge PlayLayer가 일반 플레이처럼 끝나거나 음악을 바꾸지 않게 막는다.
#include "compat/Files.hpp"
#include "Season2Lounge.hpp"
#include "Season2LoungeLevel.hpp"
#include "BattleLevelData.hpp"
#include "GdRequests.hpp"
#include "GroupFreePlay.hpp"
#include "MapCache.hpp"
#include "MatchLaunch.hpp"
#include "Replay.hpp"
#include "SpriteFrames.hpp"
#include "VersusAudio.hpp"
#include "VersusService.hpp"
#include "WeakRefs.hpp"
#include "compat/Compat.hpp"
#include "compat/GameHooks.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/ColorAction.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <Geode/binding/MusicDownloadDelegate.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <Geode/utils/Keyboard.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>
#include <string>

using namespace geode::prelude;

namespace versus::lounge {
namespace {
// kSettleSeconds: 장면이 자리잡은 뒤 lounge를 붙이기까지 기다리는 시간(초), kMusicCheck: 음악 유지 확인 주기(초),
// kFallY: 이 높이 아래로 떨어지면 레벨을 다시 시작함.
constexpr float kSettleSeconds = .05f;
constexpr float kMusicCheck = .5f;
constexpr float kFallY = -400.f;
constexpr float kDrop = 0.f;

// s_constructing: lounge PlayLayer를 만드는 중, s_inSetup: setupHasCompleted 진행 중,
// s_identity / s_play: 현재 lounge PlayLayer (훅이 일반 플레이와 구분하는 데 사용).
bool s_constructing = false;
bool s_inSetup = false;
PlayLayer* s_identity = nullptr;
Weak<PlayLayer> s_play;
int s_teleportsLogged = 0;
std::string s_editorMap;

// layer가 lounge 소유 PlayLayer인지 (생성 중이거나 아직 살아 있을 때).
bool isLounge(GJBaseGameLayer const* layer) {
    if (!layer || layer != s_identity) return false;
    return s_constructing || s_play.lock() != nullptr;
}

// 문자열 양끝 공백 제거와 파일 읽기 도우미.
std::string trimmed(std::string text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto const last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string readFile(std::filesystem::path const& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    return versus::files::read(path).value_or(std::string());
}
// 레벨 문자열(압축 또는 원문)을 풀어 플랫포머 레벨이면 원문을, 아니면 빈 문자열을 반환한다.
std::string platformerData(std::string text) {
    text = trimmed(std::move(text));
    if (text.empty()) return {};
    std::string decoded = text.find(';') == std::string::npos
                              ? std::string(ZipUtils::decompressString(std::string(text), false, 11))
                              : text;
    return !decoded.empty() && platformerLevelData(decoded) ? decoded : std::string();
}
// 레벨 데이터에서 노래 트리거(오브젝트 ID 1934)가 가리키는 노래 ID 목록을 중복 없이 뽑는다.
std::vector<int> triggerSongs(std::string_view data) {
    std::vector<int> songs;
    for (size_t at = data.find(';'); at != std::string_view::npos;) {
        size_t const next = data.find(';', at + 1);
        auto const object =
            data.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
        at = next;
        if (object.find("1934") == std::string_view::npos) continue;
        int id = 0, song = 0;
        for (size_t pos = 0; pos < object.size();) {
            size_t const comma = object.find(',', pos);
            if (comma == std::string_view::npos) break;
            size_t const end = std::min(object.find(',', comma + 1), object.size());
            auto const key = object.substr(pos, comma - pos);
            auto const value = object.substr(comma + 1, end - comma - 1);
            if (key == "1")
                std::from_chars(value.data(), value.data() + value.size(), id);
            else if (key == "392")
                std::from_chars(value.data(), value.data() + value.size(), song);
            pos = end + 1;
        }
        if (id == 1934 && song > 0 && std::find(songs.begin(), songs.end(), song) == songs.end()) songs.push_back(song);
    }
    return songs;
}

// kRecheck: 서버 버전을 다시 확인하는 최소 간격, kCheckLimit: 한 번의 확인 / 다운로드 제한 시간.
// 캐시 파일은 standard / demon 컵 맵별로 따로 둔다.
constexpr auto kRecheck = std::chrono::seconds(30);
constexpr auto kCheckLimit = std::chrono::seconds(6);
constexpr char const* kCacheFile = "season2-lounge-map.txt";
constexpr char const* kDemonCacheFile = "season2-lounge-map-demon.txt";
// 맵 안에서 쓰는 그룹 ID(매치 방, 안내판), 그리기 순서(z) 상수, 내 그림자 크기 / 불투명도,
// 스폰 트리거 오브젝트 ID, 맵을 만들 때 쓰는 게임 버전 값.
constexpr int kRoomGroup = 900;
constexpr int kBillboardGroup = 999;
constexpr int kMeZ = 100000;
constexpr int kAuraZ = kMeZ - 1;
constexpr int kShadowZ = kMeZ - 2;
constexpr int kBillboardZ = kMeZ - 10;
constexpr int kCrowdHostZ = 10;
constexpr float kShadowSize = 96.f;
constexpr GLubyte kShadowOpacity = 255;
constexpr GLubyte kShadowBoost = 64;
constexpr int kSpawnTriggerID = 1268;
constexpr int kLoungeGameVersion = 22;

// lounge 맵 레벨 하나를 최신으로 유지한다. GD 서버에서 레벨 ID로 검색해 버전을 확인하고,
// 로컬 캐시보다 새롭거나 효과음 정보가 없으면 내려받아 캐시 파일(첫 줄: ID 버전 sfx:목록, 다음: 레벨 문자열)에 저장한다.
// 제한 시간 안에 응답이 없거나 실패하면 가진 복사본을 그대로 쓴다. m_after는 먼저 끝나야 하는 앞선 맵(동시 요청 방지).
// 1.0.8: 응답은 GdRequests 라우터로 받는다(GD의 델리게이트 자리를 쥐지도, 비기를 기다리지도 않는다).
class LoungeMap final : public CCNode, public LevelManagerDelegate, public LevelDownloadDelegate {
    using Clock = std::chrono::steady_clock;
    enum class Step { Idle, Searching, Downloading, Done };
    Step m_step = Step::Idle;
    Clock::time_point m_began, m_checkedAt;
    bool m_checked = false, m_searchPending = false, m_downloadPending = false, m_searchDone = false;
    bool m_downloadDone = false, m_downloadFailed = false;
    bool m_failed = false;
    std::string m_key;
    Ref<GJSearchObject> m_search;
    int m_serverVersion = -1;
    int m_levelID = 0;
    bool m_wasSaved = true;
    char const* m_cacheFile = "";
    LoungeMap const* m_after = nullptr;

    // 캐시에서 읽은 정보(버전, 효과음 / 노래 목록)를 지연해서 읽어 보관한다.
    struct Kept {
        int version = -1;
        bool assets = false;
        std::vector<int> sfx, songs;
    };
    mutable std::optional<Kept> m_keptInfo;
    std::filesystem::path cachePath() const { return versus::compat::stdPath(Mod::get()->getSaveDir() / m_cacheFile); }
    Kept const& keptInfo() const {
        if (!m_keptInfo) m_keptInfo = readKept();
        return *m_keptInfo;
    }
    // 캐시 파일 첫 줄을 해석해 버전과 효과음 ID를 읽고, 본문에서 노래 트리거 노래를 뽑는다 (ID가 다르면 무시).
    Kept readKept() const {
        Kept result;
        auto const text = readFile(cachePath());
        auto const end = text.find('\n');
        if (end == std::string::npos) return result;
        auto const head = text.substr(0, end);
        int id = 0, version = -1;
        auto const space = head.find(' ');
        if (space == std::string::npos) return result;
        std::from_chars(head.data(), head.data() + space, id);
        auto const rest = std::string_view(head).substr(space + 1);
        std::from_chars(rest.data(), rest.data() + rest.size(), version);
        if (id != m_levelID) return result;
        result.version = version;
        result.songs = triggerSongs(platformerData(text.substr(end + 1)));
        if (auto const at = rest.find("sfx:"); at != std::string_view::npos) {
            result.assets = true;
            auto list = rest.substr(at + 4);
            list = list.substr(0, list.find(' '));
            for (size_t start = 0; start < list.size();) {
                auto stop = list.find(',', start);
                if (stop == std::string_view::npos) stop = list.size();
                int sfx = 0;
                auto parsed = std::from_chars(list.data() + start, list.data() + stop, sfx);
                if (parsed.ec == std::errc() && sfx > 0) result.sfx.push_back(sfx);
                start = stop + 1;
            }
        }
        return result;
    }
    // 진행 중 플래그를 내린다. 기다리던 검색의 늦은 응답은 버리고, 다운로드의 늦은 응답은 그래도 받아 캐시를
    // 새로 고친다(GD Saved 정리 포함). finish는 확인 절차를 마무리한다.
    void releaseDelegates() {
        if (m_searchPending) versus::gdnet::drop(m_key);
        m_searchPending = m_downloadPending = false;
        versus::gdnet::release(static_cast<LevelManagerDelegate*>(this));
        versus::gdnet::release(static_cast<LevelDownloadDelegate*>(this));
    }
    void finish(char const* why, bool failed) {
        releaseDelegates();
        m_search = nullptr;
        m_step = Step::Done;
        m_checked = true;
        m_failed = failed;
        m_checkedAt = Clock::now();
        log::info("Versus: Season 2 lounge map {}: {}", m_levelID, why);
    }
    LoungeMap(int levelID, char const* cacheFile, LoungeMap const* after)
        : m_levelID(levelID), m_cacheFile(cacheFile), m_after(after) {}
    // 스케줄러에 등록해 update()가 매 프레임 돌게 한다. 컵별로 하나씩 유지되는 싱글턴이다.
    static LoungeMap* make(int levelID, char const* cacheFile, LoungeMap const* after = nullptr) {
        auto* map = new LoungeMap(levelID, cacheFile, after);
        map->init();
        CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(map, 1, false);
        map->release();
        return map;
    }

public:
    static LoungeMap& standard() {
        static auto* instance = make(kLoungeLevelID, kCacheFile);
        return *instance;
    }
    static LoungeMap& demon() {
        static auto* instance = make(kDemonLoungeLevelID, kDemonCacheFile, &standard());
        return *instance;
    }
    static LoungeMap& of(std::string_view cup) { return cup == "demon" ? demon() : standard(); }
    int levelID() const { return m_levelID; }
    // 버전 확인을 시작한다 (진행 중이거나 최근에 확인했으면 건너뜀, force면 무시).
    void check(bool force = false) {
        if (m_step == Step::Searching || m_step == Step::Downloading) return;
        if (!force && m_checked && Clock::now() - m_checkedAt < kRecheck) {
            m_step = Step::Done;
            return;
        }
        m_step = Step::Searching;
        m_began = Clock::now();
        m_searchDone = m_downloadDone = m_downloadFailed = false;
        m_serverVersion = -1;
    }
    // ready: 확인이 끝난 상태. wantsRetry: 실패했거나 가진 복사본이 없어 재시도가 필요한 상태. status: UI에 보여 줄 다운로드 상태.
    bool ready() const { return m_step == Step::Done || m_step == Step::Idle; }
    bool wantsRetry() const { return ready() && (m_failed || keptInfo().version < 0); }
    MapDownload status() const {
        MapDownload result;
        auto const kept = keptInfo();
        result.levelID = m_levelID;
        result.version = kept.version;
        result.state = m_step == Step::Searching     ? MapDownload::Checking
                       : m_step == Step::Downloading ? MapDownload::Downloading
                       : kept.version < 0            ? MapDownload::Missing
                                                     : MapDownload::Ready;
        result.failed = m_step == Step::Done && m_failed;
        return result;
    }
    std::vector<int> const& sfx() const { return keptInfo().sfx; }
    std::vector<int> const& songs() const { return keptInfo().songs; }
    bool hasKept() const { return keptInfo().version >= 0; }
    std::string kept() const {
        if (keptInfo().version < 0) return {};
        auto const text = readFile(cachePath());
        auto const end = text.find('\n');
        return end == std::string::npos ? std::string() : platformerData(text.substr(end + 1));
    }
    // GD의 레벨 검색 / 다운로드 콜백. 검색에서는 서버의 레벨 버전을, 다운로드에서는 레벨 문자열을 받는다.
    // 받은 문자열이 플랫포머 레벨일 때만 캐시에 저장하고, 아니면 기존 복사본을 유지한다.
    void loadLevelsFinished(CCArray* levels, char const* key) override {
        if (!m_searchPending || !key || m_key != key) return;
        if (levels)
            for (auto* level : CCArrayExt<GJGameLevel*>(levels))
                if (level && level->m_levelID.value() == m_levelID) m_serverVersion = level->m_levelVersion;
        m_searchDone = true;
    }
    void loadLevelsFinished(CCArray* levels, char const* key, int) override { loadLevelsFinished(levels, key); }
    void loadLevelsFailed(char const* key) override {
        if (m_searchPending && key && m_key == key) m_searchDone = true;
    }
    void loadLevelsFailed(char const* key, int) override { loadLevelsFailed(key); }
    void setupPageInfo(versus::compat::String, char const*) override {}
    void levelDownloadFinished(GJGameLevel* level) override {
        if (!level || level->m_levelID.value() != m_levelID) return;
        if (!m_wasSaved) versus::dropFromSaved(m_levelID, level);
        std::string const text(level->m_levelString);
        if (!platformerData(text).empty()) {
            std::string sfx(level->m_sfxIDs);
            std::erase_if(sfx, [](char c) { return !(c == ',' || (c >= '0' && c <= '9')); });
            versus::files::write(cachePath(), std::to_string(m_levelID) + ' ' + std::to_string(level->m_levelVersion) +
                                                  " sfx:" + sfx + '\n' + text);
            m_keptInfo.reset();
            log::info("Versus: Season 2 lounge map {} v{} downloaded (sfx {})", m_levelID, level->m_levelVersion,
                      sfx.empty() ? std::string("none") : sfx);
        } else {
            log::warn("Versus: Season 2 lounge map {} is not a platformer level; kept the copy we had", m_levelID);
            if (m_downloadPending) m_downloadFailed = true;
        }
        if (m_downloadPending) m_downloadDone = true;
    }
    void levelDownloadFailed(int) override {
        if (m_downloadPending) {
            m_downloadDone = m_downloadFailed = true;
        }
    }
    // 상태 머신: Searching(레벨 검색으로 서버 버전 확인) -> Downloading(캐시가 낡았을 때 다운로드) -> Done.
    // GD가 같은 키를 이미 요청 중이면 그 요청이 끝날 때까지 기다린다(제한 시간은 그대로).
    void update(float) override {
        if (m_step != Step::Searching && m_step != Step::Downloading) return;
        if (m_after && !m_after->ready() && !m_searchPending && !m_downloadPending) {
            m_began = Clock::now();
            return;
        }
        if (Clock::now() - m_began > kCheckLimit) {
            finish("no answer in time, the copy we have is used", true);
            return;
        }
        auto* manager = GameLevelManager::sharedState();
        if (m_step == Step::Searching) {
            if (m_searchDone) {
                releaseDelegates();
                auto const& kept = keptInfo();
                if (m_serverVersion < 0) {
                    finish("not found on GD's servers, the copy we have is used", true);
                    return;
                }
                if (kept.version >= m_serverVersion && kept.assets) {
                    finish("up to date", false);
                    return;
                }
                m_step = Step::Downloading;
                return;
            }
            if (m_searchPending) return;
            m_search = GJSearchObject::create(SearchType::Search, std::string(std::to_string(m_levelID)));
            m_key = m_search->getKey();
            versus::compat::resetRequestTimer(manager, m_key.c_str());
            if (versus::gdnet::active(m_key)) return;
            m_searchPending = true;
            if (!versus::gdnet::search(m_search, this)) m_searchPending = false;
            return;
        }
        if (m_downloadDone) {
            if (m_downloadFailed)
                finish("download failed, the copy we have is used", true);
            else
                finish("updated", false);
            return;
        }
        if (m_downloadPending || versus::gdnet::active(versus::gdnet::downloadKey(m_levelID))) return;
        m_downloadPending = true;
        m_wasSaved = versus::inGdSaved(m_levelID);
        if (!versus::gdnet::download(m_levelID, this)) m_downloadPending = false;
    }
};

// lounge 맵들이 쓰는 노래 / 효과음을 GD 음악 라이브러리에서 내려받는다. 최대 2개씩 동시에 요청하고,
// 실패하면 2n초 뒤 재시도하며 3번 실패하면 포기(failed)한다. 효과음은 키를 음수로 구분한다.
class LoungeAudio final : public CCNode, public MusicDownloadDelegate {
    using Clock = std::chrono::steady_clock;
    bool m_running = false, m_watching = false, m_askedURL = false;
    Clock::time_point m_urlAskedAt;
    float m_timer = 0.f;
    std::map<int, Clock::time_point> m_requested;
    std::map<int, int> m_attempts;
    std::map<int, Clock::time_point> m_retryAt;
    std::set<int> m_failed;
    static std::vector<int> songs() {
        std::vector<int> ids{kStandardCupSong};
        for (auto const* map : {&LoungeMap::standard(), &LoungeMap::demon()})
            for (int id : map->songs())
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        return ids;
    }
    static std::vector<int> sounds() {
        std::set<int> ids(LoungeMap::standard().sfx().begin(), LoungeMap::standard().sfx().end());
        ids.insert(LoungeMap::demon().sfx().begin(), LoungeMap::demon().sfx().end());
        return {ids.begin(), ids.end()};
    }
    static bool downloaded(MusicDownloadManager* music, int key) {
        return key > 0 ? music->isSongDownloaded(key) : music->isSFXDownloaded(-key);
    }
    static bool running(MusicDownloadManager* music, int key) {
        return key > 0 ? music->isRunningActionForSongID(key)
                       : versus::compat::musicRequest(music, music->getSFXDownloadKey(-key)) != nullptr;
    }
    void failedOnce(int key, char const* why) {
        int const tries = ++m_attempts[key];
        log::warn("Versus: Season 2 {} {} - {} (try {})", key > 0 ? "song" : "SFX", key > 0 ? key : -key, why, tries);
        if (tries >= 3)
            m_failed.insert(key);
        else
            m_retryAt[key] = Clock::now() + std::chrono::seconds(2 * tries);
    }
    void failed(int key) {
        if (m_requested.erase(key)) failedOnce(key, "download failed");
    }

public:
    static LoungeAudio& get() {
        static auto* instance = [] {
            auto* audio = new LoungeAudio;
            audio->init();
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(audio, 2, false);
            audio->release();
            return audio;
        }();
        return *instance;
    }
    // 다운로드 시작. retry면 실패 기록을 지우고 처음부터 다시 시도한다.
    void start(bool retry) {
        m_running = true;
        if (retry) {
            m_failed.clear();
            m_attempts.clear();
            m_retryAt.clear();
            m_askedURL = false;
        }
    }
    bool wantsRetry() const { return !m_failed.empty(); }
    void downloadSongFailed(int id, GJSongError) override { failed(id); }
    void downloadSFXFailed(int id, GJSongError) override { failed(-id); }
    // 노래 또는 효과음 묶음의 진행 상황 (완료 수, 전체 수, 진행률 %, 진행 중 여부, 실패 여부).
    AudioDownload status(bool sfx) const {
        AudioDownload result;
        auto* music = MusicDownloadManager::sharedState();
        if (!music) return result;
        auto const ids = sfx ? sounds() : songs();
        result.total = static_cast<int>(ids.size());
        double units = 0.;
        int failures = 0;
        for (int id : ids) {
            int const key = sfx ? -id : id;
            bool const done = downloaded(music, key);
            result.done += done ? 1 : 0;
            if (!done && m_failed.count(key)) ++failures;
            units += done
                         ? 1.
                         : std::clamp(sfx ? music->getSFXDownloadProgress(id) : music->getDownloadProgress(id), 0, 99) /
                               100.;
        }
        result.failed = failures > 0;
        result.percent = result.total ? static_cast<int>(units * 100. / result.total) : 100;
        result.busy = m_running && result.done + failures < result.total;
        return result;
    }
    // 0.25초마다: 완료/만료된 요청 정리, 아직 없는 파일을 GD 라이브러리에 요청한다. 라이브러리 URL을 10초 안에
    // 받지 못하면 남은 항목을 실패 처리한다. 모두 끝나고 두 맵 확인도 끝났으면 멈춘다.
    void update(float delta) override {
        if (!m_running || (m_timer -= delta) > 0.f) return;
        m_timer = .25f;
        auto* music = MusicDownloadManager::sharedState();
        if (!music) return;
        if (!m_watching) {
            versus::compat::loadAudioLibraries(music);
            music->addMusicDownloadDelegate(this);
            m_watching = true;
        }
        std::vector<int> wanted = songs();
        for (int id : sounds()) wanted.push_back(-id);
        auto const now = Clock::now();
        for (auto it = m_requested.begin(); it != m_requested.end();) {
            int const key = it->first;
            if (downloaded(music, key)) {
                it = m_requested.erase(it);
                if (key > 0) audio::refreshCupSong();
                continue;
            }
            if (!running(music, key) && now - it->second > std::chrono::seconds(3)) {
                it = m_requested.erase(it);
                failedOnce(key, "the request ended without the file");
                continue;
            }
            ++it;
        }
        bool const missing = std::any_of(wanted.begin(), wanted.end(),
                                         [&](int key) { return !downloaded(music, key) && !m_failed.count(key); });
        if (!missing && m_requested.empty()) {
            if (LoungeMap::standard().ready() && LoungeMap::demon().ready()) m_running = false;
            return;
        }
        if (!missing) return;
        if (music->m_customContentURL.empty()) {
            if (!m_askedURL) {
                m_askedURL = true;
                m_urlAskedAt = now;
                music->getCustomContentURL();
            } else if (now - m_urlAskedAt > std::chrono::seconds(10)) {
                for (int key : wanted)
                    if (!downloaded(music, key)) m_failed.insert(key);
                log::warn("Versus: Season 2 audio - GD's music library did not answer");
            }
            return;
        }
        for (int key : wanted) {
            if (m_requested.size() >= 2) return;
            if (downloaded(music, key) || m_failed.count(key) || m_requested.count(key) || running(music, key))
                continue;
            if (auto retry = m_retryAt.find(key); retry != m_retryAt.end() && now < retry->second) continue;
            m_requested[key] = now;
            if (key > 0)
                music->downloadSong(key);
            else
                music->downloadSFX(-key);
        }
    }
};

// lounge에 쓸 레벨 데이터 선택 순서: 에디터 Plaza Test 맵 -> 저장 폴더의 season2-lounge.txt
// -> 컵별 내려받은 맵 -> (demon이면) standard 맵 -> 내장 레벨.
std::string levelData(std::string_view cup) {
    if (!s_editorMap.empty()) {
        log::info("Versus: the Season 2 lounge uses the edited level (Plaza Test)");
        return s_editorMap;
    }
    if (auto* mod = Mod::get()) {
        if (auto text = readFile(versus::compat::stdPath(mod->getSaveDir() / "season2-lounge.txt")); !trimmed(text).empty()) {
            if (auto decoded = platformerData(std::move(text)); !decoded.empty()) {
                log::info("Versus: the Season 2 lounge uses season2-lounge.txt");
                return decoded;
            }
            log::warn("Versus: season2-lounge.txt is not a platformer level; not used");
        }
        if (auto kept = LoungeMap::of(cup).kept(); !kept.empty()) return kept;
        if (cup == "demon") {
            log::warn("Versus: the Demon Cup's lounge map {} is not downloaded yet; the Standard Cup's is used",
                      kDemonLoungeLevelID);
            if (auto kept = LoungeMap::standard().kept(); !kept.empty()) return kept;
        }
    }
    return builtInLevel();
}

// macOS에서 레벨이 잠근 커서를 다시 보이게 한다.
void showCursor() {
#if defined(GEODE_IS_MACOS)
    PlatformToolbox::toggleLockCursor(false);
    PlatformToolbox::showCursor();
#endif
}

// 노드가 속한 씬을 찾는다.
CCScene* sceneOf(CCNode* node) {
    while (node && node->getParent()) node = node->getParent();
    return typeinfo_cast<CCScene*>(node);
}

class Host;

Weak<Host> s_loungeHost;

// lounge 호스트 레이어. 화면에 붙으면 가짜 레벨(PlayLayer)을 만들어 자식으로 두고,
// 내 아이콘 조작(키 입력 -> 버튼 큐), 카메라 줌 / 흔들림, 군중 / 장식 / 안내판의 그리기 순서 배치,
// 매치 방 그룹 스폰, 텔레포트, 방 색 바꾸기, 화면 이탈 시 정리를 담당한다.
class Host final : public CCLayer {
    Hooks m_hooks;
    // m_settle: 붙이기 전 대기 시간, m_frozen: 장면 정지 중, m_leaving: 화면을 떠나는 중.
    // m_teleport / m_spawnRoom / m_restart: 다음 프레임에 처리할 요청. m_builds: 레벨을 새로 만든 횟수.
    float m_settle = 0.f, m_music = 0.f, m_timeScale = 1.f;
    bool m_failed = false, m_leaving = false, m_frozen = false;
    Ref<CCNode> m_badge;
    Ref<CCNode> m_crowd;
    int m_crowdBuild = -1;
    std::optional<int> m_crowdZ;
    Ref<CCNode> m_decor;
    Ref<CCNode> m_billboard;
    Ref<CCSprite> m_shadow;
    int m_billboardBuild = -1;
    std::optional<CCPoint> m_billboardAt;
    bool m_inputWas = true;
    std::vector<int> m_levelSongs;
    bool m_keyLeft = false, m_keyRight = false, m_keyJump = false;
    bool m_heldLeft = false, m_heldRight = false, m_heldJump = false;
    Ref<CCNode> m_effects;
    bool m_cinematic = false, m_uiHidden = false;
    bool m_holdWanted = false;
    std::optional<float> m_holdY;
    float m_holdTurn = 0.f;
    float m_zoom = 1.f, m_toward = 0.f;
    CCPoint m_focus, m_shake;
    std::optional<CCPoint> m_teleport;
    bool m_spawnRoom = false;
    bool m_spawnInStep = false;
    bool m_restart = false;
    static constexpr float kPlaceSettle = .35f;
    float m_runFor = 0.f;
    std::optional<CCPoint> m_startSpot;
    std::optional<CCPoint> m_lastSpot;
    int m_builds = 0;
    static constexpr int kRoomMain = 10, kRoomSub = 11;
    static constexpr float kRoomFade = .6f;
    struct Channel {
        ccColor3B color = ccWHITE;
        bool blending = false;
        float opacity = 1.f;
    };
    std::optional<std::pair<uint32_t, uint32_t>> m_roomWanted, m_roomShown;
    std::optional<std::array<Channel, 2>> m_mapChannels;

    // 레이어 기본 설정과 효과 노드(m_effects)를 만든다.
    bool init(Hooks hooks) {
        if (!CCLayer::init()) return false;
        m_hooks = std::move(hooks);
        setID("season2-lounge"_spr);
        setAnchorPoint({0.f, 0.f});
        setPositionY(-kDrop);
        m_effects = CCNode::create();
        m_effects->setID("season2-lounge-effects"_spr);
        addChild(m_effects, 15);
        scheduleUpdate();
        return true;
    }
    // 내 아이콘 위치를 호스트 좌표로 변환한다 (유한한 값이 아니면 실패).
    bool localSpot(PlayLayer* play, CCPoint& out) {
        auto* player = play->m_player1;
        if (!player || !player->getParent()) return false;
        out = convertToNodeSpace(player->getParent()->convertToWorldSpace(player->getPosition()));
        return std::isfinite(out.x) && std::isfinite(out.y);
    }
    // 줌 / 초점 / 흔들림을 호스트 레이어의 배율과 위치로 적용한다. 줌 중에는 화면 밖이 보이지 않게 위치를 제한한다.
    void applyCamera(PlayLayer* play) {
        CCPoint const base{0.f, -kDrop};
        CCPoint spot;
        bool const off = m_zoom == 1.f && m_toward == 0.f && m_shake.x == 0.f && m_shake.y == 0.f;
        if (off || !localSpot(play, spot)) {
            if (getScale() != 1.f) setScale(1.f);
            if (getPositionX() != base.x || getPositionY() != base.y) setPosition(base);
            return;
        }
        CCPoint const natural = base + spot;
        CCPoint const target = natural + (m_focus - natural) * m_toward;
        setScale(m_zoom);
        CCPoint position = target - spot * m_zoom + m_shake;
        if (m_zoom > 1.f) {
            auto const win = CCDirector::sharedDirector()->getWinSize();
            position.x = std::clamp(position.x, win.width - m_zoom * (win.width - base.x), m_zoom * base.x);
            position.y = std::clamp(position.y, win.height - m_zoom * (win.height - base.y), m_zoom * base.y);
        }
        setPosition(position);
    }
    // 플레이어를 속도 0으로 해당 위치에 놓는다.
    static void place(PlayerObject* player, CCPoint at) {
        player->m_yVelocity = 0.;
        player->m_platformerXVelocity = 0.;
        player->m_position = at;
        player->setPosition(at);
    }
    // 오브젝트가 해당 그룹에 속하는지.
    static bool inGroup(GameObject* object, int group) {
        if (!object->m_groups) return false;
        int const count = std::clamp<int>(object->m_groupCount, 0, 10);
        for (int i = 0; i < count; ++i)
            if ((*object->m_groups)[i] == group) return true;
        return false;
    }
    // lounge에서 필요 없는 GD UI(일시정지 버튼, 체크포인트, 진행도 라벨 등)를 숨기고 키 입력을 끈다.
    void quiet(PlayLayer* play) {
        if (auto* ui = play->m_uiLayer) {
            if (ui->getPositionY() != kDrop) ui->setPositionY(kDrop);
            if (m_cinematic != m_uiHidden) {
                m_uiHidden = m_cinematic;
                ui->setVisible(!m_cinematic);
            }
            if (ui->isKeypadEnabled()) ui->setKeypadEnabled(false);
            if (ui->isKeyboardEnabled()) ui->setKeyboardEnabled(false);
            if (auto* pause = ui->m_pauseBtn) {
                pause->setVisible(false);
                pause->setEnabled(false);
            }
            if (auto* checkpoints = ui->m_checkpointMenu) {
                checkpoints->setVisible(false);
                checkpoints->setEnabled(false);
            }
        }
        for (CCNode* node : {static_cast<CCNode*>(play->m_attemptLabel), static_cast<CCNode*>(play->m_percentageLabel),
                             static_cast<CCNode*>(play->m_progressBar), static_cast<CCNode*>(play->m_progressFill),
                             static_cast<CCNode*>(play->m_infoLabel), static_cast<CCNode*>(play->m_statusLabel)})
            if (node) node->setVisible(false);
        if (auto* bar = play->getChildByID("section-progress-bar")) bar->setVisible(false);
    }
    // 내 아이콘이 갑자기 먼 곳으로 이동하면(텔레포트) 처음 3번만 로그를 남긴다.
    static void noteJump(CCPoint from, CCPoint to, float dt) {
        if (s_teleportsLogged >= 3) return;
        float const distance = ccpDistance(from, to);
        if (!std::isfinite(distance) || distance <= 120.f + 1200.f * dt) return;
        ++s_teleportsLogged;
        log::info("Versus: Season 2 lounge: my icon jumped from ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) - a teleport",
                  from.x, from.y, to.x, to.y);
    }
    // 눌린 모든 입력을 놓는다.
    void letGo(PlayLayer* play) {
        m_keyLeft = m_keyRight = m_keyJump = false;
        m_heldLeft = m_heldRight = m_heldJump = false;
        if (!play->m_player1) return;
        play->m_queuedButtons.clear();
        for (int button = 1; button <= 3; ++button) play->handleButton(false, button, true);
    }
    // GD 입력 큐에 버튼(1 점프, 2 왼쪽, 3 오른쪽)을 넣는다. 키 시각이 너무 어긋나면 현재 시각을 쓴다.
    static void queue(PlayLayer* play, int button, bool down, double at) {
        double const now = play->m_timestamp;
        play->queueButton(button, down, false, std::isfinite(at) && std::abs(at - now) <= 2. ? at : now);
    }
    // 키 상태를 입력 허용 여부와 합쳐 바뀐 버튼만 큐에 넣는다.
    void keys(PlayLayer* play, double at) {
        if (!play->m_player1 || !play->m_started || m_frozen) return;
        bool const open = inputAllowed();
        bool const left = open && m_keyLeft, right = open && m_keyRight, jump = open && m_keyJump;
        if (left != m_heldLeft) {
            m_heldLeft = left;
            queue(play, 2, left, at);
        }
        if (right != m_heldRight) {
            m_heldRight = right;
            queue(play, 3, right, at);
        }
        if (jump != m_heldJump) {
            m_heldJump = jump;
            queue(play, 1, jump, at);
        }
    }
    // 내 아이콘 밑에 깔 검은 그림자 스프라이트 (두 겹으로 가장자리를 부드럽게).
    static CCSprite* makeShadow() {
        auto* shadow = loadedFrameSprite("particle_158_001.png", "GJ_ParticleSheet.plist");
        if (!shadow) return nullptr;
        shadow->setID("season2-lounge-shadow"_spr);
        shadow->setColor(ccc3(0, 0, 0));
        shadow->setOpacity(kShadowOpacity);
        if (auto* again = loadedFrameSprite("particle_158_001.png", "GJ_ParticleSheet.plist")) {
            again->setColor(ccc3(0, 0, 0));
            again->setOpacity(kShadowBoost);
            again->setPosition(shadow->getContentSize() / 2.f);
            shadow->addChild(again);
        }
        return shadow;
    }
    // 매 프레임 내 아이콘 주변 노드를 배치한다: 아이콘을 맨 위로, 그림자, 안내판(BILLBOARD 그룹 위치),
    // 오라 장식, 군중을 레벨 노드 안에 넣고, 배지는 호스트 좌표로 따라가게 한다.
    void placeBadge(PlayLayer* play) {
        auto* player = play->m_player1;
        if (!player || !player->getParent()) return;
        if (player->getZOrder() != kMeZ) player->getParent()->reorderChild(player, kMeZ);
        if (!m_shadow) m_shadow = makeShadow();
        if (m_shadow) {
            auto* level = player->getParent();
            if (m_shadow->getParent() != level) {
                m_shadow->removeFromParentAndCleanup(false);
                level->addChild(m_shadow, kShadowZ);
            }
            float const across =
                std::max(1.f, std::max(m_shadow->getContentSize().width, m_shadow->getContentSize().height));
            m_shadow->setPosition(player->getPosition());
            m_shadow->setScale(kShadowSize / across * std::abs(player->getScaleY()));
            m_shadow->setVisible(!player->m_isDead && player->isVisible());
        }
        if (m_billboard) {
            auto* level = player->getParent();
            if (m_billboardBuild != m_builds) {
                m_billboardBuild = m_builds;
                CCPoint at;
                m_billboardAt.reset();
                if (groupSpot(play, kBillboardGroup, at)) m_billboardAt = at;
            }
            if (m_billboard->getParent() != level) {
                m_billboard->removeFromParentAndCleanup(false);
                level->addChild(m_billboard, kBillboardZ);
            }
            if (m_billboardAt) m_billboard->setPosition(*m_billboardAt);
            m_billboard->setVisible(m_billboardAt.has_value());
        }
        if (m_decor) {
            auto* level = player->getParent();
            if (m_decor->getParent() != level) {
                m_decor->removeFromParentAndCleanup(false);
                level->addChild(m_decor, kAuraZ);
            }
            m_decor->setPosition(player->getPosition());
            m_decor->setScale(std::abs(player->getScaleY()));
            m_decor->setVisible(!player->m_isDead);
        }
        if (m_crowd) placeCrowd(play, player->getParent());
        if (!m_badge) return;
        m_badge->setPosition(convertToNodeSpace(player->getParent()->convertToWorldSpace(player->getPosition())));
        m_badge->setVisible(!player->m_isDead && !m_cinematic);
    }
    // 레벨 노드의 자식 중 주어진 배치 레이어들이 차지하는 가장 높은(또는 낮은) z를 구한다.
    static std::optional<int> topOf(std::initializer_list<CCNode*> batches, CCNode* level, bool lowest = false) {
        std::optional<int> found;
        for (auto* node : batches) {
            while (node && node->getParent() && node->getParent() != level) node = node->getParent();
            if (!node || node->getParent() != level) continue;
            int const z = node->getZOrder();
            if (!found || (lowest ? z < *found : z > *found)) found = z;
        }
        return found;
    }
    // 군중을 GD 레벨의 T1 레이어 맨 위 z에 둔다 (레벨 오브젝트 위, T2 레이어 아래). T1이 없으면 호스트 위에 둔다.
    void placeCrowd(PlayLayer* play, CCNode* level) {
        if (m_crowdBuild != m_builds) {
            m_crowdBuild = m_builds;
            m_crowdZ = topOf({play->m_gameLayerT1, play->m_gameBlendingLayerT1, play->m_glowLayerT1,
                              play->m_specialLayerT1, play->m_textLayerT1, play->m_textBlendingLayerT1,
                              play->m_fireLayerT1, play->m_fireBlendingLayerT1, play->m_pixelLayerT1,
                              play->m_pixelBlendingLayerT1, play->m_particleLayerT1, play->m_particleBlendingLayerT1,
                              play->m_game2LayerT1, play->m_game2BlendingLayerT1},
                             level);
            auto const t2 = topOf({play->m_gameLayerT2, play->m_gameBlendingLayerT2, play->m_glowLayerT2,
                                   play->m_specialLayerT2, play->m_textLayerT2, play->m_textBlendingLayerT2,
                                   play->m_fireLayerT2, play->m_fireBlendingLayerT2, play->m_pixelLayerT2,
                                   play->m_pixelBlendingLayerT2, play->m_particleLayerT2,
                                   play->m_particleBlendingLayerT2, play->m_game2LayerT2, play->m_game2BlendingLayerT2},
                                  level, true);
            log::info("Versus: the plaza's players at z {} in GD's level (T1's top; T2 from {})",
                      m_crowdZ ? std::to_string(*m_crowdZ) : std::string("- (none: over the level)"),
                      t2 ? std::to_string(*t2) : std::string("-"));
        }
        CCNode* const parent = m_crowdZ ? level : static_cast<CCNode*>(this);
        int const z = m_crowdZ ? *m_crowdZ : kCrowdHostZ;
        if (m_crowd->getParent() != parent) {
            m_crowd->removeFromParentAndCleanup(false);
            parent->addChild(m_crowd, z);
        } else if (m_crowd->getZOrder() != z)
            parent->reorderChild(m_crowd, z);
    }
    // 장면을 정지 / 재개한다. 정지할 때 입력을 모두 놓고 PlayLayer 스케줄러를 멈춘다.
    void freeze(PlayLayer* play, bool frozen) {
        if (frozen == m_frozen) return;
        m_frozen = frozen;
        if (frozen) {
            letGo(play);
            play->pauseSchedulerAndActions();
        } else
            play->resumeSchedulerAndActions();
    }
    // lounge PlayLayer를 만든다: 저장하지 않는 임시 레벨("Season 2 Lounge")에 선택한 맵 데이터를 넣고
    // PlayLayer::create를 호출한다. 이때 다른 모드를 일시 정지하고 훅이 이 레이어를 식별하게 한다.
    // 실패하면 이후 시도하지 않는다 (m_failed).
    void attach() {
        auto* level = GJGameLevel::create();
        level->m_gameVersion = kLoungeGameVersion;
        std::string const data = levelData(Service::get().rankedCup());
        m_levelSongs = triggerSongs(data);
        level->m_levelString = versus::compat::compressLevelString(data);
        level->m_levelName = "Season 2 Lounge";
        level->m_levelType = GJLevelType::Saved;
        level->m_levelID = 0;
        level->m_dontSave = true;
        level->m_audioTrack = 0;
        level->m_songID = 0;
        m_timeScale = CCDirector::sharedDirector()->getScheduler()->getTimeScale();
        pauseBattleMods();
        s_constructing = true;
        auto* layer = versus::compat::createPlayLayer(level);
        s_constructing = false;
        if (!layer || layer != s_identity || !s_play.lock()) {
            log::warn("Versus: the Season 2 lounge could not be opened");
            m_failed = true;
            s_identity = nullptr;
            s_play = nullptr;
            return;
        }
        m_music = 0.f;
        ++m_builds;
        s_teleportsLogged = 0;
        addChild(layer);
        quiet(layer);
        if (m_badge) m_badge->setVisible(false);
        audio::keepRankedMusic(m_levelSongs);
        showCursor();
        log::info("Versus: Season 2 lounge open (game version {}, {} song trigger song(s))", level->m_gameVersion,
                  m_levelSongs.size());
    }
    // 지금 붙여도 되는지: Season 2 사용 중, 화면이 실행 중이고 전환 중이 아님, 실제 플레이 / 매치 / 프리플레이 /
    // 리플레이가 없음, 맵 확인이 끝남, 호스트가 허용함.
    bool mayAttach(bool early = false) {
        if (m_failed || m_leaving || !Service::get().season2()) return false;
        if (!early) {
            if (!isRunning()) return false;
            auto* director = CCDirector::sharedDirector();
            auto* running = director->getRunningScene();
            if (!running || director->getNextScene() || typeinfo_cast<CCTransitionScene*>(running) ||
                sceneOf(this) != running)
                return false;
        }
        if (PlayLayer::get() || GJBaseGameLayer::get() || isMatchLaunchActive() || freeplay::active() ||
            freeplay::opening() || replay::viewing() || replay::preparing())
            return false;
        if (!LoungeMap::of(Service::get().rankedCup()).ready()) return false;
        return !m_hooks.attachAllowed || m_hooks.attachAllowed();
    }
    // UI가 부르는 공개 메서드들. attachNow: 즉시 붙이기, detach: 레벨을 제거하고 상태 초기화.
public:
    static Host* create(Hooks hooks) {
        auto* host = new Host;
        if (host->init(std::move(hooks))) {
            host->autorelease();
            return host;
        }
        delete host;
        return nullptr;
    }
    void attachNow() {
        if (!s_play.lock() && !s_constructing && mayAttach(true)) attach();
    }
    void detach() {
        auto play = s_play.lock();
        bool const ours = play && play->getParent() == this;
        m_holdWanted = false;
        m_holdY.reset();
        if (m_decor) m_decor->removeFromParentAndCleanup(false);
        if (m_billboard) m_billboard->removeFromParentAndCleanup(false);
        if (m_shadow) m_shadow->removeFromParentAndCleanup(false);
        if (m_crowd) m_crowd->removeFromParentAndCleanup(false);
        m_billboardBuild = -1;
        m_billboardAt.reset();
        m_crowdBuild = -1;
        m_crowdZ.reset();
        if (ours) {
            if (auto* scheduler = CCDirector::sharedDirector()->getScheduler();
                scheduler->getTimeScale() != m_timeScale)
                scheduler->setTimeScale(m_timeScale);
            if (auto* ui = play->m_uiLayer) versus::compat::releaseKeyboard(ui);
            versus::compat::releaseKeyboard(play);
            play->removeFromParentAndCleanup(true);
            audio::endLevelSong();
            log::info("Versus: Season 2 lounge closed");
        }
        if (ours || !play) {
            s_identity = nullptr;
            s_play = nullptr;
        }
        m_frozen = false;
        m_settle = 0.f;
        if (m_badge) m_badge->setVisible(false);
        m_startSpot.reset();
        m_lastSpot.reset();
        m_spawnRoom = m_spawnInStep = false;
        m_restart = false;
        m_runFor = 0.f;
        m_mapChannels.reset();
        m_roomShown.reset();
        m_uiHidden = false;
        m_zoom = 1.f;
        m_toward = 0.f;
        m_shake = CCPointZero;
        setScale(1.f);
        setPosition({0.f, -kDrop});
    }
    // 배지 / 장식 / 안내판 / 군중 노드를 등록한다 (기존 것은 제거, 새 노드는 부모에서 떼어 호스트가 관리).
    void setBadge(CCNode* badge) {
        if (m_badge == badge) return;
        if (m_badge) m_badge->removeFromParent();
        m_badge = badge;
        if (!badge) return;
        badge->removeFromParent();
        badge->setVisible(false);
        addChild(badge, 20);
    }
    void setDecor(CCNode* decor) {
        if (m_decor == decor) return;
        if (m_decor) m_decor->removeFromParent();
        m_decor = decor;
        if (decor) decor->removeFromParent();
    }
    void setBillboard(CCNode* board) {
        if (m_billboard == board) return;
        if (m_billboard) m_billboard->removeFromParent();
        m_billboard = board;
        m_billboardBuild = -1;
        m_billboardAt.reset();
        if (board) board->removeFromParent();
    }
    void setCrowd(CCNode* crowd) {
        if (m_crowd == crowd) return;
        if (m_crowd) m_crowd->removeFromParent();
        m_crowd = crowd;
        m_crowdBuild = -1;
        if (crowd) crowd->removeFromParent();
    }
    bool crowdInLevel() const { return m_crowd && m_crowd->getParent() && m_crowd->getParent() != this; }
    // 조작 가능한 내 아이콘 (장면이 정지되었거나 죽었으면 nullptr)과 그 위치 / 회전 / 방향.
    PlayerObject* walker() {
        auto play = s_play.lock();
        if (!play || play->getParent() != this || m_frozen || !play->m_started) return nullptr;
        auto* player = play->m_player1;
        return player && player->getParent() && !player->m_isDead ? player : nullptr;
    }
    bool pose(Pose& out) {
        auto* player = walker();
        if (!player) return false;
        out.x = player->getPositionX();
        out.y = player->getPositionY();
        out.rotation = player->getRotation();
        out.left = player->m_isGoingLeft;
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.rotation);
    }
    bool spot(CCPoint& out) {
        auto play = s_play.lock();
        return play && play->getParent() == this && localSpot(play.data(), out);
    }
    // 카메라 설정 (줌 0.5~4, 초점으로 이동하는 비율 0~1, 흔들림).
    void camera(float zoom, CCPoint focus, float toward, CCPoint shake) {
        m_zoom = std::isfinite(zoom) ? std::clamp(zoom, .5f, 4.f) : 1.f;
        m_focus = focus;
        m_toward = std::isfinite(toward) ? std::clamp(toward, 0.f, 1.f) : 0.f;
        m_shake = shake;
    }
    // cinematic: 연출 중에는 GD UI와 입력을 숨긴다. hold / pin: 켜면 현재 높이와 각도로 아이콘을 고정한다.
    void cinematic(bool on) { m_cinematic = on; }
    void hold(bool on) {
        m_holdWanted = on;
        m_holdY.reset();
        if (on)
            if (auto* player = walker()) pin(player);
    }
    void pin(PlayerObject* player) {
        if (!m_holdWanted || !player) return;
        if (!m_holdY) {
            m_holdY = player->getPositionY();
            float const turn = player->getRotation();
            m_holdTurn = std::isfinite(turn) ? std::round(turn / 90.f) * 90.f : 0.f;
            player->m_platformerXVelocity = 0.;
        }
        if (!std::isfinite(*m_holdY)) return;
        player->m_yVelocity = 0.;
        CCPoint const at{player->getPositionX(), *m_holdY};
        player->m_position = at;
        player->setPosition(at);
        if (player->getRotation() != m_holdTurn) player->setRotation(m_holdTurn);
    }
    // 레벨의 셰이더 효과를 초기화한다.
    void stopEffects() {
        auto play = s_play.lock();
        if (!play || play->getParent() != this || !play->m_started) return;
        if (auto* shaders = play->m_shaderLayer) shaders->resetAllShaders();
    }
    int builds() const { return m_builds; }
    // 아이콘을 가로로 dx만큼 옮기고 화면에서 실제로 움직인 거리를 반환한다.
    float shift(float dx) {
        auto* player = walker();
        auto play = s_play.lock();
        CCPoint before, after;
        if (!player || !play || !std::isfinite(dx) || !localSpot(play.data(), before)) return 0.f;
        place(player, player->getPosition() + CCPoint{dx, 0.f});
        return localSpot(play.data(), after) ? after.x - before.x : 0.f;
    }
    // 매치 방 색(메인 / 보조 색 채널 10, 11)을 바꾼다. 맵 원래 색을 저장해 두었다가 None이면 복원한다.
    void roomColors(std::optional<std::pair<uint32_t, uint32_t>> colours) { m_roomWanted = colours; }
    void applyRoomColors(PlayLayer* play) {
        if (m_roomShown == m_roomWanted) return;
        auto* effects = play->m_effectManager;
        if (!effects) return;
        int const ids[] = {kRoomMain, kRoomSub};
        if (!m_mapChannels) {
            std::array<Channel, 2> own;
            for (int i = 0; i < 2; ++i)
                if (auto* action = versus::compat::colorAction(effects, ids[i]))
                    own[i] = {action->m_color, action->m_blending,
                              std::isfinite(action->m_currentOpacity) ? std::clamp(action->m_currentOpacity, 0.f, 1.f)
                                                                      : 1.f};
            m_mapChannels = own;
        }
        for (int i = 0; i < 2; ++i) {
            auto const& own = (*m_mapChannels)[i];
            ccColor3B colour = own.color;
            if (m_roomWanted) {
                uint32_t const packed = i ? m_roomWanted->second : m_roomWanted->first;
                colour = ccc3(packed >> 16 & 255, packed >> 8 & 255, packed & 255);
            }
            ccHSVValue hsv = cchsv(0.f, 1.f, 1.f, false, false);
            play->updateColor(colour, kRoomFade, ids[i], own.blending, own.opacity, hsv, 0, false, nullptr, 0, 0);
        }
        m_roomShown = m_roomWanted;
    }
    // pendingMove / startSpot: 대기 중인 텔레포트 목표와 처음 서 있던 위치를 돌려준다.
    bool pendingMove(CCPoint& out) const {
        if (!m_teleport) return false;
        out = *m_teleport;
        return true;
    }
    bool startSpot(CCPoint& out) const {
        if (!m_startSpot || !std::isfinite(m_startSpot->x) || !std::isfinite(m_startSpot->y)) return false;
        out = *m_startSpot;
        return true;
    }
    // 화면 좌표가 GD의 이동 / 점프 버튼 근처(여유 24)인지 판정한다 (버튼 위 터치는 UI가 가로채지 않도록).
    bool onControls(CCPoint at) {
        auto play = s_play.lock();
        if (!play || play->getParent() != this) return false;
        auto* ui = play->m_uiLayer;
        if (!ui || !ui->isVisible() || !ui->m_uiNodes) return false;
        constexpr float pad = 24.f;
        for (auto* node : CCArrayExt<CCNode*>(ui->m_uiNodes)) {
            if (!node || !node->isVisible() || !node->getParent()) continue;
            auto* button = versus::compat::gdCast<GJUINode>(node);
            CCNode* const parts[] = {node, button ? button->m_firstSprite : nullptr,
                                     button ? button->m_secondSprite : nullptr};
            for (auto* part : parts) {
                if (!part || !part->getParent()) continue;
                auto const size = part->getContentSize();
                if (size.width <= 0.f || size.height <= 0.f) continue;
                auto const a = part->convertToWorldSpace(CCPointZero);
                auto const b = part->convertToWorldSpace({size.width, size.height});
                if (at.x >= std::min(a.x, b.x) - pad && at.x <= std::max(a.x, b.x) + pad &&
                    at.y >= std::min(a.y, b.y) - pad && at.y <= std::max(a.y, b.y) + pad)
                    return true;
            }
        }
        return false;
    }
    // 텔레포트는 다음 프레임 이후(kPlaceSettle 뒤)에 적용된다. spawnRoomGroup: 매치 방 그룹 스폰 요청.
    // restart: 매치가 끝났을 때 레벨을 다시 시작.
    CCNode* effects() { return m_effects; }
    void teleport(CCPoint level) {
        if (std::isfinite(level.x) && std::isfinite(level.y)) m_teleport = level;
    }
    void spawnRoomGroup() { m_spawnRoom = true; }
    void restart() {
        m_restart = true;
        m_teleport.reset();
        m_spawnRoom = m_spawnInStep = false;
    }
    // 물리 스텝 시작 시 대기 중인 방 그룹 스폰을 실행한다 (processCommands 훅에서 호출).
    void stepBegin(PlayLayer* play) {
        if (std::exchange(m_spawnInStep, false)) spawnRoom(play);
    }
    void spawnRoom(PlayLayer* play) {
        versus::compat::Vector<int> remap;
        play->spawnGroup(kRoomGroup, false, 0., remap, 0, 0);
        log::info("Versus: Season 2 lounge: group {} spawned (a match's room)", kRoomGroup);
    }
    // 그룹에 속한 오브젝트 중 위치 기준점을 찾는다 (그룹 소속 스폰 트리거 > 그룹 소속 > 그 그룹을 대상으로 하는 스폰 트리거).
    static bool groupSpot(PlayLayer* play, int group, CCPoint& out) {
        GameObject* found = nullptr;
        int rank = 0;
        if (auto* objects = play->m_objects) {
            for (auto* object : CCArrayExt<GameObject*>(objects)) {
                if (!object) continue;
                bool const spawn = object->m_objectID == kSpawnTriggerID;
                bool const member = inGroup(object, group);
                int score = spawn && member ? 3 : member ? 2 : 0;
                if (!score && spawn)
                    if (auto* effect = typeinfo_cast<EffectGameObject*>(object);
                        effect && effect->m_targetGroupID == group)
                        score = 1;
                if (score > rank) {
                    rank = score;
                    found = object;
                    if (score == 3) break;
                }
            }
        }
        if (!found) return false;
        out = found->getRealPosition();
        return std::isfinite(out.x) && std::isfinite(out.y);
    }
    // 매치 방 기준점 (방 그룹 오브젝트, 없으면 시작 위치).
    bool roomAnchor(CCPoint& out) {
        auto play = s_play.lock();
        if (!play || play->getParent() != this) return false;
        if (groupSpot(play.data(), kRoomGroup, out)) return true;
        if (!m_startSpot) return false;
        out = *m_startSpot;
        return true;
    }
    // 레벨 좌표를 호스트 좌표로 변환한다.
    bool toHost(CCPoint level, CCPoint& out) {
        auto play = s_play.lock();
        if (!play || play->getParent() != this) return false;
        auto* player = play->m_player1;
        if (!player || !player->getParent()) return false;
        out = convertToNodeSpace(player->getParent()->convertToWorldSpace(level));
        return true;
    }
    // 뒤로 가기 전달 / 포기 처리 / 키 입력 (입력이 허용된 때만 눌림을 받음).
    void back() {
        if (m_hooks.back) m_hooks.back();
    }
    void giveUp() { m_failed = true; }
    void key(int button, bool pressed, double at) {
        if (pressed && !inputAllowed()) return;
        (button == 1 ? m_keyJump : button == 2 ? m_keyLeft : m_keyRight) = pressed;
        if (auto play = s_play.lock(); play && play->getParent() == this) keys(play.data(), at);
    }
    // 입력 허용 여부: 떠나는 중, 정지, 연출 중이 아니고 호스트 콜백이 허용할 때. 터치는 touchFree도 확인한다.
    bool inputAllowed() const {
        return !m_leaving && !m_frozen && !m_cinematic && (!m_hooks.inputAllowed || m_hooks.inputAllowed());
    }
    bool touchAllowed(CCPoint at) const {
        return inputAllowed() && (!m_hooks.touchFree || m_hooks.touchFree(at.x, at.y));
    }
    // 화면에 들어오면 활성 호스트로 등록, 떠나기 시작하면 장면을 정지, 완전히 나가면 detach한다.
    void onEnter() override {
        CCLayer::onEnter();
        s_loungeHost = this;
        m_leaving = false;
        m_settle = 0.f;
        m_inputWas = true;
    }
    void onExitTransitionDidStart() override {
        m_leaving = true;
        if (auto play = s_play.lock(); play && play->getParent() == this) freeze(play, true);
        CCLayer::onExitTransitionDidStart();
    }
    void onExit() override {
        m_leaving = true;
        detach();
        if (s_loungeHost.peek() == this) s_loungeHost = nullptr;
        CCLayer::onExit();
    }
    ~Host() override {
        if (s_loungeHost.peek() == this) s_loungeHost = nullptr;
    }
    // 매 프레임: 아직 붙지 않았으면 대기 후 attach(). 붙은 뒤에는 다른 곳에서 PlayLayer가 열렸거나 매치 / 프리플레이가 시작되면 detach한다.
    // 그 외에는 UI 숨김, 입력 처리, 재시작 / 텔레포트 / 고정 / 방 색 적용, 음악 유지,
    // 그리고 kFallY 아래로 떨어지면 레벨을 다시 시작한다.
    void update(float dt) override {
        dt = std::clamp(dt, 0.f, .25f);
        auto play = s_play.lock();
        if (!play || play->getParent() != this) {
            if (play || m_leaving) return;
            m_settle += dt;
            if (m_settle >= kSettleSeconds && mayAttach()) attach();
            return;
        }
        if (PlayLayer::get() != play.data() || !Service::get().season2() || isMatchLaunchActive() ||
            freeplay::active() || freeplay::opening() || replay::preparing()) {
            detach();
            return;
        }
        quiet(play);
        freeze(play, m_leaving || (m_hooks.attachAllowed && !m_hooks.attachAllowed()));
        if (m_frozen) return;
        bool const open = inputAllowed();
        if (m_inputWas && !open) letGo(play);
        m_inputWas = open;
        keys(play, play->m_timestamp);
        if (auto* player = play->m_player1; player && play->m_started) {
            if (m_restart) {
                m_restart = false;
                play->resetLevel();
                m_runFor = 0.f;
                m_lastSpot.reset();
                m_holdY.reset();
                log::info("Versus: Season 2 lounge restarted (the match is over)");
            }
            if (!m_startSpot) m_startSpot = player->getPosition();
            m_runFor += dt;
            bool const placed = m_teleport.has_value() && m_runFor >= kPlaceSettle;
            if (placed)
                place(player, *std::exchange(m_teleport, std::nullopt));
            else if (!m_teleport && std::exchange(m_spawnRoom, false))
                m_spawnInStep = true;
            if (placed || player->m_isDead) m_holdY.reset();
            if (m_holdWanted && !placed && !player->m_isDead) pin(player);
            CCPoint const spot = player->getPosition();
            if (m_lastSpot && !placed) noteJump(*m_lastSpot, spot, dt);
            m_lastSpot = spot;
            applyRoomColors(play.data());
        }
        if ((m_music += dt) >= kMusicCheck) {
            m_music = 0.f;
            audio::keepRankedMusic(m_levelSongs);
            showCursor();
        }
        if (auto* player = play->m_player1; player && play->m_started && player->getPositionY() < kFallY) {
            play->resetLevel();
            m_runFor = 0.f;
            m_lastSpot.reset();
            m_holdY.reset();
        }
    }
    // 그리기 직전에 아이콘 고정, UI 정리, 배지 / 장식 배치, 카메라를 적용한다.
    void visit() override {
        if (auto play = s_play.lock(); play && play->getParent() == this) {
            if (m_holdWanted)
                if (auto* player = walker()) pin(player);
            quiet(play);
            placeBadge(play);
            applyCamera(play);
        }
        CCLayer::visit();
    }
};

// 현재 살아 있는 호스트, 그리고 lounge PlayLayer를 호스트에서 떼어 내는 정리 함수 (giveUp이면 이후 다시 붙지 않음).
Host* liveHost() {
    auto host = s_loungeHost.lock();
    return host ? host.data() : nullptr;
}
void yieldTo(bool giveUp) {
    if (s_constructing || !s_identity) return;
    if (auto play = s_play.lock()) {
        if (auto* host = typeinfo_cast<Host*>(play->getParent())) {
            host->detach();
            if (giveUp) host->giveUp();
        } else {
            if (auto* ui = play->m_uiLayer) versus::compat::releaseKeyboard(ui);
            versus::compat::releaseKeyboard(play);
            play->removeFromParentAndCleanup(true);
        }
    }
    s_identity = nullptr;
    s_play = nullptr;
}
// PlayLayer::init 훅에서 lounge 레이어를 식별용으로 등록하고, 키 코드를 GD 버튼 번호로 바꾸는 도우미들.
void bind(PlayLayer* layer) {
    s_identity = layer;
    rebind(s_play, layer);
}
void forwardBack() {
    if (auto* host = liveHost()) host->back();
}
void quitFrom(PlayLayer* layer) {
    if (layer && layer == s_identity) yieldTo(true);
}
bool inputOpen() {
    auto* host = liveHost();
    return host && host->inputAllowed();
}
void key(int button, bool pressed, double at) {
    if (auto* host = liveHost()) host->key(button, pressed, at);
}
void stepBegin(GJBaseGameLayer* layer) {
    auto play = s_play.lock();
    if (!play || static_cast<GJBaseGameLayer*>(play.data()) != layer) return;
    if (auto* host = typeinfo_cast<Host*>(play->getParent())) host->stepBegin(play.data());
}
int keyButton(enumKeyCodes code) {
    switch (code) {
    case KEY_W:
    case KEY_Up:
    case KEY_ArrowUp:
    case KEY_Space:
    case CONTROLLER_A: return 1;
    case KEY_A:
    case KEY_Left:
    case KEY_ArrowLeft:
    case CONTROLLER_Left: return 2;
    case KEY_D:
    case KEY_Right:
    case KEY_ArrowRight:
    case CONTROLLER_Right: return 3;
    default: return 0;
    }
}
bool touchOpen(CCTouch* touch) {
    auto* host = liveHost();
    return host && touch && host->touchAllowed(touch->getLocation());
}
}

// lounge.hpp의 공개 함수 구현. 대부분 host가 Host인지 확인하고 같은 이름의 메서드로 위임한다.
void setBadge(CCNode* host, CCNode* badge) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->setBadge(badge);
}
void setDecor(CCNode* host, CCNode* decor) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->setDecor(decor);
}
void setCrowd(CCNode* host, CCNode* crowd) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->setCrowd(crowd);
}
void setBillboard(CCNode* host, CCNode* board) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->setBillboard(board);
}
bool pose(CCNode* host, Pose& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->pose(out);
}
bool toHost(CCNode* host, CCPoint level, CCPoint& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->toHost(level, out);
}
bool crowdInLevel(CCNode* host) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->crowdInLevel();
}
// 맵 확인과 오디오 다운로드를 시작한다. mapLevel: 쓸 맵의 레벨 ID
// (에디터 맵 / 파일 맵이면 0, 내려받은 복사본이 없으면 내장 레벨 ID).
void prepareMap() {
    LoungeMap::standard().check();
    LoungeMap::demon().check();
    LoungeAudio::get().start(false);
}
bool mapReady() {
    return LoungeMap::standard().ready();
}
int mapLevel(std::string_view cup) {
    if (!s_editorMap.empty()) return 0;
    if (auto* mod = Mod::get())
        if (auto text = readFile(versus::compat::stdPath(mod->getSaveDir() / "season2-lounge.txt"));
            !trimmed(text).empty() && !platformerData(std::move(text)).empty())
            return 0;
    if (LoungeMap::of(cup).hasKept()) return LoungeMap::of(cup).levelID();
    return kLoungeLevelID;
}
// 맵 / 노래 / 효과음 다운로드 상태를 모으고, 재시도가 필요한지 판단한다.
Downloads downloads() {
    Downloads result;
    result.standard = LoungeMap::standard().status();
    result.demon = LoungeMap::demon().status();
    result.song = LoungeAudio::get().status(false);
    result.sfx = LoungeAudio::get().status(true);
    result.retry = LoungeMap::standard().wantsRetry() || LoungeMap::demon().wantsRetry() ||
                   LoungeAudio::get().wantsRetry() || (!result.song.busy && result.song.done < result.song.total) ||
                   (!result.sfx.busy && result.sfx.done < result.sfx.total);
    return result;
}
void resumeDownloads() {
    LoungeAudio::get().start(false);
}
void retryDownloads() {
    for (auto* map : {&LoungeMap::standard(), &LoungeMap::demon()})
        if (map->wantsRetry()) map->check(true);
    LoungeAudio::get().start(true);
}
// 에디터 Plaza Test 맵 등록 / 해제 / 조회.
bool setEditorMap(std::string const& text) {
    auto decoded = platformerData(text);
    if (decoded.empty()) return false;
    s_editorMap = std::move(decoded);
    return true;
}
void clearEditorMap() {
    s_editorMap.clear();
}
bool editorMap() {
    return !s_editorMap.empty();
}
void attachNow(CCNode* host) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->attachNow();
}
bool spot(CCNode* host, CCPoint& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->spot(out);
}
void camera(CCNode* host, float zoom, CCPoint focus, float toward, CCPoint shake) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->camera(zoom, focus, toward, shake);
}
void cinematic(CCNode* host, bool on) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->cinematic(on);
}
void hold(CCNode* host, bool on) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->hold(on);
}
void stopEffects(CCNode* host) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->stopEffects();
}
CCNode* effects(CCNode* host) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge ? lounge->effects() : nullptr;
}
bool roomAnchor(CCNode* host, CCPoint& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->roomAnchor(out);
}
void teleport(CCNode* host, CCPoint level) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->teleport(level);
}
void restart(CCNode* host) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->restart();
}
void spawnRoomGroup(CCNode* host) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->spawnRoomGroup();
}
int builds(CCNode* host) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge ? lounge->builds() : 0;
}
bool startSpot(CCNode* host, CCPoint& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->startSpot(out);
}
void roomColors(CCNode* host, std::optional<std::pair<uint32_t, uint32_t>> colours) {
    if (auto* lounge = typeinfo_cast<Host*>(host)) lounge->roomColors(colours);
}
float shift(CCNode* host, float dx) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge ? lounge->shift(dx) : 0.f;
}
bool pendingMove(CCNode* host, CCPoint& out) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->pendingMove(out);
}
bool onControls(CCNode* host, CCPoint screen) {
    auto* lounge = typeinfo_cast<Host*>(host);
    return lounge && lounge->onControls(screen);
}

// 호스트 생성 (Season 2가 아니면 만들지 않음). 그 뒤는 lounge 활성 여부 / 소유 확인 / 실제 PlayLayer / 해제.
CCNode* createHost(Hooks hooks) {
    if (!Service::get().season2()) return nullptr;
    return Host::create(std::move(hooks));
}
bool active() {
    return s_constructing || (s_identity && s_play.lock() != nullptr);
}
bool owns(GJBaseGameLayer const* layer) {
    return isLounge(layer);
}
PlayLayer* realPlayLayer() {
    auto* layer = PlayLayer::get();
    return layer && isLounge(layer) ? nullptr : layer;
}
void release() {
    PlayLayer* const layer = s_identity;
    if (!layer || s_constructing) return;
    yieldTo(false);
    releasePausedMods(layer);
}
}

// lounge 이동 / 점프 키(WASD, 방향키, Space, 컨트롤러)를 키보드 이벤트로 받아 호스트에 전달한다.
$execute {
    for (auto code : {KEY_W, KEY_Up, KEY_ArrowUp, KEY_Space, CONTROLLER_A, KEY_A, KEY_Left, KEY_ArrowLeft,
                      CONTROLLER_Left, KEY_D, KEY_Right, KEY_ArrowRight, CONTROLLER_Right}) {
        KeyboardInputEvent(code)
            .listen([code](KeyboardInputData& data) {
                int const button = versus::lounge::keyButton(code);
                if (data.action == KeyboardInputData::Action::Press)
                    versus::lounge::key(button, true, data.timestamp);
                else if (data.action == KeyboardInputData::Action::Release)
                    versus::lounge::key(button, false, data.timestamp);
                return ListenerResult::Propagate;
            })
            .leak();
    }
}

using versus::lounge::owns;
namespace lounge = versus::lounge;

// PlayLayer 훅: lounge가 만드는 레이어를 식별하고, lounge일 때는 GD 음악 시작, 일시정지, 사망, 클리어 화면,
// 플랫포머 종료 연출, 연습 모드를 막는다. 나가기는 다음 틱에 lounge를 정리한다.
class $modify(VersusLoungePlayLayer, PlayLayer){
    static void onModify(auto& self){(void)self.setHookPriorityPre("PlayLayer::init", Priority::First);
}
bool init(VERSUS_PLAY_INIT_PARAMS) {
    if (lounge::s_constructing) {
        lounge::bind(this);
        versus::attachPausedMods(this);
    } else {
        lounge::yieldTo(false);
    }
    return PlayLayer::init(VERSUS_PLAY_INIT_ARGS);
}
void setupHasCompleted() {
    if (!owns(this)) {
        PlayLayer::setupHasCompleted();
        return;
    }
    lounge::s_inSetup = true;
    PlayLayer::setupHasCompleted();
    lounge::s_inSetup = false;
    lounge::showCursor();
}
#if defined(GEODE_IS_MACOS)
void startGame() {
    PlayLayer::startGame();
    if (owns(this)) lounge::showCursor();
}
void resetLevel() {
    PlayLayer::resetLevel();
    if (owns(this)) lounge::showCursor();
}
#endif
void startMusic() {
    if (!owns(this)) PlayLayer::startMusic();
}
void pauseGame(bool unfocused) {
    if (!owns(this)) PlayLayer::pauseGame(unfocused);
}
void onQuit() {
    if (!owns(this)) {
        PlayLayer::onQuit();
        return;
    }
    PlayLayer* const self = this;
    Loader::get()->queueInMainThread([self] { lounge::quitFrom(self); });
}
// lounge에서는 GD의 안티치트 가시에만 죽고, 죽으면 재시도 화면 없이 자동 재시도로 다시 시작한다.
void destroyPlayer(PlayerObject* player, GameObject* object) {
    if (!owns(this)) {
        PlayLayer::destroyPlayer(player, object);
        return;
    }
    if (object != m_anticheatSpike) return;
    versus::compat::AutoRetryScope retry;
    PlayLayer::destroyPlayer(player, object);
}
void levelComplete() {
    if (!owns(this)) PlayLayer::levelComplete();
}
void showEndLayer() {
    if (!owns(this)) PlayLayer::showEndLayer();
}
void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
    if (!owns(this)) PlayLayer::playPlatformerEndAnimationToPos(position, instant);
}
void togglePracticeMode(bool practiceMode) {
    if (!owns(this) || !practiceMode) PlayLayer::togglePracticeMode(practiceMode);
}
}
;

// GJBaseGameLayer 훅: 입력이 막힌 동안 버튼 누름 무시, 오디오 정지 막기, 스텝 시작에 방 그룹 스폰.
class $modify(VersusLoungeGameLayer, GJBaseGameLayer){
    void handleButton(bool down, int button, bool isPlayer1){if (owns(this) && down && !lounge::inputOpen()) return;
GJBaseGameLayer::handleButton(down, button, isPlayer1);
}
void pauseAudio() {
    if (!owns(this)) GJBaseGameLayer::pauseAudio();
}
void processCommands(float dt, bool isHalfTick, bool isLastTick) {
    if (owns(this)) lounge::stepBegin(this);
    GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
}
}
;

// UILayer 훅: 터치가 허용될 때만 받고, 뒤로 가기는 호스트 콜백으로 넘긴다.
class $modify(VersusLoungeUILayer, UILayer){
    bool ccTouchBegan(CCTouch * touch, CCEvent* event){if (owns(m_gameLayer) && !lounge::touchOpen(touch)) return false;
return UILayer::ccTouchBegan(touch, event);
}
void keyBackClicked() {
    if (owns(m_gameLayer)) {
        lounge::forwardBack();
        return;
    }
    UILayer::keyBackClicked();
}
}
;

// lounge의 GD 키 입력(점프, 일시 정지 등)은 누를 때 무시한다. lounge 이동 키는 위의 키보드 이벤트가 따로 받는다.
// 키 입력 훅은 GD 버전마다 달라 compat/gd2081, compat/gd2091의 GameHooks.cpp에 있다.
bool versus::hooks::loungeKey(UILayer* ui, Key, bool down) {
    return down && versus::lounge::owns(ui->m_gameLayer);
}

// FMOD 훅: lounge 레벨을 만드는 중(setupHasCompleted 포함)에는 GD가 음악을 로드하거나 재생 위치를 옮기지 않게 막는다
// (랭크 음악 유지). GD의 음악 준비(prepareMusic)가 하는 일이 이 둘이고, 그 뒤의 음악 시작은 startMusic 훅이 막는다.
class $modify(VersusLoungeAudio, FMODAudioEngine){
    void loadMusic(versus::compat::String path, float speed, float unknown, float volume, bool shouldLoop, int musicID,
                   int channelID, bool dontReset){if (lounge::s_constructing || lounge::s_inSetup) return;
FMODAudioEngine::loadMusic(path, speed, unknown, volume, shouldLoop, musicID, channelID, dontReset);
}
void setMusicTimeMS(unsigned int time, bool dontWait, int musicID) {
    if (lounge::s_constructing || lounge::s_inSetup) return;
    FMODAudioEngine::setMusicTimeMS(time, dontWait, musicID);
}
}
;
