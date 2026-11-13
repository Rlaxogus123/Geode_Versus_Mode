// 1v1 방의 준비(Ready) 버튼이 지금 무엇을 해야 하는지 정하는 순수 함수 모음.
// 상태 값(시작 여부, 맵 유무, 캐시 여부 등)만 받아 버튼 문구와 동작을 돌려준다.
#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace versus {
// 버튼이 수행할 동작. None이면 비활성.
enum class ReadyAction { None, ChooseMap, Download, Ready, Unready };
// 버튼 문구와 동작 한 쌍.
struct ReadyControl {
    char const* caption;
    ReadyAction action;
    bool enabled() const { return action != ReadyAction::None; }
};

// 방 상태에 따라 버튼을 정한다. 우선순위: 시작됨 > 서버 응답 대기 > 맵 없음(방장은 맵 선택)
// > 이미 준비함(해제) > 레벨이 캐시됨(준비) > 다운로드 중(비활성) > 다운로드.
// 1.0.8: 다운로드 중에도 이 버튼은 음악 건너뛰기로 바뀌지 않는다(준비를 누르던 손에 건너뛰기가 눌리던 문제).
// 건너뛰기는 따로 둔 버튼이 맡는다(skipMusicShown / SkipMusicArm).
inline ReadyControl readyControl(bool started, bool pending, bool host, bool hasMap, bool cached, bool downloading,
                                 bool ready) {
    if (started) return {"Preparing", ReadyAction::None};
    if (pending) return {"Updating...", ReadyAction::None};
    if (!hasMap) return {host ? "Choose Map" : "Wait for Map", host ? ReadyAction::ChooseMap : ReadyAction::None};
    if (ready) return {"Unready", ReadyAction::Unready};
    if (cached) return {"Ready", ReadyAction::Ready};
    if (downloading) return {"Downloading...", ReadyAction::None};
    return {"Download", ReadyAction::Download};
}

// 1.0.8: 따로 둔 음악 건너뛰기 버튼을 보일지. 맵을 받는 중이고 아직 건너뛰지 않았을 때만(시작/응답 대기/
// 맵 없음/이미 받음/이미 준비함이면 숨긴다).
inline bool skipMusicShown(bool started, bool pending, bool hasMap, bool cached, bool downloading, bool ready,
                           bool audioSkipped) {
    return !started && !pending && hasMap && !cached && downloading && !ready && !audioSkipped;
}

// 1.0.8: 건너뛰기 버튼은 보이고 나서 이만큼 지나야 눌린다(다운로드/준비를 두 번 눌러 건너뛰는 일 방지).
inline constexpr double kSkipMusicArmSeconds = 1.;

// 건너뛰기 버튼이 처음 보인 시각을 맵(또는 추첨) 키별로 기억한다. 키가 바뀌면 그때부터 다시 센다.
class SkipMusicArm {
public:
    using Clock = std::chrono::steady_clock;
    // 버튼이 보이는 동안 부른다. 누를 수 있을 때까지 남은 초(0이면 눌러도 된다).
    double wait(std::string_view key, Clock::time_point now = Clock::now()) {
        if (key != m_key) {
            m_key = std::string(key);
            m_shownAt = now;
        }
        double const shown = std::chrono::duration<double>(now - m_shownAt).count();
        return std::max(0., kSkipMusicArmSeconds - shown);
    }
    void reset() { m_key.clear(); }

private:
    std::string m_key;
    Clock::time_point m_shownAt{};
};
}
