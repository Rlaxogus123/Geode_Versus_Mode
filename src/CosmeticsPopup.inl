// 꾸미기 팝업 모음: 오라(AuraPopup), 피니셔(FinisherPopup), 닉네임 스타일(NicknamePopup),
// 칭호/업적(TitlesPopup). 모두 CustomizePopup을 상속하고, 선택값은 m_draft에 두었다가
// commit으로 저장한다. 다른 소스 파일에 포함(include)되어 쓰이는 조각이다.
// 창은 설계 크기 그대로 배치하고, 화면이 그보다 좁거나 낮으면 CustomizePopup::show가 창 전체를 줄인다.

// ---- 공통 모양: 탭, 상태 알약 ----
// 탭(오라·피니셔의 All/Standard/Membership, 업적의 Season 1/Season 2/Versus): 둥근 판 위에 이름(+ 달성 수).
// 선택된 탭은 금색 판에 흰 글자, 나머지는 남색 판에 하늘색 글자.
struct CosmeticTab {
    CCMenuItemSpriteExtra* item = nullptr;
    NineSlice* back = nullptr;
    CCLabelBMFont* name = nullptr;
    CCLabelBMFont* count = nullptr;
    float width = 0.f;
};
constexpr float kTabHeight = 18.f;
constexpr ccColor3B kTabFill = {16, 38, 72}, kTabOnFill = {222, 158, 40};

// 이름과 달성 수를 탭 가운데에 나란히 놓는다. 탭 폭을 넘으면 둘 다 같은 비율로 줄인다.
void layoutCosmeticTab(CosmeticTab& tab) {
    constexpr float nameScale = .36f, countScale = .28f, gap = 5.f;
    float const nameW = tab.name->getContentSize().width * nameScale;
    bool const counted = tab.count && tab.count->isVisible();
    float const countW = counted ? tab.count->getContentSize().width * countScale : 0.f;
    float const total = nameW + (counted ? gap + countW : 0.f);
    float const room = std::max(1.f, tab.width - 12.f);
    float const fit = total > room ? room / total : 1.f;
    float const left = tab.width / 2.f - total * fit / 2.f, middle = kTabHeight / 2.f;
    tab.name->setAnchorPoint({0.f, .5f});
    tab.name->setScale(nameScale * fit);
    tab.name->setPosition({left, middle + .5f});
    if (!counted) return;
    tab.count->setAnchorPoint({0.f, .5f});
    tab.count->setScale(countScale * fit);
    tab.count->setPosition({left + (nameW + gap) * fit, middle});
}
// 선택 표시(판 색과 글자 색).
void selectCosmeticTab(CosmeticTab& tab, bool selected) {
    tab.back->setColor(selected ? kTabOnFill : kTabFill);
    tab.back->setOpacity(selected ? 255 : 225);
    tab.name->setColor(selected ? ccWHITE : kIce);
    if (tab.count) tab.count->setColor(selected ? ccc3(255, 242, 200) : kMuted);
}
// 탭 하나를 만들어 menu에 붙인다. center는 탭 가운데, counted면 이름 오른쪽에 달성 수 칸을 둔다(처음엔 비어 있음).
CosmeticTab cosmeticTab(CCMenu* menu, CCObject* target, SEL_MenuHandler action, int tag, std::string const& name,
                        CCPoint center, float width, bool counted) {
    CosmeticTab tab;
    tab.width = width;
    auto* node = CCNode::create();
    node->setContentSize({width, kTabHeight});
    tab.back = NineSlice::create("square02b_001.png");
    tab.back->setContentSize(node->getContentSize());
    tab.back->setPosition(node->getContentSize() / 2.f);
    node->addChild(tab.back, -1);
    tab.name = label(node, name, {width / 2.f, kTabHeight / 2.f}, .36f, 0.f, kIce);
    if (counted) {
        tab.count = label(node, "", {width / 2.f, kTabHeight / 2.f}, .28f, 0.f, kMuted);
        tab.count->setVisible(false);
    }
    tab.item = CCMenuItemSpriteExtra::create(node, target, action);
    tab.item->m_scaleMultiplier = 1.06f;
    tab.item->setTag(tag);
    tab.item->setPosition(center);
    menu->addChild(tab.item);
    layoutCosmeticTab(tab);
    selectCosmeticTab(tab, false);
    return tab;
}
// 탭의 달성 수 글자를 바꾼다(빈 문자열이면 숨김).
void setCosmeticTabCount(CosmeticTab& tab, std::string const& text) {
    if (!tab.count) return;
    tab.count->setString(text.c_str());
    tab.count->setVisible(!text.empty());
    layoutCosmeticTab(tab);
}

// 상태 알약(IN USE 등): 글자 폭에 맞춘 둥근 판.
struct StatePill {
    NineSlice* back = nullptr;
    CCLabelBMFont* text = nullptr;
};
constexpr float kPillHeight = 16.f;
StatePill statePill(CCNode* parent) {
    StatePill pill;
    pill.back = NineSlice::create("square02b_001.png");
    pill.back->setContentSize({40.f, kPillHeight});
    parent->addChild(pill.back);
    pill.text = label(parent, "", {0.f, 0.f}, .3f);
    return pill;
}
// 알약의 글자와 색을 바꾸고 오른쪽 끝을 right에 맞춘다. 알약 폭을 돌려준다.
float setStatePill(StatePill& pill, std::string const& text, ccColor3B fill, ccColor3B color, float right, float y) {
    pill.text->setString(text.c_str());
    pill.text->setScale(.3f);
    float const width = pill.text->getContentSize().width * .3f + 14.f;
    pill.back->setContentSize({width, kPillHeight});
    pill.back->setPosition({right - width / 2.f, y});
    pill.back->setColor(fill);
    pill.text->setColor(color);
    pill.text->setPosition({right - width / 2.f, y + .5f});
    return width;
}

// 꾸미기 팝업의 공통 기반. 편집 중인 값(m_draft)과 저장 후 콜백을 들고 있다.
class CustomizePopup : public ::versus::DeferredPopup {
protected:
    Cosmetics m_draft;
    std::function<void()> m_saved;
    // 닫기 버튼(GJ_closeBtn_001 x0.8, 창 왼쪽 위 모서리에서 (3, -3))이 창 밖으로 약 16 나간다.
    // 화면 가장자리와 창 사이에 이만큼은 남긴다.
    static constexpr float kScreenEdge = 18.f;
    // 팝업을 초기화하고 현재 프로필의 꾸미기 값을 에디션에 맞게 정리해 m_draft로 가져온다.
    bool begin(float width, float height, char const* title, std::function<void()> saved) {
        if (!Popup::init(width, height, "GJ_square02.png")) return false;
        setTitle(title, "bigFont.fnt", .62f);
        m_saved = std::move(saved);
        m_draft = cosmeticsForEdition(Service::get().profile().cosmetics, Service::get().profile().membership);
        return true;
    }
    // 멤버십 전용 항목이라는 안내 창. 미리보기는 계속 할 수 있다.
    static void membershipAlert(char const* what) {
        FLAlertLayer::create(
            "Tipp7 Membership",
            fmt::format("{} require <cg>Tipp7 Membership</c>. You can still preview them here.", what).c_str(), "OK")
            ->show();
    }
    // 값을 저장한다. 저장에 실패하면(홈 화면이 아닐 때) 안내 창을 띄우고 false.
    // 성공하면 m_draft를 갱신하고 saved 콜백을 호출한다.
    bool commit(Cosmetics const& value) {
        if (!Service::get().saveCosmetics(value)) {
            FLAlertLayer::create("Cosmetics", "Please return to the Versus home screen before changing cosmetics.",
                                 "OK")
                ->show();
            return false;
        }
        m_draft = value;
        if (m_saved) m_saved();
        return true;
    }

public:
    // 화면이 창(닫기 버튼 포함)보다 좁거나 낮으면 창 전체를 같은 비율로 줄여서 띄운다.
    // 안의 배치는 설계 크기 그대로라 어떤 화면 비율에서도 요소가 창이나 화면 밖으로 나가지 않는다.
    void show() override {
        DeferredPopup::show();
        auto const win = CCDirector::sharedDirector()->getWinSize();
        float const fit = std::min({1.f, (win.width - 2.f * kScreenEdge) / std::max(1.f, m_size.width),
                                    (win.height - 2.f * kScreenEdge) / std::max(1.f, m_size.height)});
        if (fit >= 1.f || fit <= 0.f) return;
        m_mainLayer->stopAllActions();
        m_mainLayer->setScale(.1f * fit);
        m_mainLayer->runAction(CCEaseElasticOut::create(CCScaleTo::create(.5f, fit), .6f));
    }
};

