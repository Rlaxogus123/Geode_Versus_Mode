// Argon(GD 계정 인증, MIT, github.com/GlobedGD/argon) 연결: GD 2.2081용 Argon 1.5는 C++ 라이브러리라
// CMakeLists.txt가 CPM으로 받아 링크한다(받지 못하면 VERSUS_ARGON=0이고 Versus는 Argon을 부르지 않는다).
#include "../Compat.hpp"

#if VERSUS_ARGON
#include <argon/argon.hpp>
#include <Geode/utils/async.hpp>

namespace versus::compat {
void argonForget(int account) {
    argon::clearToken(account);
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
#endif
