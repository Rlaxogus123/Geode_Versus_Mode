// 랜덤 맵 후보 검색. GD 온라인 레벨 검색(MostLiked 또는 ID 목록)을 난이도별로 돌려 가며
// 무작위 페이지를 요청하고, 필터에 맞는 레벨을 모아 최대 10개를 셔플해 콜백으로 돌려준다.
// 한 번에 하나의 검색만 실행되며, 방/필터가 바뀌거나 제한 시간(기본 90초)이 지나면 끝낸다.
// 1.0.8: 응답은 GdRequests 라우터로 받으므로 GD의 레벨 델리게이트 자리가 비기를 기다리지 않는다. 실패가 이어지면
// 1~6초씩 쉬고, 두 번 실패한 난이도는 그만 묻는다(전에는 0.4초마다 끝없이 다시 물어 GD 서버에 몰아쳤다).
#include "RandomMapSearch.hpp"
#include "RandomMapPolicy.hpp"
#include "BattleLevelData.hpp"
#include "GdRequests.hpp"
#include "LevelSelector.hpp"
#include "MapCache.hpp"
#include "compat/Compat.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <random>
#include <set>

using namespace geode::prelude;
namespace versus {
namespace {
// 검색을 진행하는 싱글턴 노드. 매 프레임 update()에서 요청을 하나씩 보내고
// LevelManagerDelegate 콜백으로 결과를 받는다.
class RandomSearch final : public CCNode, public LevelManagerDelegate {
    using Clock = std::chrono::steady_clock;
    RandomMapsCallback callback;
    MapSelection filter;
    RandomMapExtras extras;
    // 난이도 질의 하나(그룹)의 검색 상태: 후보, 요청한 페이지, 전체 개수, 요청 횟수, 피처 필터, ID 묶음.
    // likes가 켜지면 좋아요 순 정렬을 이용해 유효 페이지 범위를 좁힌다.
    struct Group {
        RandomMapQuery query;
        std::vector<LevelInfo> candidates;
        std::set<int> pages;
        int total = 100, requests = 0;
        // 1.0.8: 이 그룹에서 실패한 요청 수(응답 없음 / GD 실패). 두 번이면 소진으로 본다.
        int failures = 0;
        int feature = 0;
        std::string ids;
        bool likes = false;
        int page = 0;
        int likedSeen = -1;
        int likedPages = -1;
        // 서버가 알려 준 총 개수 기준 마지막 페이지(페이지당 10개).
        int allPages() const { return std::max(0, (total - 1) / 10); }
        int lastPage() const { return likedPages >= 0 ? std::min(allPages(), likedPages) : allPages(); }
        bool probing() const { return likes && likedPages == -1; }
        // 더 요청할 페이지가 없는지. ID 검색은 1회, 그 외는 최대 6회(좋아요 조건이면 8회) 또는 모든 유효 페이지 요청.
        // 두 번 실패한 그룹도 소진이다.
        bool exhausted() const {
            if (failures >= 2) return true;
            if (!ids.empty()) return requests >= 1;
            if (likedPages == -2 || requests >= (likes ? 8 : 6)) return true;
            if (probing()) return false;
            int asked = 0;
            for (int asked_page : pages) asked += asked_page <= lastPage();
            return asked >= lastPage() + 1;
        }
        // 좋아요 조건이 있을 때 이번 페이지의 결과로 유효 페이지 상한(likedPages)을 갱신한다.
        // 전부 미달이거나 빈 페이지면 이전 페이지까지, 일부 미달이면 이 페이지까지가 상한이다.
        void noteLikes(int count, int below) {
            if (!likes || !ids.empty()) return;
            int bound = -1;
            if (count == 0 || below == count)
                bound = page - 1;
            else if (below > 0)
                bound = page;
            else {
                likedSeen = std::max(likedSeen, page);
                return;
            }
            if (bound < 0) bound = -2;
            likedPages = likedPages == -1 ? bound : std::min(likedPages, bound);
        }
    };
    std::vector<Group> groups;
    size_t groupIndex = 0;
    std::mt19937 rng{std::random_device{}()};
    std::string roomID, key;
    std::function<bool()> alive;
    Ref<GJSearchObject> search;
    Clock::time_point began, requested, next;
    bool pending = false, completed = false, failed = false;
    // 1.0.8: 연속 실패 수(쉬는 시간을 정한다)와, GD가 이미 요청 중이라 보내지 못한 키를 처음 본 때.
    int failuresInRow = 0;
    std::string busyKey;
    Clock::time_point busySince;
    // 후보가 이만큼 모였으면 30초 뒤에는 남은 그룹을 기다리지 않고 끝낸다.
    static constexpr auto kSoftLimit = std::chrono::seconds(30);

