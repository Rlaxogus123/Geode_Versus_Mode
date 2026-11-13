// 게임 실행 없이 준비 버튼의 우선순위와, 따로 둔 음악 건너뛰기 버튼의 표시/대기 시간을 검증한다.
#include "../src/RoomControls.hpp"
// Release 빌드에서도 아래 검증을 실행한다.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <initializer_list>
#include <string_view>

int main() {
    using namespace versus;
    for (bool host : {false, true}) {
        // 1.0.8: 다운로드 중의 준비 버튼은 비활성이다(건너뛰기로 바뀌지 않는다).
        auto control = readyControl(false, false, host, true, false, true, false);
        assert(control.action == ReadyAction::None && !control.enabled());
        assert(std::string_view(control.caption) == "Downloading...");
        control = readyControl(false, false, host, true, true, false, false);
        assert(control.action == ReadyAction::Ready && control.enabled());

        // 다운로드 완료와 클릭이 겹치거나 이미 준비한 경우에도 동작을 보존한다.
        assert(readyControl(false, false, host, true, true, true, false).action == ReadyAction::Ready);
        assert(readyControl(false, false, host, true, true, false, true).action == ReadyAction::Unready);
        assert(readyControl(false, false, host, true, false, false, false).action == ReadyAction::Download);

        // 시작/서버 응답 대기/맵 미선택 중
        assert(!readyControl(true, false, host, true, false, true, false).enabled());
        assert(!readyControl(false, true, host, true, false, true, false).enabled());
        assert(readyControl(false, false, host, false, false, true, false).action ==
               (host ? ReadyAction::ChooseMap : ReadyAction::None));
    }

    // 따로 둔 건너뛰기 버튼: 맵을 받는 중이고 아직 건너뛰지 않았을 때만 보인다.
    assert(skipMusicShown(false, false, true, false, true, false, false));
    assert(!skipMusicShown(false, false, true, false, true, false, true));   // 이미 건너뜀
    assert(!skipMusicShown(false, false, true, false, false, false, false));  // 받는 중이 아님
    assert(!skipMusicShown(false, false, true, true, true, false, false));    // 이미 받음
    assert(!skipMusicShown(false, false, true, false, true, true, false));    // 이미 준비함
    assert(!skipMusicShown(true, false, true, false, true, false, false));    // 시작됨
    assert(!skipMusicShown(false, true, true, false, true, false, false));    // 서버 응답 대기
    assert(!skipMusicShown(false, false, false, false, true, false, false));  // 맵 없음

    // 보이고 나서 kSkipMusicArmSeconds 가 지나야 눌린다. 다른 맵이면 다시 센다.
    using Clock = SkipMusicArm::Clock;
    SkipMusicArm arm;
    auto const t0 = Clock::now();
    assert(arm.wait("map-1", t0) == kSkipMusicArmSeconds);
    assert(arm.wait("map-1", t0 + std::chrono::milliseconds(400)) > 0.);
    assert(arm.wait("map-1", t0 + std::chrono::milliseconds(1000)) == 0.);
    assert(arm.wait("map-1", t0 + std::chrono::seconds(5)) == 0.);
    assert(arm.wait("map-2", t0 + std::chrono::seconds(5)) == kSkipMusicArmSeconds);
    arm.reset();
    assert(arm.wait("map-2", t0 + std::chrono::seconds(6)) == kSkipMusicArmSeconds);
}
