// Versus 효과음과 Ranked 음악 구현 (FMOD 사용).
// 효과음은 모드 리소스의 sounds 폴더에서 base 이름의 변형 파일(base, base1, base2 ...)을 무작위로 고르고
// (직전과 같은 파일은 피함), 없으면 GD 내장 효과음으로 대체한다.
// 음악은 FMOD 0번 채널에 Ranked/컵 음악을 틀어 두고, 라운지에서 맵 노래가 끼어들거나 끊기는 것을 감시한다.
#include "VersusAudio.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace versus::audio {
namespace {

// 결과 화면 음악이 쓰는 FMOD 채널 번호.
constexpr int kResultMusicChannel = 2;
// 상태: 결과 음악 재생 중 / Ranked 음악 재생 중과 그 파일 경로 / 컵이 지정한 노래 ID와 현재 그 노래를 쓰는지 /
// 라운지에서 감시 중인 맵 노래 경로와 실제 들렸는지(s_heard), 조용해진 확인 횟수.
bool s_resultMusicPlaying = false;
bool s_rankedMusicPlaying = false;
std::string s_rankedMusicPath;
int s_cupSong = 0;
bool s_playingCupSong = false;
std::string s_levelSong;
std::string s_watched;
bool s_heard = false;
int s_quietChecks = 0;

// 감시 중이던 맵 노래 상태를 초기화한다.
void forgetWatched() {
    s_levelSong.clear();
    s_watched.clear();
    s_heard = false;
    s_quietChecks = 0;
}

// 채널 0의 재생 상태 (정지 / 일시정지 / 재생 중).
enum class Music { Stopped, Paused, Playing };
Music channelZero(FMODAudioEngine* engine) {
    auto* channel = engine->getActiveMusicChannel(0);
    bool playing = false;
    if (!channel || channel->isPlaying(&playing) != FMOD_OK || !playing) return Music::Stopped;
    bool paused = false;
    return channel->getPaused(&paused) == FMOD_OK && paused ? Music::Paused : Music::Playing;
}

// 경로에서 파일 이름만 뽑는다.
std::string fileName(std::string const& path) {
    auto const slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// path의 파일 이름(확장자 제외)이 songs 목록의 노래 ID 중 하나와 같은지.
bool songOf(std::string const& path, std::vector<int> const& songs) {
    auto name = fileName(path);
    if (auto const dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    if (name.empty()) return false;
    return std::any_of(songs.begin(), songs.end(), [&](int song) { return song > 0 && name == std::to_string(song); });
}

// 내려받은 노래 ID의 파일 경로. 아직 없으면 빈 문자열.
std::string songPath(int song) {
    if (song <= 0) return {};
    auto* music = MusicDownloadManager::sharedState();
    if (!music || !music->isSongDownloaded(song)) return {};
    return std::string(music->pathForSong(song));
}

// 지금 채널 0에서 재생 중인 음악이 우리가 튼 Ranked 음악인지.
bool ownsRankedMusic(FMODAudioEngine* engine) {
    return engine && s_rankedMusicPlaying && !s_rankedMusicPath.empty() &&
           engine->getActiveMusic(0) == gd::string(s_rankedMusicPath);
}

// 볼륨 값을 0~kRankedMusicMax로 제한한다 (유한하지 않은 값이면 기본 0.65).
float safeVolume(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, kRankedMusicMax) : .65f;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// 파일이 base 이름의 변형인지: 확장자가 mp3/wav/ogg이고, 이름이 base로 시작하며 뒤에 숫자만 붙은 경우.
bool matchesVariant(std::filesystem::path const& path, std::string_view base) {
    auto extension = lowercase(path.extension().string());
    if (extension != ".mp3" && extension != ".wav" && extension != ".ogg") return false;
    auto stem = lowercase(path.stem().string());
    if (!stem.starts_with(base)) return false;
    auto suffix = std::string_view(stem).substr(base.size());
    return suffix.empty() ||
           std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// 리소스 폴더(sounds, resources/sounds, 루트)에서 base의 변형 파일을 모두 찾아 정렬해 돌려준다.
std::vector<std::filesystem::path> variants(std::string_view base) {
    std::vector<std::filesystem::path> result;
    auto* mod = Mod::get();
    if (!mod) return result;
    auto const root = mod->getResourcesDir();
    std::array const folders{root / "sounds", root / "resources" / "sounds", root};
    std::error_code error;
    for (auto const& folder : folders) {
        error.clear();
        if (!std::filesystem::is_directory(folder, error) || error) continue;
        for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) || error || !matchesVariant(it->path(), base)) continue;
            auto canonical = std::filesystem::weakly_canonical(it->path(), error);
            if (error) {
                error.clear();
                canonical = it->path();
            }
            if (std::find(result.begin(), result.end(), canonical) == result.end())
                result.push_back(std::move(canonical));
        }
        error.clear();
    }
    std::sort(result.begin(), result.end());
    return result;
}

// 변형 중 하나를 무작위로 고른다. 직전에 고른 것과 같은 파일은 피하고, 검색 결과는 base별로 캐시한다.
std::filesystem::path randomVariant(std::string_view base) {
    static std::unordered_map<std::string, std::vector<std::filesystem::path>> cache;
    auto [entry, inserted] = cache.try_emplace(std::string(base));
    if (inserted) entry->second = variants(base);
    auto const& sounds = entry->second;
    if (sounds.empty()) return {};
    static std::mt19937 generator(std::random_device{}());
    static std::unordered_map<std::string, size_t> last;
    auto& previous = last.try_emplace(std::string(base), sounds.size()).first->second;
    size_t index = 0;
    if (sounds.size() > 1) {
        std::uniform_int_distribution<size_t> pick(0, sounds.size() - 2);
        index = pick(generator);
        if (previous < sounds.size() && index >= previous) ++index;
    }
    previous = index;
    return sounds[index];
}

// base 변형 효과음을 재생한다. 변형 파일이 없으면 fallback 내장 효과음을 쓴다.
void playFile(std::string_view base, char const* fallback) {
    if (auto* engine = FMODAudioEngine::sharedEngine()) {
        auto path = randomVariant(base);
        if (!path.empty()) {
            engine->playEffect(gd::string(path.string()));
            return;
        }
    }
    playBuiltin(fallback);
}

}

