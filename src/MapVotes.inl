// 맵 좋아요/싫어요(Map Votes) UI: 레벨의 득표 수를 보여 주는 MapVotesBadge,
// 투표 버튼 MapVoteButtons, 한 플레이어가 남긴 평가 목록 팝업 MapRatingsPopup.
// 득표 기능은 Season 2 접근 권한이 있을 때만 보인다.
//
// 공용 도우미 모음. kRefreshSeconds는 배지의 갱신 주기(초), kDim은 선택되지 않은 쪽 버튼의 투명도(0~255).
namespace mapvotes {
inline bool shown() {
    return Service::get().season2Access();
}
constexpr float kRefreshSeconds = 20.f;
constexpr float kDim = 95.f;

// 득표 수를 짧게 표시한다(1.2K, 15K, 1.0M).
inline std::string count(int value) {
    if (value >= 1'000'000) return fmt::format("{:.1f}M", value / 1'000'000.);
    if (value >= 10'000) return fmt::format("{}K", value / 1'000);
    if (value >= 1'000) return fmt::format("{:.1f}K", value / 1'000.);
    return std::to_string(value);
}
// GameSheet03의 프레임 스프라이트를 높이에 맞춰 만든다. 투명 여백이 잘린 프레임은
// 실제 이미지 영역만 쓰도록 다시 만들어 크기가 어긋나지 않게 한다.
inline CCSprite* icon(char const* frame, float height) {
    auto* sprite = loadedFrameSprite(frame, "GJ_GameSheet03.plist");
    if (!sprite) return nullptr;
    auto const rect = sprite->getTextureRect();
    auto const full = sprite->getContentSize();
    if (sprite->getTexture() && rect.size.width > 0.f && rect.size.height > 0.f &&
        (rect.size.width + .5f < full.width || rect.size.height + .5f < full.height)) {
        // 사각형을 받는 createWithTexture는 Apple Silicon macOS 2.2091 바인딩에 주소가 없어 텍스처만 넘기고 영역은 바로 아래에서 정한다.
        if (auto* trimmed = CCSprite::createWithTexture(sprite->getTexture())) {
            trimmed->setTextureRect(rect, sprite->isTextureRectRotated(), rect.size);
            sprite = trimmed;
        }
    }
    sprite->setScale(height / std::max(1.f, sprite->getContentSize().height));
    return sprite;
}
// 노드 트리에서 보이는 스프라이트의 가장 낮은 y를 toSpace 좌표계로 구한다.
inline void lowestPoint(CCNode* node, CCAffineTransform const& toSpace, float& low) {
    if (!node || !node->isVisible()) return;
    if (auto* sprite = typeinfo_cast<CCSprite*>(node)) {
        auto const& rect = sprite->getTextureRect();
        if (rect.size.width > 0.f && rect.size.height > 0.f) {
            CCPoint const from = sprite->getOffsetPosition();
            for (float const x : {from.x, from.x + rect.size.width})
                low = std::min(low, CCPointApplyAffineTransform({x, from.y}, toSpace).y);
        }
    }
    if (auto* children = node->getChildren())
        for (auto* child : CCArrayExt<CCNode*>(children))
            if (child) lowestPoint(child, CCAffineTransformConcat(child->nodeToParentTransform(), toSpace), low);
}
// 노드 내용의 가장 낮은 지점이 bottom에 오도록 세로 위치를 보정한다.
inline void sitOn(CCNode* node, float bottom) {
    if (!node) return;
    float low = std::numeric_limits<float>::max();
    lowestPoint(node, node->nodeToParentTransform(), low);
    if (std::isfinite(low) && low < std::numeric_limits<float>::max() / 2.f)
        node->setPositionY(node->getPositionY() + bottom - low);
}
// 레벨 ID로 레벨 정보 화면(LevelInfoLayer)을 연다. 로비(lounge)를 먼저 정리한 뒤 씬을 푸시한다.
inline void openMapPage(int64_t id, std::string const& name) {
    if (id <= 0 || id > std::numeric_limits<int>::max()) return;
    auto* level = GJGameLevel::create();
    level->m_levelID = static_cast<int>(id);
    level->m_levelName = name;
    level->m_levelType = GJLevelType::Saved;
    lounge::release();
    if (auto* scene = LevelInfoLayer::scene(level, false))
        CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.2f, scene));
}
}

