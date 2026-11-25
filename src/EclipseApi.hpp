// Eclipse(선택 의존성)의 설정 읽기 API를 넣는다. 어떤 헤더를 쓸지는 CMakeLists.txt가
// geode-deps/eclipse.eclipse-menu의 mod.json을 보고 VERSUS_ECLIPSE_API로 정한다.
//   1 = Geode 6용 패키지, Geode CLI 4가 include/를 벗긴 배치(eclipse.eclipse-menu/config.hpp)
//   2 = Geode 6용 패키지인데 include/를 그대로 둔 배치
//   0 = 패키지가 없거나 Geode 5용이다. Geode 5용 Eclipse 헤더는 Geode 6 이벤트와 맞지 않아 넣지 않는다.
// 0이면 Eclipse 설정을 읽을 수 없으므로 ExternalCheatMonitor.cpp는 Eclipse가 로드돼 있기만 해도 금지 기능으로 본다.
#pragma once

#ifndef VERSUS_ECLIPSE_API
#define VERSUS_ECLIPSE_API 0
#endif

#if VERSUS_ECLIPSE_API == 1
#include <eclipse.eclipse-menu/config.hpp>
#elif VERSUS_ECLIPSE_API == 2
#include <eclipse.eclipse-menu/include/config.hpp>
#endif