// GD 내장 효과음 파일을 재생한다.
void playBuiltin(char const* filename) {
    if (!filename || !*filename) return;
    if (auto* engine = FMODAudioEngine::sharedEngine()) engine->playEffect(filename);
}

// base 이름의 효과음 재생 (사용자 파일 우선, 없으면 fallback).
void fx(char const* base, char const* fallback) {
    if (base && *base) playFile(base, fallback);
}

// Cue별로 사용자 파일 이름(base)과 내장 대체 효과음을 연결한다.
void play(Cue cue) {
    switch (cue) {
    case Cue::ReadyLock: playBuiltin("reward01.ogg"); break;
    case Cue::Finish: playFile("finish", "highscoreGet02.ogg"); break;
    case Cue::Throw: playFile("throw", "playSound_01.ogg"); break;
    case Cue::Blast: playFile("blast", "magicExplosion.ogg"); break;
    case Cue::MatchFound: playFile("match", "highscoreGet02.ogg"); break;
    case Cue::SwordFly: playFile("sword0", "playSound_01.ogg"); break;
    case Cue::SwordImpact: playFile("sword1", "magicExplosion.ogg"); break;
    case Cue::Rating: playFile("rating", "reward01.ogg"); break;
    case Cue::TierChange: playFile("change", "magicExplosion.ogg"); break;
    case Cue::RunnerMilestone: playFile("jump", "reward01.ogg"); break;
    case Cue::Clock: playFile("clock", "counter003.ogg"); break;
    case Cue::Reroll: playFile("fx_reload", "counter003.ogg"); break;
    }
}

// 결과 화면 음악(finbgm 변형)을 채널 2에서 반복 재생한다.
void startResultMusic() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    auto path = randomVariant("finbgm");
    if (path.empty()) return;
    engine->stopMusic(kResultMusicChannel);
    engine->playMusic(gd::string(path.string()), true, .18f, kResultMusicChannel);
    engine->resumeMusic(kResultMusicChannel);
    s_resultMusicPlaying = true;
}

