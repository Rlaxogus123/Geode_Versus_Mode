// Eclipse(선택 의존성)의 설정 읽기. Eclipse의 설정 API 헤더는 Eclipse가 빌드된 Geode 버전의 이벤트를 쓰므로,
// 이 빌드의 Geode와 같은 메이저 버전용 Eclipse 패키지가 있을 때만 읽을 수 있다. CMakeLists.txt가 그 패키지를 보고
// compat/eclipse/Readable.cpp 또는 Unreadable.cpp를 빌드에 넣는다.
#pragma once

#include <string_view>

namespace versus::compat {
// 이 빌드가 Eclipse 설정을 읽을 수 있는지. false면 아래 함수들은 fallback을 돌려준다.
bool eclipseReadable();
bool eclipseBool(std::string_view key, bool fallback);
double eclipseDouble(std::string_view key, double fallback);
int eclipseInt(std::string_view key, int fallback);
}
