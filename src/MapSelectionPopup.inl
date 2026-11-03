// 방장이 방의 맵 방식을 정하는 Map Mode 팝업(MapSelectionPopup)과, 무작위 추첨 연출
// 팝업(MapRoulettePopup). 방식은 Map Pool(풀에서 추첨) / Random Map(필터에 맞는 맵 추첨)이며,
// 난이도, 별, 길이, Rated, 2-Player, 최소 좋아요 필터를 설정한다.
//
// mapmode: 팝업 레이아웃 상수와 버튼 도우미. kBox/kPanel 등은 영역의 y 좌표.
namespace mapmode {
// 높이 286: 310이면 왼쪽 위 닫기 버튼이 화면(높이 320) 위로 나간다. 위쪽 줄들을 16 내리고 아래 필터 상자를 줄였다.
constexpr float kWidth = 480.f, kHeight = 286.f;
constexpr float kTabsY = 242.f, kRulesY = 211.f, kRowH = 26.f;
constexpr float kPanelX = 14.f, kPanelW = kWidth - 28.f, kPanelBottom = 102.f, kPanelTop = 190.f;
constexpr float kFaceY = 167.f, kStarY = 134.f, kStarY2 = 115.f;
constexpr float kBoxBottom = 40.f, kBoxTop = 96.f, kLineY = 82.f, kLineY2 = 54.f, kApplyY = 24.f;
constexpr float kLeftBoxW = 286.f, kBoxGap = 8.f;
constexpr ccColor3B kOnPlate{38, 118, 205}, kOffPlate{10, 26, 62};
constexpr char const* kLengthNames[] = {"Tiny", "Short", "Medium", "Long", "XL"};
// 칩 모양 토글 버튼(아이콘 + 글자). paint로 켜짐/꺼짐 색을 정하고, say로 글자를 폭에 맞춘다.
struct Chip {
    CCMenuItemSpriteExtra* item = nullptr;
    NineSlice* plate = nullptr;
    CCNodeRGBA* content = nullptr;
    CCLabelBMFont* text = nullptr;
    CCSprite* icon = nullptr;
    float width = 0.f, height = 0.f, iconW = 0.f;
    void layout() {
        float const textW = text ? text->getScaledContentSize().width : 0.f;
        float const total = iconW + textW;
        if (icon) icon->setPosition({width / 2.f - total / 2.f + (iconW - 3.f) / 2.f, height / 2.f});
        if (text) text->setPosition({width / 2.f - total / 2.f + iconW + textW / 2.f, height / 2.f + .5f});
    }
    void paint(bool on) {
        if (!item) return;
        plate->setColor(on ? kOnPlate : kOffPlate);
        plate->setOpacity(on ? 240 : 205);
        content->setColor(ccWHITE);
        if (text) {
            text->setColor(on ? ccWHITE : ccc3(105, 115, 135));
            text->updateDisplayedColor(ccWHITE);
        }
        if (icon) {
            icon->setColor(on ? ccWHITE : ccc3(95, 95, 95));
            icon->updateDisplayedColor(ccWHITE);
        }
    }
    void say(std::string const& value, float scale, float room) {
        if (!text) return;
        if (value != text->getString()) text->setString(value.c_str());
        text->setScale(scale);
        if (text->getContentSize().width * scale > room)
            text->setScale(room / std::max(1.f, text->getContentSize().width));
        layout();
    }
};
// 칩 버튼을 만들어 menu에 붙이고 Chip을 돌려준다.
inline Chip chip(CCMenu* menu, CCObject* target, SEL_MenuHandler action, std::string const& caption,
                 char const* iconFrame, float width, float height, CCPoint at, float textScale) {
    Chip result;
    result.width = width;
    result.height = height;
    auto* node = CCNodeRGBA::create();
    node->setContentSize({width, height});
    node->setCascadeOpacityEnabled(true);
    result.plate = NineSlice::create("square02b_001.png");
    result.plate->setContentSize({width, height});
    result.plate->setPosition({width / 2.f, height / 2.f});
    node->addChild(result.plate);
    result.content = CCNodeRGBA::create();
    result.content->setCascadeColorEnabled(true);
    result.content->setCascadeOpacityEnabled(true);
    node->addChild(result.content, 1);
    if (iconFrame) {
        if ((result.icon = loadedFrameSprite(iconFrame))) {
            result.icon->setScale((height - 6.f) / std::max(1.f, result.icon->getContentSize().height));
            result.iconW = result.icon->getScaledContentSize().width + 3.f;
            result.content->addChild(result.icon);
        }
    }
    result.text = createCosmeticLabel(caption.c_str(), "bigFont.fnt");
    result.content->addChild(result.text);
    result.say(caption, textScale, width - 8.f - result.iconW);
    result.item = CCMenuItemSpriteExtra::create(node, target, action);
    result.item->m_scaleMultiplier = 1.06f;
    result.item->setPosition(at);
    menu->addChild(result.item);
    return result;
}
// 큰 버튼(ButtonSprite)을 만든다. width가 0이면 글자 길이에 맞춘다.
inline CCMenuItemSpriteExtra* bigButton(CCMenu* menu, CCObject* target, SEL_MenuHandler action,
                                        std::string const& caption, CCPoint at, float scale,
                                        char const* frame = "GJ_button_04.png", int width = 0) {
    auto* sprite = width > 0 ? ButtonSprite::create(caption.c_str(), width, true, "bigFont.fnt", frame, 30.f, .6f)
                             : ButtonSprite::create(caption.c_str(), "bigFont.fnt", frame);
    sprite->setScale(scale);
    auto* item = CCMenuItemSpriteExtra::create(sprite, target, action);
    item->setPosition(at);
    menu->addChild(item);
    return item;
}
// 기존 버튼의 모양과 문구를 바꾼다.
inline void caption(CCMenuItemSpriteExtra* item, std::string const& text, char const* frame = "GJ_button_04.png",
                    float scale = .5f, int width = 0) {
    auto* sprite = width > 0 ? ButtonSprite::create(text.c_str(), width, true, "bigFont.fnt", frame, 30.f, .6f)
                             : ButtonSprite::create(text.c_str(), "bigFont.fnt", frame);
    sprite->setScale(scale);
    item->setNormalImage(sprite);
    item->updateSprite();
}
inline char const* twoPlayerCaption(int value) {
    return value == 1 ? "No 2-Player" : value == 2 ? "2-Player only" : "2-Player: Any";
}
}

