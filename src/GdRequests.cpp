// GD 레벨 검색 / 다운로드 응답 라우터. Versus가 보낸 요청의 키를 기억해 두었다가, GD가 그 키의 응답을 처리할 때만
// 델리게이트 자리를 요청한 쪽으로 잠깐 바꿔 응답을 넘기고 원래 주인에게 되돌린다. 다른 키의 응답은 손대지 않는다.
#include "GdRequests.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <charconv>
#include <chrono>
#include <optional>
#include <unordered_map>

using namespace geode::prelude;
namespace {
using Clock = std::chrono::steady_clock;
// 키 하나의 행선지. 같은 키를 일부러 두 번 보냈으면(forget 후 다시 요청) 응답 두 개를 모두 받는다.
struct Route {
    LevelManagerDelegate* manager = nullptr;
    LevelDownloadDelegate* download = nullptr;
    int outstanding = 0;
    bool dropped = false;
    Clock::time_point sent;
};
std::unordered_map<std::string, Route>& routes() {
    static std::unordered_map<std::string, Route> value;
    return value;
}
std::string& delivering() {
    static std::string value;
    return value;
}
// 응답이 끝내 오지 않은 요청의 기록은 3분(버린 요청은 45초) 뒤에 지운다. 그 뒤에 온 응답은 GD의 원래 처리대로 간다.
void prune() {
    auto const now = Clock::now();
    std::erase_if(routes(), [now](auto const& item) {
        auto const age = now - item.second.sent;
        return age > std::chrono::minutes(3) || (item.second.dropped && age > std::chrono::seconds(45));
    });
}
// 응답 하나를 받을 행선지를 꺼낸다. 남은 응답이 없으면 기록을 지운다.
std::optional<Route> take(std::string const& key) {
    auto found = routes().find(key);
    if (found == routes().end()) return std::nullopt;
    auto route = found->second;
    if (--found->second.outstanding <= 0) routes().erase(found);
    return route;
}
// 델리게이트 자리를 잠깐 바꾸고, 그동안 다른 곳이 그 자리를 바꾸지 않았으면 원래대로 되돌린다.
template <class T>
class Swap {
    T*& m_slot;
    T* m_before;
    T* m_value;

public:
    Swap(T*& slot, T* value) : m_slot(slot), m_before(slot), m_value(value) { m_slot = value; }
    ~Swap() {
        if (m_slot == m_value) m_slot = m_before;
    }
    Swap(Swap const&) = delete;
    Swap& operator=(Swap const&) = delete;
};
// 라우팅된 콜백이 도는 동안 deliveringKey()가 그 키를 돌려주게 한다.
class Delivering {
    std::string m_before;

public:
    explicit Delivering(std::string key) : m_before(std::exchange(delivering(), std::move(key))) {}
    ~Delivering() { delivering() = std::move(m_before); }
};
Route& remember(std::string const& key) {
    prune();
    auto& route = routes()[key];
    route.manager = nullptr;
    route.download = nullptr;
    route.dropped = false;
    ++route.outstanding;
    route.sent = Clock::now();
    return route;
}
}

namespace versus::gdnet {
bool search(GJSearchObject* object, LevelManagerDelegate* target) {
    auto* manager = GameLevelManager::sharedState();
    if (!manager || !object) return false;
    std::string const key = object->getKey();
    if (manager->isDLActive(key.c_str())) return false;
    remember(key).manager = target;
    if (!manager->m_levelManagerDelegate) manager->m_levelManagerDelegate = target;
    // 저장된 결과로 곧바로 답하는 경우에도 요청한 쪽이 받도록 보내는 동안만 자리를 바꿔 둔다.
    {
        Delivering scope{key};
        Swap swap(manager->m_levelManagerDelegate, target);
        manager->getOnlineLevels(object);
    }
    if (!manager->isDLActive(key.c_str())) take(key);
    return true;
}
bool download(int levelID, LevelDownloadDelegate* target) {
    auto* manager = GameLevelManager::sharedState();
    if (!manager || levelID <= 0) return false;
    auto const key = downloadKey(levelID);
    if (manager->isDLActive(key.c_str())) return false;
    remember(key).download = target;
    if (!manager->m_levelDownloadDelegate) manager->m_levelDownloadDelegate = target;
    {
        Delivering scope{key};
        Swap swap(manager->m_levelDownloadDelegate, target);
        manager->downloadLevel(levelID, false, 0);
    }
    if (!manager->isDLActive(key.c_str())) take(key);
    return true;
}
void release(LevelManagerDelegate* target) {
    if (auto* manager = GameLevelManager::sharedState(); manager && target && manager->m_levelManagerDelegate == target)
        manager->m_levelManagerDelegate = nullptr;
}
void release(LevelDownloadDelegate* target) {
    if (auto* manager = GameLevelManager::sharedState();
        manager && target && manager->m_levelDownloadDelegate == target)
        manager->m_levelDownloadDelegate = nullptr;
}
std::string downloadKey(int levelID) {
    return fmt::format("{}_0_0", levelID);
}
std::string const& deliveringKey() {
    return delivering();
}
int levelOfKey(std::string const& key) {
    int id = 0;
    auto const end = key.find('_');
    if (end == std::string::npos || end == 0) return 0;
    auto parsed = std::from_chars(key.data(), key.data() + end, id);
    return parsed.ec == std::errc() && parsed.ptr == key.data() + end && id > 0 ? id : 0;
}
void drop(std::string const& key) {
    if (auto found = routes().find(key); found != routes().end()) {
        found->second.manager = nullptr;
        found->second.download = nullptr;
        found->second.dropped = true;
    }
}
bool active(std::string const& key) {
    auto* manager = GameLevelManager::sharedState();
    return manager && !key.empty() && manager->isDLActive(key.c_str());
}
void forget(std::string const& key) {
    if (auto* manager = GameLevelManager::sharedState(); manager && !key.empty())
        manager->removeDLFromActive(key.c_str());
}
}

// GD가 검색 / 다운로드 응답을 처리하는 곳. Versus가 보낸 키일 때만 델리게이트를 요청한 쪽(버린 키면 nullptr)으로
// 바꿔 원래 처리를 그대로 돌리고, 끝나면 되돌린다.
class $modify(VersusGdRoutes, GameLevelManager) {
    void onGetOnlineLevelsCompleted(gd::string response, gd::string tag) {
        auto route = take(std::string(tag));
        if (!route) {
            GameLevelManager::onGetOnlineLevelsCompleted(response, tag);
            return;
        }
        Delivering scope{std::string(tag)};
        Swap swap(m_levelManagerDelegate, route->manager);
        GameLevelManager::onGetOnlineLevelsCompleted(response, tag);
    }
    void processOnDownloadLevelCompleted(gd::string response, gd::string tag, bool update) {
        auto route = update ? std::nullopt : take(std::string(tag));
        if (!route) {
            GameLevelManager::processOnDownloadLevelCompleted(response, tag, update);
            return;
        }
        Delivering scope{std::string(tag)};
        Swap swap(m_levelDownloadDelegate, route->download);
        GameLevelManager::processOnDownloadLevelCompleted(response, tag, update);
    }
};
