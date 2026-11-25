// Eclipse 설정 읽기: 이 빌드의 Geode와 맞는 Eclipse 패키지가 없을 때(compat/Eclipse.hpp).
// 예를 들어 Geode 6용 Eclipse가 아직 없으면 Geode CLI는 Geode 5용 패키지를 받는데, 그 헤더는 Geode 6에서 컴파일되지 않는다.
#include "../Eclipse.hpp"

namespace versus::compat {
bool eclipseReadable() {
    return false;
}
bool eclipseBool(std::string_view, bool fallback) {
    return fallback;
}
double eclipseDouble(std::string_view, double fallback) {
    return fallback;
}
int eclipseInt(std::string_view, int fallback) {
    return fallback;
}
}
