// compat/Compat.hpp의 GD 2.2091 + Geode 6 구현.
// 2.2091 바인딩에는 macOS / iOS 주소가 없는 GD 함수가 있다. 주소가 있는 플랫폼에서는 GD 함수를 그대로 부르고,
// 없는 플랫폼에서만 GD가 하는 일을 멤버로 직접 한다. 플랫폼마다 달라지는 곳은 이 파일에만 둔다.
// 바인딩에 주소가 생기면 그 플랫폼 분기만 지우면 된다.
#include "../Compat.hpp"

#include <vector>

#include <Geode/binding/ColorAction.hpp>
#include <Geode/binding/CustomSongWidget.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <iterator>

namespace versus::compat {
void unfocus(geode::TextInput* input) {
    if (input) input->unfocus();
}
// Geode 6의 TextInput은 크기를 바꾸면 배경과 글자 영역을 다시 배치한다.
void setWidth(geode::TextInput* input, float width) {
    input->setContentSize({width, input->getContentHeight()});
}

cocos2d::CCScene* playScene(GJGameLevel* level) {
    return PlayLayer::scene(level, false);
}
PlayLayer* createPlayLayer(GJGameLevel* level) {
    return PlayLayer::create(level, false);
}
void clearStartPos(PlayLayer* layer) {
    layer->setActiveStartPosObject(nullptr);
}
// 2.2091에서 압축 수준 인자가 생겼다(zlib 기본 수준 6).
std::string compressLevelString(std::string const& data) {
    return cocos2d::ZipUtils::compressString(data, false, 6, 11);
}
CustomSongWidget* songWidget(SongInfoObject* info, bool musicLibrary) {
    return CustomSongWidget::create(info, nullptr, false, false, false, false, false, musicLibrary, 0, false, false);
}

// GameLevelManager::isDLActive / removeDLFromActive: 진행 중인 요청 사전(m_downloadObjects). iOS에는 주소가 없다.
bool levelRequestActive(GameLevelManager* manager, char const* key) {
#ifdef GEODE_IS_IOS
    return manager->m_downloadObjects && manager->m_downloadObjects->objectForKey(key);
#else
    return manager->isDLActive(key);
#endif
}
void forgetLevelRequest(GameLevelManager* manager, char const* key) {
#ifdef GEODE_IS_IOS
    if (manager->m_downloadObjects) manager->m_downloadObjects->removeObjectForKey(key);
#else
    manager->removeDLFromActive(key);
#endif
}
// GameLevelManager::resetTimerForKey: 요청 대기 시간 사전(m_timerDict). Apple Silicon macOS와 iOS에는 주소가 없다.
void resetRequestTimer(GameLevelManager* manager, char const* key) {
#if defined(GEODE_IS_ARM_MAC) || defined(GEODE_IS_IOS)
    if (manager->m_timerDict) manager->m_timerDict->removeObjectForKey(key);
#else
    manager->resetTimerForKey(key);
#endif
}
// MusicDownloadManager::getDLObject / removeDLFromActive: 진행 중인 오디오 다운로드 사전(m_activeDownloads).
// iOS에는 주소가 없다.
cocos2d::CCObject* musicRequest(MusicDownloadManager* music, char const* key) {
#ifdef GEODE_IS_IOS
    return music->m_activeDownloads ? music->m_activeDownloads->objectForKey(key) : nullptr;
#else
    return music->getDLObject(key);
#endif
}
void forgetMusicRequest(MusicDownloadManager* music, char const* key) {
#ifdef GEODE_IS_IOS
    if (music->m_activeDownloads) music->m_activeDownloads->removeObjectForKey(key);
#else
    music->removeDLFromActive(key);
#endif
}
// MusicDownloadManager::tryLoadLibraries. iOS에는 이 함수와 효과음 라이브러리를 읽는 함수의 주소가 없어서
// 음악 라이브러리만 읽는다. 한 번만 읽게 하는 GD의 표시는 건드리지 않아 GD가 나중에 효과음 라이브러리를 읽을 수 있다.
void loadAudioLibraries(MusicDownloadManager* music) {
#ifdef GEODE_IS_IOS
    if (!music->isMusicLibraryLoaded()) music->parseMusicLibrary();
#else
    music->tryLoadLibraries();
#endif
}
// GameStatsManager::storePendingUserCoin: 보류 코인 사전(m_pendingUserCoins). Apple Silicon macOS와 iOS에는 주소가 없다
// (Windows에서도 GD는 이 한 줄을 인라인으로 넣는다).
void storePendingUserCoin(GameStatsManager* stats, char const* key) {
#if defined(GEODE_IS_ARM_MAC) || defined(GEODE_IS_IOS)
    if (stats->m_pendingUserCoins) stats->m_pendingUserCoins->setObject(stats->m_trueString, key);
#else
    stats->storePendingUserCoin(key);
#endif
}

// GJEffectManager::getColorAction. iOS에는 주소가 없어서 이미 만들어진 색 채널만 읽는다(없으면 nullptr).
ColorAction* colorAction(GJEffectManager* effects, int channel) {
#ifdef GEODE_IS_IOS
    auto const& actions = effects->m_colorActionVector;
    if (channel < 0 || static_cast<size_t>(channel) >= actions.size()) return nullptr;
    return actions[channel];
#else
    return effects->getColorAction(channel);
#endif
}

// LevelTools::nameForArtist. iOS에는 주소가 없어서 GD와 같은 표(공식 노래 작곡가 0~11)를 쓴다.
std::string artistName(int artist) {
#ifdef GEODE_IS_IOS
    static constexpr char const* kNames[] = {"DJVI",      "Waterflame", "OcularNebula", "ForeverBound",
                                             "Step",      "DJ-Nate",    "F-777",        "MDK",
                                             "Dex Arson", "Boom Kitty", "Bossfight",    "Hinkik"};
    return artist >= 0 && artist < static_cast<int>(std::size(kNames)) ? kNames[artist] : "";
#else
    return LevelTools::nameForArtist(artist);
#endif
}

// CCScheduler::isTargetPaused. Apple Silicon macOS에는 주소가 없어서 멈추지 않은 것으로 본다
// (워치독이 스케줄러 정지를 따로 알아채지 못할 뿐, 일시 정지 / 실행 상태 검사는 그대로다).
bool targetPaused(cocos2d::CCScheduler* scheduler, cocos2d::CCObject* target) {
#ifdef GEODE_IS_ARM_MAC
    (void)scheduler;
    (void)target;
    return false;
#else
    return scheduler->isTargetPaused(target);
#endif
}

// FMODAudioEngine::setChannelVolume(id, MusicChannel, volume)와 같은 일(macOS / iOS에는 주소가 없다).
// GD는 채널 볼륨을 기억해 두고, 재생 중인 채널에는 볼륨 × 볼륨 배율을 바로 적용한다.
void setMusicChannelVolume(FMODAudioEngine* engine, int channel, float volume) {
    if (!engine) return;
    engine->m_audioState.m_volumeForMusicChannels[channel] = volume;
    if (auto* active = engine->getActiveMusicChannel(channel))
        active->setVolume(volume * engine->m_audioState.m_volumeModForMusicChannels[channel]);
}
// 키보드 디스패처에서 node 객체에 속한 대상을 모두 뺀다(UILayer는 키보드 대상 기반 클래스를 둘 가질 수 있어
// 등록된 대상의 실제 객체 주소로 찾는다).
void releaseKeyboard(cocos2d::CCNode* node) {
    auto* dispatcher = cocos2d::CCKeyboardDispatcher::get();
    if (!node || !dispatcher || !dispatcher->m_pHandlers) return;
    std::vector<cocos2d::CCKeyboardDelegate*> found;
    for (auto* handler : geode::cocos::CCArrayExt<cocos2d::CCKeyboardHandler*>(dispatcher->m_pHandlers)) {
        auto* delegate = handler ? handler->getDelegate() : nullptr;
        if (delegate && dynamic_cast<void*>(delegate) == dynamic_cast<void*>(node)) found.push_back(delegate);
    }
    for (auto* delegate : found) dispatcher->removeDelegate(delegate);
}
}
