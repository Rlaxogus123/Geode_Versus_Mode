// 방의 Map Pool 목록 위젯(MapPoolList)과 그 도우미(mappool 네임스페이스).
// 풀에 담긴 맵을 한 줄씩 보여 주고, 맵 추가/제거, 순서(Random) 전환, Map Mode / 규칙 창 열기 버튼을 둔다.
// 방 상태가 바뀔 때만 다시 그린다.
//
// 방 참가자 목록. 그룹 방이면 그룹 멤버(방장이 맨 앞), 1v1 방이면 host와 guest.
namespace mappool {
struct Player {
    std::string uid, name;
    bool host = false;
};
inline std::vector<Player> players(RoomInfo const& room) {
    std::vector<Player> result;
    if (auto const& group = Service::get().group(); group && group->id == room.id) {
        for (auto const& member : group->members)
            result.push_back({member.profile.uid, member.profile.name, member.profile.uid == group->hostUid});
        std::stable_partition(result.begin(), result.end(), [](Player const& player) { return player.host; });
        return result;
    }
    result.push_back({room.host.uid, room.host.name, true});
    if (room.guest) result.push_back({room.guest->uid, room.guest->name, false});
    return result;
}
// 화면을 다시 그려야 하는지 비교하기 위한 문자열. 방/맵/풀 설정/허용 목록/맵 목록/참가자를 이어 붙인다.
inline std::string poolSignature(RoomInfo const& room) {
    std::string text = room.id + "|" + std::to_string(room.level.id) + "|" + (room.started ? "s" : "-") + "|" +
                       (Service::get().isHost() ? "h" : "-");
    if (!room.mapPool) return text + "|none";
    auto const& pool = *room.mapPool;
    auto const& s = pool.settings;
    text += fmt::format("|{}:{}:{}:{}:{}:{}:{}:{}", s.perPlayer, s.rated, s.mask, s.twoPlayer, s.random, s.lengths,
                        s.minLikes, s.filter);
    for (auto const& uid : pool.permit) text += "|+" + uid;
    for (auto const& entry : pool.maps) text += "|" + std::to_string(entry.level.id) + ":" + entry.by;
    for (auto const& player : players(room)) text += "|@" + player.uid + ":" + player.name;
    return text;
}
// 참가자 허용 여부를 나타내는 아이콘(허용이 아니면 흐리고 회색).
inline CCSprite* permitSprite(bool allowed, float size = 20.f) {
    auto* box = CCSprite::create();
    box->setContentSize({size, size});
    if (auto* icon = loadedFrameSprite("GJ_viewLevelsBtn_001.png")) {
        icon->setScale((size - 1.f) / std::max(1.f, icon->getContentSize().height));
        icon->setPosition({size / 2.f, size / 2.f});
        icon->setOpacity(allowed ? 255 : 90);
        if (!allowed) icon->setColor(ccc3(120, 120, 120));
        box->addChild(icon);
    }
    return box;
}
// Map Pool에 맵을 추가한다. 레벨 검색 창을 열고, 고른 맵을 서버에 보내 결과를 알린다.
// 방에 풀이 없거나 이미 시작된 뒤에는 동작하지 않는다.
inline void addMap() {
    auto const& room = Service::get().room();
    if (!room || !room->mapPool || room->started) return;
    auto const settings = room->mapPool->settings;
    openPoolSearch(settings, [](LevelInfo level, bool twoPlayer) {
        auto const name = level.name;
        Service::get().addToMapPool(std::move(level), twoPlayer, [name](bool ok, std::string detail) {
            if (!ok) {
                error(detail);
                return;
            }
            Notification::create(fmt::format("Added to the Map Pool: {}", name), NotificationIcon::Success, 2.f)
                ->show();
        });
    });
}
}

