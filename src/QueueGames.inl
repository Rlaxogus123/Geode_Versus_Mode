// 매치메이킹 대기 중에 즐기는 미니게임 팝업들(게임 선택 창, 지뢰찾기).
// 대기열은 계속 유지되며, 매칭되면 서버 흐름에서 이 팝업들이 닫힌다.
// 대기 미니게임 팝업의 공통 부모. 닫힘 처리를 한 번만 하도록 보장하고 닫힐 때 콜백을 부른다.
class QueueGamePopup : public ::versus::DeferredPopup {
    bool m_queueDismissed = false;
    std::function<void()> m_queueClosed;

protected:
    void onClose(CCObject*) override { dismiss(); }

public:
    void setClosedCallback(std::function<void()> closed) { m_queueClosed = std::move(closed); }
    // 한 번만 실행되는 닫기. 콜백을 먼저 꺼낸 뒤 입력/스케줄을 끊고 팝업을 닫은 다음 콜백을 호출한다.
    void dismiss() {
        if (m_queueDismissed) return;
        Ref<QueueGamePopup> keepAlive(this);
        m_queueDismissed = true;
        auto closed = std::move(m_queueClosed);
        setKeyboardEnabled(false);
        unscheduleAllSelectors();
        stopAllActions();
        Popup::onClose(nullptr);
        if (closed) closed();
    }
    // 화면에서 빠질 때(강제 포함) 콜백을 버리고 입력과 스케줄을 모두 끊는다.
    void onExit() override {
        m_queueDismissed = true;
        m_queueClosed = {};
        setTouchEnabled(false);
        setKeypadEnabled(false);
        setKeyboardEnabled(false);
        unscheduleAllSelectors();
        stopAllActions();
        Popup::onExit();
    }
};

