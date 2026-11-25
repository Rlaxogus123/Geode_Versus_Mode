// Season 2 듀얼(레이팅이 바뀌지 않는 친선 대결) 요청 UI (VersusUI.cpp에 #include되는 조각).
// Season2DuelPopup은 상대를 이름으로 찾거나 최근 듀얼 상대(Rematch)에서 골라 요청을 보내는 팝업,
// Season2DuelTierPopup은 요청에 쓸 컵과 티어(맵 / 생명 규칙)를 고르는 팝업이다.
// 칼 모양 듀얼 아이콘: 이미지가 없으면 칼 스프라이트를 45도 돌려 대신 쓴다.
CCNode* season2DuelSword(float size) {
    auto* holder = art::artHolder(size);
    float const length = size * 1.25f, middle = length * .2585f;
    auto* sword = season2Sword(length);
    sword->setRotation(-45.f);
    sword->setPosition({size / 2.f + middle, size / 2.f - middle});
    holder->addChild(sword);
    return holder;
}
// 듀얼 아이콘 (s2-duel.png를 size 안에 맞춰 넣고, 이미지가 없으면 칼 아이콘으로 대체).
CCNode* season2DuelArt(float size) {
    auto* art = art::artSprite("s2-duel.png", size);
    if (!art) return season2DuelSword(size);
    auto* holder = art::artHolder(size);
    float const wide = art->getScaledContentSize().width;
    if (wide > size) art->setScale(art->getScale() * size / wide);
    art->setPosition({size / 2.f, size / 2.f});
    holder->addChild(art);
    return holder;
}

