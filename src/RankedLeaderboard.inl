// Ranked 레이팅 순위표(TOP 100)와 플레이어 검색 팝업.
// 구성: 순위 한 줄을 그리는 rankedRow, 이름 검색 팝업 RankedPlayerSearchPopup,
// TOP 100 + 내 순위를 보여 주는 RankedLeaderboardPopup. 서버 응답은 Service 콜백으로 받는다.
//
// 순위표/검색 결과 한 줄(순위, 아이콘, 닉네임, 칭호, 티어 얼굴, 레이팅)을 cell 안에 그린다.
// 1~3위는 금색, 1000위 이상은 글자를 줄인다.
void rankedRow(CCNode* cell, RankedProfile const& row) {
    label(cell, row.globalRank > 0 ? fmt::format("#{}", row.globalRank) : std::string("-"), {23.f, 16.f},
          row.globalRank >= 1000 ? .24f : .29f, 38.f,
          row.globalRank > 0 && row.globalRank <= 3 ? ccc3(255, 206, 105) : ccWHITE);
    auto* icon = SimplePlayer::create(
        std::clamp(row.player.icon, 1, std::max(1, GameManager::sharedState()->countForType(IconType::Cube))));
    applyIconLook(icon, row.player.color1, row.player.color2, row.player.glow);
    icon->setScale(.65f);
    icon->setPosition({59.f, 16.f});
    cell->addChild(icon);
    rankedNickname(cell, row.player, row.globalRank, {169.f, 21.f}, .33f, 178.f);
    titleBadge(cell, row.player.cosmetics.title, {169.f, 8.f}, .72f, 170.f, false);
    rankedFace(cell, row.tier, {278.f, 16.f}, .52f);
    rankedGradient(label(cell, fmt::format("R{:.0f}", row.rating), {331.f, 16.f}, .43f, 62.f), row.tier);
}

// 닉네임으로 랭크 플레이어를 찾는 팝업. 결과 행을 누르면 PlayerPopup을 연다.
class RankedPlayerSearchPopup final : public ::versus::DeferredPopup {
    TextInput* m_query = nullptr;
    CCLabelBMFont* m_status = nullptr;
    // 닫힐 때 입력창 포커스를 풀어 키보드 입력이 남지 않게 한다.
    void onExit() override {
        if (m_query) m_query->defocus();
        Popup::onExit();
    }