// Map Mode 팝업. 위쪽 두 탭(Map Pool / Random Map)에 따라 필터 영역이 다른 값을 편집한다:
// Random 탭은 m_selection(방의 맵 선택 규칙), Map Pool 탭은 m_pool(풀 설정).
// Apply로 서버에 보낸다.
class MapSelectionPopup final : public ::versus::DeferredPopup {
    MapSelection m_selection;
    std::string m_roomID;
    bool m_pending = false;
    CCMenuItemSpriteExtra* m_manual = nullptr;
    CCMenuItemSpriteExtra* m_random = nullptr;
    CCMenuItemSpriteExtra* m_apply = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_difficulties;
    std::vector<CCSprite*> m_checks;
    // 별 칩 하나: 난이도 카테고리 번호와 별 비트.
    struct StarChoice {
        int category, bit;
        mapmode::Chip chip;
    };
    std::vector<StarChoice> m_stars;
    std::vector<mapmode::Chip> m_lengths;
    mapmode::Chip m_rated, m_two;
    NineSlice* m_likesPlate = nullptr;
    CCSprite* m_likesIcon = nullptr;
    CCLabelBMFont* m_likesText = nullptr;
    TextInput* m_likesInput = nullptr;
    CCNode* m_filterArea = nullptr;
    CCNode* m_shade = nullptr;
    CCLabelBMFont* m_shadeText = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    CCSprite* m_clock = nullptr;
    bool m_poolMode = false;
    MapPoolSettings m_pool;
    CCNode* m_poolRow = nullptr;
    CCLabelBMFont* m_perPlayer = nullptr;
    CCMenuItemSpriteExtra *m_fewer = nullptr, *m_more = nullptr, *m_filterBox = nullptr;
    CCSprite* m_filterCheck = nullptr;
    // Pool 탭이 열려 있는지(Random이 아닐 때), 필터를 보여 줄지, 필터가 실제로 적용 중인지.
    // 아래 접근자들은 지금 탭에 맞는 설정 값(마스크, 길이, 2-Player, 최소 좋아요, Rated)을 가리킨다.
    bool poolTab() const { return m_poolMode && !m_selection.random; }
    bool filtersShown() const { return m_selection.random || poolTab(); }
    bool filtersActive() const { return m_selection.random || (poolTab() && m_pool.filter); }
    int& mask() { return poolTab() ? m_pool.mask : m_selection.mask; }
    int& lengths() { return poolTab() ? m_pool.lengths : m_selection.lengths; }
    int& twoPlayer() { return poolTab() ? m_pool.twoPlayer : m_selection.twoPlayer; }
    int& minLikes() { return poolTab() ? m_pool.minLikes : m_selection.minLikes; }
    bool rated() const { return poolTab() ? m_pool.rated : m_selection.rated; }
    void setRated(bool value) { (poolTab() ? m_pool.rated : m_selection.rated) = value; }
    // 현재 설정에 맞춰 모든 위젯의 선택 상태, 표시 여부, 활성 여부, 문구를 갱신한다.
    // Pool 탭에서 Filtering이 꺼져 있으면 필터 영역을 어둡게 덮는다.
    void refresh() {
        auto tint = [](CCMenuItemSpriteExtra* item, bool selected) {
            auto* sprite = static_cast<ButtonSprite*>(item->getNormalImage());
            sprite->m_BGSprite->setColor(selected ? ccc3(100, 255, 125) : ccc3(155, 155, 155));
        };
        tint(m_manual, !m_selection.random);
        tint(m_random, m_selection.random);
        bool const shown = filtersShown(), active = filtersActive() && !m_pending;
        m_filterArea->setVisible(shown);
        int const shownMask = mask();
        for (int i = 0; i < 10; ++i) {
            bool const selected = (shownMask & CATEGORY_MASKS[i]) != 0;
            auto* face = static_cast<CCSprite*>(m_difficulties[i]->getNormalImage());
            face->setColor(selected ? ccWHITE : ccc3(110, 110, 110));
            m_checks[i]->setVisible(selected);
            m_difficulties[i]->setVisible(shown);
            m_difficulties[i]->setEnabled(shown && active);
        }
        for (auto& choice : m_stars) {
            bool const on = (shownMask & choice.bit) != 0;
            choice.chip.paint(on);
            choice.chip.item->setVisible(shown);
            choice.chip.item->setEnabled(shown && active);
        }
        for (int length = 0; length <= 4; ++length) {
            m_lengths[length].paint((lengths() & (1 << length)) != 0);
            m_lengths[length].item->setVisible(shown);
            m_lengths[length].item->setEnabled(shown && active);
        }
        m_rated.paint(rated());
        m_rated.say(rated() ? "Rated maps only" : "Any rating", .36f, m_rated.width - 8.f - m_rated.iconW);
        m_two.paint(twoPlayer() != 0);
        m_two.say(mapmode::twoPlayerCaption(twoPlayer()), .36f, m_two.width - 8.f);
        paintLikes();
        if (m_likesInput) {
            if (typedLikes() != minLikes())
                m_likesInput->setString(minLikes() > 0 ? std::to_string(minLikes()).c_str() : "");
            m_likesInput->setEnabled(shown && active);
            m_likesInput->setVisible(shown);
        }
        for (auto* chip : {&m_rated, &m_two}) {
            chip->item->setVisible(shown);
            chip->item->setEnabled(shown && active);
        }
        bool const dimmed = poolTab() && !m_pool.filter;
        m_shade->setVisible(dimmed);
        m_shadeText->setVisible(dimmed);
        if (m_poolRow) {
            m_poolRow->setVisible(poolTab());
            m_perPlayer->setString(std::to_string(m_pool.perPlayer).c_str());
            for (auto* item : {m_fewer, m_more, m_filterBox}) item->setVisible(poolTab());
            enabled(m_fewer, poolTab() && !m_pending && m_pool.perPlayer > 1);
            enabled(m_more, poolTab() && !m_pending && m_pool.perPlayer < kPoolPerPlayerMax);
            enabled(m_filterBox, poolTab() && !m_pending);
            m_filterCheck->setDisplayFrame(CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(
                m_pool.filter ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png"));
        }
        m_hint->setVisible(!poolTab());
        m_hint->setString(m_selection.random ? "Every round draws one map that fits these filters."
                                             : "Pick the map yourself: Search Map opens GD's search with its filters.");
        m_hint->setPositionY(m_selection.random ? mapmode::kRulesY
                                                : (mapmode::kPanelBottom + mapmode::kBoxBottom) / 2.f + 40.f);
        mapmode::caption(m_apply, m_selection.random || m_poolMode ? "Apply" : "Search Map ..", "GJ_button_01.png",
                         .62f, 150);
        enabled(m_apply, !m_pending && (poolTab() ? validPoolSettings(m_pool) : validSelection(m_selection)));
        enabled(m_manual, !m_pending);
        enabled(m_random, !m_pending);
    }
    void plate(CCNode* parent, float x, float y, float w, float h, GLubyte opacity) {
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({w, h});
        back->setPosition({x + w / 2.f, y + h / 2.f});
        back->setColor(ccc3(0, 0, 0));
        back->setOpacity(opacity);
        parent->addChild(back, -1);
    }
    // 팝업을 만들고 모든 위젯을 배치한다: 탭, 난이도 얼굴 10개와 별 칩, 길이 칩, Rated, 2-Player,
    // 최소 좋아요 입력, Filtering 덮개, Pool 행(Maps each, Filtering 체크), Apply.
    bool setup(RoomInfo const& room) {
        using namespace mapmode;
        m_poolMode = true;
        if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;
        m_selection = room.mapSelection;
        m_roomID = room.id;
        m_selection.platformer = false;
        if (room.mapPool) m_pool = room.mapPool->settings;
        setTitle("Map Mode", "bigFont.fnt", .65f);
        m_manual = bigButton(m_buttonMenu, this, menu_selector(MapSelectionPopup::onManual),
                             m_poolMode ? "Map Pool" : "Map Select", {132.f, kTabsY}, .6f, "GJ_button_01.png");
        m_random = bigButton(m_buttonMenu, this, menu_selector(MapSelectionPopup::onRandom), "Random Map",
                             {348.f, kTabsY}, .6f, "GJ_button_01.png");
        m_filterArea = CCNode::create();
        m_mainLayer->addChild(m_filterArea);
        auto* filterMenu = CCMenu::create();
        filterMenu->setPosition({0.f, 0.f});
        m_filterArea->addChild(filterMenu, 5);
        plate(m_filterArea, kPanelX, kPanelBottom, kPanelW, kPanelTop - kPanelBottom, 95);
        float const pitch = kPanelW / 10.f;
        for (int i = 0; i < 10; ++i) {
            float const x = kPanelX + pitch * (i + .5f);
            auto* face = GJDifficultySprite::create(CATEGORY_FACES[i], GJDifficultyName::Short);
            auto const size = face->getContentSize();
            face->setScale(
                std::min(.66f, std::min((pitch - 6.f) / std::max(1.f, size.width), 40.f / std::max(1.f, size.height))));
            auto* item = CCMenuItemSpriteExtra::create(face, this, menu_selector(MapSelectionPopup::onDifficulty));
            item->m_scaleMultiplier = 1.1f;
            item->setPosition({x, kFaceY});
            filterMenu->addChild(item);
            item->setTag(i);
            m_difficulties.push_back(item);
            auto* check = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            check->setScale(.42f);
            check->setPosition({size.width - 1.f, size.height - 2.f});
            face->addChild(check);
            m_checks.push_back(check);
            int const bits = CATEGORY_MASKS[i];
            int row = 0;
            for (int bitIndex = 0; bitIndex < 13; ++bitIndex)
                if (bits & (1 << bitIndex)) {
                    auto value = std::to_string(bitIndex < 8 ? bitIndex + 2 : 10);
                    auto star =
                        chip(filterMenu, this, menu_selector(MapSelectionPopup::onStar), value, "star_small01_001.png",
                             std::min(36.f, pitch - 6.f), 16.f, {x, row == 0 ? kStarY : kStarY2}, .34f);
                    star.item->setTag(1 << bitIndex);
                    m_stars.push_back({i, 1 << bitIndex, star});
                    ++row;
                }
        }
        float const rightX = kPanelX + kLeftBoxW + kBoxGap, rightW = kPanelW - kLeftBoxW - kBoxGap;
        plate(m_filterArea, kPanelX, kBoxBottom, kLeftBoxW, kBoxTop - kBoxBottom, 80);
        plate(m_filterArea, rightX, kBoxBottom, rightW, kBoxTop - kBoxBottom, 80);
        {
            constexpr float lengthW = 46.f, gap = 4.f, clockW = 16.f;
            float const total = clockW + 6.f + 5.f * lengthW + 4.f * gap;
            float x = kPanelX + kLeftBoxW / 2.f - total / 2.f;
            if ((m_clock = loadedFrameSprite("GJ_timeIcon_001.png"))) {
                m_clock->setScale(clockW / std::max(1.f, m_clock->getContentSize().height));
                m_clock->setPosition({x + clockW / 2.f, kLineY});
                m_filterArea->addChild(m_clock);
            }
            x += clockW + 6.f;
            for (int length = 0; length <= 4; ++length) {
                auto entry = chip(filterMenu, this, menu_selector(MapSelectionPopup::onLength), kLengthNames[length],
                                  nullptr, lengthW, 20.f, {x + lengthW / 2.f, kLineY}, .34f);
                entry.item->setTag(length);
                m_lengths.push_back(entry);
                x += lengthW + gap;
            }
            m_rated = chip(filterMenu, this, menu_selector(MapSelectionPopup::onRated), "Rated maps only",
                           "GJ_starsIcon_001.png", 150.f, 22.f, {kPanelX + kLeftBoxW / 2.f, kLineY2}, .36f);
        }
        m_two = chip(filterMenu, this, menu_selector(MapSelectionPopup::onTwoPlayer), "2-Player: Any", nullptr,
                     rightW - 16.f, 22.f, {rightX + rightW / 2.f, kLineY}, .36f);
        {
            float const plateW = rightW - 16.f, plateX = rightX + rightW / 2.f, inputW = 66.f;
            m_likesPlate = NineSlice::create("square02b_001.png");
            m_likesPlate->setContentSize({plateW, 22.f});
            m_likesPlate->setPosition({plateX, kLineY2});
            m_filterArea->addChild(m_likesPlate, 1);
            float x = plateX - plateW / 2.f + 6.f;
            if ((m_likesIcon = loadedFrameSprite("GJ_likesIcon_001.png"))) {
                m_likesIcon->setScale(16.f / std::max(1.f, m_likesIcon->getContentSize().height));
                m_likesIcon->setPosition({x + m_likesIcon->getScaledContentSize().width / 2.f, kLineY2});
                m_filterArea->addChild(m_likesIcon, 2);
                x += m_likesIcon->getScaledContentSize().width + 4.f;
            }
            m_likesText = label(m_filterArea, "Likes", {x, kLineY2 + .5f}, .34f, plateW - inputW - 34.f, ccWHITE, true);
            m_likesText->setZOrder(2);
            m_likesInput = TextInput::create(inputW / .6f, "Any");
            m_likesInput->setCommonFilter(CommonFilter::Uint);
            m_likesInput->setMaxCharCount(7);
            m_likesInput->setScale(.6f);
            m_likesInput->setPosition({plateX + plateW / 2.f - 4.f - inputW / 2.f, kLineY2});
            m_likesInput->setID("map-mode-likes-input"_spr);
            styleInput(m_likesInput);
            m_likesInput->setCallback([self = Weak<MapSelectionPopup>(this)](std::string const&) {
                if (auto owner = self.lock()) owner->onLikesTyped();
            });
            m_filterArea->addChild(m_likesInput, 5);
        }
        m_shade = CCNode::create();
        m_filterArea->addChild(m_shade, 20);
        auto* shade = NineSlice::create("square02b_001.png");
        shade->setContentSize({kPanelW, kPanelTop - kBoxBottom});
        shade->setPosition({kPanelX + kPanelW / 2.f, (kPanelTop + kBoxBottom) / 2.f});
        shade->setColor(ccc3(0, 0, 0));
        shade->setOpacity(165);
        m_shade->addChild(shade);
        m_shadeText = label(m_shade, "Filtering off: any Classic map can be added",
                            {kWidth / 2.f, (kPanelTop + kBoxBottom) / 2.f}, .42f, kPanelW - 60.f, ccc3(225, 235, 255));
        m_shadeText->setZOrder(2);
        auto* pill = NineSlice::create("square02b_001.png");
        pill->setContentSize({m_shadeText->getScaledContentSize().width + 28.f, 26.f});
        pill->setPosition(m_shadeText->getPosition());
        pill->setColor(ccc3(0, 0, 0));
        pill->setOpacity(235);
        m_shade->addChild(pill, 1);
        m_hint = label(m_mainLayer, "", {kWidth / 2.f, kRulesY}, .62f, kPanelW - 20.f, kMuted, false, "chatFont.fnt");
        if (m_poolMode) {
            m_poolRow = CCNode::create();
            m_mainLayer->addChild(m_poolRow);
            // Pool 탭 전용 행: 한 사람당 추가 가능한 맵 수(Maps each)와 Filtering 체크박스.
            plate(m_poolRow, kPanelX, kRulesY - kRowH / 2.f, kPanelW, kRowH, 90);
            constexpr float rowLeft = kWidth / 2.f - 249.f / 2.f;
            label(m_poolRow, "Maps each", {rowLeft, kRulesY}, .34f, 62.f, kIce, true);
            m_fewer = bigButton(m_buttonMenu, this, menu_selector(MapSelectionPopup::onFewer), "<",
                                {rowLeft + 76.f, kRulesY}, .42f);
            m_perPlayer = label(m_poolRow, "3", {rowLeft + 100.f, kRulesY}, .45f, 24.f);
            m_more = bigButton(m_buttonMenu, this, menu_selector(MapSelectionPopup::onMore), ">",
                               {rowLeft + 124.f, kRulesY}, .42f);
            auto* box = CCSprite::create();
            box->setContentSize({96.f, 24.f});
            m_filterCheck = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            m_filterCheck->setScale(.55f);
            m_filterCheck->setPosition({12.f, 12.f});
            box->addChild(m_filterCheck);
            auto* filterText = createCosmeticLabel("Filtering", "bigFont.fnt");
            filterText->setScale(.36f);
            filterText->setAnchorPoint({0.f, .5f});
            filterText->setPosition({27.f, 12.5f});
            box->addChild(filterText);
            m_filterBox = CCMenuItemSpriteExtra::create(box, this, menu_selector(MapSelectionPopup::onFiltering));
            m_filterBox->setID("map-pool-filtering"_spr);
            m_filterBox->setPosition({rowLeft + 131.f + 36.f + 48.f, kRulesY});
            m_buttonMenu->addChild(m_filterBox);
        }
        m_apply = bigButton(m_buttonMenu, this, menu_selector(MapSelectionPopup::onApply), "Apply",
                            {kWidth / 2.f, kApplyY}, .62f, "GJ_button_01.png", 150);
        m_apply->setID("map-mode-apply"_spr);
        refresh();
        // 한 프레임 뒤에 한 번 더 갱신해 위젯 배치가 끝난 상태에서 표시를 맞춘다.
        Loader::get()->queueInMainThread([self = Weak<MapSelectionPopup>(this)] {
            if (auto popup = self.lock()) popup->refresh();
        });
        return true;
    }
    void onManual(CCObject*) {
        m_selection.random = false;
        refresh();
    }
    void onRandom(CCObject*) {
        m_selection.random = true;
        refresh();
    }
    // 난이도 카테고리 전체를 켜고 끈다.
    void onDifficulty(CCObject* sender) {
        int bits = CATEGORY_MASKS[sender->getTag()];
        if (mask() & bits)
            mask() &= ~bits;
        else
            mask() |= bits;
        refresh();
    }
    // 별 하나를 켜고 끈다.
    void onStar(CCObject* sender) {
        mask() ^= sender->getTag();
        refresh();
    }
    // 길이를 켜고 끈다. 마지막 하나 남은 길이는 끌 수 없다.
    void onLength(CCObject* sender) {
        int const bit = 1 << sender->getTag();
        if ((lengths() & bit) && lengths() == bit) return;
        lengths() ^= bit;
        refresh();
    }
    void onRated(CCObject*) {
        setRated(!rated());
        refresh();
    }
    void onTwoPlayer(CCObject*) {
        twoPlayer() = (twoPlayer() + 1) % 3;
        refresh();
    }
    // 입력창 숫자를 최소 좋아요 값으로 읽는다. 숫자가 아닌 문자가 있으면 0, 상한(kLikesMax)을 넘으면 상한으로 자른다.
    int typedLikes() const {
        if (!m_likesInput) return 0;
        auto const text = std::string(m_likesInput->getString());
        int64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return 0;
            value = std::min<int64_t>(value * 10 + (c - '0'), kLikesMax);
        }
        return static_cast<int>(value);
    }
    // 입력이 바뀔 때마다 최소 좋아요를 갱신한다. 상한을 넘은 입력은 상한 값으로 되돌린다.
    void onLikesTyped() {
        if (m_pending) return;
        minLikes() = typedLikes();
        if (m_likesInput && m_likesInput->getString().size() >= 7 && minLikes() == kLikesMax &&
            std::string(m_likesInput->getString()) != std::to_string(kLikesMax)) {
            Loader::get()->queueInMainThread([self = Weak<MapSelectionPopup>(this)] {
                if (auto owner = self.lock(); owner && owner->m_likesInput)
                    owner->m_likesInput->setString(std::to_string(kLikesMax));
            });
        }
        paintLikes();
        enabled(m_apply, !m_pending && (poolTab() ? validPoolSettings(m_pool) : validSelection(m_selection)));
    }
    // 최소 좋아요 칩의 켜짐 색과 표시 여부를 갱신한다.
    void paintLikes() {
        if (!m_likesPlate) return;
        bool const on = minLikes() > 0;
        m_likesPlate->setColor(on ? mapmode::kOnPlate : mapmode::kOffPlate);
        m_likesPlate->setOpacity(on ? 240 : 205);
        if (m_likesIcon) m_likesIcon->setColor(on ? ccWHITE : ccc3(95, 95, 95));
        m_likesText->setColor(on ? ccWHITE : ccc3(105, 115, 135));
        bool const shown = filtersShown();
        for (CCNode* node :
             {static_cast<CCNode*>(m_likesPlate), static_cast<CCNode*>(m_likesIcon), static_cast<CCNode*>(m_likesText)})
            if (node) node->setVisible(shown);
    }
    // 닫힐 때 입력창 포커스를 푼다.
    void onExit() override {
        if (m_likesInput) m_likesInput->defocus();
        Popup::onExit();
    }
    void onFewer(CCObject*) {
        m_pool.perPlayer = std::max(1, m_pool.perPlayer - 1);
        refresh();
    }
    void onMore(CCObject*) {
        m_pool.perPlayer = std::min(kPoolPerPlayerMax, m_pool.perPlayer + 1);
        refresh();
    }
    void onFiltering(CCObject*) {
        m_pool.filter = !m_pool.filter;
        if (!m_pool.filter && m_pool.mask == 0) m_pool.mask = kPoolMaskAll;
        refresh();
    }
    // Apply: 현재 탭의 설정을 서버에 보낸다. 풀 탭이면 풀 설정을, 아니면 맵 선택 규칙을 보내고
    // 성공하면 닫는다. Map Select(직접 선택) 방식이면 닫은 뒤 레벨 검색 창을 열어 맵을 고르게 한다.
    void onApply(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || !room || room->id != m_roomID) return;
        m_pending = true;
        refresh();
        if (poolTab()) {
            auto settings = m_pool;
            if (room->mapPool) settings.random = room->mapPool->settings.random;
            Service::get().configureMapPool(settings,
                                            [self = Weak<MapSelectionPopup>(this)](bool ok, std::string detail) {
                                                auto popup = self.lock();
                                                if (!popup) return;
                                                popup->m_pending = false;
                                                popup->refresh();
                                                if (!ok) {
                                                    error(detail);
                                                    return;
                                                }
                                                popup->onClose(nullptr);
                                            });
            return;
        }
        Service::get().configureMapSelection(
            m_selection, [self = Weak<MapSelectionPopup>(this)](bool ok, std::string detail) {
                auto popup = self.lock();
                if (!popup) return;
                popup->m_pending = false;
                popup->refresh();
                if (!ok) {
                    error(detail);
                    return;
                }
                bool manual = !popup->m_selection.random;
                popup->onClose(nullptr);
                if (manual)
                    openLevelSearch([](LevelInfo level) {
                        Service::get().selectLevel(std::move(level), [](bool ok, std::string detail) {
                            if (!ok) error(detail);
                        });
                    });
            });
    }

public:
    // 화면이 팝업 너비보다 좁으면 창을 맞춰 줄여서 띄운다.
    void show() override {
        DeferredPopup::show();
        auto const window = CCDirector::sharedDirector()->getWinSize();
        float const fit = std::min(1.f, (window.width - 12.f) / mapmode::kWidth);
        if (fit >= 1.f) return;
        m_mainLayer->stopAllActions();
        m_mainLayer->setScale(.1f * fit);
        m_mainLayer->runAction(CCEaseElasticOut::create(CCScaleTo::create(.5f, fit), .6f));
    }
    static MapSelectionPopup* create(RoomInfo const& room) {
        auto* popup = new MapSelectionPopup;
        if (popup->setup(room)) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// 무작위 맵 추첨 연출 팝업. 서버가 정한 추첨 정보(m_draw)의 시작 시각을 기준으로
// 카드가 돌아가다가 정해진 맵에서 멈춘다. 모든 참가자가 같은 서버 시간으로 같은 화면을 본다.
class MapRoulettePopup final : public ::versus::DeferredPopup {
    MapDraw m_draw;
    std::string m_roomID;
    std::vector<CCNode*> m_cards;
    CCLabelBMFont* m_caption = nullptr;
    int m_tick = -1;
    float m_soundCooldown = 0.f;
    bool m_revealed = false;
    // 후보 맵 카드를 클리핑 영역 안에 이어 붙여 만든다. 후보가 5개 미만이면 5개 이상이 되도록 반복한다.
    bool setup(RoomInfo const& room) {
        if (!room.mapDraw || !Popup::init(440.f, 225.f, "GJ_square02.png")) return false;
        m_draw = *room.mapDraw;
        m_roomID = room.id;
        setTitle("Random Map", "bigFont.fnt", .65f);
        m_closeBtn->setVisible(false);
        auto* clip = CCClippingNode::create();
        auto* stencil = CCDrawNode::create();
        CCPoint vertices[] = {{0, 0}, {400, 0}, {400, 108}, {0, 108}};
        stencil->drawPolygon(vertices, 4, {1, 1, 1, 1}, 0, {0, 0, 0, 0});
        clip->setStencil(stencil);
        clip->setPosition({20.f, 65.f});
        m_mainLayer->addChild(clip);
        size_t const count = m_draw.levels.size();
        size_t const copies = count == 0 || count >= 5 ? 1 : (5 + count - 1) / count;
        for (size_t i = 0; i < count * copies; ++i) {
            auto const& level = m_draw.levels[i % count];
            auto* card = panel(clip, {}, {134.f, 100.f}, false);
            auto* face = mapDifficulty(level);
            if (face) {
                face->setPosition({67.f, 76.f});
                face->setScale(.65f);
                card->addChild(face);
            }
            label(card, level.name, {67.f, 47.f}, .39f, 124.f);
            label(card, level.creator.empty() ? "Unknown creator" : level.creator, {67.f, 29.f}, .36f, 122.f, ccWHITE,
                  false, "goldFont.fnt");
            starRating(card, std::to_string(level.stars), {level.userCoins > 0 ? 45.f : 67.f, 11.f}, .36f, 60.f);
            mapCoinCount(card, level, {89.f, 11.f});
            m_cards.push_back(card);
        }
        label(m_mainLayer, "v", {220.f, 180.f}, .55f, 30.f, ccc3(255, 220, 100));
        m_caption = label(m_mainLayer,
                          room.mapPool              ? "Rolling the Map Pool..."
                          : room.mapSelection.rated ? "Rolling rated maps..."
                                                    : "Rolling maps...",
                          {220.f, 42.f}, .5f, 400.f, kIce);
        scheduleUpdate();
        update(0);
        return true;
    }
    // 추첨 중에는 직접 닫을 수 없다.
    void onClose(CCObject*) override {}

public:
    // 서버 시간 기준 경과 시간으로 카드 위치를 정한다. 방이 바뀌었거나 추첨 정보가 사라지면 닫는다.
    // 칸이 넘어갈 때 틱 소리를 내고, 회전이 끝나면 당첨 맵 이름을 보여 준다.
    // 서버가 추첨을 마쳤고(settled) 종료 시간이 지나면 닫는다.
    void update(float dt) override {
        auto const& room = Service::get().room();
        if (!room || room->id != m_roomID || !room->mapDraw || room->mapDraw->id != m_draw.id) {
            Popup::onClose(nullptr);
            return;
        }
        int64_t elapsed = Service::get().serverNow() - m_draw.at;
        auto position = roulettePosition(static_cast<int>(m_cards.size()), m_draw.selected, elapsed);
        auto count = static_cast<double>(m_cards.size());
        for (size_t i = 0; i < m_cards.size(); ++i) {
            double offset = std::fmod(double(i) - position, count);
            if (offset < -count / 2.) offset += count;
            if (offset > count / 2.) offset -= count;
            auto* card = m_cards[i];
            card->setPosition({float(200. + offset * 142. - 67.), 4.f});
            card->setVisible(std::abs(offset) < 2.1);
        }
        m_soundCooldown = std::max(0.f, m_soundCooldown - dt);
        int tick = static_cast<int>(position);
        if (m_tick >= 0 && tick != m_tick && m_soundCooldown <= 0.f && !m_revealed) {
            audio::playBuiltin("counter003.ogg");
            m_soundCooldown = .06f;
        }
        m_tick = tick;
        if (elapsed >= DRAW_LEAD_MS + DRAW_SPIN_MS && !m_revealed) {
            m_revealed = true;
            m_caption->setString(m_draw.levels[m_draw.selected].name.c_str());
            m_caption->limitLabelWidth(390.f, .6f, .25f);
            audio::playBuiltin("gold01.ogg");
        }
        if (room->mapDraw->settled && elapsed >= DRAW_END_MS) Popup::onClose(nullptr);
    }
    static MapRoulettePopup* create(RoomInfo const& room) {
        auto* popup = new MapRoulettePopup;
        if (popup->setup(room)) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};