// 오라·피니셔 창의 공통 틀(440 x 280). 위에서부터 제목, 미리보기 무대, 이름 + 상태 알약,
// 탭(All/Standard/Membership), 카드 목록(양옆 화살표로 한 페이지씩). 두 창이 같은 자리·같은 모양을 쓴다.
class CardPickerPopup : public CustomizePopup {
protected:
    static constexpr float kW = 440.f, kH = 280.f;
    // 미리보기 무대(어두운 판). 제목(창 위에서 20 아래) 바로 밑.
    static constexpr float kStageX = 16.f, kStageY = 144.f, kStageW = 408.f, kStageH = 98.f;
    // 이름 줄: 이름은 무대 왼쪽 끝에, 상태 알약은 무대 오른쪽 끝에 맞춘다.
    static constexpr float kInfoY = 130.f, kInfoLeft = 22.f, kInfoRight = 418.f;
    // 탭 줄(가운데 정렬)과 카드 목록(보이는 카드 4장), 화살표. 줄 사이는 5~7씩 띄운다.
    static constexpr float kTabsY = 108.f, kTabW = 100.f, kTabGap = 8.f;
    static constexpr float kListX = 40.f, kListY = 12.f, kListW = 360.f, kListH = 84.f;
    static constexpr float kCardW = 80.f, kCardH = 76.f, kCardGap = 8.f;
    static constexpr float kArrowH = 24.f, kArrowX = 21.f;

    // 카드 한 장의 바뀌는 부분: 판(상태 색), 이름, 쓰는 중 체크 표시.
    struct Card {
        NineSlice* back = nullptr;
        CCLabelBMFont* name = nullptr;
        CCSprite* check = nullptr;
    };
    std::map<int, Card> m_cards;
    AuraCarousel* m_list = nullptr;
    std::array<CosmeticTab, 3> m_tabs{};
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    CCLabelBMFont* m_name = nullptr;
    StatePill m_pill;

    // 틀을 만든다: 무대 판, 이름 줄, 탭, 카드 목록(잘라 그리는 칸), 화살표.
    bool frame(char const* title, std::function<void()> saved) {
        if (!begin(kW, kH, title, std::move(saved))) return false;
        panel(m_mainLayer, {kStageX, kStageY}, {kStageW, kStageH}, false);
        m_name = label(m_mainLayer, "", {kInfoLeft, kInfoY}, .55f, 0.f, ccWHITE, true, "goldFont.fnt");
        m_pill = statePill(m_mainLayer);
        float const tabsLeft = kW / 2.f - (3.f * kTabW + 2.f * kTabGap) / 2.f;
        for (int i = 0; i < 3; ++i) {
            m_tabs[i] =
                cosmeticTab(m_buttonMenu, this, menu_selector(CardPickerPopup::onCategory), i, kAuraCategoryNames[i],
                            {tabsLeft + kTabW / 2.f + i * (kTabW + kTabGap), kTabsY}, kTabW, false);
            m_tabs[i].item->setID(fmt::format("cosmetics-category-{}", i));
        }
        auto* stencil = CCLayerColor::create(ccc4(255, 255, 255, 255), kListW, kListH);
        auto* clip = CCClippingNode::create(stencil);
        clip->setPosition({kListX, kListY});
        m_mainLayer->addChild(clip);
        m_list = AuraCarousel::create({kListW, kListH}, kCardW, kCardGap);
        clip->addChild(m_list);
        m_previous = pageArrow(menu_selector(CardPickerPopup::onPrevious), false, {kArrowX, kListY + kListH / 2.f});
        m_previous->setID("cosmetics-previous"_spr);
        m_next = pageArrow(menu_selector(CardPickerPopup::onNext), true, {kW - kArrowX, kListY + kListH / 2.f});
        m_next->setID("cosmetics-next"_spr);
        m_list->setOnScroll([this] { updateArrows(); });
        return true;
    }
    // 카드 목록 옆 화살표(GD의 초록 화살표 GJ_arrow_01, 없으면 분홍 GJ_arrow_03). 둘 다 없으면 "<" ">" 글자 버튼.
    CCMenuItemSpriteExtra* pageArrow(SEL_MenuHandler action, bool next, CCPoint position) {
        auto* arrow = loadedFrameSprite("GJ_arrow_01_001.png", "GJ_GameSheet03.plist");
        if (!arrow) arrow = loadedFrameSprite("GJ_arrow_03_001.png", "GJ_GameSheet03.plist");
        if (!arrow) return button(m_buttonMenu, this, action, next ? ">" : "<", position, .38f);
        arrow->setScale(kArrowH / std::max(1.f, arrow->getContentSize().height));
        arrow->setFlipX(next);
        auto* item = CCMenuItemSpriteExtra::create(arrow, this, action);
        item->setPosition(position);
        m_buttonMenu->addChild(item);
        return item;
    }
    // 카드 오른쪽 위 작은 표시(자물쇠, 체크). 프레임이 없으면 없이 간다.
    static CCSprite* cardBadge(CCNode* card, char const* frame, ccColor3B color) {
        auto* sprite = loadedFrameSprite(frame, "GJ_GameSheet03.plist");
        if (!sprite) return nullptr;
        sprite->setScale(11.f / std::max(1.f, sprite->getContentSize().height));
        sprite->setColor(color);
        sprite->setPosition({kCardW - 10.f, kCardH - 10.f});
        card->addChild(sprite, 3);
        return sprite;
    }
    // 카드 한 장을 목록에 넣는다. art가 위쪽 그림(오라: 아이콘, 피니셔: 메달)을 card 안에 만들어 돌려주고,
    // 이름은 아래에 쓴다. 쓸 수 없는(멤버십 전용) 항목은 그림과 이름을 흐리게 하고 자물쇠를 단다.
    CCMenuItemSpriteExtra* addCard(int tag, char const* name, bool usable, SEL_MenuHandler action,
                                   std::function<CCNode*(CCNode*)> const& art) {
        auto* node = CCNodeRGBA::create();
        node->setContentSize({kCardW, kCardH});
        Card card;
        card.back = NineSlice::create("square02b_001.png");
        card.back->setContentSize(node->getContentSize());
        card.back->setPosition(node->getContentSize() / 2.f);
        card.back->setOpacity(235);
        node->addChild(card.back, -3);
        auto* picture = art(node);
        card.name = label(node, name, {kCardW / 2.f, 11.f}, .28f, kCardW - 6.f, ccWHITE);
        if (!usable) {
            if (picture) previewOpacity(picture, 110);
            card.name->setOpacity(150);
            cardBadge(node, "GJ_lockGray_001.png", ccc3(255, 217, 144));
        }
        card.check = cardBadge(node, "GJ_completesIcon_001.png", ccWHITE);
        if (card.check) card.check->setVisible(false);
        auto* item = CCMenuItemSpriteExtra::create(node, this, action);
        item->m_scaleMultiplier = 1.04f;
        item->setTag(tag);
        m_list->addCard(item);
        m_cards[tag] = card;
        return item;
    }
    // 카드 색: 쓰는 중이면 청록 + 체크, 미리보는 중이면 파랑, 나머지는 남색.
    void paintCards(int inUse, int previewed) {
        for (auto& [tag, card] : m_cards) {
            bool const used = tag == inUse, picked = tag == previewed;
            card.back->setColor(used ? ccc3(32, 112, 98) : picked ? ccc3(44, 78, 136) : ccc3(15, 37, 68));
            card.name->setColor(used ? ccc3(150, 255, 190) : ccWHITE);
            if (card.check) card.check->setVisible(used);
        }
    }
    // 이름 줄: 왼쪽에 이름, 오른쪽 끝에 상태 알약(쓰는 중 / 멤버십 전용 / 아직 저장 안 된 미리보기).
    void setInfo(std::string const& name, bool used, bool usable) {
        char const* state = used ? "IN USE" : usable ? "PREVIEW" : "MEMBERSHIP ONLY";
        ccColor3B const fill = used ? ccc3(32, 112, 98) : usable ? kTabFill : ccc3(112, 76, 20);
        ccColor3B const color = used ? ccc3(150, 255, 190) : usable ? kIce : ccc3(255, 214, 120);
        float const pill = setStatePill(m_pill, state, fill, color, kInfoRight, kInfoY);
        m_name->setString(name.c_str());
        m_name->setScale(.55f);
        float const room = kInfoRight - pill - 10.f - kInfoLeft;
        float const natural = m_name->getContentSize().width * .55f;
        if (natural > room) m_name->setScale(.55f * room / natural);
    }
    void selectCategory(AuraCategory category) {
        m_list->setCategory(category);
        for (int i = 0; i < 3; ++i) selectCosmeticTab(m_tabs[i], i == static_cast<int>(category));
    }
    void onCategory(CCObject* sender) {
        int const category = sender->getTag();
        if (category >= 0 && category < 3) selectCategory(static_cast<AuraCategory>(category));
    }
    // 이전/다음 화살표: 한 페이지(보이는 카드 4장)씩 넘긴다. 끝에 닿은 쪽 화살표는 흐리게 막아 둔다.
    void onPrevious(CCObject*) { m_list->page(-1); }
    void onNext(CCObject*) { m_list->page(1); }
    void updateArrows() {
        if (m_previous) enabled(m_previous, !m_list->atStart());
        if (m_next) enabled(m_next, !m_list->atEnd());
    }
};

