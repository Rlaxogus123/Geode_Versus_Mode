// 게임 실행 없이 음악 건너뛰기와 준비 버튼의 우선순위를 검증한다.
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
        auto control = readyControl(false, false, host, true, false, true, false);
        assert(control.action == ReadyAction::SkipMusic && control.enabled());
        assert(std::string_view(control.caption) == "Skip Music");

        // 음악을 건너뛰어도 레벨 데이터를 받기 전에는 준비할 수 없다.
        control = readyControl(false, false, host, true, false, true, false, true);
        assert(control.action == ReadyAction::None && !control.enabled());
        assert(std::string_view(control.caption) == "Downloading...");
        control = readyControl(false, false, host, true, true, false, false, true);
        assert(control.action == ReadyAction::Ready && control.enabled());

        // 다운로드 완료와 클릭이 겹치거나 이미 준비한 경우에도 동작을 보존한다.
        assert(readyControl(false, false, host, true, true, true, false).action == ReadyAction::Ready);
        assert(readyControl(false, false, host, true, true, false, true, true).action == ReadyAction::Unready);
        assert(readyControl(false, false, host, true, false, false, false).action == ReadyAction::Download);
        assert(readyControl(false, false, host, true, false, false, false, true).action == ReadyAction::Download);

        // 시작/서버 응답 대기/맵 미선택 중에는 건너뛰기가 활성화되지 않는다.
        for (bool skipped : {false, true}) {
            assert(!readyControl(true, false, host, true, false, true, false, skipped).enabled());
            assert(!readyControl(false, true, host, true, false, true, false, skipped).enabled());
            assert(readyControl(false, false, host, false, false, true, false, skipped).action ==
                   (host ? ReadyAction::ChooseMap : ReadyAction::None));
        }
    }
}
