// compat/Compat.hpp의 GD 2.2081 + Geode 5 구현. 모든 플랫폼에 주소가 있어 GD 함수를 그대로 부른다.
#include "../Compat.hpp"

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

namespace versus::compat {
void unfocus(geode::TextInput* input) {
    if (input) input->defocus();
}
void setWidth(geode::TextInput* input, float width) {
    input->setWidth(width);
}

cocos2d::CCScene* playScene(GJGameLevel* level) {
    return PlayLayer::scene(level, false, false);
}
PlayLayer* createPlayLayer(GJGameLevel* level) {
    return PlayLayer::create(level, false, false);
}
void clearStartPos(PlayLayer* layer) {
    layer->setStartPosObject(nullptr);
}
std::string compressLevelString(std::string const& data) {
    return std::string(cocos2d::ZipUtils::compressString(::gd::string(data), false, 11));
}
CustomSongWidget* songWidget(SongInfoObject* info, bool musicLibrary) {
    return CustomSongWidget::create(info, nullptr, false, false, false, false, false, musicLibrary, 0);
}

bool levelRequestActive(GameLevelManager* manager, char const* key) {
    return manager->isDLActive(key);
}
void forgetLevelRequest(GameLevelManager* manager, char const* key) {
    manager->removeDLFromActive(key);
}
void resetRequestTimer(GameLevelManager* manager, char const* key) {
    manager->resetTimerForKey(key);
}
cocos2d::CCObject* musicRequest(MusicDownloadManager* music, char const* key) {
    return music->getDLObject(key);
}
void forgetMusicRequest(MusicDownloadManager* music, char const* key) {
    music->removeDLFromActive(key);
}
void loadAudioLibraries(MusicDownloadManager* music) {
    music->tryLoadLibraries();
}
void storePendingUserCoin(GameStatsManager* stats, char const* key) {
    stats->storePendingUserCoin(key);
}

ColorAction* colorAction(GJEffectManager* effects, int channel) {
    return effects->getColorAction(channel);
}
std::string artistName(int artist) {
    return std::string(LevelTools::nameForArtist(artist));
}
bool targetPaused(cocos2d::CCScheduler* scheduler, cocos2d::CCObject* target) {
    return scheduler->isTargetPaused(target);
}
void setMusicChannelVolume(FMODAudioEngine* engine, int channel, float volume) {
    if (engine) engine->setChannelVolume(channel, AudioTargetType::MusicChannel, volume);
}
}