// 오라 선택 팝업. 무대 가운데에 미리보는 오라를 두른 내 아이콘.
// 카드는 멤버십으로 가정한 프로필로 그려 모두 미리볼 수 있다(쓸 수 없는 오라는 흐리게 + 자물쇠).
class AuraPopup final : public CardPickerPopup {
    CCNode* m_preview = nullptr;
    int m_selected = 0;

    bool init(std::function<void()> saved) {
        if (!frame("Aura", std::move(saved))) return false;
        setID("versus-aura"_spr);
        m_selected = m_draft.aura;
        m_preview = CCNode::create();
        m_mainLayer->addChild(m_preview);
        bool const member = Service::get().profile().membership;
        for (int i : kAuraDisplayOrder) {
            auto* item = addCard(i, kAuraNames[i], canUseAura(i, member), menu_selector(AuraPopup::onAura),
                                 [this, i](CCNode* card) -> CCNode* {
                                     auto profile = Service::get().profile();
                                     profile.membership = true;
                                     profile.cosmetics = m_draft;
                                     profile.cosmetics.aura = i;
                                     return player(card, profile, {kCardW / 2.f, 45.f}, .78f);
                                 });
            item->setID(fmt::format("aura-{}", i));
        }
        selectCategory(AuraCategory::All);
        m_list->reveal(m_draft.aura);
        refresh();
        return true;
    }
    // 무대의 아이콘, 이름 줄, 카드 색을 선택에 맞춘다.
    void refresh() {
        m_preview->removeAllChildrenWithCleanup(true);
        auto profile = Service::get().profile();
        profile.membership = true;
        profile.cosmetics = m_draft;
        profile.cosmetics.aura = m_selected;
        player(m_preview, profile, {kW / 2.f, kStageY + kStageH / 2.f}, 1.25f);
        bool const member = Service::get().profile().membership;
        setInfo(kAuraNames[m_selected], m_selected == m_draft.aura, canUseAura(m_selected, member));
        paintCards(m_draft.aura, m_selected);
    }
    // 카드를 누르면 미리보기로 선택하고, 사용할 수 있는 오라이면 바로 저장한다.
    // 저장이 안 됐던 카드(홈 화면 밖)는 다시 누르면 다시 저장해 본다.
    void onAura(CCObject* sender) {
        int const choice = sender->getTag();
        if (choice < 0 || choice >= kAuraCount) return;
        bool const usable = canUseAura(choice, Service::get().profile().membership);
        if (choice == m_selected && (choice == m_draft.aura || !usable)) return;
        m_selected = choice;
        if (usable && choice != m_draft.aura) {
            auto next = m_draft;
            next.aura = choice;
            commit(next);
        }
        refresh();
    }

public:
    static AuraPopup* create(std::function<void()> saved) {
        auto* result = new AuraPopup;
        if (result->init(std::move(saved))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 피니셔 선택 팝업: 무대에서 고른 피니셔를 두 플레이어 장면으로 계속 다시 재생한다.
class FinisherPopup final : public CardPickerPopup {
    CCNode* m_view = nullptr;
    int m_preview = 0;
    unsigned m_replay = 0;
    // 장면을 잘라 그리는 칸(무대 판 안쪽 2씩, 404 x 94).
    static constexpr float kViewW = kStageW - 4.f, kViewH = kStageH - 4.f;

    // 카드에 넣는 피니셔 메달.
    static CCNode* cardArt(CCNode* card, int kind) {
        auto* node = art::finisherIcon(std::clamp(kind, 0, kExecutionCount - 1));
        node->setScale(.82f);
        node->setPosition({kCardW / 2.f, 45.f});
        card->addChild(node);
        return node;
    }
    // UI 구성. 카드는 kExecutionDisplayOrder 순서로 만들고, 카테고리 필터를 적용한다.
    bool init(std::function<void()> saved) {
        if (!frame("Finisher", std::move(saved))) return false;
        setID("versus-finisher"_spr);
        auto* stencil = CCLayerColor::create(ccc4(255, 255, 255, 255), kViewW, kViewH);
        auto* clip = CCClippingNode::create(stencil);
        clip->setPosition({kStageX + 2.f, kStageY + 2.f});
        m_mainLayer->addChild(clip);
        m_view = CCNode::create();
        clip->addChild(m_view);
        bool const member = Service::get().profile().membership;
        for (int i : kExecutionDisplayOrder) {
            auto* item =
                addCard(i, kExecutionNames[i], canUseExecution(i, member), menu_selector(FinisherPopup::onChoice),
                        [i](CCNode* card) -> CCNode* { return cardArt(card, i); });
            item->setID(fmt::format("finisher-{}", i));
        }
        m_list->setFilter(executionInCategory);
        m_preview = std::clamp(m_draft.execution, 0, kExecutionCount - 1);
        selectCategory(AuraCategory::All);
        m_list->reveal(m_preview);
        refresh();
        return true;
    }
    // 카드 색을 맞추고 미리보기를 다시 재생한다(이름 줄은 play가 맞춘다).
    void refresh() {
        paintCards(m_draft.execution, m_preview);
        play();
    }
    // 선택한 피니셔를 두 플레이어 장면으로 재생한다. 0번(Random)이면 매번 다른 것을 골라
    // 보여주고, 재생이 끝나면 일정 시간 뒤 자동으로 다시 재생한다.
    void play() {
        m_view->stopAllActions();
        m_view->removeAllChildrenWithCleanup(true);
        // 장면(505 x 132)을 칸 높이에 맞춰 줄이고 가로 가운데에 둔다.
        float const scale = .8f * kViewH / 110.f;
        auto* holder = CCNode::create();
        holder->setScale(scale);
        holder->setPosition({(kViewW - 505.f * scale) / 2.f, 4.f * scale / .8f});
        m_view->addChild(holder);
        auto* stage = CCNode::create();
        stage->setContentSize({505.f, 132.f});
        stage->setAnchorPoint({.5f, .5f});
        stage->setPosition({252.5f, 66.f});
        holder->addChild(stage);
        auto profile = Service::get().profile();
        profile.cosmetics.aura = 8;
        auto* winner = player(stage, profile, {150.f, 46.f}, 1.25f);
        auto opponent = profile;
        opponent.color1 = 0xE989AA;
        opponent.color2 = 0xFFFFFF;
        auto* loser = player(stage, opponent, {340.f, 46.f}, 1.25f);
        loser->setFlipX(true);
        bool const member = Service::get().profile().membership;
        int const kind = m_preview ? m_preview : resolveExecution(0, member, fmt::format("preview-{}", ++m_replay));
        setInfo(m_preview == 0 ? fmt::format("Random: {}", kExecutionNames[kind]) : std::string(kExecutionNames[kind]),
                m_preview == m_draft.execution, canUseExecution(m_preview, member));
        execution::play(stage, winner, loser, kind);
        m_view->runAction(CCSequence::create(CCDelayTime::create(std::max(3.8f, .85f + execution::length(kind))),
                                             CallFuncExt::create([this] { play(); }), nullptr));
    }
    // 카드를 누르면 미리보기로 선택하고, 사용할 수 있으면 바로 저장한다.
    // 저장이 안 됐던 카드(홈 화면 밖)는 다시 누르면 다시 저장해 본다.
    void onChoice(CCObject* sender) {
        int const choice = sender->getTag();
        if (choice < 0 || choice >= kExecutionCount) return;
        bool const usable = canUseExecution(choice, Service::get().profile().membership);
        if (choice == m_preview && (choice == m_draft.execution || !usable)) return;
        m_preview = choice;
        if (usable && choice != m_draft.execution) {
            auto next = m_draft;
            next.execution = choice;
            commit(next);
        }
        refresh();
    }

public:
    static FinisherPopup* create(std::function<void()> saved) {
        auto* result = new FinisherPopup;
        if (result->init(std::move(saved))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 닉네임 스타일 팝업: 글꼴 목록, 시작/끝 색, 그라데이션 방향 설정.
class NicknamePopup final : public CustomizePopup {
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_angleText = nullptr;
    CCLabelBMFont* m_fontText = nullptr;
    CCSprite* m_startSwatch = nullptr;
    CCSprite* m_endSwatch = nullptr;
    Slider* m_angle = nullptr;
    std::vector<int> m_fonts;
    TapScrollLayer* m_fontList = nullptr;
    std::vector<CCNode*> m_fontRows;
    std::vector<CCLabelBMFont*> m_fontLabels;
    std::vector<bool> m_fontStyled;
    static constexpr float kListWidth = 176.f, kListHeight = 140.f, kRowHeight = 27.f;

    // UI 구성. 실제로 존재하는 글꼴 파일만 목록에 넣는다. 비멤버십은 색 설정을 미리보기만 할
    // 수 있다.
    bool init(std::function<void()> saved) {
        // 높이 280: 300이면 왼쪽 위 닫기 버튼이 화면(높이 320) 위로 살짝 나간다. 위쪽 줄들을 그만큼 내렸다.
        if (!begin(440.f, 280.f, "Nickname Style", std::move(saved))) return false;
        setID("versus-nickname"_spr);
        panel(m_mainLayer, {14.f, 196.f}, {412.f, 44.f}, false);
        m_fonts.push_back(0);
        for (int font = 1; font < kNicknameFontCount; ++font)
            if (nicknameFontFile(font)) m_fonts.push_back(font);
        if (std::find(m_fonts.begin(), m_fonts.end(), m_draft.font) == m_fonts.end()) m_draft.font = 0;
        constexpr float column = 124.f;
        if (!Service::get().profile().membership)
            label(m_mainLayer, "Colors: Membership only - preview", {column, 184.f}, .24f, 220.f, ccc3(255, 205, 110));
        button(m_buttonMenu, this, menu_selector(NicknamePopup::onFontPrevious), "<", {30.f, 162.f}, .62f)
            ->setID("nickname-font-previous"_spr);
        m_fontText = label(m_mainLayer, "", {column, 162.f}, .42f, 132.f, ccWHITE, false, "goldFont.fnt");
        button(m_buttonMenu, this, menu_selector(NicknamePopup::onFontNext), ">", {218.f, 162.f}, .62f)
            ->setID("nickname-font-next"_spr);
        auto swatch = [&](int tag, float x, char const* text) {
            auto* disc = CCSprite::create("GJ_button_01.png");
            disc->setScale(.6f);
            auto* fill = loadedFrameSprite("GJ_colorBtn_001.png");
            if (!fill) fill = CCSprite::createWithSpriteFrameName("GJ_sMagicIcon_001.png");
            fill->setPosition(disc->getContentSize() / 2.f);
            fill->setScale(28.f / std::max(1.f, fill->getContentSize().height));
            disc->addChild(fill);
            auto* item = CCMenuItemSpriteExtra::create(disc, this, menu_selector(NicknamePopup::onColor));
            item->setPosition({x, 124.f});
            item->setTag(tag);
            m_buttonMenu->addChild(item);
            label(m_mainLayer, text, {x, 102.f}, .3f, 100.f, kIce);
            return fill;
        };
        m_startSwatch = swatch(0, 74.f, "Start color");
        m_endSwatch = swatch(1, 174.f, "End color");
        m_angleText = label(m_mainLayer, "", {column, 87.f}, .34f, 200.f, kIce);
        m_angle = Slider::create(this, menu_selector(NicknamePopup::onAngle), .5f);
        m_angle->setPosition({column, 70.f});
        m_angle->setValue(m_draft.angle / 359.f);
        m_mainLayer->addChild(m_angle);
        button(m_buttonMenu, this, menu_selector(NicknamePopup::onRotateLeft), "<", {30.f, 70.f}, .58f)
            ->setID("nickname-direction-left"_spr);
        button(m_buttonMenu, this, menu_selector(NicknamePopup::onRotateRight), ">", {218.f, 70.f}, .58f)
            ->setID("nickname-direction-right"_spr);
        button(m_buttonMenu, this, menu_selector(NicknamePopup::onUse), "Use", {column, 28.f}, .62f);
        label(m_mainLayer, fmt::format("FONTS ({})", m_fonts.size()), {338.f, 184.f}, .32f, kListWidth, kIce);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({kListWidth + 4.f, kListHeight + 4.f});
        paper->setPosition({338.f, 28.f + kListHeight / 2.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(220);
        m_mainLayer->addChild(paper);
        m_fontList = TapScrollLayer::create({kListWidth, kListHeight}, [self = Weak<NicknamePopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->selectFont(row->getTag(), false);
        });
        m_fontList->setPosition({338.f - kListWidth / 2.f, 28.f});
        m_fontList->setID("nickname-font-list"_spr);
        m_mainLayer->addChild(m_fontList);
        buildFontList();
        refresh();
        scrollToFont(false);
        scheduleUpdate();
        return true;
    }
    // 글꼴마다 행을 만든다. 글꼴 적용은 비용이 있어 update에서 나눠서 한다.
    void buildFontList() {
        auto* content = m_fontList->m_contentLayer;
        float const height = std::max(kListHeight, m_fonts.size() * kRowHeight + 4.f);
        content->setContentSize({kListWidth, height});
        auto const& profile = Service::get().profile();
        std::string const name = profile.name.empty() ? "Player" : profile.name;
        for (size_t i = 0; i < m_fonts.size(); ++i) {
            auto* row = CCNode::create();
            row->setContentSize({kListWidth - 6.f, kRowHeight - 3.f});
            row->setAnchorPoint({.5f, .5f});
            row->setPosition({kListWidth / 2.f, height - 2.f - kRowHeight * (i + .5f)});
            row->setTag(static_cast<int>(i));
            auto* back = NineSlice::create("square02b_001.png");
            back->setContentSize(row->getContentSize());
            back->setPosition(row->getContentSize() / 2.f);
            back->setID("font-row-back"_spr);
            row->addChild(back, -1);
            label(row, m_fonts[i] == 0 ? std::string("D") : std::to_string(m_fonts[i]),
                  {12.f, (kRowHeight - 3.f) / 2.f}, .3f, 18.f, kMuted);
            m_fontLabels.push_back(
                label(row, name, {(kListWidth - 6.f) / 2.f + 9.f, (kRowHeight - 3.f) / 2.f}, .42f, kListWidth - 40.f));
            m_fontStyled.push_back(m_fonts[i] == 0);
            m_fontRows.push_back(row);
            content->addChild(row);
        }
    }
    // 화면에 보이는 행만 프레임당 최대 3개씩 글꼴을 적용한다(한꺼번에 로드하지 않기 위함).
    void update(float) override {
        int budget = 3;
        for (size_t i = 0; i < m_fontRows.size() && budget > 0; ++i) {
            if (m_fontStyled[i] || !m_fontRows[i]->isVisible()) continue;
            Cosmetics value{};
            value.font = m_fonts[i];
            styleNickname(m_fontLabels[i], false, value);
            m_fontStyled[i] = true;
            --budget;
        }
    }
    int selectedIndex() const {
        auto it = std::find(m_fonts.begin(), m_fonts.end(), m_draft.font);
        return it == m_fonts.end() ? 0 : static_cast<int>(it - m_fonts.begin());
    }
    // 글꼴을 선택하고 미리보기를 갱신한다. scroll이면 목록에서 보이도록 스크롤한다.
    void selectFont(int index, bool scroll) {
        if (index < 0 || index >= static_cast<int>(m_fonts.size())) return;
        m_draft.font = m_fonts[index];
        refresh();
        if (scroll) scrollToFont(true);
    }
    // 선택한 글꼴 행이 보이는 위치로 목록을 스크롤한다. onlyIfHidden이면 가려져 있을 때만.
    void scrollToFont(bool onlyIfHidden) {
        auto* content = m_fontList->m_contentLayer;
        float const height = content->getContentSize().height;
        float const center = height - 2.f - kRowHeight * (selectedIndex() + .5f);
        float const offset = content->getPositionY();
        if (onlyIfHidden && center - kRowHeight / 2.f + offset >= 0.f &&
            center + kRowHeight / 2.f + offset <= kListHeight)
            return;
        content->setPositionY(std::clamp(kListHeight / 2.f - center, kListHeight - height, 0.f));
    }
    // 색 견본, 방향 문구, 글꼴 이름, 목록 선택 표시와 닉네임 미리보기를 다시 그린다.
    void refresh() {
        m_startSwatch->setColor(unpackColor(m_draft.start));
        m_endSwatch->setColor(unpackColor(m_draft.end));
        m_angleText->setString(fmt::format("Direction: {} deg", m_draft.angle).c_str());
        m_fontText->setString(nicknameFontName(m_draft.font).c_str());
        m_fontText->limitLabelWidth(132.f, .42f, .2f);
        int const selected = selectedIndex();
        for (size_t i = 0; i < m_fontRows.size(); ++i)
            if (auto* back = static_cast<NineSlice*>(m_fontRows[i]->getChildByID("font-row-back"_spr))) {
                bool const on = static_cast<int>(i) == selected;
                back->setColor(on ? ccc3(40, 120, 70) : ccc3(14, 30, 64));
                back->setOpacity(on ? 235 : 190);
            }
        if (m_name) m_name->removeFromParentAndCleanup(true);
        auto const& profile = Service::get().profile();
        m_name = label(m_mainLayer, profile.name.empty() ? "Player" : profile.name, {220.f, 236.f}, .6f, 390.f);
        styleNickname(m_name, true, m_draft);
    }
    // 글꼴을 이전/다음으로 이동한다(처음과 끝은 이어진다).
    void stepFont(int direction) {
        if (m_fonts.empty()) return;
        int const count = static_cast<int>(m_fonts.size());
        selectFont(((selectedIndex() + direction) % count + count) % count, true);
    }
    void onFontPrevious(CCObject*) { stepFont(-1); }
    void onFontNext(CCObject*) { stepFont(1); }
    // 색 선택 창을 열고, 선택된 색을 시작 색 또는 끝 색에 반영한다.
    void onColor(CCObject* sender) {
        int const target = sender->getTag();
        if (auto* picker = ColorPickPopup::create(unpackColor(target ? m_draft.end : m_draft.start))) {
            picker->setCallback([self = Weak<NicknamePopup>(this), target](ccColor4B const& color) {
                if (auto owner = self.lock()) {
                    int const rgb = (color.r << 16) | (color.g << 8) | color.b;
                    (target ? owner->m_draft.end : owner->m_draft.start) = rgb;
                    owner->refresh();
                }
            });
            picker->show();
        }
    }
    // 슬라이더 값을 방향(0~359도)으로 바꾼다.
    void onAngle(CCObject*) {
        m_draft.angle = std::clamp(static_cast<int>(std::lround(m_angle->getValue() * 359.f)), 0, 359);
        refresh();
    }
    // 방향을 amount도만큼 돌린다(360도 순환).
    void rotate(int amount) {
        m_draft.angle = (m_draft.angle + amount + 360) % 360;
        m_angle->setValue(m_draft.angle / 359.f);
        refresh();
    }
    void onRotateLeft(CCObject*) { rotate(-15); }
    void onRotateRight(CCObject*) { rotate(15); }
    // 저장. 비멤버십은 글꼴만 저장하고, 색을 바꿨다면 멤버십 안내 창을 띄운다.
    void onUse(CCObject*) {
        if (!Service::get().profile().membership) {
            auto value = cosmeticsForEdition(Service::get().profile().cosmetics, false);
            value.font = m_draft.font;
            bool const gradient =
                m_draft.start != value.start || m_draft.end != value.end || m_draft.angle != value.angle;
            auto const preview = m_draft;
            if (commit(value)) {
                m_draft = preview;
                refresh();
            }
            if (gradient) membershipAlert("Nickname gradients");
            return;
        }
        if (commit(m_draft)) refresh();
    }

public:
    static NicknamePopup* create(std::function<void()> saved) {
        auto* result = new NicknamePopup;
        if (result->init(std::move(saved))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 칭호/업적 팝업: 서버에서 받은 업적 상태로 계열(series)별 목록을 그리고 칭호를 장착한다.
// 위쪽 탭(Season 1 / Season 2 / Versus)으로 계열을 나눠 보고, 탭마다 "얻은 업적/전체"를 보여 준다.
// 오른쪽 끝 Unlocked 체크 상자를 켜면 얻은 칭호(가 있는 계열)만 보인다.
class TitlesPopup final : public CustomizePopup {
    TapScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_empty = nullptr;
    std::optional<AchievementState> m_state;
    std::optional<AchievementRates> m_rates;
    std::set<int> m_open;
    bool m_ownedOnly = false;
    CCMenuItemSpriteExtra* m_filter = nullptr;
    CCSprite* m_checkOff = nullptr;
    CCSprite* m_checkOn = nullptr;
    std::array<CosmeticTab, kAchievementGroupCount> m_tabs{};
    AchievementGroup m_group = AchievementGroup::Versus;
    // 마지막으로 본 탭(게임을 켜 둔 동안 기억). 처음 열 때는 장착한 칭호가 있는 탭.
    static inline int s_lastGroup = -1;
    // 배치(설계 크기 420 x 280, 높이 296이면 닫기 버튼이 높이 320 화면 위로 나간다): 탭 줄과 목록 칸.
    static constexpr float kW = 420.f, kH = 280.f, kTabsY = 235.f, kTabGap = 5.f;
    static constexpr float kListX = 12.f, kListY = 18.f, kListW = 396.f, kListH = 202.f;

    // UI 구성. 탭 줄과 목록 영역을 만든 뒤 다음 프레임에 load로 데이터를 요청한다.
    bool init(std::function<void()> saved) {
        if (!begin(kW, kH, "Achievements", std::move(saved))) return false;
        setID("versus-titles"_spr);
        // 오른쪽 끝에 Unlocked 체크 상자, 남은 폭을 탭 세 개가 똑같이 나눠 쓴다.
        float const filterW = buildFilter();
        float const tabW = (kListW - filterW - 10.f - 2.f * kTabGap) / kAchievementGroupCount;
        for (int i = 0; i < kAchievementGroupCount; ++i) {
            m_tabs[i] = cosmeticTab(m_buttonMenu, this, menu_selector(TitlesPopup::onGroup), i,
                                    std::string(kAchievementGroupNames[i]),
                                    {kListX + tabW / 2.f + i * (tabW + kTabGap), kTabsY}, tabW, true);
            m_tabs[i].item->setID(fmt::format("titles-group-{}", i));
        }
        selectGroup(s_lastGroup >= 0 ? s_lastGroup : static_cast<int>(groupOfTitle(m_draft.title)), false);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({kListW, kListH + 4.f});
        paper->setPosition({kListX + kListW / 2.f, kListY + kListH / 2.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(220);
        m_mainLayer->addChild(paper);
        m_scroll = TapScrollLayer::create({kListW, kListH}, [self = Weak<TitlesPopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->onRow(row->getTag());
        });
        m_scroll->setPosition({kListX, kListY});
        m_mainLayer->addChild(m_scroll);
        float const middle = kListY + kListH / 2.f;
        m_status = label(m_mainLayer, "Loading achievements...", {kW / 2.f, middle + 8.f}, .45f, 360.f, kIce, false,
                         "goldFont.fnt");
        m_empty = label(m_mainLayer, "", {kW / 2.f, middle}, .4f, 360.f, kMuted, false, "goldFont.fnt");
        m_empty->setVisible(false);
        button(m_buttonMenu, this, menu_selector(TitlesPopup::onRetry), "Retry", {kW / 2.f, middle - 26.f}, .45f)
            ->setID("titles-retry"_spr);
        runAction(CCSequence::create(CCDelayTime::create(0.f), CallFuncExt::create([this] { load(); }), nullptr));
        return true;
    }
    // Unlocked 체크 상자(켜면 얻은 칭호만)를 탭 줄 오른쪽 끝에 만들고 그 폭을 돌려준다.
    // GD 체크 상자 프레임이 없으면 예전의 All/Unlocked 글자 버튼으로 대신한다.
    float buildFilter() {
        float const right = kListX + kListW;
        m_checkOff = loadedFrameSprite("GJ_checkOff_001.png", "GJ_GameSheet03.plist");
        m_checkOn = loadedFrameSprite("GJ_checkOn_001.png", "GJ_GameSheet03.plist");
        if (!m_checkOff || !m_checkOn) {
            m_checkOff = m_checkOn = nullptr;
            m_filter = button(m_buttonMenu, this, menu_selector(TitlesPopup::onFilter), "All", {right - 36.f, kTabsY},
                              .42f, "GJ_button_04.png");
            m_filter->setID("titles-unlocked"_spr);
            return 72.f;
        }
        constexpr float box = 15.f;
        auto* node = CCNode::create();
        for (auto* sprite : {m_checkOff, m_checkOn}) {
            sprite->setScale(box / std::max(1.f, sprite->getContentSize().height));
            sprite->setPosition({box / 2.f, kTabHeight / 2.f});
            node->addChild(sprite);
        }
        m_checkOn->setVisible(false);
        auto* text = label(node, "Unlocked", {box + 4.f, kTabHeight / 2.f + .5f}, .3f, 60.f, kIce, true);
        float const width = box + 4.f + text->getScaledContentSize().width;
        node->setContentSize({width, kTabHeight});
        m_filter = CCMenuItemSpriteExtra::create(node, this, menu_selector(TitlesPopup::onFilter));
        m_filter->m_scaleMultiplier = 1.08f;
        m_filter->setPosition({right - width / 2.f, kTabsY});
        m_filter->setID("titles-unlocked"_spr);
        m_buttonMenu->addChild(m_filter);
        return width;
    }
    // 칭호가 보이는 탭: 시즌 1 최종 칭호는 Season 1, 나머지는 칭호가 든 계열의 탭(계열 밖이면 Versus).
    static AchievementGroup groupOfTitle(int title) {
        return isS1FinalTitle(title) ? AchievementGroup::Season1 : titleSeriesGroup(title);
    }
    // 탭을 고른다. redraw면 목록을 맨 위부터 다시 그린다.
    void selectGroup(int group, bool redraw) {
        group = std::clamp(group, 0, kAchievementGroupCount - 1);
        m_group = static_cast<AchievementGroup>(group);
        s_lastGroup = group;
        for (int i = 0; i < kAchievementGroupCount; ++i) selectCosmeticTab(m_tabs[i], i == group);
        if (redraw && m_state) {
            m_scroll->m_contentLayer->removeAllChildrenWithCleanup(true);
            render();
        }
    }
    void onGroup(CCObject* sender) {
        if (sender->getTag() != static_cast<int>(m_group)) selectGroup(sender->getTag(), true);
    }
    // 탭마다 "얻은 업적/전체"(멤버십 칭호는 업적이 아니라 세지 않는다). Season 2가 안 열렸으면 비워 둔다.
    void updateTabCounts() {
        for (int g = 0; g < kAchievementGroupCount; ++g) {
            auto const group = static_cast<AchievementGroup>(g);
            if (group == AchievementGroup::Season2 && !m_state->season2) {
                setCosmeticTabCount(m_tabs[g], "");
                continue;
            }
            int owned = 0, total = 0;
            for (int s = 0; s < static_cast<int>(kSeries.size()); ++s) {
                if (seriesGroup(s) != group) continue;
                for (int i = 0; i < kSeries[s].count; ++i) {
                    int const title = kSeries[s].titles[i];
                    if (achievementOfTitle(title) < 0) continue;
                    ++total;
                    owned += m_state->ownsTitle(title);
                }
            }
            setCosmeticTabCount(m_tabs[g], fmt::format("{}/{}", owned, total));
        }
    }
    // 업적 상태와 달성률을 서버에서 받아온다. 실패하면 메시지와 Retry 버튼을 보여준다.
    // 달성률은 한 번만 받으며, 상태가 이미 있으면 받자마자 다시 그린다.
    void load() {
        m_status->setVisible(true);
        m_status->setString("Loading achievements...");
        m_empty->setVisible(false);
        if (auto* retry = m_buttonMenu->getChildByID("titles-retry"_spr)) retry->setVisible(false);
        Service::get().fetchAchievements(
            {}, [self = Weak<TitlesPopup>(this)](std::optional<AchievementState> state, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                if (!state) {
                    log::warn("Versus achievements unavailable: {}", detail);
                    owner->m_status->setString(detail.empty() ? "Achievements unavailable" : detail.c_str());
                    owner->m_status->limitLabelWidth(360.f, .45f, .2f);
                    if (auto* retry = owner->m_buttonMenu->getChildByID("titles-retry"_spr)) retry->setVisible(true);
                    return;
                }
                noteTitles(*state);
                owner->m_state = std::move(state);
                owner->m_status->setVisible(false);
                owner->render();
            });
        if (!m_rates)
            Service::get().fetchAchievementRates(
                [self = Weak<TitlesPopup>(this)](std::optional<AchievementRates> rates) {
                    auto owner = self.lock();
                    if (!owner || !rates || owner->m_rates) return;
                    owner->m_rates = std::move(rates);
                    if (owner->m_state) owner->render();
                });
    }
    void onRetry(CCObject*) { load(); }
    // Unlocked 필터를 켜고 끄고 목록을 다시 그린다.
    void onFilter(CCObject*) {
        m_ownedOnly = !m_ownedOnly;
        if (m_checkOn) {
            m_checkOn->setVisible(m_ownedOnly);
            m_checkOff->setVisible(!m_ownedOnly);
        } else {
            auto* sprite = ButtonSprite::create(m_ownedOnly ? "Unlocked" : "All", "goldFont.fnt",
                                                m_ownedOnly ? "GJ_button_01.png" : "GJ_button_04.png");
            sprite->setScale(.42f);
            m_filter->setNormalImage(sprite);
            m_filter->updateSprite();
        }
        if (m_state) {
            m_scroll->m_contentLayer->removeAllChildrenWithCleanup(true);
            render();
        }
    }
    // 계열 헤더 행을 구분하는 태그 시작값(kHeaderTag + 계열 번호).
    static constexpr int kHeaderTag = 1000;
    // 계열에서 아직 얻지 못한 첫 단계(모두 얻었다면 마지막 단계)의 번호.
    int currentTier(TitleSeries const& series) const {
        for (int i = 0; i < series.count; ++i)
            if (!m_state->ownsTitle(series.titles[i])) return i;
        return series.count - 1;
    }
    // 칭호 행에 표시할 상태 문구와 진행률(0~1)을 계산한다.
    std::pair<std::string, float> titleState(int title) const {
        bool const owned = m_state->ownsTitle(title);
        if (title == m_draft.title) return {"EQUIPPED", 1.f};
        if (title == 0) return {"DEFAULT", 1.f};
        if (isDeveloperTitle(title)) return {owned ? "DEVELOPER" : "", owned ? 1.f : 0.f};
        if (isAdminTitle(title)) return {owned ? "STAFF" : "", owned ? 1.f : 0.f};
        if (isS1FinalTitle(title)) return {owned ? "SEASON 1" : "", owned ? 1.f : 0.f};
        if (isMembershipTitle(title)) return {owned ? "MEMBERSHIP" : "LOCKED", owned ? 1.f : 0.f};
        int const index = achievementOfTitle(title);
        if (index < 0) return {"", 0.f};
        auto const& entry = m_state->entries[index];
        float const fraction = std::clamp(entry.progress / static_cast<float>(std::max(1, entry.goal)), 0.f, 1.f);
        return {owned ? "UNLOCKED" : fmt::format("{} / {}", entry.progress, entry.goal), owned ? 1.f : fraction};
    }
    // 계열 헤더 행의 나무판 프레임을 사각형 레이어로 그린다. 장착 중이면 초록, 열려 있으면
    // 밝은 색.
    static void woodFrame(CCNode* cell, CCSize size, bool open, bool equipped) {
        float const w = size.width, h = size.height, t = 4.f;
        auto rect = [cell](ccColor4B color, float x, float y, float rw, float rh, int z) {
            auto* layer = CCLayerColor::create(color, rw, rh);
            layer->setPosition({x, y});
            cell->addChild(layer, z);
        };
        rect(equipped ? ccc4(52, 96, 62, 245) : ccc4(104, 62, 30, 245), t, t, w - 2.f * t, h - 2.f * t, -6);
        for (float const g : {.3f, .62f, .84f})
            rect(ccc4(70, 40, 18, 70), t + 6.f + g * 40.f, t + (h - 2.f * t) * g, (w - 2.f * t) * (.55f - g * .25f),
                 1.f, -6);
        ccColor4B const dark = ccc4(52, 28, 12, 255);
        ccColor4B const wood = open ? ccc4(184, 120, 62, 255) : ccc4(156, 96, 48, 255);
        ccColor4B const light = open ? ccc4(232, 176, 110, 255) : ccc4(204, 142, 80, 255);
        rect(dark, 0.f, 0.f, w, h, -8);
        rect(wood, 1.f, 1.f, w - 2.f, h - 2.f, -7);
        rect(light, 1.f, h - 2.f, w - 2.f, 1.f, -7);
        rect(light, 1.f, 1.f, 1.f, h - 2.f, -7);
        rect(dark, t - 1.f, t - 1.f, w - 2.f * t + 2.f, 1.f, -7);
        rect(dark, t - 1.f, h - t, w - 2.f * t + 2.f, 1.f, -7);
        rect(dark, t - 1.f, t - 1.f, 1.f, h - 2.f * t + 2.f, -7);
        rect(dark, w - t, t - 1.f, 1.f, h - 2.f * t + 2.f, -7);
        for (float nx : {1.5f, w - 3.5f})
            for (float ny : {1.5f, h - 3.5f}) rect(ccc4(70, 42, 20, 255), nx, ny, 2.f, 2.f, -6);
    }
    // 이 칭호를 가진 플레이어 비율 문구. 달성률 데이터가 없거나 업적과 무관한 칭호면 없음.
    static constexpr float kRateColumn = 73.f;
    std::optional<std::string> playersRate(int title) const {
        if (!m_rates) return std::nullopt;
        float share = 1.f;
        if (title != 0) {
            if (achievementOfTitle(title) < 0 || kTitles[title].achievement.empty()) return std::nullopt;
            share = m_rates->share(std::string(kTitles[title].achievement));
        }
        float const percent = share * 100.f;
        std::string const value = percent <= 0.f      ? "0%"
                                  : percent < .1f     ? "<0.1%"
                                  : percent >= 99.95f ? "100%"
                                  : percent < 10.f    ? fmt::format("{:.1f}%", percent)
                                                      : fmt::format("{:.0f}%", percent);
        return value + " of players";
    }
    // 칭호 행 하나(또는 계열 헤더)를 만든다: 배지, 조건 문구, 상태 문구, 진행 막대, 달성 비율.
    // 획득하지 못한 일반 행은 반투명으로 보인다.
    // goat: Season 2 탭 맨 위의 G.O.A.T 전용 칸 - 얻었으면 불씨 빛 판 위로 G.O.A.T 불꽃 셰이더가 타오르고,
    // 위쪽에 "kept for good"(G.O.A.T에서 내려와도 남는다) 머리말.
    CCNode* titleRow(int title, CCSize size, float indent, std::string const& caption, bool header, bool open,
                     int badge = -1, std::string const& detail = "", bool goatSpecial = false) {
        bool const owned = m_state->ownsTitle(title), equipped = m_draft.title == title;
        auto* cell = CCNode::create();
        cell->setContentSize(size);
        float const mid = size.height / 2.f;
        if (header)
            woodFrame(cell, size, open, equipped);
        else {
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(size);
            bg->setPosition(size / 2.f);
            bg->setColor(goatSpecial && owned ? ccc3(74, 28, 10)
                         : equipped           ? ccc3(32, 112, 98)
                         : owned              ? ccc3(24, 52, 92)
                                              : ccc3(16, 24, 40));
            bg->setOpacity(goatSpecial && owned ? 245 : 225);
            cell->addChild(bg, -1);
            if (goatSpecial && owned)
                if (auto* fire = goat::band({size.width - 8.f, size.height * 1.05f}, ccc3(255, 140, 30), .55f)) {
                    fire->setPosition({size.width / 2.f, size.height * 1.05f / 2.f + 1.f});
                    fire->setID("goat-row-fire"_spr);
                    cell->addChild(fire, -1);
                }
        }
        titleBadge(cell, badge >= 0 ? badge : title, {indent + 58.f, mid - (header || goatSpecial ? 2.f : 0.f)},
                   header || goatSpecial ? 1.12f : 1.f, 110.f, header || owned);
        float const textX = indent + 122.f;
        auto [state, fraction] = titleState(title);
        if (header)
            label(cell, caption, {textX, size.height - 8.f}, .3f, 190.f, ccc3(255, 221, 120), true, "goldFont.fnt");
        if (goatSpecial)
            label(cell, "G.O.A.T  -  kept for good", {textX, size.height - 8.f}, .3f, 190.f, ccc3(255, 196, 90), true,
                  "goldFont.fnt");
        std::string const requirement = isS1FinalTitle(title)
                                            ? std::string("Your last rating in Season 1")
                                            : std::string(kTitles[std::clamp(title, 0, kTitleCount - 1)].requirement);
        label(cell, header ? detail : requirement, {textX, header ? mid - 2.f : mid + 5.f}, header ? .27f : .26f,
              size.width - textX - 82.f,
              header                     ? ccWHITE
              : isMembershipTitle(title) ? ccc3(255, 205, 110)
              : owned                    ? ccWHITE
                                         : kIce,
              true, "bigFont.fnt");
        label(cell, state, {size.width - (header ? 26.f : 10.f), mid + 3.f}, .38f, 72.f,
              equipped ? ccc3(150, 255, 190)
              : owned  ? ccc3(255, 221, 120)
                       : kIce,
              false, "goldFont.fnt")
            ->setAnchorPoint({1.f, .5f});
        float const barWidth = size.width - textX - kRateColumn;
        if (auto const rate = playersRate(title))
            label(cell, *rate, {size.width - 7.f, 8.5f}, .22f, kRateColumn - 12.f, kMuted, false, "bigFont.fnt")
                ->setAnchorPoint({1.f, .5f});
        auto* track = CCLayerColor::create(ccc4(255, 255, 255, 40), barWidth, 3.f);
        track->setPosition({textX, 5.f});
        cell->addChild(track);
        auto* fill =
            CCLayerColor::create(owned ? ccc4(120, 230, 160, 255) : ccc4(120, 190, 255, 255), barWidth * fraction, 3.f);
        fill->setPosition({textX, 5.f});
        cell->addChild(fill);
        if (header) {
            auto* arrow = loadedFrameSprite("edit_downBtn_001.png");
            if (!arrow) arrow = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
            if (arrow) {
                arrow->setScale(14.f / std::max(1.f, arrow->getContentSize().height));
                arrow->setRotation(open ? 180.f : 0.f);
                arrow->setPosition({size.width - 12.f, mid});
                cell->addChild(arrow);
            }
        }
        if (!owned && !header) previewOpacity(cell, 150);
        return cell;
    }
    // 고른 탭의 목록 전체를 다시 그린다. 탭 맨 위에 낱개 칭호(Versus: 기본·개발자·운영자,
    // Season 1: 시즌 1 최종 칭호), 그 뒤에 그 탭의 계열을 (달성 개수, 비율, 다음 단계 진행률 순) 정렬해
    // 붙이고, 열린 계열은 펼쳐서 얻은 것부터 보여준다.
    // 이전 스크롤 위치(위쪽 기준)는 유지한다(탭·필터를 바꿀 때는 비우고 불러 맨 위부터).
    void render() {
        if (!m_state) return;
        updateTabCounts();
        auto* content = m_scroll->m_contentLayer;
        bool const keepScroll = content->getChildrenCount() > 0;
        float const scrollFromTop = content->getContentSize().height + content->getPositionY();
        content->removeAllChildrenWithCleanup(true);
        constexpr float headerH = 40.f, rowH = 32.f, goatH = 42.f, gap = 3.f;
        struct Item {
            int tag, title;
            float height;
            bool header;
            int series;
        };
        std::vector<Item> items;
        if (m_group == AchievementGroup::Versus) {
            items.push_back({0, 0, rowH, false, -1});
            if (m_state->ownsTitle(kDeveloperTitle))
                items.push_back({kDeveloperTitle, kDeveloperTitle, rowH, false, -1});
            if (m_state->ownsTitle(kAdminTitle)) items.push_back({kAdminTitle, kAdminTitle, rowH, false, -1});
        }
        if (m_group == AchievementGroup::Season1 && m_state->s1Final)
            items.push_back({m_state->s1Final, m_state->s1Final, rowH, false, -1});
        // Season 2: 맨 위에 G.O.A.T 전용 칸(불꽃 장식). 데몬 컵 계열을 펼쳐도 그 칭호 행은 여기 대신 빠진다.
        bool const goatRow = m_group == AchievementGroup::Season2 && m_state->season2;
        if (goatRow && (!m_ownedOnly || m_state->ownsTitle(kSeason2GoatTitle)))
            items.push_back({kSeason2GoatTitle, kSeason2GoatTitle, goatH, false, -1});
        int const seriesCount = static_cast<int>(kSeries.size());
        std::vector<int> order;
        std::vector<std::tuple<int, float, float>> keys(kSeries.size());
        for (int s = 0; s < seriesCount; ++s) {
            if (seriesGroup(s) != m_group) continue;
            if (s >= kFirstSeason2Series && !m_state->season2) continue;
            auto const& series = kSeries[s];
            int owned = 0;
            for (int i = 0; i < series.count; ++i) owned += m_state->ownsTitle(series.titles[i]);
            float const next = owned >= series.count ? 1.f : titleState(series.titles[currentTier(series)]).second;
            keys[s] = {owned, owned / static_cast<float>(std::max(1, series.count)), next};
            order.push_back(s);
        }
        std::stable_sort(order.begin(), order.end(), [&keys](int a, int b) { return keys[a] > keys[b]; });
        for (int const s : order) {
            auto const& series = kSeries[s];
            bool const any = std::get<0>(keys[s]) > 0;
            if (m_ownedOnly && !any) continue;
            items.push_back({kHeaderTag + s, series.titles[currentTier(series)], headerH, true, s});
            if (!m_open.contains(s)) continue;
            for (bool const owned : {true, false}) {
                if (!owned && m_ownedOnly) break;
                for (int i = 0; i < series.count; ++i)
                    if (m_state->ownsTitle(series.titles[i]) == owned &&
                        !(goatRow && series.titles[i] == kSeason2GoatTitle))
                        items.push_back({series.titles[i], series.titles[i], rowH, false, s});
            }
        }
        // 보일 것이 없으면(Unlocked만 켰는데 이 탭에서 얻은 게 없을 때, 안 열린 Season 2) 목록 가운데 안내.
        bool const closed = m_group == AchievementGroup::Season2 && !m_state->season2;
        m_empty->setString(closed ? "Season 2 achievements are not open yet" : "No unlocked achievements here yet");
        m_empty->limitLabelWidth(360.f, .4f, .2f);
        m_empty->setVisible(items.empty());
        float height = 6.f;
        for (auto const& item : items) height += item.height + gap;
        height = std::max(202.f, height);
        content->setContentSize({396.f, height});
        float y = height - 3.f;
        for (auto const& item : items) {
            CCNode* cell;
            if (item.header) {
                auto const& series = kSeries[item.series];
                int owned = 0;
                for (int i = 0; i < series.count; ++i) owned += m_state->ownsTitle(series.titles[i]);
                auto const& next = kTitles[item.title];
                std::string const detail = owned >= series.count
                                               ? fmt::format("All {} titles unlocked", series.count)
                                               : fmt::format("Next: {} - {}", next.name, next.requirement);
                cell = titleRow(item.title, {388.f, item.height}, 0.f,
                                fmt::format("{}   {} / {}", series.name, owned, series.count), true,
                                m_open.contains(item.series), series.titles[series.count - 1], detail);
            } else {
                float const indent = item.series >= 0 ? 18.f : 0.f;
                bool const special = item.title == kSeason2GoatTitle && item.series < 0;
                cell = titleRow(item.title, {388.f - indent, item.height}, 0.f, "", false, false, -1, "", special);
                if (item.series >= 0) {
                    auto* branch = CCLayerColor::create(ccc4(255, 221, 120, 120), 2.f, item.height + gap);
                    branch->setPosition({-10.f, 0.f});
                    cell->addChild(branch, -2);
                }
            }
            cell->setAnchorPoint({.5f, .5f});
            cell->setTag(item.tag);
            cell->setPosition({198.f + (item.header || item.series < 0 ? 0.f : 9.f), y - item.height / 2.f});
            content->addChild(cell);
            y -= item.height + gap;
        }
        if (keepScroll) {
            float const maxY = 0.f, minY = m_scroll->getContentSize().height - height;
            content->setPositionY(std::clamp(scrollFromTop - height, minY, maxY));
        } else
            m_scroll->scrollToTop();
    }
    // 행을 누르면: 계열 헤더는 펼침/접힘, 나머지는 칭호 선택. render는 다음 프레임으로 미룬다.
    void onRow(int tag) {
        if (isS1FinalTitle(tag)) {
            selectTitle(tag);
            return;
        }
        if (tag >= kHeaderTag) {
            int const series = tag - kHeaderTag;
            if (!m_open.erase(series)) m_open.insert(series);
            runAction(CCSequence::create(CCDelayTime::create(0.f), CallFuncExt::create([this] { render(); }), nullptr));
            return;
        }
        selectTitle(tag);
    }
    // 칭호를 장착한다. 얻지 못한 칭호는 조건 안내 창을 띄운다(특수 칭호는 무시).
    // 장착은 commit으로 저장한 뒤 다시 그린다.
    void selectTitle(int title) {
        if (!m_state || !validTitle(title)) return;
        if (!m_state->ownsTitle(title)) {
            if (isS1FinalTitle(title)) return;
            if (isDeveloperTitle(title) || isAdminTitle(title)) return;
            std::string body;
            if (isMembershipTitle(title))
                body = "Unlock condition: <cy>Tipp7 Membership</c> active.\nEvery member owns all Membership titles.";
            else {
                auto const& entry = m_state->entries[std::max(0, achievementOfTitle(title))];
                body =
                    fmt::format("Unlock condition: <cy>{}</c>\nProgress: <cg>{} / {}</c>\nPast matches already count.",
                                kTitles[title].requirement, entry.progress, entry.goal);
            }
            FLAlertLayer::create(std::string(kTitles[title].name).c_str(), body, "OK")->show();
            return;
        }
        if (title == m_draft.title) return;
        auto next = m_draft;
        next.title = title;
        if (commit(next))
            runAction(CCSequence::create(CCDelayTime::create(0.f), CallFuncExt::create([this] { render(); }), nullptr));
    }

public:
    static TitlesPopup* create(std::function<void()> saved) {
        auto* result = new TitlesPopup;
        if (result->init(std::move(saved))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};
