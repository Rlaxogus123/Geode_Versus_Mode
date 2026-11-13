// Versus의 효과음과 Ranked 배경음악 API. 구현은 VersusAudio.cpp.
// 효과음은 Cue 단위로 재생하고, 음악은 FMOD 0번 채널(Ranked/라운지)과
// 2번 채널(결과 화면)을 따로 관리한다.
#pragma once

#include <vector>

namespace versus::audio {

// 게임 흐름의 효과음 종류. 각 Cue가 어떤 파일을 쓰는지는 play()에서 정한다.
enum class Cue {
    ReadyLock,
    Finish,
    Throw,
    Blast,
    MatchFound,
    SwordFly,
    SwordImpact,
    Rating,
    TierChange,
    RunnerMilestone,
    Clock,
    Reroll,
};

// Cue에 해당하는 효과음을 재생한다 (사용자 파일이 있으면 그것, 없으면 내장 효과음).
void play(Cue cue);
// GD 내장 효과음 파일 이름을 그대로 재생한다.
void playBuiltin(char const* filename);
// 리소스 폴더에서 base 이름의 변형 파일을 골라 재생한다. 하나도 없으면 fallback(내장 효과음)을 쓴다.
void fx(char const* base, char const* fallback = nullptr);
// 결과 화면 BGM 시작/정지 (채널 2).
void startResultMusic();
void stopResultMusic();
// Ranked 배경음악 시작/정지. returnToMenu가 true면 정지 후 GD 메뉴 음악으로 돌아간다.
void startRankedMusic();
void stopRankedMusic(bool returnToMenu);
// Ranked 음악이 계속 흐르도록 감시한다. 주기적으로 호출하며, levelSongs는 라운지에서
// 재생을 허용할 맵 노래 ID 목록이다.
void keepRankedMusic(std::vector<int> const& levelSongs = {});
// 맵 노래 재생을 끝내고 Ranked 음악으로 되돌린다.
void endLevelSong();
// 다른 음악이 끼어들었으면 컵 음악(Ranked 음악)을 다시 건다.
void restoreRankedMusic();
// Ranked 음악 볼륨 상한 (1.0이 원래 크기).
inline constexpr float kRankedMusicMax = 1.5f;
float rankedMusicVolume();
void setRankedMusicVolume(float volume);
// 사용자가 고른 Ranked 음악의 Newgrounds 노래 ID (0이면 기본 음악).
int rankedMusicSong();
void setRankedMusicSong(int songID);
// 1.0.8: 고른 로비 음악이 기기에서 지워졌으면(GD의 노래 수 한도) 다시 받는다. 다 받으면 대신 나오던 음악을
// 그 곡으로 바꾼다. 맵 다운로드와 겹치지 않게 매치가 없을 때 주기적으로 부른다.
void refetchLobbySong();
// 컵(시즌)이 지정한 노래 ID. 사용자 설정보다 우선한다.
void setCupSong(int songID);
int cupSong();
void refreshCupSong();

}
