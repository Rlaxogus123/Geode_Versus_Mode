// 꾸미기 팝업 모음: 오라(AuraPopup), 피니셔(FinisherPopup), 닉네임 스타일(NicknamePopup),
// 칭호/업적(TitlesPopup). 모두 CustomizePopup을 상속하고, 선택값은 m_draft에 두었다가
// commit으로 저장한다. 다른 소스 파일에 포함(include)되어 쓰이는 조각이다.
// 꾸미기 팝업의 공통 기반. 편집 중인 값(m_draft)과 저장 후 콜백을 들고 있다.
class CustomizePopup : public ::versus::DeferredPopup {
protected:
    Cosmetics m_draft;
    std::function<void()> m_saved;
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
};

// 오라 선택 팝업: 미리보기, 카테고리 탭, 가로 스크롤 카드 목록.
class AuraPopup final : public CustomizePopup {
    CCNode* m_preview = nullptr;
    CCLabelBMFont* m_auraName = nullptr;
    CCLabelBMFont* m_auraTier = nullptr;
    AuraCarousel* m_auraList = nullptr;
    std::array<NineSlice*, 3> m_categoryBackgrounds{};
    std::array<CCLabelBMFont*, 3> m_categoryLabels{};
    std::array<CCMenuItemSpriteExtra*, kAuraCount> m_auraButtons{};
    std::array<NineSlice*, kAuraCount> m_auraBackgrounds{};
    std::array<CCLabelBMFont*, kAuraCount> m_auraLabels{};
    int m_selected = 0;