// 듀얼 요청을 보낼 컵(standard / demon)과 티어를 고르는 팝업. 컵 탭 + 티어 칩 격자(최대 18개)로
// 구성되고, 고른 티어의 맵 조건과 생명 규칙을 아래에 보여 준다. Send Duel은 서버에 요청을 보내
// 성공하면 팝업을 닫고 RankedState를 콜백으로 넘긴다.
class Season2DuelTierPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 290.f, ChipW = 134.f, ChipH = 26.f, PitchX = 140.f, PitchY = 28.f;
    static constexpr float GridTop = 194.f;
    std::string m_target, m_name;
    std::function<void(RankedState)> m_onState;
    std::function<void()> m_sent;
    std::array<std::optional<Season2Tiers>, 2> m_tiers;
    std::array<std::string, 2> m_errors;
    std::array<bool, 2> m_loaded{};
    int m_cup = 0;
    std::string m_choice;
    bool m_sending = false, m_closed = false;
    std::array<CCMenuItemSpriteExtra*, 2> m_tabs{};
    CCNode* m_grid = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCMenuItemSpriteExtra* m_send = nullptr;
    static char const* cupId(int index) { return index == 1 ? "demon" : "standard"; }
    // 창, 컵 탭, Send 버튼을 만들고 현재 랭크 컵을 초기 탭으로 한다. 두 컵의 티어 표를 각각 요청한다.
    bool init(std::string target, std::string name, std::function<void(RankedState)> onState,
              std::function<void()> sent) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_target = std::move(target);
        m_name = std::move(name);
        m_onState = std::move(onState);
        m_sent = std::move(sent);
        setID("season2-duel-tier"_spr);
        setTitle("Duel Tier", "goldFont.fnt", .75f);
        label(m_mainLayer, fmt::format("Duel {} at a tier of either cup: its maps and lives.", m_name),
              {W / 2.f, H - 44.f}, .5f, W - 40.f, kIce, false, "chatFont.fnt");
        for (int i = 0; i < 2; ++i) {
            m_tabs[i] = button(m_buttonMenu, this, menu_selector(Season2DuelTierPopup::onTab), "",
                               {W / 2.f + (i ? 72.f : -72.f), H - 68.f});
            m_tabs[i]->setTag(i);
        }
        m_cup = Service::get().rankedCup() == "demon" ? 1 : 0;
        m_info = label(m_mainLayer, "", {W / 2.f, 56.f}, .5f, W - 30.f, ccWHITE, false, "chatFont.fnt");
        m_send =
            button(m_buttonMenu, this, menu_selector(Season2DuelTierPopup::onSend), "Send Duel", {W / 2.f, 26.f}, .62f);
        m_send->setID("season2-duel-send"_spr);
        render();
        for (int i = 0; i < 2; ++i)
            Service::get().fetchSeason2Tiers(cupId(i), [self = Weak<Season2DuelTierPopup>(this),
                                                        i](std::optional<Season2Tiers> tiers, std::string error) {
                auto owner = self.lock();
                if (!owner || owner->m_closed) return;
                owner->m_loaded[i] = true;
                owner->m_tiers[i] = std::move(tiers);
                owner->m_errors[i] = std::move(error);
                owner->render();
            });
        return true;
    }
    // 현재 컵에서 고른 티어 행 (고르지 않았거나 표를 못 받았으면 nullptr).
    Season2TierRow const* chosen() const {
        if (!m_tiers[m_cup] || m_choice.empty()) return nullptr;
        for (auto const& tier : m_tiers[m_cup]->tiers)
            if (tier.name == m_choice) return &tier;
        return nullptr;
    }
    // 탭 / 티어 칩 격자 / 안내 문구 / Send 버튼 상태를 다시 구성한다. 칩은 높은 티어부터 3열로 놓는다.
    void render() {
        for (int i = 0; i < 2; ++i) paintChoice(m_tabs[i], season2CupName(cupId(i)).c_str(), i == m_cup, 120.f);
        if (m_grid) m_grid->removeFromParent();
        m_grid = CCNode::create();
        m_mainLayer->addChild(m_grid, 2);
        auto const& tiers = m_tiers[m_cup];
        if (!tiers) {
            label(m_grid,
                  !m_loaded[m_cup]          ? "LOADING..."
                  : m_errors[m_cup].empty() ? "Could not load the tiers."
                                            : m_errors[m_cup],
                  {W / 2.f, 130.f}, .4f, W - 40.f, kIce);
        } else {
            auto* chips = menu(m_grid);
            std::vector<Season2TierRow const*> rows;
            for (auto it = tiers->tiers.rbegin(); it != tiers->tiers.rend(); ++it) rows.push_back(&*it);
            for (size_t index = 0; index < rows.size() && index < 18; ++index) {
                auto const& tier = *rows[index];
                bool const picked = tier.name == m_choice;
                auto* chip = CCNode::create();
                chip->setContentSize({ChipW, ChipH});
                auto* back = NineSlice::create("square02b_001.png");
                back->setContentSize({ChipW, ChipH});
                back->setPosition({ChipW / 2.f, ChipH / 2.f});
                back->setColor(picked ? ccc3(120, 92, 22) : ccc3(12, 22, 44));
                back->setOpacity(picked ? 245 : 200);
                chip->addChild(back, -1);
                season2Face(chip, tier.tier, {15.f, ChipH / 2.f}, .3f);
                auto* name = Season2TierText::create();
                name->setPosition({78.f, ChipH / 2.f});
                chip->addChild(name, 2);
                name->set(tier.name, tier.tier, .26f, ChipW - 40.f, false);
                auto* item = CCMenuItemSpriteExtra::create(chip, this, menu_selector(Season2DuelTierPopup::onTier));
                item->setTag(static_cast<int>(rows[index] - tiers->tiers.data()));
                int const column = static_cast<int>(index % 3), line = static_cast<int>(index / 3);
                item->setPosition({W / 2.f + (column - 1) * PitchX, GridTop - line * PitchY});
                chips->addChild(item);
            }
        }
        auto const* tier = chosen();
        if (tier && tiers) {
            m_info->setString(fmt::format("{}  |  {}  |  {}", season2TierName(tier->name), season2MapText(*tier),
                                          season2LivesText(tier->lives, tiers->livesByLength, tier->lengths))
                                  .c_str());
            m_info->setColor(ccc3(255, 221, 120));
        } else {
            m_info->setString("Choose a tier. The duel never changes either player's rating.");
            m_info->setColor(kMuted);
        }
        m_info->limitLabelWidth(W - 30.f, .5f, .25f);
        enabled(m_send, tier != nullptr && !m_sending);
    }
    // 탭을 바꾸면 선택한 티어를 비운다. 전송 중에는 무시.
    void onTab(CCObject* sender) {
        int const cup = std::clamp(sender->getTag(), 0, 1);
        if (cup == m_cup || m_sending) return;
        m_cup = cup;
        m_choice.clear();
        render();
    }
    // 티어 칩 선택. 칩을 누르는 도중 다시 그리면 메뉴가 깨지므로 다음 메인 스레드 틱에 render한다.
    void onTier(CCObject* sender) {
        int const index = sender->getTag();
        auto const& tiers = m_tiers[m_cup];
        if (m_sending || !tiers || index < 0 || index >= static_cast<int>(tiers->tiers.size())) return;
        m_choice = tiers->tiers[static_cast<size_t>(index)].name;
        Loader::get()->queueInMainThread([self = Weak<Season2DuelTierPopup>(this)] {
            if (auto owner = self.lock(); owner && !owner->m_closed) owner->render();
        });
    }
    // 듀얼 요청 전송. 치트 감지로 막힌 상태면 보내지 않는다. 성공하면 팝업을 닫고 sent / onState를 호출하고,
    // 실패하면 점검 공지 또는 오류 알림을 띄운다.
    void onSend(CCObject*) {
        auto const* tier = chosen();
        if (!tier || m_sending) return;
        if (rankedBlockedByCheat()) return;
        m_sending = true;
        render();
        Service::get().challengeSeason2(
            m_target, cupId(m_cup), tier->name,
            [self = Weak<Season2DuelTierPopup>(this)](std::optional<RankedState> state, std::string error) {
                auto owner = self.lock();
                if (!owner || owner->m_closed) return;
                owner->m_sending = false;
                if (!state) {
                    owner->render();
                    if (!showMaintenanceNotice(error))
                        FLAlertLayer::create(
                            "Duel",
                            (error.empty() ? std::string("The duel request could not be sent.") : plainAlertText(error))
                                .c_str(),
                            "OK")
                            ->show();
                    return;
                }
                auto onState = owner->m_onState;
                auto sent = owner->m_sent;
                owner->m_closed = true;
                owner->Popup::onClose(nullptr);
                if (sent) sent();
                if (onState) onState(std::move(*state));
            });
    }
    void onClose(CCObject* sender) override {
        m_closed = true;
        Popup::onClose(sender);
    }

