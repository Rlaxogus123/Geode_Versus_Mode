// 배틀 맵 준비를 맡는 캐시. GD 서버에서 레벨 데이터를 받고, 필요한 노래/효과음을 내려받으며,
// 재시도, 타임아웃, 오디오 건너뛰기(Ranked) 규칙을 적용해 완료 콜백을 호출한다.
// 내려받은 레벨은 최대 8개까지 메모리에 두고, 배틀용으로만 받은 레벨은 GD Saved 목록에서 빼 둔다.
#include "MapCache.hpp"
#include "BattleLevelData.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/MusicDownloadDelegate.hpp>
#include <Geode/binding/SongInfoObject.hpp>
#include <Geode/cocos/extensions/network/HttpRequest.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <utility>

using namespace geode::prelude;
namespace {
using Clock = std::chrono::steady_clock;
// "1,2,3" 형태의 ID 목록을 파싱해 양수 ID만 result에 넣는다.
void ids(std::set<int>& result, gd::string const& source) {
    std::string value(source);
    for (size_t start = 0; start < value.size();) {
        auto end = value.find(',', start);
        if (end == std::string::npos) end = value.size();
        int id = 0;
        auto parsed = std::from_chars(value.data() + start, value.data() + end, id);
        if (parsed.ec == std::errc() && parsed.ptr == value.data() + end && id > 0) result.insert(id);
        start = end + 1;
    }
}
// 레벨이 쓰는 (노래 ID 집합, 효과음 ID 집합).
std::pair<std::set<int>, std::set<int>> assets(GJGameLevel* level) {
    std::pair<std::set<int>, std::set<int>> result;
    if (level->m_songID > 0) result.first.insert(level->m_songID);
    ids(result.first, level->m_songIDs);
    ids(result.second, level->m_sfxIDs);
    return result;
}
using versus::AudioNeed;
// 작업 대기열을 처리하는 싱글턴. 레벨/음악 다운로드 델리게이트를 겸한다.
// 맵 하나씩(current) 순서대로 처리하며 같은 맵의 호출자는 하나의 작업에 합쳐진다.
class Cache final : public CCNode, public LevelDownloadDelegate, public MusicDownloadDelegate {
    // 호출자: 완료 콜백, 오디오 요구 수준, 미리 받기 여부.
    struct Caller {
        versus::Done done;
        AudioNeed need;
        bool prefetch;
    };
    struct Job {
        int id;
        std::vector<Caller> callbacks;
    };
    // Ranked에서 오디오 건너뛰기로 한 기록(맵별 노래/효과음, 전체 무음). 일정 시간 동안만 유효.
    struct Waiver {
        std::map<int, Clock::time_point> songs, sounds;
        std::optional<Clock::time_point> noAudio;
    };
    // 맵별 건너뛰기 기록.
    std::map<int, Waiver> waivers;
    std::string skippedRoom, skippedDraw;
    int64_t skippedLevel = 0;
    // 미리 받기 중 포기한 오디오(맵과 무관하게 현재 작업 동안만 유지).
    std::set<int> droppedSongs, droppedSounds;
    Clock::time_point waitFrom;
    std::deque<Job> queue;
    std::optional<Job> current;
    // 메모리에 보관한 레벨(최대 8개)과 최근 사용 순번(LRU).
    std::map<int, Ref<GJGameLevel>> levels;
    std::map<int, std::pair<std::set<int>, std::set<int>>> dependencies;
    std::map<int, uint64_t> levelUse;
    // Platformer 판정 결과 캐시.
    std::map<int, bool> platformers;
    // 배틀용으로만 받아서 GD Saved에서 곧 뺄 레벨 / 이미 뺀 레벨의 원래 키 / 계속 남길 레벨.
    std::map<int, Ref<GJGameLevel>> unsaveSoon;
    std::map<int, std::string> unsaved;
    std::set<int> keepSaved;
    // 레벨 다운로드 시작 전에 GD Saved에 이미 있었는지.
    bool pendingWasSaved = true;
    uint64_t useCounter = 0;
    bool pending = false;
    int pendingID = 0;
    // generation은 작업 번호, requestGeneration은 진행 중 레벨 요청이 속한 작업 번호(취소 감지용).
    uint64_t generation = 0, requestGeneration = 0;
    bool watchingMusic = false, askedURL = false;
    // 현재 작업이 필요로 하는 오디오와 이미 요청한 항목들.
    std::set<int> songs, sounds, requestedSongs, requestedSounds, requestedInfo;
    // 항목별 시도 횟수와 다음 재시도 가능 시각.
    std::map<int, unsigned> songAttempts, soundAttempts, infoAttempts;
    std::map<int, Clock::time_point> songRetryAt, soundRetryAt, infoRetryAt;
    unsigned levelAttempts = 0;
    Clock::time_point began, progressed, levelRetryAt, audioBegan;
    std::string failure;
    size_t completedAssets = 0;
    int lastProgress = 0;
    float timer = 0.f;

public:
    // 맵 작업을 취소한다. 대기열에서 빼고 진행 중이면 실패로 마무리한다.
    void cancel(int64_t id) {
        if (skippedLevel == id) skippedLevel = 0;
        std::erase_if(queue, [id](auto const& job) { return job.id == id; });
        if (current && current->id == id) finish(false, "Map selection changed.");
    }
    // 선택 범위는 방/레벨/추첨에 묶고, 장면 전환 중에는 유지한다.
    bool audioSkipped(int64_t id) {
        auto const& room = versus::Service::get().room();
        if (!room || room->id != skippedRoom || room->level.id != skippedLevel ||
            (room->mapDraw ? room->mapDraw->id : std::string()) != skippedDraw)
            skippedLevel = 0;
        return id > 0 && skippedLevel == id;
    }
    bool skipAudio(int64_t id) {
        auto const& room = versus::Service::get().room();
        if (!room || room->level.id != id || id <= 0 || id > std::numeric_limits<int>::max()) return false;
        if ((!current || current->id != id) &&
            std::none_of(queue.begin(), queue.end(), [id](auto const& job) { return job.id == id; }))
            return false;
        skippedRoom = room->id;
        skippedDraw = room->mapDraw ? room->mapDraw->id : std::string();
        skippedLevel = id;
        if (current && current->id == id) {
            auto* music = MusicDownloadManager::sharedState();
            // GD의 stopDownload와 같은 방식으로 이 작업이 요청한 오디오만 취소한다.
            auto stop = [music](char const* key) {
                if (auto* request = static_cast<cocos2d::extension::CCHttpRequest*>(music->getDLObject(key)))
                    request->setShouldCancel(true);
                music->removeDLFromActive(key);
            };
            for (int song : requestedSongs) stop(music->getSongDownloadKey(song));
            for (int sound : requestedSounds) stop(music->getSFXDownloadKey(sound));
            requestedSongs.clear();
            requestedSounds.clear();
            // 노래 정보 요청은 메타데이터만 받으므로 완료되어도 오디오를 시작하지 않는다.
            requestedInfo.clear();
            // 오디오 실패 직후 눌러도 레벨 오류까지 숨기지는 않는다.
            if (auto* value = level(static_cast<int>(id)); value && !platformer(value)) {
                failure.clear();
                began = progressed = Clock::now();
            }
        }
        return true;
    }
    // 싱글턴 생성. 스케줄러에 등록해 매 프레임 update를 받는다.
    static Cache& get() {
        static auto* instance = [] {
            auto* cache = new Cache;
            cache->init();
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(cache, 1, false);
            cache->release();
            return cache;
        }();
        return *instance;
    }
    // 레벨을 메모리에 보관하고 사용 순번을 갱신한다. 8개를 넘으면 가장 오래 쓰지 않은 것을 버린다(현재 작업 제외).
    void remember(GJGameLevel* value) {
        int const id = value->m_levelID.value();
        levels[id] = value;
        levelUse[id] = ++useCounter;
        dependencies.erase(id);
        while (levels.size() > 8) {
            auto oldest = levelUse.end();
            for (auto it = levelUse.begin(); it != levelUse.end(); ++it) {
                if (it->first == id || (current && it->first == current->id)) continue;
                if (oldest == levelUse.end() || it->second < oldest->second) oldest = it;
            }
            if (oldest == levelUse.end()) break;
            dependencies.erase(oldest->first);
            levels.erase(oldest->first);
            levelUse.erase(oldest);
        }
    }
    // Platformer 레벨인지 판정한다. 결과는 캐시에 둔다. 레벨 데이터가 압축 상태면 풀어서 검사한다.
    bool platformer(GJGameLevel* value) {
        int const id = value->m_levelID.value();
        if (auto found = platformers.find(id); found != platformers.end()) return found->second;
        bool result = value->isPlatformer();
        if (!result && !value->m_levelString.empty()) {
            auto const& original = value->m_levelString;
            bool const compressed = std::string_view(original.c_str()).find(';') == std::string_view::npos;
            auto const decoded = compressed ? ZipUtils::decompressString(original, false, 11) : original;
            result = versus::platformerLevelData(std::string_view(decoded.c_str()));
        }
        if (result) log::warn("Versus map {} is a Platformer level; refusing it", id);
        platformers[id] = result;
        return result;
    }
    // 메모리 -> GD Saved 순으로 레벨을 찾는다. 레벨 데이터가 없으면 nullptr.
    GJGameLevel* level(int id) {
        if (auto found = levels.find(id); found != levels.end()) {
            levelUse[id] = ++useCounter;
            return found->second.data();
        }
        auto* saved = GameLevelManager::sharedState()->getSavedLevel(id);
        if (!saved || saved->m_levelString.empty()) return nullptr;
        remember(saved);
        return saved;
    }
    // 레벨이 쓰는 오디오 ID 목록(캐시).
    auto const& assetsFor(GJGameLevel* value) {
        int const id = value->m_levelID.value();
        auto found = dependencies.find(id);
        if (found == dependencies.end()) found = dependencies.emplace(id, assets(value)).first;
        return found->second;
    }
    // 건너뛰기 기록이 아직 유효한지.
    static bool fresh(Clock::time_point at) {
        return Clock::now() - at <= std::chrono::seconds(versus::kRankedAudioWaiverSeconds);
    }
    // 이 오디오를 건너뛰기로 한 상태인지(전체 무음 포함).
    bool waived(int id, int asset, bool sound) const {
        auto found = waivers.find(id);
        if (found == waivers.end()) return false;
        auto const& waiver = found->second;
        if (waiver.noAudio && fresh(*waiver.noAudio)) return true;
        auto const& assets = sound ? waiver.sounds : waiver.songs;
        auto at = assets.find(asset);
        return at != assets.end() && fresh(at->second);
    }
    // 오디오 하나를 건너뛰기로 기록한다.
    void waive(int id, int asset, bool sound) {
        (sound ? waivers[id].sounds : waivers[id].songs)[asset] = Clock::now();
        log::warn("Versus Ranked map {}: playing without {} {}", id, sound ? "sound" : "song", asset);
    }
    void waiveAudio(int id) { waivers[id].noAudio = Clock::now(); }
    bool dropped(int asset, bool sound) const { return (sound ? droppedSounds : droppedSongs).contains(asset); }
    // 오디오를 포기한다. 미리 받기면 이번 작업에서만 제외하고, 아니면 건너뛰기로 기록한다.
    void giveUp(int asset, bool sound) {
        if (prefetchOnly())
            (sound ? droppedSounds : droppedSongs).insert(asset);
        else
            waive(current->id, asset, sound);
    }
    // 요구 수준에 맞게 맵이 준비됐는지: 레벨 데이터가 있고 Platformer가 아니며 오디오가 모두 있거나 건너뛰기 상태.
    bool ready(int id, AudioNeed need = AudioNeed::All) {
        auto* value = level(id);
        if (!value || value->m_levelString.empty() || platformer(value)) return false;
        if (audioSkipped(id)) return true;
        auto const& [audio, effects] = assetsFor(value);
        auto* manager = MusicDownloadManager::sharedState();
        bool const ranked = need != AudioNeed::All, noAudio = need == AudioNeed::RankedNoAudio;
        return std::all_of(audio.begin(), audio.end(),
                           [&](int i) {
                               return noAudio || manager->isSongDownloaded(i) || (ranked && waived(id, i, false));
                           }) &&
               std::all_of(effects.begin(), effects.end(), [&](int i) {
                   return noAudio || manager->isSFXDownloaded(i) || (ranked && waived(id, i, true));
               });
    }
    // Ranked에서 건너뛴 오디오가 있으면 사용자에게 보여 줄 안내 문구를 만든다(없으면 빈 문자열).
    // RankedNoAudio면 이 맵을 무음으로 기록한다.
    std::string skipped(int id, AudioNeed need) {
        if (audioSkipped(id)) return {};
        if (need == AudioNeed::All) return {};
        auto* value = level(id);
        if (!value) return {};
        auto const& [audio, effects] = assetsFor(value);
        auto* manager = MusicDownloadManager::sharedState();
        int songsOut = 0, soundsOut = 0;
        for (int i : audio)
            if (!manager->isSongDownloaded(i)) ++songsOut;
        for (int i : effects)
            if (!manager->isSFXDownloaded(i)) ++soundsOut;
        if (!songsOut && !soundsOut) return {};
        if (need == AudioNeed::RankedNoAudio) {
            if (!current || current->id != id || jobNeed() == AudioNeed::RankedNoAudio) waiveAudio(id);
            if (songsOut && soundsOut) return "Always Download Song off: no song or sounds this match.";
            return songsOut ? "Always Download Song off: no song this match."
                            : "Always Download Song off: missing sounds skipped.";
        }
        if (songsOut && soundsOut) return "Song and sounds unavailable: playing without them.";
        if (songsOut) return "Song unavailable: this match plays without music.";
        return "Some sounds unavailable: playing without them.";
    }
    // 현재 작업 호출자들의 요구를 합친 수준: 하나라도 All이면 All, 모두 무음이면 RankedNoAudio.
    AudioNeed jobNeed() const {
        if (!current || current->callbacks.empty()) return AudioNeed::All;
        bool noAudio = true;
        for (auto const& caller : current->callbacks) {
            if (caller.need == AudioNeed::All) return AudioNeed::All;
            noAudio = noAudio && caller.need == AudioNeed::RankedNoAudio;
        }
        return noAudio ? AudioNeed::RankedNoAudio : AudioNeed::Ranked;
    }
    // 현재 작업이 모두 미리 받기 호출자뿐인지.
    bool prefetchOnly() const {
        return current && std::all_of(current->callbacks.begin(), current->callbacks.end(),
                                      [](Caller const& caller) { return caller.prefetch; });
    }
    // 맵 준비를 요청한다. Platformer면 거부하고, 이미 준비돼 있으면 바로 완료,
    // 같은 맵이 진행 중이거나 대기 중이면 호출자를 합치고, 아니면 대기열에 넣는다.
    void add(int id, versus::Done callback, AudioNeed need, bool prefetch) {
        if (auto* known = level(id); known && platformer(known)) {
            callback(false, versus::kPlatformerMapMessage);
            return;
        }
        if (need == AudioNeed::All)
            waivers.erase(id);
        else if (need == AudioNeed::Ranked)
            if (auto found = waivers.find(id); found != waivers.end()) found->second.noAudio.reset();
        if (ready(id, need)) {
            callback(true, skipped(id, need));
            return;
        }
        // 미리 받기 작업에 실제 호출자가 붙으면 대기 기준 시각과 재시도 기록을 초기화한다.
        if (current && current->id == id) {
            if (!prefetch && prefetchOnly()) {
                waitFrom = Clock::now();
                droppedSongs.clear();
                droppedSounds.clear();
                songAttempts.clear();
                soundAttempts.clear();
                infoAttempts.clear();
                songRetryAt.clear();
                soundRetryAt.clear();
                infoRetryAt.clear();
            }
            lastProgress = 0;
            progressed = Clock::now();
            current->callbacks.push_back({std::move(callback), need, prefetch});
            return;
        }
        for (auto& job : queue)
            if (job.id == id) {
                job.callbacks.push_back({std::move(callback), need, prefetch});
                return;
            }
        queue.push_back({id, {}});
        queue.back().callbacks.push_back({std::move(callback), need, prefetch});
    }
    // 현재 작업이 끝낼 수 있는 상태인지: 준비 완료이거나, Ranked에서 남은 오디오가 모두 건너뛰기/포기 상태.
    bool settled(AudioNeed need) {
        if (ready(current->id, need)) return true;
        if (need == AudioNeed::All || (droppedSongs.empty() && droppedSounds.empty())) return false;
        auto* value = level(current->id);
        if (!value || value->m_levelString.empty() || platformer(value)) return false;
        auto const& [audio, effects] = assetsFor(value);
        auto* manager = MusicDownloadManager::sharedState();
        return std::all_of(audio.begin(), audio.end(),
                           [&](int i) {
                               return need == AudioNeed::RankedNoAudio || manager->isSongDownloaded(i) ||
                                      waived(current->id, i, false) || dropped(i, false);
                           }) &&
               std::all_of(effects.begin(), effects.end(), [&](int i) {
                   return need == AudioNeed::RankedNoAudio || manager->isSFXDownloaded(i) ||
                          waived(current->id, i, true) || dropped(i, true);
               });
    }
    // 현재 작업을 마무리하고 모든 호출자에게 결과를 알린다. 작업 상태를 모두 초기화한다.
    void finish(bool success, std::string detail) {
        if (!current) return;
        if (success && detail.empty()) detail = skipped(current->id, jobNeed());
        if (watchingMusic) MusicDownloadManager::sharedState()->removeMusicDownloadDelegate(this);
        watchingMusic = false;
        auto callbacks = std::move(current->callbacks);
        current.reset();
        songs.clear();
        sounds.clear();
        requestedSongs.clear();
        requestedSounds.clear();
        requestedInfo.clear();
        songAttempts.clear();
        soundAttempts.clear();
        infoAttempts.clear();
        songRetryAt.clear();
        soundRetryAt.clear();
        infoRetryAt.clear();
        droppedSongs.clear();
        droppedSounds.clear();
        for (auto& caller : callbacks)
            if (caller.done) caller.done(success, detail);
    }
    // 레벨 다운로드 델리게이트를 해제한다.
    void releaseDelegate() {
        pending = false;
        auto* manager = GameLevelManager::sharedState();
        if (manager->m_levelDownloadDelegate == this) manager->m_levelDownloadDelegate = nullptr;
    }
    // 레벨 다운로드 완료. 데이터가 없거나 Platformer이면 재시도 또는 실패 처리한다.
    // 배틀용으로만 받은 레벨은 GD Saved에서 뺄 예약을 건다.
    void levelDownloadFinished(GJGameLevel* value) override {
        if (!pending) return;
        bool valid = current && current->id == pendingID && generation == requestGeneration;
        if (!value) {
            releaseDelegate();
            if (valid) retryLevel("The selected map returned no data.");
            return;
        }
        if (value->m_levelID.value() != pendingID) return;
        releaseDelegate();
        if (!value->m_levelString.empty()) {
            remember(value);
            if (!pendingWasSaved) dropSoon(pendingID, value);
            if (valid && platformer(value)) failure = versus::kPlatformerMapMessage;
        } else if (valid)
            retryLevel("The selected map contains no level data.");
    }
    // 레벨 다운로드 재시도 예약. 최대 3회, 대기 시간은 2초 x 시도 횟수.
    void retryLevel(std::string const& detail) {
        log::warn("Versus map {} download attempt {} failed: {}", pendingID, levelAttempts, detail);
        if (levelAttempts >= 3)
            failure = detail;
        else
            levelRetryAt = Clock::now() + std::chrono::seconds(2 * levelAttempts);
    }
    void levelDownloadFailed(int id) override {
        if (!pending || (id != 0 && id != pendingID)) return;
        bool valid = current && current->id == pendingID && generation == requestGeneration;
        releaseDelegate();
        if (valid) retryLevel(fmt::format("Map {} could not be downloaded from the level server.", pendingID));
    }
    // 오디오 다운로드/정보 실패 처리. 사용할 수 없는 오디오(오류 2)이거나 3회 실패하면
    // Ranked는 건너뛰고, 그 외에는 작업 실패로 한다. 아니면 2초 x 시도 횟수 후 재시도.
    void audioFailed(int id, GJSongError error, bool sound, bool info = false) {
        auto& requested = info ? requestedInfo : sound ? requestedSounds : requestedSongs;
        if (!current || !requested.erase(id)) return;
        auto& attempts = info ? infoAttempts : sound ? soundAttempts : songAttempts;
        auto& retryAt = info ? infoRetryAt : sound ? soundRetryAt : songRetryAt;
        auto kind = info ? "Song information" : sound ? "Sound" : "Song";
        log::warn("Versus {} {} failed ({}), attempt {}", kind, id, static_cast<int>(error), attempts[id]);
        bool const lost = static_cast<int>(error) == 2 || attempts[id] >= 3;
        if (lost && jobNeed() != AudioNeed::All) {
            giveUp(id, sound);
            return;
        }
        if (static_cast<int>(error) == 2)
            failure = fmt::format("{} {} is unavailable or not allowed by the audio provider.", kind, id);
        else if (attempts[id] >= 3)
            failure = fmt::format("{} {} could not be fetched from the audio provider (error {}).", kind, id,
                                  static_cast<int>(error));
        else
            retryAt[id] = Clock::now() + std::chrono::seconds(2 * attempts[id]);
    }
    void loadSongInfoFinished(SongInfoObject* song) override {
        if (current && song && requestedInfo.erase(song->m_songID)) progressed = Clock::now();
    }
    void loadSongInfoFailed(int id, GJSongError error) override { audioFailed(id, error, false, true); }
    void downloadSongFailed(int id, GJSongError error) override { audioFailed(id, error, false); }
    void downloadSFXFailed(int id, GJSongError error) override { audioFailed(id, error, true); }
    // GD의 온라인 레벨 캐시(Saved 목록에 쓰는 딕셔너리).
    static CCDictionary* savedLevels() {
        auto* manager = GameLevelManager::sharedState();
        return manager ? manager->m_onlineLevels : nullptr;
    }
    // 딕셔너리에서 이 레벨의 키를 찾는다(없으면 빈 문자열).
    static std::string savedKey(CCDictionary* saved, int id) {
        if (!saved) return {};
        std::string const direct = std::to_string(id);
        if (auto* found = typeinfo_cast<GJGameLevel*>(saved->objectForKey(gd::string(direct)));
            found && found->m_levelID.value() == id)
            return direct;
        for (auto [key, value] : CCDictionaryExt<std::string, CCObject*>(saved)) {
            auto* level = typeinfo_cast<GJGameLevel*>(value);
            if (level && level->m_levelID.value() == id) return key;
        }
        return {};
    }

public:
    // 배틀용으로만 받은 레벨을 곧 Saved 목록에서 빼도록 예약한다(유지 대상이면 제외).
    void dropSoon(int id, GJGameLevel* value) {
        if (value && !keepSaved.contains(id)) unsaveSoon[id] = value;
    }
    // 이 레벨을 Saved에 계속 남긴다. 이미 뺐다면 원래 키로 다시 넣는다.
    void keep(int id) {
        if (!keepSaved.insert(id).second) return;
        unsaveSoon.erase(id);
        auto found = unsaved.find(id);
        if (found == unsaved.end()) return;
        auto* saved = savedLevels();
        auto* value = level(id);
        if (saved && value && savedKey(saved, id).empty()) {
            saved->setObject(value, gd::string(found->second));
            log::info("Versus map {}: back in GD's Saved (played from LIVE)", id);
        }
        unsaved.erase(found);
    }

private:
    // 예약된 레벨을 GD Saved 목록에서 실제로 제거한다.
    void unsaveNow() {
        if (unsaveSoon.empty()) return;
        auto* saved = savedLevels();
        for (auto& [id, value] : std::exchange(unsaveSoon, {})) {
            if (!saved || keepSaved.contains(id)) continue;
            auto const key = savedKey(saved, id);
            if (key.empty()) continue;
            saved->removeObjectForKey(gd::string(key));
            unsaved[id] = key;
            log::info("Versus map {}: taken out of GD's Saved (downloaded only for Versus)", id);
        }
    }

public:
    // 매 프레임(0.2초 간격으로 진행): 대기열에서 작업을 꺼내고, 레벨을 받고, 필요한 오디오를 최대 3개씩 동시에 내려받는다.
    void update(float dt) override {
        unsaveNow();
        audioSkipped(skippedLevel);
        timer -= dt;
        if (timer > 0.f) return;
        timer = .2f;
        // 취소된 요청이 남긴 델리게이트를 정리한다.
        if (pending && (!current || generation != requestGeneration)) {
            auto* manager = GameLevelManager::sharedState();
            if (!manager->isDLActive(manager->getLevelDownloadKey(pendingID, false, 0))) releaseDelegate();
        }
        // 다음 작업 시작: 상태를 초기화한다.
        if (!current) {
            if (queue.empty()) return;
            current = std::move(queue.front());
            queue.pop_front();
            ++generation;
            began = progressed = waitFrom = Clock::now();
            levelRetryAt = began;
            levelAttempts = 0;
            completedAssets = 0;
            lastProgress = 0;
            failure.clear();
            askedURL = false;
        }
        if (!failure.empty()) {
            finish(false, std::exchange(failure, {}));
            return;
        }
        // 진행 없이 120초, 또는 전체 10분이 지나면 시간 초과로 실패시킨다.
        if (Clock::now() - progressed > std::chrono::seconds(120) || Clock::now() - began > std::chrono::minutes(10)) {
            finish(false, "Map/audio download timed out. Try again.");
            return;
        }
        auto* value = level(current->id);
        // 레벨 데이터가 없으면 다운로드를 요청한다(다른 다운로드가 진행 중이면 기다림).
        if (!value) {
            auto* manager = GameLevelManager::sharedState();
            if (Clock::now() < levelRetryAt || pending || manager->m_levelDownloadDelegate ||
                manager->isDLActive(manager->getLevelDownloadKey(current->id, false, 0)))
                return;
            pending = true;
            pendingID = current->id;
            requestGeneration = generation;
            ++levelAttempts;
            pendingWasSaved = manager->getSavedLevel(pendingID) != nullptr;
            manager->m_levelDownloadDelegate = this;
            manager->downloadLevel(pendingID, false, 0);
            return;
        }
        // 건너뛰기는 레벨 다운로드/검증 후에만 완료하며, 추가 오디오 요청을 만들지 않는다.
        if (audioSkipped(current->id)) {
            bool const valid = !value->m_levelString.empty() && !platformer(value);
            finish(valid, valid ? std::string() : versus::kPlatformerMapMessage);
            return;
        }
        // 레벨이 준비됐으면 필요한 오디오 목록을 잡고 음악 다운로드 델리게이트를 등록한다.
        auto* music = MusicDownloadManager::sharedState();
        AudioNeed const need = jobNeed();
        if (!watchingMusic) {
            auto const& required = assetsFor(value);
            songs = required.first;
            sounds = required.second;
            music->tryLoadLibraries();
            music->addMusicDownloadDelegate(this);
            watchingMusic = true;
            progressed = audioBegan = Clock::now();
            if (need == AudioNeed::RankedNoAudio) waiveAudio(current->id);
        }
        if (settled(need)) {
            finish(true, {});
            return;
        }
        // Ranked 오디오 대기 제한: 오디오 단계 25초 경과 + 8초 이상 진행 없음, 또는 시작 후 40초 경과이면
        // 못 받은 오디오를 건너뛰기로 기록하고 완료 처리한다(미리 받기는 기록하지 않음).
        if (need != AudioNeed::All) {
            auto const from = std::max(began, waitFrom), audioFrom = std::max(audioBegan, from);
            if ((Clock::now() - audioFrom >= std::chrono::seconds(versus::kRankedAudioSeconds) &&
                 Clock::now() - progressed >= std::chrono::seconds(versus::kRankedAudioStallSeconds)) ||
                Clock::now() - from >= std::chrono::seconds(versus::kRankedAudioCapSeconds)) {
                if (!prefetchOnly()) {
                    if (need == AudioNeed::Ranked)
                        for (int i : songs)
                            if (!music->isSongDownloaded(i) && !waived(current->id, i, false))
                                waive(current->id, i, false);
                    for (int i : sounds)
                        if (!music->isSFXDownloaded(i) && !waived(current->id, i, true)) waive(current->id, i, true);
                }
                finish(true, {});
                return;
            }
        }
        // 아직 받아야 하는 오디오인지(건너뛰었거나 포기한 것은 제외).
        auto const wanted = [&](int i, bool sound) {
            return need == AudioNeed::All || (!waived(current->id, i, sound) && !dropped(i, sound));
        };
        // 전체 진행 퍼센트가 늘면 진행 시각을 갱신한다(정지 판정용).
        auto const progress = versus::mapDownloadProgress(current->id, need != AudioNeed::All);
        if (progress.percent > lastProgress) {
            lastProgress = progress.percent;
            progressed = Clock::now();
        }
        // 커스텀 콘텐츠 URL이 필요한 오디오(외부 노래, 효과음)가 있는데 아직 모르면 요청하고 기다린다.
        bool needsURL =
            std::any_of(songs.begin(), songs.end(),
                        [&](int i) { return i > 10000000 && !music->isSongDownloaded(i) && wanted(i, false); }) ||
            std::any_of(sounds.begin(), sounds.end(),
                        [&](int i) { return !music->isSFXDownloaded(i) && wanted(i, true); });
        if (needsURL && music->m_customContentURL.empty()) {
            if (!askedURL) {
                askedURL = true;
                music->getCustomContentURL();
            }
            return;
        }
        // 완료된 오디오 수가 늘면 진행 시각을 갱신한다.
        size_t const completed =
            std::count_if(songs.begin(), songs.end(), [music](int id) { return music->isSongDownloaded(id); }) +
            std::count_if(sounds.begin(), sounds.end(), [music](int id) { return music->isSFXDownloaded(id); });
        if (completed > completedAssets) {
            completedAssets = completed;
            progressed = Clock::now();
        }
        // 동시에 최대 3개까지만 요청한다. 노래는 정보(URL)를 먼저 받아야 하며, 정보가 3회 실패하면 실패 처리한다.
        int pendingAssets = static_cast<int>(requestedInfo.size());
        for (int i : requestedSongs)
            if (!music->isSongDownloaded(i)) ++pendingAssets;
        for (int i : requestedSounds)
            if (!music->isSFXDownloaded(i)) ++pendingAssets;
        for (int i : songs) {
            if (pendingAssets >= 3) break;
            if (music->isSongDownloaded(i) || requestedSongs.contains(i) || music->isRunningActionForSongID(i) ||
                !wanted(i, false))
                continue;
            if (i <= 10000000) {
                auto* info = music->getSongInfoObject(i);
                if (!info || info->m_songUrl.empty() || info->m_unloaded) {
                    if (requestedInfo.contains(i) || Clock::now() < infoRetryAt[i]) continue;
                    if (infoAttempts[i] >= 3) {
                        if (need != AudioNeed::All) {
                            giveUp(i, false);
                            continue;
                        }
                        failure = fmt::format("Song {} has no downloadable audio URL.", i);
                        return;
                    }
                    requestedInfo.insert(i);
                    ++infoAttempts[i];
                    ++pendingAssets;
                    music->getSongInfo(i, false);
                    continue;
                }
            }
            if (Clock::now() < songRetryAt[i]) continue;
            requestedSongs.insert(i);
            ++songAttempts[i];
            ++pendingAssets;
            music->downloadSong(i);
        }
        // 효과음 다운로드 요청.
        for (int i : sounds) {
            if (pendingAssets >= 3) break;
            if (music->isSFXDownloaded(i) || requestedSounds.contains(i) ||
                music->isDLActive(music->getSFXDownloadKey(i)) || !wanted(i, true))
                continue;
            if (Clock::now() < soundRetryAt[i]) continue;
            requestedSounds.insert(i);
            ++soundAttempts[i];
            ++pendingAssets;
            music->downloadSFX(i);
        }
    }
};
}
namespace versus {
// 진행 단계별 화면 문구.
std::string MapDownloadProgress::caption() const {
    if (!mapReady) return "LOCAL: Downloading map...";
    if (complete())
        return songsSkipped || soundsSkipped ? "LOCAL: Map ready (missing audio skipped)" : "LOCAL: Map + audio ready";
    return fmt::format("LOCAL: Preparing {}%", percent);
}
// 음악/SFX 개수까지 포함한 상세 문구.
std::string MapDownloadProgress::detail() const {
    if (!mapReady) return "Map ...  |  Music / SFX: checking";
    auto const music = songsSkipped ? std::string("Music skipped") : fmt::format("Music {}/{}", songsDone, songsTotal);
    auto const sfx = soundsSkipped ? fmt::format("SFX {}/{} ({} skipped)", soundsDone, soundsTotal, soundsSkipped)
                                   : fmt::format("SFX {}/{}", soundsDone, soundsTotal);
    return fmt::format("Map OK  |  {}  |  {}", music, sfx);
}
// "항상 노래 다운로드" 설정. 기본값은 켜짐.
bool alwaysDownloadSong() {
    return Mod::get()->getSavedValue<bool>("ranked-always-download-song", true);
}
void setAlwaysDownloadSong(bool value) {
    Mod::get()->setSavedValue<bool>("ranked-always-download-song", value);
}
AudioNeed rankedAudioNeed() {
    return alwaysDownloadSong() ? AudioNeed::Ranked : AudioNeed::RankedNoAudio;
}
// 레벨 1 + 오디오 항목 수를 분모로 한 진행도. 다운로드 중인 항목은 진행률로 부분 반영한다(완료 전에는 99% 상한).
MapDownloadProgress mapDownloadProgress(int64_t id, bool ranked) {
    MapDownloadProgress result;
    auto* level = cachedLevel(id);
    if (!level || level->m_levelString.empty()) return result;
    result.mapReady = true;
    auto const& [songs, sounds] = Cache::get().assetsFor(level);
    result.songsTotal = static_cast<int>(songs.size());
    result.soundsTotal = static_cast<int>(sounds.size());
    auto* music = MusicDownloadManager::sharedState();
    auto& cache = Cache::get();
    bool const skipAudio = cache.audioSkipped(id);
    int const map = static_cast<int>(id);
    double units = 1.;
    for (int song : songs) {
        bool const skipped =
            !music->isSongDownloaded(song) && (skipAudio || (ranked && cache.waived(map, song, false)));
        bool const done = music->isSongDownloaded(song) || skipped;
        result.songsDone += done;
        result.songsSkipped += skipped;
        units += done ? 1. : std::clamp(music->getDownloadProgress(song), 0, 99) / 100.;
    }
    for (int sound : sounds) {
        bool const skipped =
            !music->isSFXDownloaded(sound) && (skipAudio || (ranked && cache.waived(map, sound, true)));
        bool const done = music->isSFXDownloaded(sound) || skipped;
        result.soundsDone += done;
        result.soundsSkipped += skipped;
        units += done ? 1. : std::clamp(music->getSFXDownloadProgress(sound), 0, 99) / 100.;
    }
    result.percent =
        result.complete()
            ? 100
            : std::clamp(static_cast<int>(100. * units / (1 + result.songsTotal + result.soundsTotal)), 0, 99);
    return result;
}
// 캐시된 레벨을 돌려준다. int 범위를 벗어난 ID는 nullptr.
GJGameLevel* cachedLevel(int64_t id) {
    return id > 0 && id <= std::numeric_limits<int>::max() ? Cache::get().level(static_cast<int>(id)) : nullptr;
}
// 맵 플레이 시간(초). 레벨의 측정 길이가 길이 분류와 맞으면 그것을 쓰고,
// 아니면 레벨 데이터를 풀어 속도 포탈로 추정한다. 3시간 초과나 불명은 음수. 결과는 캐시한다.
double mapPlaySeconds(int64_t id) {
    static std::map<int64_t, double> known;
    if (auto found = known.find(id); found != known.end()) return found->second;
    auto* level = cachedLevel(id);
    if (!level || level->m_levelString.empty()) return -1.;
    double seconds = level->m_timestamp > 0 ? level->m_timestamp / 240. : -1.;
    if (!versus::timeMatchesLength(seconds, level->m_levelLength)) {
        auto const& original = level->m_levelString;
        bool const compressed = std::string_view(original.c_str()).find(';') == std::string_view::npos;
        auto const decoded = compressed ? ZipUtils::decompressString(original, false, 11) : original;
        seconds = versus::levelDataSeconds(std::string_view(decoded.c_str()));
    }
    if (seconds > 3. * 3600.) seconds = -1.;
    known[id] = seconds < 0. ? -2. : seconds;
    return known[id];
}
// 2인 모드 여부(1/0). 레벨을 모르면 -1.
int mapTwoPlayer(int64_t id) {
    auto* level = cachedLevel(id);
    if (!level || level->m_levelString.empty()) return -1;
    return level->m_twoPlayerMode ? 1 : 0;
}
// GD에 저장된 이 맵의 최고 퍼센트. 저장이 없거나 플레이 기록이 전혀 없으면 -1.
int mapLocalBest(int64_t id) {
    if (id <= 0 || id > std::numeric_limits<int>::max()) return -1;
    auto* manager = GameLevelManager::sharedState();
    auto* level = manager ? manager->getSavedLevel(static_cast<int>(id)) : nullptr;
    if (!level) return -1;
    int const best = std::clamp(level->m_normalPercent.value(), 0, 100);
    if (best <= 0 && level->m_attempts.value() <= 0) return -1;
    return best;
}
// 요구 수준에 맞게 이미 준비된 맵인지.
bool mapCached(int64_t id, AudioNeed need) {
    return id > 0 && id <= std::numeric_limits<int>::max() && Cache::get().ready(static_cast<int>(id), need);
}
bool skipMapAudio(int64_t id) {
    return Cache::get().skipAudio(id);
}
bool mapAudioSkipped(int64_t id) {
    return Cache::get().audioSkipped(id);
}
// 맵 준비 요청의 공개 진입점. keepSaved이면 GD Saved에 계속 남긴다.
void ensureMapCached(int64_t id, Done callback, AudioNeed need, bool prefetch, bool keepSaved) {
    if (id <= 0 || id > std::numeric_limits<int>::max()) {
        callback(false, "Invalid map ID.");
        return;
    }
    if (keepSaved) Cache::get().keep(static_cast<int>(id));
    Cache::get().add(static_cast<int>(id), std::move(callback), need, prefetch);
}
// 레벨을 GD Saved에서 곧 뺄 예약.
void dropFromSaved(int64_t id, GJGameLevel* level) {
    if (id > 0 && id <= std::numeric_limits<int>::max()) Cache::get().dropSoon(static_cast<int>(id), level);
}
bool inGdSaved(int64_t id) {
    auto* manager = GameLevelManager::sharedState();
    return id > 0 && id <= std::numeric_limits<int>::max() && manager && manager->getSavedLevel(static_cast<int>(id));
}
// 배틀 화면에서 처음 쓸 때 끊김이 없도록 폰트와 스프라이트를 미리 로드한다.
void preloadBattleAssets() {
    CCLabelBMFont::create("VERSUS 0123456789%", "bigFont.fnt");
    CCLabelBMFont::create("Starting / Spectating / Paused", "chatFont.fnt");
    CCSprite::createWithSpriteFrameName("GJ_versusBtn_001.png");
}
// 해당 맵의 준비를 취소한다.
void cancelMapPreparation(int64_t id) {
    Cache::get().cancel(id);
}
}