// 레벨의 좋아요/싫어요 수를 한 줄로 보여 주는 배지. 20초마다(숨겨져 있을 땐 건너뜀) 다시 받아 온다.
// compact가 아니면 앞에 프로필 아이콘이 붙는다.
class MapVotesBadge final : public CCNodeRGBA {
    int64_t m_level = 0;
    float m_height = 10.f;
    float m_anchorX = 0.f;
    bool m_compact = false;
    CCLabelBMFont* m_up = nullptr;
    CCLabelBMFont* m_down = nullptr;
    CCSprite* m_upIcon = nullptr;
    CCSprite* m_downIcon = nullptr;
    CCSprite* m_who = nullptr;
    // 다음 갱신까지 남은 시간(초).
    float m_wait = 0.f;
    // 가장 넓은 숫자("9.9k")의 폭. 자리를 미리 확보(reserve)하는 데 쓴다.
    float m_wideCount = 0.f;
    // 마지막으로 표시한 "up|down" 값. 같으면 다시 그리지 않는다.
    std::string m_shown;

    bool init(int64_t level, float height, float anchorX, bool compact) {
        if (!CCNodeRGBA::init()) return false;
        m_level = level;
        m_height = height;
        m_anchorX = anchorX;
        m_compact = compact;
        setID("map-votes-badge"_spr);
        setCascadeOpacityEnabled(true);
        float const text = height * .028f;
        if (!compact && (m_who = mapvotes::icon("GJ_profileButton_001.png", height + 2.f))) addChild(m_who);
        if ((m_upIcon = mapvotes::icon("GJ_likesIcon_001.png", height))) addChild(m_upIcon);
        m_up = label(this, "--", {0.f, 0.f}, text, 0.f, ccWHITE, true, "bigFont.fnt");
        if ((m_downIcon = mapvotes::icon("GJ_dislikesIcon_001.png", height))) addChild(m_downIcon);
        m_down = label(this, "--", {0.f, 0.f}, text, 0.f, ccWHITE, true, "bigFont.fnt");
        m_up->setString("9.9k");
        m_wideCount = m_up->getScaledContentSize().width;
        m_up->setString("--");
        // 이미 알고 있는 득표 값이 있으면 먼저 보여 주고, 서버에 최신 값을 요청한다.
        if (auto kept = Service::get().knownMapVotes(level)) show(*kept);
        layout();
        ask();
        scheduleUpdate();
        return true;
    }
    // 득표 수를 표시한다. 0인 항목은 흐린 색으로 칠한다.
    void show(MapVotes const& votes) {
        std::string const key = fmt::format("{}|{}", votes.up, votes.down);
        if (key == m_shown) return;
        m_shown = key;
        m_up->setString(mapvotes::count(votes.up).c_str());
        m_down->setString(mapvotes::count(votes.down).c_str());
        m_up->setColor(votes.up > 0 ? ccc3(255, 255, 255) : ccc3(200, 210, 225));
        m_down->setColor(votes.down > 0 ? ccc3(255, 255, 255) : ccc3(200, 210, 225));
        layout();
    }
    static constexpr float kSplit = .85f;
    // 아이콘과 숫자를 가로로 배치하고 전체 폭에 맞춰 컨텐츠 크기와 앵커를 정한다.
    void layout() {
        float const gap = m_height * .25f, pad = m_height * .45f, split = m_height * kSplit;
        float x = 0.f;
        auto place = [&x, gap, this](CCNode* node, float width) {
            if (!node) return;
            node->setPosition({x + width / 2.f, m_height / 2.f});
            x += width + gap;
        };
        if (m_who) {
            place(m_who, m_who->getScaledContentSize().width);
            x += pad - gap;
        }
        if (m_upIcon) place(m_upIcon, m_upIcon->getScaledContentSize().width);
        m_up->setAnchorPoint({0.f, .5f});
        place(m_up, m_up->getScaledContentSize().width);
        x += split - gap;
        if (m_downIcon) place(m_downIcon, m_downIcon->getScaledContentSize().width);
        m_down->setAnchorPoint({0.f, .5f});
        place(m_down, m_down->getScaledContentSize().width);
        float const width = std::max(0.f, x - gap);
        setContentSize({width, m_height});
        setAnchorPoint({m_anchorX, .5f});
        ignoreAnchorPointForPosition(false);
    }
    // 이 레벨의 득표를 서버에 요청하고 타이머를 다시 시작한다.
    void ask() {
        m_wait = mapvotes::kRefreshSeconds;
        Service::get().fetchMapVotes({m_level}, [self = Weak<MapVotesBadge>(this),
                                                 level = m_level](std::map<int64_t, MapVotes> votes, std::string) {
            auto owner = self.lock();
            if (!owner) return;
            if (auto found = votes.find(level); found != votes.end()) owner->show(found->second);
        });
    }
    // 타이머가 끝나면 갱신한다. 이 노드나 부모가 하나라도 숨겨져 있으면 요청하지 않고 대기만 한다.
    void update(float dt) override {
        m_wait -= dt;
        if (m_wait > 0.f) return;
        for (CCNode* node = this; node; node = node->getParent())
            if (!node->isVisible()) {
                m_wait = mapvotes::kRefreshSeconds;
                return;
            }
        ask();
    }

public:
    static MapVotesBadge* create(int64_t level, float height, float anchorX, bool compact = false) {
        auto* node = new MapVotesBadge;
        if (node->init(level, height, anchorX, compact)) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    float width() const { return getContentSize().width; }
    // 가장 넓은 숫자 기준으로 필요한 폭(자리 예약용).
    float reserve() const {
        float const gap = m_height * .25f, pad = m_height * .45f, split = m_height * kSplit;
        float x = 0.f;
        if (m_who) x += m_who->getScaledContentSize().width + pad;
        if (m_upIcon) x += m_upIcon->getScaledContentSize().width + gap;
        x += m_wideCount + split;
        if (m_downIcon) x += m_downIcon->getScaledContentSize().width + gap;
        return x + m_wideCount;
    }
};

// 배지를 만들어 parent에 붙인다. 레벨 ID가 없거나 득표 기능이 꺼져 있으면 nullptr.
MapVotesBadge* mapVotesBadge(CCNode* parent, int64_t level, CCPoint at, float height, float anchorX = 0.f, int z = 0,
                             bool compact = false) {
    if (!parent || level <= 0 || !mapvotes::shown()) return nullptr;
    auto* badge = MapVotesBadge::create(level, height, anchorX, compact);
    if (!badge) return nullptr;
    badge->setPosition(at);
    parent->addChild(badge, z);
    return badge;
}

// 좋아요/싫어요 버튼 한 쌍. m_vote는 1(좋아요), -1(싫어요), 0(없음).
// 같은 버튼을 다시 누르면 평가를 철회한다. 내 기존 투표를 모르면(knownVote 범위 밖) 서버에서 받아 온다.
class MapVoteButtons final : public CCNode {
public:
    using Changed = std::function<void(MapVotes const&)>;

private:
    int64_t m_level = 0;
    std::string m_name;
    int m_vote = 0;
    bool m_known = false, m_busy = false, m_mine = true;
    float m_size = 24.f;
    CCMenuItemSpriteExtra* m_like = nullptr;
    CCMenuItemSpriteExtra* m_dislike = nullptr;
    Changed m_changed;

