// 배틀 맵(레벨 데이터 + 노래/효과음)을 로컬에 내려받아 준비하고 진행도를 알려 주는 맵 캐시의 선언.
// 구현은 MapCache.cpp의 Cache 싱글턴이 맡는다.
#pragma once
#include "VersusService.hpp"
class GJGameLevel;
namespace versus {
// 맵 준비 진행 상황. 레벨 데이터, 노래, 효과음 완료 수와 건너뛴 수, 전체 퍼센트.
struct MapDownloadProgress {
    bool mapReady = false;
    int songsDone = 0, songsTotal = 0, soundsDone = 0, soundsTotal = 0;
    int songsSkipped = 0, soundsSkipped = 0;
    int percent = 0;
    // 레벨과 모든 오디오가 끝났는지(건너뛴 항목도 완료로 센다).
    bool complete() const { return mapReady && songsDone == songsTotal && soundsDone == soundsTotal; }
    // 화면에 보여 줄 한 줄 문구.
    std::string caption() const;
    // 음악/SFX 개수까지 포함한 자세한 문구.
    std::string detail() const;
};
// 오디오 요구 수준. All은 모두 받아야 함, Ranked는 시간 초과 시 못 받은 오디오를 건너뛰고 진행,
// RankedNoAudio는 오디오를 받지 않고 진행("항상 노래 다운로드" 옵션이 꺼진 경우).
enum class AudioNeed { All, Ranked, RankedNoAudio };
// Ranked에서 오디오 대기 시간 기준(초). 오디오 단계 시작 후 이 시간이 지나고 진행이 멈춰 있으면 건너뛴다.
inline constexpr int kRankedAudioSeconds = 25;
// 오디오 진행이 이 시간(초) 이상 멈추면 건너뛸 수 있다.
inline constexpr int kRankedAudioStallSeconds = 8;
// 맵 준비 시작 후 오디오 대기의 절대 상한(초).
inline constexpr int kRankedAudioCapSeconds = 40;
// 오디오를 건너뛰기로 한 결정이 유효한 시간(초).
inline constexpr int kRankedAudioWaiverSeconds = 600;
// 설정에 따른 Ranked용 오디오 요구 수준.
AudioNeed rankedAudioNeed();
// "항상 노래 다운로드" 설정 읽기/쓰기.
bool alwaysDownloadSong();
void setAlwaysDownloadSong(bool value);
// 맵 준비 진행도. ranked이면 건너뛰기로 한 오디오를 완료로 센다.
MapDownloadProgress mapDownloadProgress(int64_t levelID, bool ranked = false);
// 필요한 수준으로 준비가 끝나 있는지.
bool mapCached(int64_t levelID, AudioNeed need = AudioNeed::All);
// 맵을 내려받아 준비하고 끝나면 callback(성공 여부, 안내 문구)을 호출한다.
// prefetch는 미리 받기(건너뛴 오디오를 기억하지 않음), keepSaved는 GD 저장 목록에 남겨 둠.
void ensureMapCached(int64_t levelID, Done callback, AudioNeed need = AudioNeed::All, bool prefetch = false,
                     bool keepSaved = false);
// 배틀용으로만 받은 레벨을 GD 저장(Saved) 목록에서 곧 빼도록 예약한다.
void dropFromSaved(int64_t levelID, GJGameLevel* level);
// GD 저장 목록에 있는 레벨인지.
bool inGdSaved(int64_t levelID);
// 해당 맵의 준비 작업(대기열 포함)을 취소한다.
void cancelMapPreparation(int64_t levelID);
// 현재 방의 선택된 맵에서 남은 음악/효과음을 건너뛴다. 레벨 데이터는 계속 받는다.
bool skipMapAudio(int64_t levelID);
// 방이나 선택된 맵(추첨)이 바뀌면 건너뛰기 선택을 해제한다.
bool mapAudioSkipped(int64_t levelID);
// 배틀 화면에서 쓸 폰트/스프라이트를 미리 로드해 둔다.
void preloadBattleAssets();
// 메모리에 있거나 GD에 저장된 레벨 객체를 돌려준다(없으면 nullptr).
GJGameLevel* cachedLevel(int64_t levelID);
// 맵의 플레이 시간 추정(초). 알 수 없으면 음수.
double mapPlaySeconds(int64_t levelID);
// 2인 모드 여부(1/0), 모르면 -1.
int mapTwoPlayer(int64_t levelID);
// GD에 저장된 이 맵의 로컬 최고 퍼센트. 플레이한 적 없으면 -1.
int mapLocalBest(int64_t levelID);
}