    // 이 콜백이 우리가 보낸 요청(key)에 대한 것인지.
    bool owns(char const* value) const { return pending && value && key == value; }
    // 기다리던 요청을 놓는다. 그 응답이 늦게 오면 아무 데도 가지 않는다(다른 화면의 델리게이트에도).
    void dropPending() {
        if (pending) versus::gdnet::drop(key);
        pending = false;
        versus::gdnet::release(this);
    }
    size_t needed() const {
        return extras.minimum > 0 ? static_cast<size_t>(extras.minimum) : MIN_RANDOM_CANDIDATES;
    }
    size_t found() const {
        size_t count = 0;
        for (auto const& group : groups) count += group.candidates.size();
        return count;
    }
    // 연속 실패 n번째 뒤에 쉬는 시간: 1, 2, 4, 6초.
    static std::chrono::milliseconds backoff(int failures) {
        static constexpr int kSeconds[] = {1, 2, 4, 6};
        return std::chrono::seconds(kSeconds[std::clamp(failures, 1, 4) - 1]);
    }
    // 검색을 끝내고 결과를 콜백에 전달한다. 중복 제거 후 개수가 모자라면 오류를 돌려주고,
    // 성공이면 셔플해서(ID 지정 시 로컬 기록이 없는 맵을 앞으로) 최대 10개만 넘긴다.
    void finish(std::string error = {}) {
        dropPending();
        auto done = std::exchange(callback, {});
        alive = {};
        search = nullptr;
        std::vector<LevelInfo> candidates;
        for (auto& group : groups)
            for (auto& level : group.candidates)
                if (std::none_of(candidates.begin(), candidates.end(),
                                 [&](auto const& old) { return old.id == level.id; }))
                    candidates.push_back(std::move(level));
        size_t const needed = this->needed();
        // 1.0.8: 보낸 요청이 모두 실패했으면 필터가 아니라 GD 서버(또는 연결) 문제다.
        bool const unanswered =
            std::any_of(groups.begin(), groups.end(), [](auto const& g) { return g.requests > 0; }) &&
            std::all_of(groups.begin(), groups.end(), [](auto const& g) { return g.failures >= g.requests; });
        if (error.empty() && candidates.size() < needed)
            error = unanswered ? "GD's level servers did not answer the map search. Trying again..."
                    : needed == MIN_RANDOM_CANDIDATES
                        ? "At least five Classic maps are needed. Try again or change the filter."
                        : "No map of this tier's rules was found yet. Searching again...";
        std::shuffle(candidates.begin(), candidates.end(), rng);
        if (!extras.ids.empty())
            std::stable_partition(candidates.begin(), candidates.end(),
                                  [](LevelInfo const& level) { return mapLocalBest(level.id) < 0; });
        if (candidates.size() > 10) candidates.resize(10);
        auto result = error.empty() ? std::move(candidates) : std::vector<LevelInfo>{};
        if (done) done(std::move(result), std::move(error));
    }

public:
    // 싱글턴 생성: 스케줄러에 등록해 매 프레임 update를 받는다.
    static RandomSearch& get() {
        static auto* value = [] {
            auto* node = new RandomSearch;
            node->init();
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 1, false);
            node->release();
            return node;
        }();
        return *value;
    }
    // 길이와 피처 상태가 추가 조건에 맞는지.
    bool fitsExtras(LevelInfo const& info) const {
        if (!extras.lengths.empty() &&
            std::find(extras.lengths.begin(), extras.lengths.end(), info.length) == extras.lengths.end())
            return false;
        return extras.features.empty() ||
               std::find(extras.features.begin(), extras.features.end(), info.featureState) != extras.features.end();
    }
    static int netLikes(GJGameLevel* level) { return level->m_likes - level->m_dislikes; }
    // 순좋아요 수, 2인 모드 여부, 플레이 시간 상한 조건을 확인한다. 시간 상한은 측정 길이가 분류와 맞을 때만 적용.
    bool fitsLikesAndTime(GJGameLevel* level) const {
        if (extras.minLikes > 0 && netLikes(level) < extras.minLikes) return false;
        if (extras.noTwoPlayer && level->m_twoPlayerMode) return false;
        if (extras.onlyTwoPlayer && !level->m_twoPlayerMode) return false;
        if (extras.maxSeconds > 0 && level->m_timestamp > 0) {
            double const seconds = level->m_timestamp / 240.;
            if (versus::timeMatchesLength(seconds, level->m_levelLength) && seconds >= extras.maxSeconds) return false;
        }
        return true;
    }
    // GD 검색에 쓰는 길이 값 문자열(쉼표 구분).
    std::string lengthQuery() const {
        if (extras.lengths.empty()) return "0,1,2,3,4";
        std::string text;
        for (int length : extras.lengths) text += (text.empty() ? "" : ",") + std::to_string(length);
        return text;
    }
    // 검색을 시작한다. 이미 실행 중이거나 방/필터가 유효하지 않으면 바로 오류 콜백을 호출한다.
    // 필터의 길이/좋아요/2인 조건을 extras에 합치고, 난이도 질의와 피처 필터별로 검색 그룹을 만든다.
    void begin(MapSelection value, RandomMapExtras rules, RandomMapsCallback done, std::function<bool()> without = {}) {
        if (callback) {
            done({}, "Random map search is already running.");
            return;
        }
        auto const& room = Service::get().room();
        if (without ? !value.random || !validSelection(value) : !room || !value.random || !validSelection(value)) {
            done({}, without ? "This map filter cannot be searched." : "Room/filter changed.");
            return;
        }
        callback = std::move(done);
        filter = value;
        extras = std::move(rules);
        roomID = room && !without ? room->id : "";
        if (value.lengths != kLengthsAll) {
            std::vector<int> lengths;
            for (int length = 0; length <= 4; ++length)
                if ((value.lengths & (1 << length)) &&
                    (extras.lengths.empty() ||
                     std::find(extras.lengths.begin(), extras.lengths.end(), length) != extras.lengths.end()))
                    lengths.push_back(length);
            if (lengths.empty()) {
                auto finished = std::exchange(callback, {});
                finished({}, "No map length to search.");
                return;
            }
            extras.lengths = std::move(lengths);
        }
        extras.minLikes = std::max(extras.minLikes, value.minLikes);
        if (value.twoPlayer == 1) extras.noTwoPlayer = true;
        if (value.twoPlayer == 2) extras.onlyTwoPlayer = true;
        alive = std::move(without);
        groups.clear();
        groupIndex = 0;
        // ID 목록 검색이면 10개씩 묶어 그룹을 만든다.
        if (!extras.ids.empty()) {
            for (size_t first = 0; first < extras.ids.size(); first += 10) {
                Group group{RandomMapQuery{0, 0}};
                for (size_t i = first; i < std::min(first + 10, extras.ids.size()); ++i)
                    group.ids += (group.ids.empty() ? "" : ",") + std::to_string(extras.ids[i]);
                group.total = 10;
                groups.push_back(std::move(group));
            }
        } else {
            // 피처 필터 결정: 추가 조건이 모두 Epic 이상이면 그 값들, Featured만이면 1, 없으면 필터 없음(0).
            std::vector<int> filters{0};
            auto const& wanted = extras.features;
            if (!wanted.empty() && std::all_of(wanted.begin(), wanted.end(), [](int value) { return value >= 2; }))
                filters = wanted;
            else if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), 0) == wanted.end())
                filters = {1};
            for (int feature : filters)
                for (auto query : randomMapQueries(filter)) {
                    Group group{query};
                    group.feature = feature;
                    group.likes = extras.minLikes > 0;
                    groups.push_back(std::move(group));
                }
        }
        if (groups.empty()) {
            auto finished = std::exchange(callback, {});
            finished({}, "No map pool to search.");
            return;
        }
        completed = failed = pending = false;
        failuresInRow = 0;
        busyKey.clear();
        began = next = Clock::now();
    }
    // 서버가 알려 주는 페이지 정보 문자열의 첫 필드(전체 레벨 수)를 읽어 현재 그룹에 반영한다.
    void setupPageInfo(versus::compat::String info, char const* value) override {
        if (!owns(value)) return;
        std::string text(info);
        int count = 0;
        auto end = text.find(':');
        std::from_chars(text.data(), text.data() + (end == std::string::npos ? text.size() : end), count);
        if (count > 0) groups[groupIndex].total = std::min(count, 10000);
    }
    // 한 페이지 결과를 받아 필터에 맞는 레벨만 후보에 추가하고 좋아요 상한을 갱신한다.
    void loadLevelsFinished(CCArray* levels, char const* value) override {
        if (!owns(value) || completed) return;
        auto& group = groups[groupIndex];
        auto& candidates = group.candidates;
        int below = 0, count = 0;
        if (levels)
            for (auto* level : CCArrayExt<GJGameLevel*>(levels)) {
                ++count;
                if (extras.minLikes > 0 && netLikes(level) < extras.minLikes) ++below;
                auto info = describeLevel(level);
                if (matchesRandomMap(info, filter) && fitsExtras(info) && fitsLikesAndTime(level) &&
                    std::none_of(candidates.begin(), candidates.end(),
                                 [&](auto const& old) { return old.id == info.id; }))
                    candidates.push_back(std::move(info));
            }
        group.noteLikes(count, below);
        completed = true;
    }
    void loadLevelsFinished(CCArray* levels, char const* value, int) override { loadLevelsFinished(levels, value); }
    void loadLevelsFailed(char const* value) override {
        if (owns(value)) {
            completed = true;
            failed = true;
        }
    }
    void loadLevelsFailed(char const* value, int) override { loadLevelsFailed(value); }
    // 매 프레임 진행: 방/필터가 바뀌었는지, 90초 초과인지 확인하고, 응답을 처리한 뒤 다음 그룹/페이지 요청을 보낸다.
    void update(float) override {
        if (!callback) return;
        auto const now = Clock::now();
        if (alive) {
            if (!alive()) {
                finish("Map search stopped.");
                return;
            }
        } else if (auto const& room = Service::get().room();
                   !room || room->id != roomID || room->mapSelection != filter || !room->guest || !room->hostReady ||
                   !room->guestReady || !Service::get().isHost()) {
            finish("Room/filter changed.");
            return;
        }
        // 전체 검색 제한 시간: 기본 90초, 추가 조건에 정해 두었으면 그 시간(Season 2: 서버가 60초에 매치를 끝낸다).
        auto const elapsed = now - began;
        if (elapsed > std::chrono::seconds(extras.seconds > 0 ? extras.seconds : 90)) {
            finish();
            return;
        }
        // 응답 처리: 모든 그룹이 충분히 샘플링되어 후보가 20개 이상이거나 모두 소진이면 종료한다.
        // 30초가 지났고 필요한 만큼 모였어도 끝낸다. 아니면 소진되지 않은 다음 그룹으로 넘어가 0.4초 쉰다
        // (실패 뒤에는 연속 실패 수만큼 1~6초).
        if (completed) {
            completed = false;
            pending = false;
            versus::gdnet::release(this);
            auto& group = groups[groupIndex];
            if (group.ids.empty() && !group.likes && group.requests == 1 && group.total > 10 && !failed)
                group.candidates.clear();
            if (failed) {
                ++group.failures;
                ++failuresInRow;
            } else
                failuresInRow = 0;
            size_t count = 0;
            bool allSampled = true;
            for (auto const& item : groups) {
                count += item.candidates.size();
                allSampled &= item.requests >= 2 || item.exhausted();
            }
            if ((allSampled && count >= 20) || (count >= needed() && elapsed >= kSoftLimit) ||
                std::all_of(groups.begin(), groups.end(), [](auto const& g) { return g.exhausted(); })) {
                finish();
                return;
            }
            do {
                groupIndex = (groupIndex + 1) % groups.size();
            } while (groups[groupIndex].exhausted());
            next = now + (failed ? backoff(failuresInRow) : std::chrono::milliseconds(400));
            failed = false;
        }
        // 요청 대기 중: 12초 넘게 응답이 없으면 실패로 처리한다(늦은 응답은 버린다).
        if (pending) {
            if (now - requested > std::chrono::seconds(12)) {
                versus::gdnet::drop(key);
                completed = true;
                failed = true;
            }
            return;
        }
        if (now < next) return;
        if (found() >= needed() && elapsed >= kSoftLimit) {
            finish();
            return;
        }
        auto& group = groups[groupIndex];
        // 요청할 페이지 선택: 첫 요청은 0페이지, 좋아요 조건 탐색 중이면 점차 뒤 페이지로,
        // 그 외에는 아직 안 쓴 페이지를 무작위로 고른다.
        int page = 0;
        if (group.requests && group.probing() && group.likedSeen >= group.allPages())
            group.likedPages = group.allPages();
        if (group.requests && group.probing()) {
            page = std::min(group.allPages(), group.likedSeen < 0 ? 2 : group.likedSeen * 4 + 3);
        } else if (group.requests) {
            int const maxPage = group.lastPage();
            page = std::uniform_int_distribution<int>(0, maxPage)(rng);
            for (int i = 0; group.pages.contains(page) && i <= maxPage; ++i) page = (page + 1) % (maxPage + 1);
        }
        group.page = page;
        // 검색 객체 구성: ID 목록이면 Type19, 아니면 MostLiked에 난이도/길이/피처/2인 필터를 설정한다.
        if (!group.ids.empty()) {
            search = GJSearchObject::create(SearchType::Type19, std::string(group.ids + "&gameVersion=22"));
        } else {
            search = GJSearchObject::create(SearchType::MostLiked);
            search->m_page = page;
            search->m_starFilter = filter.rated;
            search->m_twoPlayerFilter = extras.onlyTwoPlayer;
            search->m_length = lengthQuery();
            search->m_featuredFilter = group.feature == 1;
            search->m_epicFilter = group.feature == 2;
            search->m_legendaryFilter = group.feature == 3;
            search->m_mythicFilter = group.feature == 4;
            search->m_difficulty = std::to_string(group.query.difficulty);
            search->m_demonFilter = static_cast<GJDifficulty>(group.query.demonFilter);
        }
        key = search->getKey();
        // GD가 같은 키를 이미 요청 중이면(다른 곳의 요청이나 응답이 오지 않은 지난 요청) 1초 뒤 다시 본다.
        // 15초가 지나도 그대로면 응답이 오지 않는 요청으로 보고 GD의 목록에서 지운 뒤 보낸다.
        if (versus::gdnet::active(key)) {
            if (busyKey != key) {
                busyKey = key;
                busySince = now;
            }
            if (now - busySince < std::chrono::seconds(15)) {
                next = now + std::chrono::seconds(1);
                return;
            }
            log::warn("Versus map search: GD kept {} busy for 15 s; asking again", key);
            versus::gdnet::forget(key);
        }
        busyKey.clear();
        group.pages.insert(page);
        pending = true;
        requested = now;
        ++group.requests;
        if (!versus::gdnet::search(search, this)) {
            pending = false;
            --group.requests;
            next = now + std::chrono::seconds(1);
        }
    }
};
}
// 방의 필터로 검색(추가 조건 없음).
void findRandomMaps(MapSelection filter, RandomMapsCallback callback) {
    RandomSearch::get().begin(filter, {}, std::move(callback));
}
// 추가 조건과 함께 검색.
void findRandomMaps(MapSelection filter, RandomMapExtras extras, RandomMapsCallback callback) {
    RandomSearch::get().begin(filter, std::move(extras), std::move(callback));
}
// 방 없이 검색한다. alive가 없으면 항상 계속.
void findRandomMapsWithoutRoom(MapSelection filter, std::function<bool()> alive, RandomMapsCallback callback) {
    if (!alive) alive = [] { return true; };
    RandomSearch::get().begin(filter, {}, std::move(callback), std::move(alive));
}