    bool init(int64_t level, std::string name, float size, int knownVote, Changed changed) {
        if (!CCNode::init()) return false;
        m_level = level;
        m_name = std::move(name);
        m_size = size;
        m_changed = std::move(changed);
        setID("map-vote-buttons"_spr);
        setContentSize({size * 2.f + size * .35f, size});
        setAnchorPoint({.5f, .5f});
        ignoreAnchorPointForPosition(false);
        auto* holder = CCMenu::create();
        holder->setPosition({0.f, 0.f});
        addChild(holder);
        // 버튼 하나를 만든다.
        auto make = [this, holder, size](char const* frame, SEL_MenuHandler handler, float x, char const* id) {
            auto* sprite = mapvotes::icon(frame, size);
            if (!sprite) sprite = CCSprite::create();
            auto* item = CCMenuItemSpriteExtra::create(sprite, this, handler);
            item->setPosition({x, size / 2.f});
            item->setID(id);
            holder->addChild(item);
            return item;
        };
        m_like = make("GJ_likeBtn_001.png", menu_selector(MapVoteButtons::onLike), size / 2.f, "map-like"_spr);
        m_dislike =
            make("GJ_dislikeBtn_001.png", menu_selector(MapVoteButtons::onDislike), size * 1.85f, "map-dislike"_spr);
        if (knownVote >= -1 && knownVote <= 1) {
            m_vote = knownVote;
            m_known = true;
        }
        refresh();
        if (!m_known) {
            Service::get().fetchMapVotes({m_level}, [self = Weak<MapVoteButtons>(this),
                                                     level = m_level](std::map<int64_t, MapVotes> votes, std::string) {
                auto owner = self.lock();
                if (!owner || owner->m_known) return;
                if (auto found = votes.find(level); found != votes.end()) {
                    owner->m_vote = found->second.mine;
                    owner->m_known = true;
                    owner->refresh();
                }
            });
        }
        return true;
    }
    // 투표 상태에 맞춰 버튼 투명도와 크기를 갱신한다. 요청 중에는 비활성.
    void refresh() {
        for (auto* item : {m_like, m_dislike}) {
            cascadeOpacity(item);
            item->setEnabled(!m_busy);
        }
        GLubyte const dim = static_cast<GLubyte>(mapvotes::kDim);
        m_like->setOpacity(m_vote == -1 ? dim : 255);
        m_dislike->setOpacity(m_vote == 1 ? dim : 255);
        m_like->m_baseScale = m_vote == 1 ? 1.12f : 1.f;
        m_dislike->m_baseScale = m_vote == -1 ? 1.12f : 1.f;
        m_like->setScale(m_like->m_baseScale);
        m_dislike->setScale(m_dislike->m_baseScale);
    }
    // 투표를 서버에 보낸다. 요청 중 중복 입력은 무시하고, 실패하면 오류 알림만 띄우고 상태를 되돌린다.
    void send(int vote) {
        if (m_busy || m_level <= 0) return;
        int const next = m_vote == vote ? 0 : vote;
        m_busy = true;
        refresh();
        Service::get().voteMap(m_level, m_name, next,
                               [self = Weak<MapVoteButtons>(this)](std::optional<MapVotes> votes, std::string detail) {
                                   auto owner = self.lock();
                                   if (!owner) return;
                                   owner->m_busy = false;
                                   if (!votes) {
                                       owner->refresh();
                                       Notification::create(detail.empty() ? "The rating could not be saved." : detail,
                                                            NotificationIcon::Error, 2.5f)
                                           ->show();
                                       return;
                                   }
                                   owner->m_vote = votes->mine;
                                   owner->m_known = true;
                                   owner->refresh();
                                   if (owner->m_changed) owner->m_changed(*votes);
                               });
    }
    void onLike(CCObject*) { send(1); }
    void onDislike(CCObject*) { send(-1); }

public:
    static MapVoteButtons* create(int64_t level, std::string name, float size, int knownVote = -2,
                                  Changed changed = nullptr) {
        auto* node = new MapVoteButtons;
        if (node->init(level, std::move(name), size, knownVote, std::move(changed))) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    int vote() const { return m_vote; }
    void setVote(int vote) {
        m_vote = std::clamp(vote, -1, 1);
        m_known = true;
        refresh();
    }
};

// 투표 버튼을 만들어 붙인다. 레벨 ID가 없거나 득표 기능이 꺼져 있으면 nullptr.
MapVoteButtons* mapVoteButtons(CCNode* parent, int64_t level, std::string name, CCPoint at, float size,
                               int knownVote = -2, MapVoteButtons::Changed changed = nullptr, int z = 0) {
    if (!parent || level <= 0 || !mapvotes::shown()) return nullptr;
    auto* buttons = MapVoteButtons::create(level, std::move(name), size, knownVote, std::move(changed));
    if (!buttons) return nullptr;
    buttons->setPosition(at);
    parent->addChild(buttons, z);
    return buttons;
}

// 한 플레이어가 남긴 맵 평가 목록 팝업. 이름 부분을 누르면 그 맵 정보 화면을 열고,
// 내 목록이면 마크를 눌러 평가를 바꾸거나(같은 마크를 다시 누르면) 철회할 수 있다.
class MapRatingsPopup final : public ::versus::DeferredPopup {
    static constexpr float kW = 400.f, kH = 280.f, kListW = kW - 24.f, kListH = 190.f, kRowH = 28.f;
    static constexpr float kCellW = kListW - 8.f, kMarkSize = 18.f;
    static constexpr float kLikeX = kCellW - 48.f, kDislikeX = kCellW - 22.f, kNameEnd = 180.f;
    PlayerProfile m_profile;
    bool m_mine = false;
    TapScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_summary = nullptr;
    std::vector<MapVoteRecord> m_rows;
    int m_busy = -1;
    bool m_filled = false;