// 미니게임 선택 창(Runner / Minesweeper). 선택 결과를 tag(0, 1)로 콜백에 넘긴다.
class QueueGamePicker final : public QueueGamePopup {
    std::function<void(int)> m_choose;
    // searching: 매칭 중에 열렸는지. 미니게임은 매칭 없이도 열 수 있으므로(Game 버튼) 매칭 안내는 그때만 보인다.
    bool init(std::function<void(int)> choose, bool searching) {
        if (!Popup::init(340.f, 185.f, "GJ_square02.png")) return false;
        m_choose = std::move(choose);
        setTitle(searching ? "While You Wait" : "Mini Games", "bigFont.fnt", .6f);
        if (searching)
            label(m_mainLayer, "Matchmaking stays active. Games close when matched.", {170.f, 132.f}, .4f, 308.f,
                  kIce, false, "chatFont.fnt");
        auto* runner =
            button(m_buttonMenu, this, menu_selector(QueueGamePicker::onChoose), "Runner", {170.f, 93.f}, .62f);
        runner->setTag(0);
        auto* mines =
            button(m_buttonMenu, this, menu_selector(QueueGamePicker::onChoose), "Minesweeper", {170.f, 49.f}, .62f);
        mines->setTag(1);
        return true;
    }
    void onChoose(CCObject* sender) {
        Ref<QueueGamePicker> keepAlive(this);
        int choice = sender->getTag();
        auto done = std::move(m_choose);
        dismiss();
        if (done) done(choice);
    }

public:
    static QueueGamePicker* create(std::function<void(int)> choose, bool searching) {
        auto* popup = new QueueGamePicker;
        if (popup->init(std::move(choose), searching)) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// 지뢰찾기 팝업. 판 상태는 mines::Board가 가지고 있고, 이 클래스는 타일 표시와 입력만 맡는다.
class QueueMinesweeper final : public QueueGamePopup {
    mines::Board m_board;
    std::array<NineSlice*, mines::Board::COUNT> m_tiles{};
    std::array<CCLabelBMFont*, mines::Board::COUNT> m_numbers{};
    std::array<CCSprite*, mines::Board::COUNT> m_flags{}, m_mines{};
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_mode = nullptr;
    bool m_flagMode = false;
    std::function<void()> m_chooseGame;
    void onGames(CCObject*) {
        Ref<QueueMinesweeper> keepAlive(this);
        auto choose = std::move(m_chooseGame);
        dismiss();
        if (choose) choose();
    }
    // Board 상태에 맞춰 타일 색, 숫자, 깃발/지뢰 아이콘, 상태 문구, 모드 버튼을 갱신한다.
    void refresh() {
        static constexpr ccColor3B colors[] = {{230, 235, 255}, {90, 195, 255},  {105, 244, 140},
                                               {255, 116, 98},  {195, 151, 255}, {255, 187, 102},
                                               {100, 241, 236}, {245, 143, 208}, {245, 245, 245}};
        for (int i = 0; i < mines::Board::COUNT; ++i) {
            auto const& c = m_board.cell(i);
            bool showMine = c.mine && m_board.finished();
            m_tiles[i]->setColor(showMine ? ccc3(147, 43, 46) : c.open ? ccc3(20, 32, 50) : ccc3(58, 97, 151));
            std::string value = !showMine && !c.flag && c.open && c.adjacent ? std::to_string(c.adjacent) : "";
            if (m_flags[i]) m_flags[i]->setVisible(c.flag && !showMine);
            if (m_mines[i]) m_mines[i]->setVisible(showMine);
            m_numbers[i]->setString(value.c_str());
            m_numbers[i]->setColor(c.flag ? ccc3(255, 215, 86) : colors[std::clamp(c.adjacent, 0, 8)]);
        }
        m_status->setString(m_board.state() == mines::Board::State::Won ? "CLEARED!"
                            : m_board.state() == mines::Board::State::Lost
                                ? "BOOM! TRY AGAIN"
                                : fmt::format("{} MINES  |  {} SAFE LEFT", mines::Board::MINES - m_board.flags(),
                                              mines::Board::COUNT - mines::Board::MINES - m_board.opened())
                                      .c_str());
        m_status->limitLabelWidth(305.f, .32f, .2f);
        auto* sprite = ButtonSprite::create(m_flagMode ? "Mark" : "Reveal", "bigFont.fnt", "GJ_button_01.png");
        auto* gem = CCSprite::createWithSpriteFrameName("checkpoint_01_001.png");
        if (gem) {
            gem->setScale(.55f);
            gem->setPosition({-12.f, sprite->getContentSize().height / 2});
            sprite->addChild(gem);
        }
        sprite->setScale(.48f);
        m_mode->setSprite(sprite);
    }
    // 칸 선택. Mark 모드면 깃발, 아니면 열기. 게임이 막 끝난 순간에 승리/폭발 효과음을 한 번 낸다.
    void onCell(CCObject* sender) {
        bool finished = m_board.finished();
        if (m_flagMode)
            m_board.flag(sender->getTag());
        else
            m_board.reveal(sender->getTag());
        if (!finished && m_board.finished()) {
            if (m_board.state() == mines::Board::State::Won)
                audio::playBuiltin("reward01.ogg");
            else {
                auto path = MusicDownloadManager::sharedState()->pathForSFX(4451);
                if (CCFileUtils::sharedFileUtils()->isFileExist(path.c_str()))
                    audio::playBuiltin(path.c_str());
                else
                    audio::playBuiltin("explode_11.mp3");
            }
        }
        refresh();
    }
    void onMode(CCObject*) {
        m_flagMode = !m_flagMode;
        refresh();
    }
    // 새 게임: 새 시드로 판을 다시 만든다.
    void onRestart(CCObject*) {
        m_board = mines::Board(std::random_device{}());
        m_flagMode = false;
        refresh();
    }
    // 8열 격자의 타일 버튼과 하단 버튼을 만든다. 폭발 효과음(SFX 4451)이 없으면 미리 받아 둔다.
    bool init() {
        if (!Popup::init(350.f, 310.f, "GJ_square02.png")) return false;
        setTitle("Minesweeper", "bigFont.fnt", .6f);
        label(m_mainLayer, "FIRST CLICK SAFE", {175.f, 269.f}, .28f, 316.f, kIce);  // (매칭 없이도 열리므로 매칭 문구 없음)
        m_board = mines::Board(std::random_device{}());
        loadedFrame("diffIcon_02_btn_001.png", "GJ_GameSheet03.plist");
        auto* music = MusicDownloadManager::sharedState();
        if (!CCFileUtils::sharedFileUtils()->isFileExist(music->pathForSFX(4451).c_str())) music->downloadSFX(4451);
        for (int i = 0; i < mines::Board::COUNT; ++i) {
            auto* card = CCSprite::create();
            card->setContentSize({23.f, 23.f});
            auto* tile = NineSlice::create("square02b_001.png");
            tile->setContentSize({23.f, 23.f});
            tile->setPosition({11.5f, 11.5f});
            card->addChild(tile);
            m_tiles[i] = tile;
            m_numbers[i] = label(card, "", {11.5f, 11.5f}, .43f, 20.f);
            for (int type = 0; type < 2; ++type) {
                auto* image = loadedFrameSprite(type ? "diffIcon_02_btn_001.png" : "checkpoint_01_001.png");
                if (!image) continue;
                image->setScale(18.f /
                                std::max(1.f, std::max(image->getContentSize().width, image->getContentSize().height)));
                image->setPosition({11.5f, 11.5f});
                card->addChild(image, 2);
                (type ? m_mines[i] : m_flags[i]) = image;
            }
            auto* item = CCMenuItemSpriteExtra::create(card, this, menu_selector(QueueMinesweeper::onCell));
            item->m_scaleMultiplier = 1.03f;
            item->setTag(i);
            item->setPosition({87.5f + (i % 8) * 25.f, 249.f - (i / 8) * 25.f});
            m_buttonMenu->addChild(item);
        }
        m_status = label(m_mainLayer, "", {175.f, 53.f}, .32f, 310.f, kIce);
        m_mode = button(m_buttonMenu, this, menu_selector(QueueMinesweeper::onMode), "Reveal", {102.f, 26.f}, .48f);
        button(m_buttonMenu, this, menu_selector(QueueMinesweeper::onRestart), "New Game", {250.f, 26.f}, .48f);
        refresh();
        return true;
    }

public:
    // 게임 선택 창으로 돌아가는 버튼을 단다.
    void setGameSwitcher(std::function<void()> choose) {
        m_chooseGame = std::move(choose);
        iconButton(m_buttonMenu, this, menu_selector(QueueMinesweeper::onGames), "GJ_gpgBtn_001.png", {323.f, 288.f},
                   25.f);
    }
    static QueueMinesweeper* create() {
        auto* popup = new QueueMinesweeper;
        if (popup->init()) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};
