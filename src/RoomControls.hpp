// 1v1 방의 준비(Ready) 버튼이 지금 무엇을 해야 하는지 정하는 순수 함수 모음.
// 상태 값(시작 여부, 맵 유무, 캐시 여부 등)만 받아 버튼 문구와 동작을 돌려준다.
#pragma once

namespace versus {
// 버튼이 수행할 동작. None이면 비활성.
enum class ReadyAction { None, ChooseMap, Download, SkipMusic, Ready, Unready };
// 버튼 문구와 동작 한 쌍.
struct ReadyControl {
    char const* caption;
    ReadyAction action;
    bool enabled() const { return action != ReadyAction::None; }
};

// 방 상태에 따라 버튼을 정한다. 우선순위: 시작됨 > 서버 응답 대기 > 맵 없음(방장은 맵 선택)
// > 이미 준비함(해제) > 레벨이 캐시됨(준비) > 다운로드 중 음악 건너뛰기 > 다운로드.
inline ReadyControl readyControl(bool started, bool pending, bool host, bool hasMap, bool cached, bool downloading,
                                 bool ready, bool audioSkipped = false) {
    if (started) return {"Preparing", ReadyAction::None};
    if (pending) return {"Updating...", ReadyAction::None};
    if (!hasMap) return {host ? "Choose Map" : "Wait for Map", host ? ReadyAction::ChooseMap : ReadyAction::None};
    if (ready) return {"Unready", ReadyAction::Unready};
    if (cached) return {"Ready", ReadyAction::Ready};
    if (downloading)
        return {audioSkipped ? "Downloading..." : "Skip Music",
                audioSkipped ? ReadyAction::None : ReadyAction::SkipMusic};
    return {"Download", ReadyAction::Download};
}
}