    // UI 구성. 카테고리 탭 3개와 오라 카드를 만든다(카드는 멤버십으로 가정한 프로필로 그려
    // 모두 미리볼 수 있고,
    // 사용 불가한 오라는 반투명 처리).
    bool init(std::function<void()> saved) {
        if (!begin(420.f, 290.f, "Aura", std::move(saved))) return false;
        setID("versus-aura"_spr);
        m_selected = m_draft.aura;
        panel(m_mainLayer, {16.f, 150.f}, {150.f, 102.f}, false);
        m_preview = CCNode::create();
        m_mainLayer->addChild(m_preview);
        m_auraName = label(m_mainLayer, "", {295.f, 228.f}, .7f, 220.f, ccWHITE, false, "goldFont.fnt");
        m_auraTier = label(m_mainLayer, "", {295.f, 205.f}, .32f, 220.f, kIce);
        label(m_mainLayer, "Tap an aura to use it", {295.f, 172.f}, .3f, 220.f, kMuted, false, "chatFont.fnt");
        for (int category = 0; category < 3; ++category) {
            auto* tab = CCNode::create();
            tab->setContentSize({100.f, 16.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(tab->getContentSize());
            bg->setPosition({50.f, 8.f});
            tab->addChild(bg, -1);
            m_categoryBackgrounds[category] = bg;
            m_categoryLabels[category] = label(tab, kAuraCategoryNames[category], {50.f, 8.f}, .28f, 94.f, kIce);
            auto* item = CCMenuItemSpriteExtra::create(tab, this, menu_selector(AuraPopup::onCategory));
            item->setID(fmt::format("aura-category-{}", category));
            item->setTag(category);
            item->setPosition({98.f + category * 112.f, 134.f});
            m_buttonMenu->addChild(item);
        }
        auto* stencil = CCLayerColor::create(ccc4(255, 255, 255, 255), 336.f, 86.f);
        auto* clip = CCClippingNode::create(stencil);
        clip->setPosition({42.f, 38.f});
        m_mainLayer->addChild(clip);
        m_auraList = AuraCarousel::create({336.f, 86.f});
        clip->addChild(m_auraList);
        bool const member = Service::get().profile().membership;
        for (int i : kAuraDisplayOrder) {
            auto* card = CCNodeRGBA::create();
            card->setContentSize({86.f, 80.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(card->getContentSize());
            bg->setPosition({43.f, 40.f});
            bg->setColor(ccc3(15, 37, 68));
            bg->setOpacity(235);
            card->addChild(bg, -3);
            m_auraBackgrounds[i] = bg;
            auto profile = Service::get().profile();
            profile.membership = true;
            profile.cosmetics = m_draft;
            profile.cosmetics.aura = i;
            player(card, profile, {43.f, 48.f}, .85f);
            m_auraLabels[i] = label(card, kAuraNames[i], {43.f, 12.f}, .3f, 78.f, kIce);
            auto* item = CCMenuItemSpriteExtra::create(card, this, menu_selector(AuraPopup::onAura));
            item->m_scaleMultiplier = 1.03f;
            item->setTag(i);
            item->setID(fmt::format("aura-{}", i));
            if (!canUseAura(i, member)) previewOpacity(item, 110);
            m_auraList->addCard(item);
            m_auraButtons[i] = item;
        }
        button(m_buttonMenu, this, menu_selector(AuraPopup::onPrevious), "<", {25.f, 81.f}, .38f);
        button(m_buttonMenu, this, menu_selector(AuraPopup::onNext), ">", {395.f, 81.f}, .38f);
        selectCategory(AuraCategory::All);
        refresh();
        return true;
    }
    // 선택한 오라로 미리보기, 이름, 상태 문구(IN USE/STANDARD/MEMBERSHIP)와 카드 색을
    // 갱신한다.
    void refresh() {
        m_preview->removeAllChildrenWithCleanup(true);
        auto profile = Service::get().profile();
        profile.membership = true;
        profile.cosmetics = m_draft;
        profile.cosmetics.aura = m_selected;
        player(m_preview, profile, {91.f, 205.f}, 1.15f);
        bool const member = Service::get().profile().membership;
        m_auraName->setString(kAuraNames[m_selected]);
        m_auraName->limitLabelWidth(220.f, .7f, .2f);
        m_auraTier->setString(m_selected == m_draft.aura ? "IN USE"
                              : canUseAura(m_selected, member)
                                  ? (isStandardAura(m_selected) ? "STANDARD" : "MEMBERSHIP")
                                  : "MEMBERSHIP - PREVIEW");
        for (int i = 0; i < kAuraCount; ++i) {
            bool const used = i == m_draft.aura, picked = i == m_selected;
            m_auraBackgrounds[i]->setColor(used ? ccc3(32, 112, 98) : picked ? ccc3(40, 70, 120) : ccc3(15, 37, 68));
            m_auraLabels[i]->setColor(used ? ccc3(150, 255, 190) : kIce);
        }
    }
    // 카테고리 탭을 바꾸고 탭 색과 카드 목록 필터를 갱신한다.
    void selectCategory(AuraCategory category) {
        m_auraList->setCategory(category);
        for (int i = 0; i < 3; ++i) {
            bool const selected = i == static_cast<int>(category);
            m_categoryBackgrounds[i]->setColor(selected ? ccc3(32, 112, 98) : ccc3(15, 37, 68));
            m_categoryLabels[i]->setColor(selected ? ccc3(150, 255, 190) : kIce);
        }
    }
    void onCategory(CCObject* sender) {
        int const category = sender->getTag();
        if (category >= 0 && category < 3) selectCategory(static_cast<AuraCategory>(category));
    }
    // 이전/다음 버튼: 목록을 한 화면 분량 스크롤한다.
    void onPrevious(CCObject*) { m_auraList->scrollBy(-282.f); }
    void onNext(CCObject*) { m_auraList->scrollBy(282.f); }
    // 카드를 누르면 미리보기로 선택하고, 사용할 수 있는 오라이면 바로 저장한다.
    void onAura(CCObject* sender) {
        int const choice = sender->getTag();
        if (choice < 0 || choice >= kAuraCount || choice == m_selected) return;
        m_selected = choice;
        if (canUseAura(choice, Service::get().profile().membership) && choice != m_draft.aura) {
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

// 피니셔 선택 팝업: 위쪽에 재생 미리보기, 아래쪽에 카드 목록.
class FinisherPopup final : public CustomizePopup {
    std::array<NineSlice*, kExecutionCount> m_cards{};
    std::array<CCLabelBMFont*, kExecutionCount> m_names{};
    std::array<NineSlice*, 3> m_categoryBackgrounds{};
    std::array<CCLabelBMFont*, 3> m_categoryLabels{};
    AuraCarousel* m_list = nullptr;
    CCNode* m_view = nullptr;
    CCLabelBMFont* m_caption = nullptr;
    CCLabelBMFont* m_state = nullptr;
    int m_preview = 0;
    unsigned m_replay = 0;
    static constexpr float kStageW = 404.f, kStageH = 92.f;  // 미리보기 칸(예전 110)

    // 카드에 넣는 피니셔 아이콘.
    static CCNode* cardArt(int kind) {
        auto* node = art::finisherIcon(std::clamp(kind, 0, kExecutionCount - 1));
        node->setScale(.95f);
        return node;
    }
    // UI 구성. 카드는 kExecutionDisplayOrder 순서로 만들고, 카테고리 필터를 적용한다.
    bool init(std::function<void()> saved) {
        // 높이 280: 300이면 왼쪽 위 닫기 버튼이 화면(높이 320) 위로 살짝 나간다. 미리보기 칸이 그만큼 낮아진다.
        if (!begin(440.f, 280.f, "Finisher", std::move(saved))) return false;
        setID("versus-finisher"_spr);
        panel(m_mainLayer, {14.f, 146.f}, {412.f, kStageH + 8.f}, false);
        auto* stencil = CCLayerColor::create(ccc4(255, 255, 255, 255), kStageW, kStageH);
        auto* clip = CCClippingNode::create(stencil);
        clip->setPosition({18.f, 150.f});
        m_mainLayer->addChild(clip);
        m_view = CCNode::create();
        clip->addChild(m_view);
        m_caption = label(m_mainLayer, "", {150.f, 134.f}, .5f, 260.f, ccWHITE, false, "goldFont.fnt");
        m_state = label(m_mainLayer, "", {350.f, 134.f}, .3f, 150.f, kIce);
        auto* listStencil = CCLayerColor::create(ccc4(255, 255, 255, 255), 356.f, 86.f);
        auto* listClip = CCClippingNode::create(listStencil);
        listClip->setPosition({42.f, 40.f});
        m_mainLayer->addChild(listClip);
        m_list = AuraCarousel::create({356.f, 86.f});
        listClip->addChild(m_list);
        bool const member = Service::get().profile().membership;
        for (int i : kExecutionDisplayOrder) {
            auto* card = CCNodeRGBA::create();
            card->setContentSize({86.f, 80.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(card->getContentSize());
            bg->setPosition({43.f, 40.f});
            bg->setColor(ccc3(15, 37, 68));
            bg->setOpacity(235);
            card->addChild(bg, -3);
            m_cards[i] = bg;
            auto* mark = cardArt(i);
            mark->setPosition({43.f, 50.f});
            card->addChild(mark);
            m_names[i] = label(card, kExecutionNames[i], {43.f, 22.f}, .3f, 80.f, ccWHITE, false, "bigFont.fnt");
            label(card, isStandardExecution(i) ? "Standard" : "Membership", {43.f, 9.f}, .32f, 80.f,
                  isStandardExecution(i) ? kIce : ccc3(255, 205, 110), false, "goldFont.fnt");
            auto* item = CCMenuItemSpriteExtra::create(card, this, menu_selector(FinisherPopup::onChoice));
            item->m_scaleMultiplier = 1.03f;
            item->setTag(i);
            item->setID(fmt::format("finisher-{}", i));
            if (!canUseExecution(i, member)) previewOpacity(item, 120);
            m_list->addCard(item);
        }
        m_list->setFilter(executionInCategory);
        button(m_buttonMenu, this, menu_selector(FinisherPopup::onPrevious), "<", {25.f, 83.f}, .38f);
        button(m_buttonMenu, this, menu_selector(FinisherPopup::onNext), ">", {415.f, 83.f}, .38f);
        for (int category = 0; category < 3; ++category) {
            auto* tab = CCNode::create();
            tab->setContentSize({100.f, 16.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(tab->getContentSize());
            bg->setPosition({50.f, 8.f});
            tab->addChild(bg, -1);
            m_categoryBackgrounds[category] = bg;
            m_categoryLabels[category] = label(tab, kAuraCategoryNames[category], {50.f, 8.f}, .28f, 94.f, kIce);
            auto* item = CCMenuItemSpriteExtra::create(tab, this, menu_selector(FinisherPopup::onCategory));
            item->setID(fmt::format("finisher-category-{}", category));
            item->setTag(category);
            item->setPosition({108.f + category * 112.f, 23.f});
            m_buttonMenu->addChild(item);
        }
        m_preview = std::clamp(m_draft.execution, 0, kExecutionCount - 1);
        selectCategory(AuraCategory::All);
        refresh();
        return true;
    }
    void selectCategory(AuraCategory category) {
        m_list->setCategory(category);
        for (int i = 0; i < 3; ++i) {
            bool const selected = i == static_cast<int>(category);
            m_categoryBackgrounds[i]->setColor(selected ? ccc3(32, 112, 98) : ccc3(15, 37, 68));
            m_categoryLabels[i]->setColor(selected ? ccc3(150, 255, 190) : kIce);
        }
    }
    void onCategory(CCObject* sender) {
        int const category = sender->getTag();
        if (category >= 0 && category < 3) selectCategory(static_cast<AuraCategory>(category));
    }
    // 카드 색과 상태 문구를 갱신하고 미리보기를 다시 재생한다.
    void refresh() {
        for (int i = 0; i < kExecutionCount; ++i) {
            bool const used = i == m_draft.execution, previewing = i == m_preview;
            m_cards[i]->setColor(used ? ccc3(32, 112, 98) : previewing ? ccc3(40, 70, 120) : ccc3(15, 37, 68));
            m_names[i]->setColor(used ? ccc3(150, 255, 190) : ccWHITE);
        }
        bool const member = Service::get().profile().membership;
        m_state->setString(m_preview == m_draft.execution ? "IN USE"
                           : canUseExecution(m_preview, member)
                               ? (isStandardExecution(m_preview) ? "STANDARD" : "MEMBERSHIP")
                               : "MEMBERSHIP - PREVIEW");
        m_state->limitLabelWidth(150.f, .3f, .15f);
        play();
    }
    // 선택한 피니셔를 두 플레이어 장면으로 재생한다. 0번(Random)이면 매번 다른 것을 골라
    // 보여주고,
    // 재생이 끝나면 일정 시간 뒤 자동으로 다시 재생한다.
    void play() {
        m_view->stopAllActions();
        m_view->removeAllChildrenWithCleanup(true);
        // 장면(505 x 132)을 미리보기 칸 높이에 맞춰 줄이고 가로 가운데에 둔다.
        float const scale = .8f * kStageH / 110.f;
        auto* holder = CCNode::create();
        holder->setScale(scale);
        holder->setPosition({(kStageW - 505.f * scale) / 2.f, 4.f * scale / .8f});
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
        m_caption->setString(m_preview == 0 ? fmt::format("Random: {}", kExecutionNames[kind]).c_str()
                                            : kExecutionNames[kind]);
        m_caption->limitLabelWidth(260.f, .5f, .2f);
        execution::play(stage, winner, loser, kind);
        m_view->runAction(CCSequence::create(CCDelayTime::create(std::max(3.8f, .85f + execution::length(kind))),
                                             CallFuncExt::create([this] { play(); }), nullptr));
    }
    void onPrevious(CCObject*) { m_list->scrollBy(-282.f); }
    void onNext(CCObject*) { m_list->scrollBy(282.f); }
    // 카드를 누르면 미리보기로 선택하고, 사용할 수 있으면 바로 저장한다.
    void onChoice(CCObject* sender) {
        int const choice = sender->getTag();
        if (choice < 0 || choice >= kExecutionCount || choice == m_preview) return;
        m_preview = choice;
        if (canUseExecution(choice, Service::get().profile().membership) && choice != m_draft.execution) {
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
class TitlesPopup final : public CustomizePopup {
    TapScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_summary = nullptr;
    std::optional<AchievementState> m_state;
    std::optional<AchievementRates> m_rates;
    std::set<int> m_open;
    bool m_ownedOnly = false;
    CCMenuItemSpriteExtra* m_filter = nullptr;

    // UI 구성. 목록 영역을 만든 뒤 다음 프레임에 load로 데이터를 요청한다.
    bool init(std::function<void()> saved) {
        // 높이 280: 296이면 왼쪽 위 닫기 버튼이 화면(높이 320) 위로 살짝 나간다. 목록이 그만큼 짧아진다.
        if (!begin(420.f, 280.f, "Achievements", std::move(saved))) return false;
        setID("versus-titles"_spr);
        m_summary = label(m_mainLayer, "", {175.f, 236.f}, .5f, 290.f, ccWHITE, false, "goldFont.fnt");
        m_filter = button(m_buttonMenu, this, menu_selector(TitlesPopup::onFilter), "All", {370.f, 236.f}, .42f,
                          "GJ_button_04.png");
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({396.f, 206.f});
        paper->setPosition({210.f, 122.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(220);
        m_mainLayer->addChild(paper);
        m_scroll = TapScrollLayer::create({396.f, 202.f}, [self = Weak<TitlesPopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->onRow(row->getTag());
        });
        m_scroll->setPosition({12.f, 21.f});
        m_mainLayer->addChild(m_scroll);
        m_status =
            label(m_mainLayer, "Loading achievements...", {210.f, 130.f}, .45f, 360.f, kIce, false, "goldFont.fnt");
        button(m_buttonMenu, this, menu_selector(TitlesPopup::onRetry), "Retry", {210.f, 96.f}, .45f)
            ->setID("titles-retry"_spr);
        runAction(CCSequence::create(CCDelayTime::create(0.f), CallFuncExt::create([this] { load(); }), nullptr));
        return true;
    }
    // 업적 상태와 달성률을 서버에서 받아온다. 실패하면 메시지와 Retry 버튼을 보여준다.
    // 달성률은 한 번만 받으며, 상태가 이미 있으면 받자마자 다시 그린다.
    void load() {
        m_status->setVisible(true);
        m_status->setString("Loading achievements...");
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
    // All / Unlocked 필터를 전환하고 목록을 다시 그린다.
    void onFilter(CCObject*) {
        m_ownedOnly = !m_ownedOnly;
        auto* sprite = ButtonSprite::create(m_ownedOnly ? "Unlocked" : "All", "goldFont.fnt",
                                            m_ownedOnly ? "GJ_button_01.png" : "GJ_button_04.png");
        sprite->setScale(.42f);
        m_filter->setNormalImage(sprite);
        m_filter->updateSprite();
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
    CCNode* titleRow(int title, CCSize size, float indent, std::string const& caption, bool header, bool open,
                     int badge = -1, std::string const& detail = "") {
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
            bg->setColor(equipped ? ccc3(32, 112, 98) : owned ? ccc3(24, 52, 92) : ccc3(16, 24, 40));
            bg->setOpacity(225);
            cell->addChild(bg, -1);
        }
        titleBadge(cell, badge >= 0 ? badge : title, {indent + 58.f, mid - (header ? 2.f : 0.f)}, header ? 1.12f : 1.f,
                   110.f, header || owned);
        float const textX = indent + 122.f;
        auto [state, fraction] = titleState(title);
        if (header)
            label(cell, caption, {textX, size.height - 8.f}, .3f, 190.f, ccc3(255, 221, 120), true, "goldFont.fnt");
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
    // 목록 전체를 다시 그린다. 기본 칭호, 특수 칭호, 시즌 1 최종 칭호 뒤에 계열을
    // (달성 개수, 비율, 다음 단계 진행률 순) 정렬해 붙이고, 열린 계열은 펼쳐서 얻은 것부터
    // 보여준다.
    // 이전 스크롤 위치(위쪽 기준)는 유지한다.
    void render() {
        if (!m_state) return;
        auto* content = m_scroll->m_contentLayer;
        bool const keepScroll = content->getChildrenCount() > 0;
        float const scrollFromTop = content->getContentSize().height + content->getPositionY();
        content->removeAllChildrenWithCleanup(true);
        int const total = kAchievementCount + (m_state->season2 ? kSeason2AchievementCount : 0);
        m_summary->setString((m_ownedOnly
                                  ? fmt::format("Unlocked {} / {}  -  Tap a group", m_state->unlocked, total)
                                  : fmt::format("Achievements {} / {}  -  Tap a group", m_state->unlocked, total))
                                 .c_str());
        m_summary->limitLabelWidth(290.f, .5f, .2f);
        constexpr float headerH = 40.f, rowH = 32.f, gap = 3.f;
        struct Item {
            int tag, title;
            float height;
            bool header;
            int series;
        };
        std::vector<Item> items;
        items.push_back({0, 0, rowH, false, -1});
        if (m_state->ownsTitle(kDeveloperTitle)) items.push_back({kDeveloperTitle, kDeveloperTitle, rowH, false, -1});
        if (m_state->ownsTitle(kAdminTitle)) items.push_back({kAdminTitle, kAdminTitle, rowH, false, -1});
        if (m_state->s1Final) items.push_back({m_state->s1Final, m_state->s1Final, rowH, false, -1});
        int const seriesCount = static_cast<int>(kSeries.size());
        std::vector<int> order;
        std::vector<std::tuple<int, float, float>> keys(kSeries.size());
        for (int s = 0; s < seriesCount; ++s) {
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
                    if (m_state->ownsTitle(series.titles[i]) == owned)
                        items.push_back({series.titles[i], series.titles[i], rowH, false, s});
            }
        }
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
                cell = titleRow(item.title, {388.f - indent, item.height}, 0.f, "", false, false);
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
