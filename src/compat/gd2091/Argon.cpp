// Argon(GD 계정 인증, MIT, github.com/GlobedGD/argon) 연결: GD 2.209부터 Argon은 Geode 모드(dankmeme.argon)라
// mod.json 의존성으로 받고, 헤더와 라이브러리는 Geode가 빌드 폴더의 geode-deps로 연결한다.
#include "../Compat.hpp"

#include <dankmeme.argon/argon.hpp>
#include <Geode/utils/async.hpp>

namespace versus::compat {
// Argon 2는 지금 로그인한 GD 계정의 토큰을 지운다(account가 그 계정이다).
void argonForget(int) {
    argon::clearToken();
}
void argonAuth(std::function<void(std::string token, std::string error)> done) {
    geode::async::spawn(argon::startAuth(), [done = std::move(done)](geode::Result<std::string> result) {
        if (result.isErr())
            done({}, result.unwrapErr());
        else
            done(std::move(result).unwrap(), {});
    });
}
}
