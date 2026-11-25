// GD 2.2091 바인딩에서 일부 플랫폼에 주소가 없는 GD 함수를 같은 동작으로 대신하는 헬퍼.
// 각 함수는 주소가 있는 플랫폼에서는 GD 함수를 그대로 부르고, 없는 플랫폼에서는 GD가 하는 일을 멤버로 직접 한다.
// 바인딩에 주소가 생기면 그 플랫폼 분기만 지우면 된다.
#pragma once
#include <Geode/Geode.hpp>
#include <Geode/binding/ColorAction.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <iterator>
#include <string>

namespace versus::gd {
// FMODAudioEngine::stopMusic. GD의 stopMusic은 stopAndRemoveMusic 한 줄이라 모든 플랫폼에서 그것을 부른다
// (iOS에는 stopMusic 주소가 없다).
inline void stopMusic(FMODAudioEngine* engine, int channel) {
    engine->stopAndRemoveMusic(channel);
}

// GameLevelManager::isDLActive: 진행 중인 요청 사전(m_downloadObjects)에 키가 있는지. iOS에는 주소가 없다.
inline bool levelRequestActive(GameLevelManager* manager, char const* key) {
#ifdef GEODE_IS_IOS
    return manager->m_downloadObjects && manager->m_downloadObjects->objectForKey(key);
#else
    return manager->isDLActive(key);
#endif
}

// GameLevelManager::removeDLFromActive. iOS에는 주소가 없다.
inline void forgetLevelRequest(GameLevelManager* manager, char const* key) {
#ifdef GEODE_IS_IOS
    if (manager->m_downloadObjects) manager->m_downloadObjects->removeObjectForKey(key);
#else
    manager->removeDLFromActive(key);
#endif
}

// GameLevelManager::resetTimerForKey: 같은 요청을 다시 보내지 못하게 하는 대기 시간(m_timerDict)을 지운다.
// Apple Silicon macOS와 iOS에는 주소가 없다.
inline void resetRequestTimer(GameLevelManager* manager, char const* key) {
#if defined(GEODE_IS_ARM_MAC) || defined(GEODE_IS_IOS)
    if (manager->m_timerDict) manager->m_timerDict->removeObjectForKey(key);
#else
    manager->resetTimerForKey(key);
#endif
}

// GameStatsManager::storePendingUserCoin. Apple Silicon macOS와 iOS에는 주소가 없다
// (Windows에서도 GD는 이 한 줄을 인라인으로 넣는다).
inline void storePendingUserCoin(GameStatsManager* stats, char const* key) {
#if defined(GEODE_IS_ARM_MAC) || defined(GEODE_IS_IOS)
    if (stats->m_pendingUserCoins) stats->m_pendingUserCoins->setObject(stats->m_trueString, key);
#else
    stats->storePendingUserCoin(key);
#endif
}

// MusicDownloadManager::getDLObject: 진행 중인 오디오 다운로드 사전(m_activeDownloads)의 요청. iOS에는 주소가 없다.
inline cocos2d::CCObject* musicRequest(MusicDownloadManager* music, char const* key) {
#ifdef GEODE_IS_IOS
    return music->m_activeDownloads ? music->m_activeDownloads->objectForKey(key) : nullptr;
#else
    return music->getDLObject(key);
#endif
}

// MusicDownloadManager::removeDLFromActive. iOS에는 주소가 없다.
inline void forgetMusicRequest(MusicDownloadManager* music, char const* key) {
#ifdef GEODE_IS_IOS
    if (music->m_activeDownloads) music->m_activeDownloads->removeObjectForKey(key);
#else
    music->removeDLFromActive(key);
#endif
}

// MusicDownloadManager::tryLoadLibraries. iOS에는 이 함수와 SFX 라이브러리를 읽는 함수의 주소가 없어서
// 음악 라이브러리만 읽는다. 한 번만 읽게 하는 GD의 표시는 건드리지 않아 GD가 나중에 SFX 라이브러리를 읽을 수 있다.
inline void loadAudioLibraries(MusicDownloadManager* music) {
#ifdef GEODE_IS_IOS
    if (!music->isMusicLibraryLoaded()) music->parseMusicLibrary();
#else
    music->tryLoadLibraries();
#endif
}

// GJEffectManager::getColorAction. iOS에는 주소가 없어서 이미 만들어진 색 채널만 읽는다 (없으면 nullptr).
inline ColorAction* colorAction(GJEffectManager* effects, int channel) {
#ifdef GEODE_IS_IOS
    auto const& actions = effects->m_colorActionVector;
    if (channel < 0 || static_cast<size_t>(channel) >= actions.size()) return nullptr;
    return actions[channel];
#else
    return effects->getColorAction(channel);
#endif
}

// LevelTools::nameForArtist. iOS에는 주소가 없어서 GD와 같은 표(공식 노래 작곡가 0~11)를 쓴다.
inline std::string artistName(int artist) {
#ifdef GEODE_IS_IOS
    static constexpr char const* kNames[] = {"DJVI",    "Waterflame", "OcularNebula", "ForeverBound",
                                             "Step",    "DJ-Nate",    "F-777",        "MDK",
                                             "Dex Arson", "Boom Kitty", "Bossfight",  "Hinkik"};
    return artist >= 0 && artist < static_cast<int>(std::size(kNames)) ? kNames[artist] : "";
#else
    return std::string(LevelTools::nameForArtist(artist));
#endif
}

// CCScheduler::isTargetPaused. Apple Silicon macOS에는 주소가 없어서 멈추지 않은 것으로 본다
// (워치독이 스케줄러 정지를 따로 알아채지 못할 뿐, 일시 정지 / 실행 상태 검사는 그대로다).
inline bool targetPaused(cocos2d::CCScheduler* scheduler, cocos2d::CCObject* target) {
#ifdef GEODE_IS_ARM_MAC
    (void)scheduler;
    (void)target;
    return false;
#else
    return scheduler->isTargetPaused(target);
#endif
}

// 사망 처리 동안 GD의 자동 재시도(게임 변수 "0026")를 잠시 켠다. GD는 자동 재시도가 꺼져 있으면 사망 뒤
// 재시도 화면(showRetryLayer)을, 켜져 있으면 delayedResetLevel을 예약한다. macOS/iOS 2.2091 바인딩에는
// showRetryLayer 주소가 없어, 재시도 화면을 훅으로 막는 대신 Versus 화면에서는 처음부터 띄우지 않게 한다.
class AutoRetryScope {
    bool m_restore = false;

public:
    AutoRetryScope() {
        auto* manager = GameManager::sharedState();
        if (manager && !manager->getGameVariable("0026")) {
            manager->setGameVariable("0026", true);
            m_restore = true;
        }
    }
    ~AutoRetryScope() {
        if (!m_restore) return;
        if (auto* manager = GameManager::sharedState()) manager->setGameVariable("0026", false);
    }
    AutoRetryScope(AutoRetryScope const&) = delete;
    AutoRetryScope& operator=(AutoRetryScope const&) = delete;
};
}
