// Eclipse 설정 읽기: 이 빌드의 Geode와 맞는 Eclipse 패키지가 있을 때(compat/Eclipse.hpp).
// VersusEclipseConfig.hpp는 CMakeLists.txt가 빌드 폴더에 만들고, 받아 둔 Eclipse 패키지의 config.hpp를 넣는다.
#include "../Eclipse.hpp"

#include <VersusEclipseConfig.hpp>

#include <string>

namespace versus::compat {
bool eclipseReadable() {
    return true;
}
bool eclipseBool(std::string_view key, bool fallback) {
    return eclipse::config::getInternal<bool>(std::string(key), fallback);
}
double eclipseDouble(std::string_view key, double fallback) {
    return eclipse::config::getInternal<double>(std::string(key), fallback);
}
int eclipseInt(std::string_view key, int fallback) {
    return eclipse::config::getInternal<int>(std::string(key), fallback);
}
}