public:
    static Season2DuelTierPopup* create(std::string target, std::string name, std::function<void(RankedState)> onState,
                                        std::function<void()> sent) {
        auto* result = new Season2DuelTierPopup;
        if (result->init(std::move(target), std::move(name), std::move(onState), std::move(sent))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 듀얼 상대를 고르는 팝업: 요청 수신 토글, 차단 목록(Blocked) 열기, 이름 검색, 최근 듀얼 상대(Rematch).
// 목록에서 "available"인 상대에게만 Duel 버튼이 나타나고, 누르면 티어 선택 팝업이 열린다.
class Season2DuelPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 400.f, H = 270.f, RowTop = 132.f, RowPitch = 27.f, RowH = 25.f;
    static constexpr float InputLeft = 15.f, InputRight = 254.f, SearchX = 289.f, RematchX = 355.f, ModeW = 80.f;
    static constexpr float ModeScale = .72f * .85f;
    // 검색 / Rematch 버튼의 모양을 같은 크기로 그린다.
    static void paintMode(CCMenuItemSpriteExtra* item, char const* text) {
        auto* sprite =
            ButtonSprite::create(text, static_cast<int>(ModeW), true, "goldFont.fnt", "GJ_button_01.png", 28.f, .62f);
        sprite->setScale(ModeScale);
        item->setSprite(sprite);
    }
    // 한 번에 보여 줄 결과 행 수. m_rematch가 true면 최근 상대 목록(m_recent), 아니면 검색 결과(m_found)를 표시한다.
    static constexpr int kRows = 5;
    std::function<void(RankedState)> m_onState;
    bool m_receive = true, m_saving = false, m_searching = false, m_closed = false;
    bool m_rematch = false, m_loadingRecent = false;
    int m_recentSerial = 0;
    std::string m_searchHint = "Find a player by their exact name (any case).", m_recentHint;
    CCMenuItemToggler* m_toggle = nullptr;
    TextInput* m_input = nullptr;
    CCMenuItemSpriteExtra* m_search = nullptr;
    CCMenuItemSpriteExtra* m_rematchButton = nullptr;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    std::vector<Season2DuelPlayer> m_found;
    std::vector<Season2DuelPlayer> m_recent;
    std::vector<Season2DuelPlayer> const& shown() const { return m_rematch ? m_recent : m_found; }
    // 요청 수신 토글, 차단 목록 버튼, 이름 입력(최대 20자), Search / Rematch 버튼을 만든다.
    bool init(std::function<void(RankedState)> onState, bool receive) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_onState = std::move(onState);
        m_receive = receive;
        setID("season2-duel-window"_spr);
        setTitle("Duel", "goldFont.fnt", .8f);
        label(m_mainLayer, "Duels never change your rating.", {W / 2.f, H - 44.f}, .34f, W - 40.f, ccc3(255, 214, 90),
              false, "bigFont.fnt");
        auto* options = menu(m_mainLayer);
        m_toggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(Season2DuelPopup::onReceive), .6f);
        m_toggle->setPosition({30.f, H - 72.f});
        m_toggle->toggle(m_receive);
        m_toggle->setID("season2-duel-receive"_spr);
        options->addChild(m_toggle);
        label(m_mainLayer, "Receive duel requests", {46.f, H - 69.f}, .34f, 200.f, ccWHITE, true);
        label(m_mainLayer, "Off: nobody can send you one", {46.f, H - 82.f}, .42f, 200.f, kMuted, true, "chatFont.fnt");
        button(options, this, menu_selector(Season2DuelPopup::onBlocked), "Blocked", {W - 43.f, H - 14.f}, .5f,
               "GJ_button_04.png")
            ->setID("season2-duel-blocked"_spr);
        float const inputW = InputRight - InputLeft;
        m_input = TextInput::create(inputW, "Player name");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(20);
        m_input->setScale(.75f);
        m_input->setContentSize({inputW / .75f, m_input->getContentHeight()});
        m_input->setPosition({InputLeft + inputW / 2.f, H - 110.f});
        styleInput(m_input);
        m_mainLayer->addChild(m_input, 4);
        m_search =
            button(options, this, menu_selector(Season2DuelPopup::onSearch), "Search", {SearchX, H - 110.f}, .55f);
        m_search->setID("season2-duel-search"_spr);
        m_rematchButton = button(options, this, menu_selector(Season2DuelPopup::onRematch), "Rematch",
                                 {RematchX, H - 110.f}, .55f, "GJ_button_04.png");
        m_rematchButton->setID("season2-duel-rematch"_spr);
        paintMode(m_search, "Search");
        paintMode(m_rematchButton, "Rematch");
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows, 2);
        m_hint = label(m_mainLayer, "", {W / 2.f, 90.f}, .5f, W - 40.f, kIce, false, "chatFont.fnt");
        render();
        return true;
    }
    void onExit() override {
        if (m_input) m_input->unfocus();
        Popup::onExit();
    }
    // 현재 목록(검색 결과 또는 최근 상대)을 행으로 그린다: 아이콘, 이름, 상태, 가능한 상대에게는 Duel 버튼.
    // 목록이 비면 안내 문구를 보여 준다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        enabled(m_search, !m_searching || m_rematch);
        enabled(m_rematchButton, !m_loadingRecent);
        m_toggle->setEnabled(!m_saving);
        auto const& rows = shown();
        m_hint->setVisible(rows.empty());
        m_hint->setString((m_rematch ? m_recentHint : m_searchHint).c_str());
        m_hint->limitLabelWidth(W - 40.f, .5f, .2f);
        if (rows.empty()) return;
        auto* actions = menu(m_rows);
        for (int row = 0; row < kRows && row < static_cast<int>(rows.size()); ++row) {
            auto const& found = rows[static_cast<size_t>(row)];
            float const y = RowTop - row * RowPitch;
            auto* cell = panel(m_rows, {14.f, y - RowH / 2.f}, {W - 28.f, RowH}, false);
            PlayerProfile shown = found.player.value_or(PlayerProfile{});
            if (shown.name.empty()) shown.name = found.name;
            float const mid = RowH / 2.f;
            player(cell, shown, {16.f, mid}, .45f);
            auto* name = label(cell, found.name, {32.f, mid}, .34f, 130.f, ccWHITE, true);
            if (found.player) styleNickname(name, found.player->membership, found.player->cosmetics);
            label(cell, found.status, {found.available ? 214.f : 250.f, mid}, .5f, found.available ? 120.f : 190.f,
                  found.available ? ccc3(130, 255, 160) : kMuted, false, "chatFont.fnt");
            if (found.available) {
                auto* duel =
                    button(actions, this, menu_selector(Season2DuelPopup::onDuel), "Duel", {W - 50.f, y}, .45f);
                duel->setTag(row);
            }
        }
    }
    // 요청 수신 토글: 서버에 설정을 저장하고, 실패하면 토글을 되돌린다. 팝업이 이미 닫혔으면 무시.
    void onReceive(CCObject*) {
        if (m_saving) return;
        bool const value = !m_toggle->isToggled();
        m_saving = true;
        m_toggle->setEnabled(false);
        Loader::get()->queueInMainThread([self = Weak<Season2DuelPopup>(this), value] {
            auto owner = self.lock();
            if (!owner || owner->m_closed) return;
            DuelSettings settings;
            settings.enabled = value;
            settings.anonymous = true;
            Service::get().setDuelSettings(settings, [self, value](std::optional<DuelBlockList> list,
                                                                   std::string error) {
                auto owner = self.lock();
                if (!owner || owner->m_closed) return;
                owner->m_saving = false;
                if (!list) {
                    owner->m_toggle->toggle(owner->m_receive);
                    if (!showMaintenanceNotice(error))
                        FLAlertLayer::create(
                            "Duel",
                            (error.empty() ? std::string("Could not save. Try again.") : plainAlertText(error)).c_str(),
                            "OK")
                            ->show();
                } else {
                    owner->m_receive = list->settings.enabled;
                    owner->m_toggle->toggle(owner->m_receive);
                    Notification::create(value ? "Duel requests: ON" : "Duel requests: OFF", NotificationIcon::Success,
                                         1.5f)
                        ->show();
                }
                owner->m_toggle->setEnabled(true);
            });
        });
    }
    // 차단 목록 팝업을 연다 (Season 2 모드).
    void onBlocked(CCObject*) {
        if (auto* popup = DuelRequestsPopup::create({}, true)) popup->show();
    }
    // 다음 메인 스레드 틱에 다시 그린다 (버튼 콜백 안에서 자기 자신의 노드를 지우지 않기 위함).
    void renderLater() {
        Loader::get()->queueInMainThread([self = Weak<Season2DuelPopup>(this)] {
            if (auto owner = self.lock(); owner && !owner->m_closed) owner->render();
        });
    }
    // 이름 검색. 앞뒤 공백을 지우고 서버에 검색을 요청하며, 검색 중에 Rematch 보기에서 다시 누르면 검색 보기로 돌아간다.
    void onSearch(CCObject*) {
        if (!m_input) return;
        if (m_searching) {
            if (m_rematch) {
                m_rematch = false;
                ++m_recentSerial;
                m_loadingRecent = false;
                renderLater();
            }
            return;
        }
        bool const back = m_rematch;
        m_rematch = false;
        ++m_recentSerial;
        m_loadingRecent = false;
        std::string name = m_input->getString();
        name.erase(0, name.find_first_not_of(' '));
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) {
            if (!back) {
                m_found.clear();
                m_searchHint = "Enter a player's name.";
            }
            renderLater();
            return;
        }
        m_input->unfocus();
        m_searching = true;
        m_found.clear();
        m_searchHint = "Searching...";
        renderLater();
        Service::get().searchSeason2Duel(name, [self = Weak<Season2DuelPopup>(this),
                                                name](std::vector<Season2DuelPlayer> players, std::string error) {
            auto owner = self.lock();
            if (!owner || owner->m_closed) return;
            owner->m_searching = false;
            owner->m_found = std::move(players);
            owner->m_searchHint = !error.empty() ? plainAlertText(error) : fmt::format("No player named \"{}\".", name);
            owner->renderLater();
        });
    }
    // 최근 듀얼 상대를 불러온다. m_recentSerial로 오래된 응답은 버린다.
    void onRematch(CCObject*) {
        if (m_loadingRecent) return;
        m_rematch = true;
        m_loadingRecent = true;
        m_recent.clear();
        m_recentHint = "Loading your duels...";
        if (m_input) m_input->unfocus();
        renderLater();
        int const serial = ++m_recentSerial;
        Service::get().recentSeason2Duels(
            [self = Weak<Season2DuelPopup>(this), serial](std::vector<Season2DuelPlayer> players, std::string error) {
                auto owner = self.lock();
                if (!owner || owner->m_closed || serial != owner->m_recentSerial) return;
                owner->m_loadingRecent = false;
                owner->m_recent = std::move(players);
                owner->m_recentHint =
                    !error.empty() ? plainAlertText(error) : std::string("You have not dueled anyone yet.");
                owner->renderLater();
            });
    }
    // 선택한 상대에게 보낼 티어를 고르는 팝업을 열고, 요청이 전송되면 이 팝업도 닫는다.
    void onDuel(CCObject* sender) {
        int const row = sender ? sender->getTag() : -1;
        auto const& rows = shown();
        if (row < 0 || row >= static_cast<int>(rows.size()) || !rows[static_cast<size_t>(row)].available) return;
        auto const& found = rows[static_cast<size_t>(row)];
        if (auto* picker =
                Season2DuelTierPopup::create(found.uid, found.name, m_onState, [self = Weak<Season2DuelPopup>(this)] {
                    if (auto owner = self.lock(); owner && !owner->m_closed) owner->onClose(nullptr);
                }))
            picker->show();
    }
    void onClose(CCObject* sender) override {
        m_closed = true;
        Popup::onClose(sender);
    }

public:
    static Season2DuelPopup* create(std::function<void(RankedState)> onState, bool receive) {
        auto* result = new Season2DuelPopup;
        if (result->init(std::move(onState), receive)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};