// Map Pool 목록 위젯. 위쪽은 맵 목록(스크롤), 아래쪽은 버튼 막대.
// - Add Map: 허용된 참가자 / Map Mode, Random: 방장 / 톱니: 규칙 창 콜백이 있을 때
// - 각 줄 오른쪽 끝의 삭제 아이콘: 방장이거나 자기가 추가한 맵(게임 시작 전)만
// 요청 중(m_pending)에는 입력을 막는다.
class MapPoolList final : public CCNode {
    static constexpr float kBarH = 24.f, kPitch = 25.f, kPlateH = 23.f;
    CCSize m_size;
    std::string m_signature;
    bool m_pending = false;
    CCNode* m_dynamic = nullptr;
    TapScrollLayer* m_scroll = nullptr;
    float m_check = 0.f;
    // 표시 중인 각 줄의 레벨 ID와 삭제 가능 여부(탭 처리용).
    std::vector<int64_t> m_rowIds;
    std::vector<bool> m_rowRemovable;
    std::function<void()> m_onMapMode, m_onRules;
    bool narrow() const { return m_size.width < 170.f; }
    float viewWidth() const { return m_size.width - 8.f; }
    float viewHeight() const { return std::max(kPitch, m_size.height - kBarH - 8.f); }
    // 다시 그릴지 판단하는 시그니처. 요청 중 / 통신 중 상태도 포함한다.
    std::string signature(RoomInfo const& room) const {
        return mappool::poolSignature(room) + (m_pending ? "|p" : "|-") + (Service::get().busy() ? "|b" : "|-");
    }
    // 맵 목록을 다시 그린다. 순서 모드이면 번호(다음 맵은 NEXT)를, Random이면 번호 없이 그린다.
    // 스크롤 위치는 가능하면 유지한다.
    void rows(RoomInfo const& room) {
        auto* content = m_scroll->m_contentLayer;
        float const scrollFromTop = content->getContentSize().height + content->getPositionY();
        bool const keep = content->getContentSize().height > 0.f && !m_rowIds.empty();
        content->removeAllChildrenWithCleanup(true);
        m_rowIds.clear();
        m_rowRemovable.clear();
        auto const& pool = *room.mapPool;
        auto const me = Service::get().profile().uid;
        bool const host = Service::get().isHost();
        float const w = viewWidth(), view = viewHeight();
        float const listH = std::max(view, kPitch * pool.maps.size() + 2.f);
        content->setContentSize({w, listH});
        bool const ordered = !pool.settings.random;
        float const numberX = narrow() ? 9.f : 13.f, shift = ordered ? 0.f : narrow() ? 15.f : 20.f;
        float const faceX = (narrow() ? 27.f : 35.f) - shift, textX = (narrow() ? 39.f : 49.f) - shift;
        for (size_t index = 0; index < pool.maps.size(); ++index) {
            auto const& entry = pool.maps[index];
            bool const next = entry.level.id == room.level.id;
            bool const removable = (host || entry.by == me) && !room.started;
            auto* row = CCSprite::create();
            row->setContentSize({w, kPlateH});
            auto* plate = NineSlice::create("square02b_001.png");
            plate->setContentSize({w, kPlateH});
            plate->setPosition({w / 2.f, kPlateH / 2.f});
            plate->setColor(next ? ccc3(255, 150, 50) : ccc3(10, 40, 90));
            plate->setOpacity(next ? 75 : 160);
            row->addChild(plate, -1);
            float const y = kPlateH / 2.f;
            if (ordered)
                label(row, next ? "NEXT" : std::to_string(index + 1), {numberX, y}, next ? .24f : .34f,
                      2.f * numberX - 4.f, next ? ccc3(255, 200, 90) : kIce);
            if (auto* face = mapDifficulty(entry.level)) {
                auto const size = face->getContentSize();
                face->setScale(std::min(.4f, 21.f / std::max(1.f, size.height)));
                face->setPosition({faceX, y});
                row->addChild(face);
            }
            float const right = w - (removable ? 24.f : 6.f);
            label(row, entry.level.name, {textX, y + 5.f}, .3f, right - textX, ccWHITE, true);
            auto* stars = starRating(row, std::to_string(entry.level.stars), {}, .26f, 26.f);
            float const starsW = stars->getContentSize().width * stars->getScale();
            stars->setPosition({right - starsW / 2.f, y - 6.f});
            label(row, fmt::format("added by {}{}", entry.name, entry.twoPlayer ? "  |  2-Player" : ""), {textX, y - 6.f},
                  .36f, std::max(10.f, right - starsW - 4.f - textX), kIce, true, "chatFont.fnt");
            if (removable) {
                auto* remove = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png");
                remove->setScale(17.f / std::max(1.f, remove->getContentSize().height));
                remove->setPosition({w - 12.f, y});
                if (m_pending) remove->setOpacity(110);
                row->addChild(remove);
            }
            row->setAnchorPoint({.5f, .5f});
            row->setPosition({w / 2.f, listH - 1.f - kPitch / 2.f - index * kPitch});
            row->setTag(static_cast<int>(index));
            content->addChild(row);
            m_rowIds.push_back(entry.level.id);
            m_rowRemovable.push_back(removable);
        }
        if (keep)
            content->setPositionY(std::clamp(scrollFromTop - listH, view - listH, 0.f));
        else
            m_scroll->scrollToTop();
    }
    // 하단 버튼 막대와 빈 목록 안내를 만든다. 버튼이 공간보다 넓으면 같은 비율로 줄여 나란히 배치한다.
    void bar(RoomInfo const& room) {
        m_dynamic->removeAllChildrenWithCleanup(true);
        auto* buttons = menu(m_dynamic);
        auto const& pool = *room.mapPool;
        auto const me = Service::get().profile().uid;
        bool const host = Service::get().isHost();
        float const w = m_size.width, h = m_size.height;
        bool const permitted = pool.permitted(me, room.host.uid) || host;
        if (pool.maps.empty()) {
            float const middle = kBarH + 4.f + viewHeight() / 2.f;
            label(m_dynamic, "Map Pool is empty", {w / 2.f, middle + 8.f}, .42f, w - 16.f);
            label(m_dynamic, permitted ? "Add Map puts a map in it" : "The host lets players add maps",
                  {w / 2.f, middle - 9.f}, .6f, w - 16.f, kMuted, false, "chatFont.fnt");
        }
        (void)h;
        float const scale = narrow() ? .3f : .36f, barY = kBarH / 2.f + 3.f, gap = 6.f;
        int const mine = pool.addedBy(me);
        CCMenuItemSpriteExtra* add = nullptr;
        CCMenuItemSpriteExtra* mode = nullptr;
        CCMenuItemSpriteExtra* order = nullptr;
        CCLabelBMFont* yours = nullptr;
        if (permitted) {
            add = mapmode::bigButton(buttons, this, menu_selector(MapPoolList::onAdd), "Add Map", {}, scale,
                                     "GJ_button_01.png");
            add->setID("map-pool-add"_spr);
            enabled(add,
                    !m_pending && !room.started && mine < pool.settings.perPlayer && pool.maps.size() < kPoolMapsMax);
        }
        if (host) {
            mode = mapmode::bigButton(buttons, this, menu_selector(MapPoolList::onMapMode), "Map Mode", {}, scale);
            mode->setID("map-pool-map-mode"_spr);
            enabled(mode, !m_pending && !Service::get().busy() && !room.started);
            order = mapmode::bigButton(buttons, this, menu_selector(MapPoolList::onOrder), "Random", {}, scale,
                                       pool.settings.random ? "GJ_button_01.png" : "GJ_button_04.png");
            order->setID("map-pool-order"_spr);
            enabled(order, !m_pending && !Service::get().busy() && !room.started && !drawingMap(room));
        } else if (permitted) {
            yours = label(m_dynamic, fmt::format("Yours {}/{}", mine, pool.settings.perPlayer), {}, .26f, 56.f, kIce);
        }
        CCMenuItemSpriteExtra* gear = nullptr;
        if (m_onRules) {
            gear = iconButton(buttons, this, menu_selector(MapPoolList::onRules), "GJ_optionsBtn_001.png", {}, kBarH);
            gear->setID("map-pool-rules"_spr);
            gear->setPosition({w - 6.f - gear->getContentSize().width / 2.f, barY});
        }
        auto widthOf = [](CCNode* node) {
            return typeinfo_cast<CCLabelBMFont*>(node) ? node->getScaledContentSize().width
                                                       : node->getContentSize().width;
        };
        std::vector<CCNode*> placed;
        for (CCNode* node : {static_cast<CCNode*>(add), static_cast<CCNode*>(mode), static_cast<CCNode*>(order),
                             static_cast<CCNode*>(yours)})
            if (node) placed.push_back(node);
        if (placed.empty()) return;
        float const space = w - 12.f - (gear ? gear->getContentSize().width + gap : 0.f);
        float total = gap * static_cast<float>(placed.size() - 1);
        for (auto* node : placed) total += widthOf(node);
        float const fit = std::clamp(space / std::max(1.f, total), .1f, 1.f);
        float left = 6.f;
        for (auto* node : placed) {
            bool const text = typeinfo_cast<CCLabelBMFont*>(node) != nullptr;
            if (fit < 1.f) {
                node->setScale(node->getScale() * fit);
                if (auto* item = typeinfo_cast<CCMenuItemSpriteExtra*>(node)) item->m_baseScale = fit;
            }
            float const nodeW = widthOf(node) * (text ? 1.f : fit);
            node->setPosition({left + nodeW / 2.f, barY});
            left += nodeW + gap * fit;
        }
    }
    // 방 상태로 목록과 막대를 다시 만든다. 룰렛이 도는 중에는 결과가 미리 보이지 않게 숨긴다.
    void rebuild() {
        auto const& current = Service::get().room();
        if (!current || !current->mapPool) return;
        auto room = *current;
        hideSpinningDraw(room, Service::get().serverNow());
        m_signature = signature(room);
        rows(room);
        bar(room);
    }
    void later() { m_signature.clear(); }
    void onAdd(CCObject*) {
        if (!m_pending) mappool::addMap();
    }
    // 방장이 풀의 순서 방식(순서대로 / Random)을 바꾼다. 맵 추첨 중에는 막는다.
    void onOrder(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || !room || !room->mapPool || !Service::get().isHost() || room->started || drawingMap(*room))
            return;
        auto settings = room->mapPool->settings;
        settings.random = !settings.random;
        m_pending = true;
        later();
        Service::get().configureMapPool(settings, [self = Weak<MapPoolList>(this)](bool ok, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_pending = false;
                owner->later();
            }
            if (!ok) error(detail);
        });
    }
    void onMapMode(CCObject*) {
        if (!m_pending && m_onMapMode) {
            auto call = m_onMapMode;
            call();
        }
    }
    void onRules(CCObject*) {
        if (m_onRules) {
            auto call = m_onRules;
            call();
        }
    }
    // 목록 줄의 삭제 아이콘 영역을 눌렀을 때 그 맵을 풀에서 제거하도록 서버에 요청한다.
    void onRow(CCNode* row, CCPoint at) {
        auto const index = static_cast<size_t>(row ? row->getTag() : -1);
        if (m_pending || index >= m_rowIds.size() || !m_rowRemovable[index]) return;
        if (at.x < viewWidth() - 26.f) return;
        m_pending = true;
        later();
        Service::get().removeFromMapPool(m_rowIds[index],
                                         [self = Weak<MapPoolList>(this)](bool ok, std::string detail) {
                                             if (auto owner = self.lock()) {
                                                 owner->m_pending = false;
                                                 owner->later();
                                             }
                                             if (!ok) error(detail);
                                         });
    }
    bool init(CCSize size, std::function<void()> onMapMode, std::function<void()> onRules) {
        if (!CCNode::init()) return false;
        m_size = size;
        m_onMapMode = std::move(onMapMode);
        m_onRules = std::move(onRules);
        setContentSize(size);
        setID("map-pool-list"_spr);
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize(size);
        back->setPosition(size / 2.f);
        back->setColor(ccc3(0, 0, 0));
        back->setOpacity(110);
        addChild(back, -1);
        m_scroll = TapScrollLayer::createAt({viewWidth(), viewHeight()},
                                            [self = Weak<MapPoolList>(this)](CCNode* row, CCPoint at) {
                                                if (auto owner = self.lock()) owner->onRow(row, at);
                                            });
        m_scroll->setPosition({4.f, kBarH + 4.f});
        addChild(m_scroll);
        m_dynamic = CCNode::create();
        addChild(m_dynamic, 2);
        rebuild();
        scheduleUpdate();
        return true;
    }

public:
    static MapPoolList* create(CCSize size, std::function<void()> onMapMode, std::function<void()> onRules) {
        auto* node = new MapPoolList;
        if (node->init(size, std::move(onMapMode), std::move(onRules))) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    // 0.25초마다 방 상태가 바뀌었는지 확인해 바뀌었으면 다시 그린다. 룰렛이 도는 중에는 건너뛴다.
    void update(float dt) override {
        m_check -= dt;
        if (m_check > 0.f) return;
        m_check = .25f;
        auto const& room = Service::get().room();
        if (room && room->mapPool && !rouletteSpinning(room->mapDraw, Service::get().serverNow()) &&
            signature(*room) != m_signature)
            rebuild();
    }
};