    TapScrollLayer* m_scroll = nullptr;
    std::vector<RankedProfile> m_rows;
    bool m_loading = false;
    void openRow(int index) {
        if (index >= 0 && index < static_cast<int>(m_rows.size()))
            if (auto* popup = PlayerPopup::create(m_rows[index].player)) popup->show();
    }
    // m_rows로 스크롤 목록을 다시 만든다. 행 높이는 34이고 위에서부터 쌓는다.
    void fill() {
        if (m_scroll) m_scroll->removeFromParent();
        // 팝업이 먼저 닫혀도 안전하도록 Weak 참조로 콜백을 건다.
        m_scroll =
            TapScrollLayer::create(CCSize{370.f, 186.f}, [self = Weak<RankedPlayerSearchPopup>(this)](CCNode* row) {
                if (auto owner = self.lock()) owner->openRow(row->getTag());
            });
        m_scroll->setPosition({15.f, 20.f});
        m_mainLayer->addChild(m_scroll);
        float const height = std::max(186.f, m_rows.size() * 34.f);
        m_scroll->m_contentLayer->setContentSize({370.f, height});
        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto* holder = CCNode::create();
            holder->setContentSize({370.f, 32.f});
            auto* cell = panel(holder, {0.f, 0.f}, {370.f, 32.f}, false);
            holder->setAnchorPoint({.5f, .5f});
            holder->setTag(static_cast<int>(i));
            holder->setPosition({185.f, height - 17.f - i * 34.f});
            m_scroll->m_contentLayer->addChild(holder);
            rankedRow(cell, m_rows[i]);
        }
        m_scroll->scrollToTop();
    }
    // 입력값 앞뒤 공백을 지우고 검색을 요청한다. 요청 중에는 중복 요청을 무시한다.
    void onSearch(CCObject*) {
        if (m_loading) return;
        auto query = std::string(m_query->getString());
        query.erase(0, query.find_first_not_of(' '));
        while (!query.empty() && query.back() == ' ') query.pop_back();
        if (query.empty()) {
            m_status->setString("Enter a player name.");
            m_status->setVisible(true);
            return;
        }
        m_loading = true;
        m_status->setString("Searching...");
        m_status->setVisible(true);
        // 응답 후 목록을 갱신하고, 오류 메시지나 결과 없음 문구를 상태 라벨에 표시한다.
        Service::get().searchRankedPlayers(
            query, [self = Weak<RankedPlayerSearchPopup>(this)](std::vector<RankedProfile> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_loading = false;
                owner->m_rows = std::move(rows);
                owner->fill();
                owner->m_status->setVisible(!detail.empty() || owner->m_rows.empty());
                owner->m_status->setString(!detail.empty() ? detail.c_str() : "No ranked player with that name.");
                owner->m_status->limitLabelWidth(340.f, .4f, .2f);
            });
    }
    bool init() override {
        if (!Popup::init(400.f, 280.f, "GJ_square02.png")) return false;
        setTitle("FIND A PLAYER", "bigFont.fnt", .6f);
        m_query = TextInput::create(230.f, "Player name");
        m_query->setCommonFilter(CommonFilter::Any);
        m_query->setMaxCharCount(20);
        m_query->setScale(.8f);
        m_query->setWidth(230.f / .8f);
        m_query->setPosition({165.f, 226.f});
        styleInput(m_query);
        m_query->setCallback([](std::string const&) {});
        m_mainLayer->addChild(m_query, 3);
        button(menu(m_mainLayer), this, menu_selector(RankedPlayerSearchPopup::onSearch), "Search", {330.f, 226.f},
               .55f);
        m_status = label(m_mainLayer, "Type a name and press Search", {200.f, 120.f}, .4f, 340.f, kIce);
        m_status->setZOrder(3);
        fill();
        return true;
    }

public:
    CREATE_FUNC(RankedPlayerSearchPopup);
};