    // 목록의 좋아요/싫어요 수를 요약 라벨에 표시한다.
    void summarize() {
        int up = 0, down = 0;
        for (auto const& row : m_rows) {
            if (row.vote > 0)
                ++up;
            else if (row.vote < 0)
                ++down;
        }
        m_summary->setString(
            fmt::format("{}  -  {} LIKED, {} DISLIKED", m_profile.name.empty() ? "Player" : m_profile.name, up, down)
                .c_str());
        m_summary->limitLabelWidth(kW - 40.f, .3f, .15f);
    }
    // 행 안의 탭 위치(x)로 동작을 고른다: 이름 영역은 맵 열기, 오른쪽 마크 영역은 투표(내 목록, 진행 중 요청이 없을 때만).
    void onTap(int index, CCPoint at) {
        if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
        auto const& row = m_rows[static_cast<size_t>(index)];
        if (at.x < kNameEnd) {
            mapvotes::openMapPage(row.levelId, row.name);
            return;
        }
        if (!m_mine || m_busy >= 0) return;
        if (at.x >= kDislikeX - kMarkSize * .75f)
            vote(index, -1);
        else if (at.x >= kLikeX - kMarkSize * .75f && at.x < kLikeX + kMarkSize * .75f)
            vote(index, 1);
    }
    // 행의 평가를 바꿔 서버에 보낸다. 성공하면 서버가 돌려준 값으로 행을 갱신하고 다시 그린다.
    void vote(int index, int value) {
        auto& row = m_rows[static_cast<size_t>(index)];
        int const next = row.vote == value ? 0 : value;
        m_busy = index;
        Service::get().voteMap(
            row.levelId, row.name, next,
            [self = Weak<MapRatingsPopup>(this), index](std::optional<MapVotes> votes, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_busy = -1;
                if (!votes) {
                    Notification::create(detail.empty() ? "The rating could not be saved." : detail,
                                         NotificationIcon::Error, 2.5f)
                        ->show();
                    return;
                }
                if (index >= 0 && index < static_cast<int>(owner->m_rows.size()))
                    owner->m_rows[static_cast<size_t>(index)].vote = votes->mine;
                owner->fill();
            });
    }
    // 행 목록을 다시 그린다. 스크롤 위치는 가능한 한 유지하고, 비었으면 안내 문구를 보인다.
    void fill() {
        auto* content = m_scroll->m_contentLayer;
        float const keepY = content->getPositionY();
        content->removeAllChildrenWithCleanup(true);
        float const height = std::max(kListH, kRowH * m_rows.size() + 4.f);
        content->setContentSize({kListW, height});
        m_status->setVisible(m_rows.empty());
        if (m_rows.empty())
            m_status->setString(m_mine ? "You have not rated a map yet.\nRate the maps of your recent matches."
                                       : "No map ratings yet.");
        auto const now = Service::get().serverNow();
        GLubyte const dim = static_cast<GLubyte>(mapvotes::kDim);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto const& row = m_rows[i];
            auto* cell = CCNode::create();
            cell->setContentSize({kCellW, kRowH - 3.f});
            cell->setPosition({4.f, height - 2.f - kRowH * (i + 1)});
            cell->setTag(static_cast<int>(i));
            content->addChild(cell);
            auto* back = NineSlice::create("square02b_001.png");
            back->setContentSize(cell->getContentSize());
            back->setAnchorPoint({0.f, 0.f});
            back->setColor({0, 0, 0});
            back->setOpacity(70);
            cell->addChild(back);
            float const mid = (kRowH - 3.f) / 2.f;
            if (row.vote == 0)
                label(cell, "-", {14.f, mid}, .3f, 20.f, kMuted);
            else if (auto* mark =
                         mapvotes::icon(row.vote > 0 ? "GJ_likesIcon_001.png" : "GJ_dislikesIcon_001.png", 13.f)) {
                mark->setPosition({14.f, mid});
                cell->addChild(mark);
            }
            label(cell, row.name.empty() ? fmt::format("Level {}", row.levelId) : row.name, {28.f, mid}, .32f, 150.f,
                  ccWHITE, true, "bigFont.fnt");
            label(cell, fmt::format("ID: {}", row.levelId), {186.f, mid + 5.f}, .36f, 70.f, kMuted, true,
                  "chatFont.fnt");
            if (row.updatedAt > 0)
                label(cell, agoText(std::max<int64_t>(0, now - row.updatedAt)), {186.f, mid - 6.f}, .36f, 90.f, kMuted,
                      true, "chatFont.fnt");
            if (m_mine) {
                auto place = [&](char const* frame, float x, int value) {
                    auto* mark = mapvotes::icon(frame, kMarkSize);
                    if (!mark) return;
                    mark->setPosition({x, mid});
                    mark->setScale(mark->getScale() * (row.vote == value ? 1.12f : 1.f));
                    mark->setOpacity(row.vote == -value && row.vote != 0 ? dim : 255);
                    cell->addChild(mark, 2);
                };
                place("GJ_likeBtn_001.png", kLikeX, 1);
                place("GJ_dislikeBtn_001.png", kDislikeX, -1);
            } else
                label(cell, row.vote > 0 ? "LIKED" : "DISLIKED", {kCellW - 8.f, mid}, .26f, 70.f,
                      row.vote > 0 ? ccc3(150, 255, 170) : ccc3(255, 150, 170), false, "bigFont.fnt")
                    ->setAnchorPoint({1.f, .5f});
        }
        if (!m_filled || height <= kListH)
            m_scroll->scrollToTop();
        else
            content->setPositionY(std::clamp(keepY, kListH - height, 0.f));
        m_filled = true;
        summarize();
    }
    // 이 플레이어의 평가 목록을 서버에서 받아 온다.
    void load() {
        Service::get().fetchPlayerMapVotes(
            m_profile.uid, [self = Weak<MapRatingsPopup>(this)](std::vector<MapVoteRecord> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                if (!detail.empty()) {
                    owner->m_status->setString("The ratings could not be loaded.");
                    return;
                }
                owner->m_rows = std::move(rows);
                owner->fill();
            });
    }
    // 내 프로필인지(m_mine) 판단하고 목록 영역과 안내 문구를 만든 뒤 로딩을 시작한다.
    bool init(PlayerProfile profile) {
        if (!Popup::init(kW, kH, "GJ_square02.png")) return false;
        m_profile = std::move(profile);
        m_mine = !m_profile.uid.empty() && m_profile.uid == Service::get().profile().uid;
        setTitle("MAP RATINGS", "goldFont.fnt", .75f);
        setID("versus-map-ratings"_spr);
        m_summary = label(m_mainLayer, "", {kW / 2.f, kH - 42.f}, .3f, kW - 40.f, kIce, false, "bigFont.fnt");
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({kListW, kListH + 4.f});
        paper->setPosition({kW / 2.f, 30.f + kListH / 2.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(200);
        m_mainLayer->addChild(paper);
        m_scroll =
            TapScrollLayer::createAt({kListW, kListH}, [self = Weak<MapRatingsPopup>(this)](CCNode* row, CCPoint at) {
                if (auto owner = self.lock(); owner && row) owner->onTap(row->getTag(), at);
            });
        m_scroll->setPosition({12.f, 30.f});
        m_mainLayer->addChild(m_scroll);
        m_status = label(m_mainLayer, "Loading...", {kW / 2.f, 30.f + kListH / 2.f}, .34f, kListW - 30.f, kIce, false,
                         "bigFont.fnt");
        m_status->setZOrder(3);
        label(m_mainLayer,
              m_mine ? "Tap a map's name to open it  |  tap a mark again to withdraw the rating"
                     : "Tap a map's name to open it",
              {kW / 2.f, 16.f}, .3f, kW - 30.f, kMuted, false, "chatFont.fnt");
        summarize();
        load();
        return true;
    }

public:
    static MapRatingsPopup* create(PlayerProfile profile) {
        auto* popup = new MapRatingsPopup;
        if (popup->init(std::move(profile))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};
