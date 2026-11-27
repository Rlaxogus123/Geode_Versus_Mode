// GD 2.2081(Geode 5)과 GD 2.2091(Geode 6)을 같은 소스로 빌드하기 위한 호환 계층.
// 두 버전에서 이름, 인자, 타입이 다른 GD / Geode 기능은 모두 versus::compat을 거친다. 기능 코드는 GD 버전을 확인하지 않는다.
//   compat/gd2081/, compat/gd2091/: 버전별 타입(GameTypes.hpp)과 구현(GameCompat.cpp, GameHooks.cpp).
//   CMakeLists.txt가 Geode SDK의 GD 버전을 보고 한쪽 폴더만 빌드에 넣는다.
// 2.2091 바인딩에는 일부 플랫폼(macOS / iOS) 주소가 없는 함수가 있다. 그 처리도 compat/gd2091 안에서만 한다.
#pragma once

#include "GameTypes.hpp" // compat/gd2081/ 또는 compat/gd2091/ (CMake가 고른 쪽)

#include <Geode/Geode.hpp>
#include <Geode/ui/ScrollLayer.hpp>

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
// GD가 레벨을 나갈 때 하는 것처럼, 노드(PlayLayer의 UILayer 등)를 키보드 입력 대상에서 뺀다. 레벨을 GD의 나가기
// 과정 없이 떼어 낼 때 부른다(2.2091의 UILayer는 스스로 키보드 대상으로 등록되어, 그대로 떼면 레벨이 없는 채로
// 키 입력을 받다가 튕긴다).
void releaseKeyboard(cocos2d::CCNode* node);
// 레벨 문자열을 GD 레벨 형식(압축 + 키 11)으로 만든다.
std::string compressLevelString(std::string const& data);
// 노래 정보 / 다운로드를 위한 GD 노래 위젯. 화면에는 보이지 않게 쓴다.
CustomSongWidget* songWidget(SongInfoObject* info, bool musicLibrary);

// GD 레벨 / 오디오 요청 관리자. 키는 GD가 요청마다 쓰는 키다. musicRequest는 진행 중인 오디오 요청(없으면
// nullptr)으로, MusicDownloadManager::isDLActive 대신 쓴다(2.2091 iOS에서 isDLActive는 주소 없는 getDLObject를 부른다).
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

// 스크롤 목록. Geode 6의 ScrollLayer는 가운데를 기준으로 놓인다(anchor 0.5, ignoreAnchorPointForPosition false).
// Versus는 목록을 Geode 5처럼 왼쪽 아래 모서리 기준으로 놓으므로 만들 때 그렇게 맞춘다(Geode 5에서는 바뀌는 것이
// 없다). ScrollLayer를 상속한 클래스는 생성자에서 placeByCorner를 부른다.
inline void placeByCorner(geode::ScrollLayer* scroll) {
    if (scroll) scroll->ignoreAnchorPointForPosition(true);
}
inline geode::ScrollLayer* scrollLayer(cocos2d::CCSize const& size) {
    auto* scroll = geode::ScrollLayer::create(size);
    placeByCorner(scroll);
    return scroll;
}

// GD 클래스인지 보고 바꾼다(typeinfo_cast와 같은 결과). 전역 이름공간의 GD 클래스(T::CLASS_NAME)에만 쓴다.
// macOS / iOS의 GD는 클래스 type info를 내보내지 않아서, 바인딩에 그 클래스의 첫 가상 함수(대개 소멸자) 주소가
// 없는 플랫폼에서는 typeinfo_cast<T*>가 링크되지 않는다(2.2091의 GJUINode, SongInfoObject). 그래서 Windows가 아니면
// 객체의 type info를 이름으로 따라간다. Geode의 typeinfo_cast도 이름을 비교하므로 결과는 같다.
template <class T>
T* gdCast(cocos2d::CCObject* object) {
#ifdef GEODE_IS_WINDOWS
    return geode::cast::typeinfo_cast<T*>(object);
#else
    if (!object) return nullptr;
    static std::string const name = std::to_string(std::char_traits<char>::length(T::CLASS_NAME)) + T::CLASS_NAME;
    auto const* vtable = *reinterpret_cast<geode::cast::VtableType const* const*>(object);
    auto const* info =
        static_cast<geode::cast::VtableTypeinfoType const*>(static_cast<geode::cast::CompleteVtableType const*>(vtable));
    auto* top = reinterpret_cast<std::byte*>(object) + info->m_offset;
    return static_cast<T*>(geode::cast::traverseTypeinfoFor(top, info->m_typeinfo, name.c_str()));
#endif
}
}