namespace {
// 1.0.8: 레벨 하나를 ID로 찾는 싱글턴. 응답은 GdRequests 라우터로 받고(델리게이트 자리를 기다리지 않음),
// 응답이 12초(전체 20초) 안에 없으면 실패로 끝낸다.
class LevelLookup final : public CCNode, public LevelManagerDelegate {
    using Clock = std::chrono::steady_clock;
    using Done = std::function<void(std::optional<LevelInfo>, std::string)>;
    Done m_done;
    int64_t m_id = 0;
    std::string m_key;
    Ref<GJSearchObject> m_search;
    Clock::time_point m_began, m_asked;
    bool m_pending = false;

    bool owns(char const* key) const { return m_pending && key && m_key == key; }
    void finish(std::optional<LevelInfo> level, std::string error) {
        if (m_pending) versus::gdnet::drop(m_key);
        m_pending = false;
        versus::gdnet::release(this);
        m_search = nullptr;
        if (auto done = std::exchange(m_done, {})) done(std::move(level), std::move(error));
    }

public:
    static LevelLookup& get() {
        static auto* value = [] {
            auto* node = new LevelLookup;
            node->init();
            CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(node, 1, false);
            node->release();
            return node;
        }();
        return *value;
    }
    void begin(int64_t id, Done done) {
        if (m_done) finish(std::nullopt, "Canceled.");
        m_id = id;
        m_done = std::move(done);
        m_began = Clock::now();
    }
    void loadLevelsFinished(CCArray* levels, char const* key) override {
        if (!owns(key)) return;
        std::optional<LevelInfo> found;
        if (levels)
            for (auto* level : CCArrayExt<GJGameLevel*>(levels))
                if (level && level->m_levelID.value() == m_id) found = describeLevel(level);
        if (found)
            finish(std::move(found), {});
        else
            finish(std::nullopt, "Not found on GD's servers.");
    }
    void loadLevelsFinished(CCArray* levels, char const* key, int) override { loadLevelsFinished(levels, key); }
    void loadLevelsFailed(char const* key) override {
        if (owns(key)) finish(std::nullopt, "Not found on GD's servers.");
    }
    void loadLevelsFailed(char const* key, int) override { loadLevelsFailed(key); }
    void setupPageInfo(versus::compat::String, char const*) override {}
    void update(float) override {
        if (!m_done) return;
        auto const now = Clock::now();
        if (m_pending ? now - m_asked > std::chrono::seconds(12) : now - m_began > std::chrono::seconds(20)) {
            finish(std::nullopt, "GD's servers did not answer. Try again.");
            return;
        }
        if (m_pending) return;
        m_search = GJSearchObject::create(SearchType::Search, std::string(std::to_string(m_id)));
        m_key = m_search->getKey();
        if (versus::gdnet::active(m_key)) return;
        m_pending = true;
        m_asked = now;
        if (!versus::gdnet::search(m_search, this)) m_pending = false;
    }
};
}
void lookupLevel(int64_t id, std::function<void(std::optional<LevelInfo>, std::string)> done) {
    if (id <= 0) {
        if (done) done(std::nullopt, "Invalid level ID.");
        return;
    }
    LevelLookup::get().begin(id, std::move(done));
}
}