void stopResultMusic() {
    if (!s_resultMusicPlaying) return;
    if (auto* engine = FMODAudioEngine::sharedEngine()) engine->stopMusic(kResultMusicChannel);
    s_resultMusicPlaying = false;
}

// Ranked 음악을 채널 0에서 시작한다. 이미 우리 음악이 나오는 중이면 아무것도 하지 않는다.
// 우선순위: 사용자가 고른 노래 -> 컵이 지정한 노래 -> bgm 변형 파일 -> 내장 secretLoop03.
// (사용자가 고른 노래가 있으면 Standard Cup의 컵 노래보다 먼저다.)
void startRankedMusic() {
    if (auto* engine = FMODAudioEngine::sharedEngine()) {
        if (ownsRankedMusic(engine)) return;
        auto path = randomVariant("bgm");
        s_rankedMusicPath = path.empty() ? "secretLoop03.mp3" : path.string();
        s_playingCupSong = false;
        for (int const song : {rankedMusicSong(), s_cupSong}) {
            if (auto chosen = songPath(song); !chosen.empty()) {
                s_rankedMusicPath = std::move(chosen);
                s_playingCupSong = song == s_cupSong;
                break;
            }
        }
        engine->playMusic(gd::string(s_rankedMusicPath), true, 0.f, 0);
        engine->setChannelVolume(0, AudioTargetType::MusicChannel, rankedMusicVolume());
        engine->resumeMusic(0);
        s_rankedMusicPlaying = true;
    }
}

// 주기적으로 호출되어 Ranked 음악 상태를 지킨다. 우리 음악이나 허용된 맵 노래(levelSongs)가
// 재생/일시정지 중이면 유지하고(모바일에서는 일시정지를 풀어 줌), 음악이 한 번 들린 뒤
// 2번 연속 멈춰 있으면 끝난 것으로 보고 Ranked 음악을 다시 시작한다. 다른 음악이 차지했을 때도 되돌린다.
void keepRankedMusic(std::vector<int> const& levelSongs) {
    if (!s_rankedMusicPlaying) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    bool const ranked = ownsRankedMusic(engine);
    std::string const active(engine->getActiveMusic(0));
    bool const level = !ranked && songOf(active, levelSongs);
    if (ranked || level) {
        auto const state = channelZero(engine);
        if (active != s_watched) {
            s_watched = active;
            s_heard = false;
            s_quietChecks = 0;
        }
        if (state != Music::Stopped) {
            s_heard = true;
            s_quietChecks = 0;
        }
        bool const quiet = state == Music::Stopped && s_heard && ++s_quietChecks >= 2;
        if (!quiet) {
#ifdef GEODE_IS_MOBILE
            if (state == Music::Paused)
                if (auto* channel = engine->getActiveMusicChannel(0)) channel->setPaused(false);
#endif
            if (level && s_levelSong != active) {
                s_levelSong = active;
                log::info("Versus: Season 2 lounge: the map's song {} plays", fileName(active));
            }
            if (ranked && s_cupSong > 0 && !s_playingCupSong) refreshCupSong();
            return;
        }
        log::info("Versus: Season 2 lounge: {} stopped - the Ranked music again",
                  level ? "the map's song " + fileName(active) : std::string("the Ranked music"));
    }
    forgetWatched();
    s_rankedMusicPlaying = false;
    s_rankedMusicPath.clear();
    startRankedMusic();
}

// 라운지에서 재생하던 맵 노래를 끝내고 Ranked 음악으로 되돌린다.
void endLevelSong() {
    if (s_levelSong.empty()) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    bool const playing = engine && std::string(engine->getActiveMusic(0)) == s_levelSong;
    forgetWatched();
    if (!playing) return;
    engine->stopMusic(0);
    if (s_rankedMusicPlaying) {
        s_rankedMusicPlaying = false;
        s_rankedMusicPath.clear();
        startRankedMusic();
    }
}