// Rating TOP 100 팝업. 순위표와 내 정확한 순위(MY RANK)를 따로 요청해 채운다.
class RankedLeaderboardPopup final : public ::versus::DeferredPopup {
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_ownStatus = nullptr;
    CCNode* m_ownPanel = nullptr;
    std::vector<RankedProfile> m_rows;
    void openRow(int index) {
        if (index >= 0 && index < static_cast<int>(m_rows.size()))
            if (auto* popup = PlayerPopup::create(m_rows[index].player)) popup->show();
    }
    // 받아 온 순위 목록으로 스크롤 영역을 만든다. 비어 있으면 안내 문구를 보인다.
    void fill(std::vector<RankedProfile> const& rows) {
        m_status->setVisible(rows.empty());
        m_status->setString("No ranked players yet");
        auto* scroll =
            TapScrollLayer::create(CCSize{370.f, 174.f}, [self = Weak<RankedLeaderboardPopup>(this)](CCNode* row) {
                if (auto owner = self.lock()) owner->openRow(row->getTag());
            });
        scroll->setPosition({15.f, 76.f});
        m_mainLayer->addChild(scroll);
        float height = std::max(174.f, rows.size() * 34.f);
        scroll->m_contentLayer->setContentSize({370.f, height});
        m_rows = rows;
        for (size_t i = 0; i < rows.size(); ++i) {
            auto const& row = rows[i];
            float y = height - 17.f - i * 34.f;
            auto* holder = CCNode::create();
            holder->setContentSize({370.f, 32.f});
            auto* cell = panel(holder, {0.f, 0.f}, {370.f, 32.f}, false);
            holder->setAnchorPoint({.5f, .5f});
            holder->setTag(static_cast<int>(i));
            holder->setPosition({185.f, y});
            scroll->m_contentLayer->addChild(holder);
            rankedRow(cell, row);
        }
        scroll->scrollToTop();
    }
    // 하단 패널에 내 순위를 그린다. 순위가 없으면 UNRANKED, 배치 전이면 레이팅을 R----로 표시한다.
    void fillOwn(RankedProfile const& row) {
        m_ownStatus->setVisible(false);
        label(m_ownPanel, "MY RANK", {37.f, 22.f}, .29f, 64.f, ccc3(126, 218, 255), false, "chatFont.fnt");
        auto rankText = row.globalRank > 0 ? fmt::format("#{}", row.globalRank) : std::string("UNRANKED");
        label(m_ownPanel, rankText, {91.f, 22.f}, row.globalRank > 0 ? .36f : .26f, 48.f,
              row.globalRank > 0 && row.globalRank <= 3 ? ccc3(255, 206, 105) : ccWHITE);
        auto* icon = SimplePlayer::create(
            std::clamp(row.player.icon, 1, std::max(1, GameManager::sharedState()->countForType(IconType::Cube))));
        applyIconLook(icon, row.player.color1, row.player.color2, row.player.glow);
        icon->setScale(.68f);
        icon->setPosition({128.f, 22.f});
        m_ownPanel->addChild(icon);
        rankedNickname(m_ownPanel, row.player, 0, {212.f, 22.f}, .35f, 140.f);
        rankedFace(m_ownPanel, row.tier, {296.f, 22.f}, .55f);
        rankedGradient(label(m_ownPanel, row.placed ? fmt::format("R{:.0f}", row.rating) : std::string("R----"),
                             {343.f, 22.f}, .43f, 50.f),
                       row.tier);
    }
    bool init() {
        if (!Popup::init(400.f, 300.f, "GJ_square02.png")) return false;
        setTitle("RATING TOP 100", "bigFont.fnt", .6f);
        auto* search = iconButton(menu(m_mainLayer), this, menu_selector(RankedLeaderboardPopup::onSearch),
                                  "gj_findBtn_001.png", {370.f, 274.f}, 24.f);
        search->setID("ranked-player-search"_spr);
        m_status = label(m_mainLayer, "Loading TOP 100...", {200.f, 163.f}, .45f, 350.f);
        m_ownPanel = panel(m_mainLayer, {15.f, 20.f}, {370.f, 46.f}, false);
        m_ownPanel->setID("my-exact-rank"_spr);
        m_ownStatus = label(m_ownPanel, "Loading my rank...", {185.f, 23.f}, .36f, 342.f, ccc3(126, 218, 255), false,
                            "chatFont.fnt");
        // TOP 100 목록 요청.
        Service::get().fetchRankedLeaderboard(
            [self = Weak<RankedLeaderboardPopup>(this)](std::vector<RankedProfile> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                if (!detail.empty()) {
                    owner->m_status->setString(detail.c_str());
                    owner->m_status->limitLabelWidth(350.f, .45f, .2f);
                } else
                    owner->fill(rows);
            });
        // 내 순위 요청(빈 인자 = 본인). 실패하면 사유를 패널에 보인다.
        Service::get().fetchRankedProfile(
            {}, [self = Weak<RankedLeaderboardPopup>(this)](std::optional<RankedProfile> rank, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                if (rank)
                    owner->fillOwn(*rank);
                else {
                    owner->m_ownStatus->setString((detail.empty() ? "My rank is unavailable" : detail).c_str());
                    owner->m_ownStatus->limitLabelWidth(342.f, .36f, .2f);
                }
            });
        return true;
    }
    void onSearch(CCObject*) {
        if (auto* popup = RankedPlayerSearchPopup::create()) popup->show();
    }

public:
    CREATE_FUNC(RankedLeaderboardPopup);
};
