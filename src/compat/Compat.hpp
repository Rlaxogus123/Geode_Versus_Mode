// GD 2.2081(Geode 5)과 GD 2.2091(Geode 6)을 같은 소스로 빌드하기 위한 호환 계층.
// 두 버전에서 이름, 인자, 타입이 다른 GD / Geode 기능은 모두 versus::compat을 거친다. 기능 코드는 GD 버전을 확인하지 않는다.
//   compat/gd2081/, compat/gd2091/: 버전별 타입(GameTypes.hpp)과 구현(GameCompat.cpp, GameHooks.cpp).
//   CMakeLists.txt가 Geode SDK의 GD 버전을 보고 한쪽 폴더만 빌드에 넣는다.
// 2.2091 바인딩에는 일부 플랫폼(macOS / iOS) 주소가 없는 함수가 있다. 그 처리도 compat/gd2091 안에서만 한다.
#pragma once

#include "GameTypes.hpp" // compat/gd2081/ 또는 compat/gd2091/ (CMake가 고른 쪽)

#include <Geode/Geode.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace versus::compat {
// 이 빌드가 대상으로 하는 GD 버전("2.2081" / "2.2091"). 업데이터가 이 버전용 패키지를 고른다.
inline constexpr std::string_view kGameVersion = GEODE_GD_VERSION_STRING;

// 텍스트 입력창. Geode 6에서 TextInput이 새로 만들어져 포커스 해제와 너비 지정 방법이 바뀌었다.
// unfocus는 nullptr이면 아무것도 하지 않는다.
void unfocus(geode::TextInput* input);
void setWidth(geode::TextInput* input, float width);

// 레벨을 여는 PlayLayer (GD 2.2091에서 리플레이 인자가 빠졌다).
cocos2d::CCScene* playScene(GJGameLevel* level);
PlayLayer* createPlayLayer(GJGameLevel* level);
// 시작 위치를 지운다(처음부터 시작).
void clearStartPos(PlayLayer* layer);
// 레벨 문자열을 GD 레벨 형식(압축 + 키 11)으로 만든다.
std::string compressLevelString(std::string const& data);
// 노래 정보 / 다운로드를 위한 GD 노래 위젯. 화면에는 보이지 않게 쓴다.
CustomSongWidget* songWidget(SongInfoObject* info, bool musicLibrary);

// GD 레벨 / 오디오 요청 관리자. 키는 GD가 요청마다 쓰는 키다.
bool levelRequestActive(GameLevelManager* manager, char const* key);
void forgetLevelRequest(GameLevelManager* manager, char const* key);
// 같은 요청을 다시 보내지 못하게 하는 GD의 대기 시간을 지운다.
void resetRequestTimer(GameLevelManager* manager, char const* key);
cocos2d::CCObject* musicRequest(MusicDownloadManager* music, char const* key);
void forgetMusicRequest(MusicDownloadManager* music, char const* key);
// GD의 음악 / 효과음 라이브러리 정보를 아직 안 읽었으면 읽는다.
void loadAudioLibraries(MusicDownloadManager* music);
void storePendingUserCoin(GameStatsManager* stats, char const* key);

// 색 채널 (없으면 nullptr일 수 있다).
ColorAction* colorAction(GJEffectManager* effects, int channel);
// 공식 노래 작곡가 이름.
std::string artistName(int artist);
// 스케줄러가 이 대상을 멈춰 두었는지.
bool targetPaused(cocos2d::CCScheduler* scheduler, cocos2d::CCObject* target);
// FMOD 음악 채널 볼륨(GD의 채널 볼륨 기억 + 재생 중인 채널에 바로 적용).
void setMusicChannelVolume(FMODAudioEngine* engine, int channel, float volume);

// Argon(GD 계정 인증). argonForget은 저장된 토큰을 지우고, argonAuth는 토큰을 받아 메인 스레드에서
// done(token, error)을 한 번 부른다. Argon 없이 빌드됐으면 VERSUS_ARGON이 0이고 이 둘을 부르지 않는다.
void argonForget(int account);
void argonAuth(std::function<void(std::string token, std::string error)> done);

// 사망 처리 동안 GD의 자동 재시도(게임 변수 "0026")를 잠시 켠다. GD는 자동 재시도가 꺼져 있으면 사망 뒤
// 재시도 화면(showRetryLayer)을, 켜져 있으면 delayedResetLevel을 예약한다. 2.2091 macOS / iOS 바인딩에는
// showRetryLayer 주소가 없어서, 두 버전 모두 재시도 화면을 훅으로 막는 대신 Versus 화면에서는 처음부터 띄우지 않는다.
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