// 다른 음악이 채널 0을 차지하고 있으면 멈추고 Ranked(컵) 음악을 다시 건다.
void restoreRankedMusic() {
    if (!s_rankedMusicPlaying) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || ownsRankedMusic(engine)) return;
    std::string const active(engine->getActiveMusic(0));
    log::info("Versus: Season 2 match found: {} stopped - the cup's music",
              active.empty() ? std::string("the music") : "the song " + fileName(active));
    if (channelZero(engine) != Music::Stopped) engine->stopMusic(0);
    forgetWatched();
    s_rankedMusicPlaying = false;
    s_rankedMusicPath.clear();
    startRankedMusic();
}

// Ranked 음악을 멈춘다. 우리 음악이나 맵 노래가 나오던 중이면 채널을 정리하고 볼륨을 원래대로 돌린다.
// returnToMenu가 true이면 GD 메뉴 음악을 다시 튼다.
void stopRankedMusic(bool returnToMenu) {
    if (!s_rankedMusicPlaying) return;
    bool owned = false;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (ownsRankedMusic(engine) ||
        (engine && !s_levelSong.empty() && std::string(engine->getActiveMusic(0)) == s_levelSong)) {
        owned = true;
        engine->setChannelVolume(0, AudioTargetType::MusicChannel, 1.f);
        engine->stopMusic(0);
    }
    forgetWatched();
    s_rankedMusicPlaying = false;
    s_rankedMusicPath.clear();
    if (returnToMenu && owned) {
        if (auto* manager = GameManager::sharedState()) manager->playMenuMusic();
    }
}

// 설정에서 읽은 Ranked 음악 볼륨 (기본 0.65) / 사용자가 고른 노래 ID (0이면 없음).
float rankedMusicVolume() {
    auto* mod = Mod::get();
    return safeVolume(mod ? mod->getSavedValue<float>("ranked-music-volume", .65f) : .65f);
}

int rankedMusicSong() {
    auto* mod = Mod::get();
    return mod ? std::max(0, mod->getSavedValue<int>("ranked-music-song", 0)) : 0;
}

// 노래 ID를 저장하고, 지금 우리 음악이 나오는 중이면 새 노래로 다시 시작한다.
void setRankedMusicSong(int songID) {
    songID = std::max(0, songID);
    if (auto* mod = Mod::get()) mod->setSavedValue("ranked-music-song", songID);
    if (auto* engine = FMODAudioEngine::sharedEngine(); ownsRankedMusic(engine)) {
        s_rankedMusicPlaying = false;
        s_rankedMusicPath.clear();
        startRankedMusic();
    }
}

// 컵이 지정한 노래 ID. 바뀌면 지금 우리 음악이 나오는 중일 때 새 노래로 다시 시작한다.
int cupSong() {
    return s_cupSong;
}

void setCupSong(int songID) {
    songID = std::max(0, songID);
    if (songID == s_cupSong) return;
    s_cupSong = songID;
    if (auto* engine = FMODAudioEngine::sharedEngine(); ownsRankedMusic(engine)) {
        s_rankedMusicPlaying = false;
        s_rankedMusicPath.clear();
        startRankedMusic();
    }
}

// 컵 노래가 방금 내려받아졌으면(재생 중인 파일과 경로가 다르면) 음악을 다시 시작해 컵 노래로 바꾼다.
void refreshCupSong() {
    if (s_cupSong <= 0 || !s_rankedMusicPlaying) return;
    if (!songPath(rankedMusicSong()).empty()) return;  // 사용자가 고른 노래가 나오는 중이면 그대로 둔다
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!ownsRankedMusic(engine)) return;
    auto const path = songPath(s_cupSong);
    if (path.empty() || path == s_rankedMusicPath) return;
    s_rankedMusicPlaying = false;
    s_rankedMusicPath.clear();
    startRankedMusic();
}

// 볼륨을 저장하고 재생 중인 Ranked 음악에 바로 적용한다.
void setRankedMusicVolume(float volume) {
    volume = safeVolume(volume);
    if (auto* mod = Mod::get()) mod->setSavedValue("ranked-music-volume", volume);
    if (auto* engine = FMODAudioEngine::sharedEngine(); ownsRankedMusic(engine))
        engine->setChannelVolume(0, AudioTargetType::MusicChannel, volume);
}

}
