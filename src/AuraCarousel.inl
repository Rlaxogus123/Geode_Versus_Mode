// 꾸미기 팝업의 카드 목록(오라·피니셔): 드래그와 마우스 휠로 가로 스크롤하는 CCMenu.
// 카테고리 필터에 맞는 카드만 배치하고, 화면 밖 카드의 미리보기 애니메이션은 멈춘다.
// 카드 폭과 간격은 create에서 정하고, 양쪽 여백은 보이는 카드가 목록 폭에 꽉 맞게 가운데 정렬되도록 계산한다.
// 화살표는 page()로 한 페이지(보이는 카드 수)씩 카드 경계에 맞춰 넘긴다.
class AuraCarousel : public CCMenu {
    // m_offset은 현재 가로 스크롤 거리, m_card/m_gap은 카드 폭과 카드 사이 간격, m_pad는 양쪽 여백.
    std::vector<CCMenuItemSpriteExtra*> m_cards;
    float m_offset = 0.f;
    float m_card = 86.f, m_gap = 8.f, m_pad = 4.f;
    CCPoint m_start{};
    float m_startOffset = 0.f;
    int m_touchID = -1;
    bool m_dragging = false, m_pressedCard = false;
    bool m_filterChanged = false;
    AuraCategory m_category = AuraCategory::All;
    using Filter = bool (*)(int, AuraCategory);
    Filter m_filter = auraInCategory;
    // 스크롤 위치가 바뀔 때마다 부른다(팝업이 화살표를 흐리게/밝게 바꾼다).
    std::function<void()> m_onScroll;

    float step() const { return m_card + m_gap; }
    // 필터를 통과한 카드 수.
    int includedCount() const {
        return static_cast<int>(std::count_if(m_cards.begin(), m_cards.end(),
                                              [this](auto* card) { return m_filter(card->getTag(), m_category); }));
    }
    // 스크롤할 수 있는 최대 거리(카드 전체 길이 - 보이는 폭).
    float extent() const {
        int const count = includedCount();
        float const total = 2.f * m_pad + count * step() - (count > 0 ? m_gap : 0.f);
        return std::max(0.f, total - getContentSize().width);
    }
    // 월드 좌표가 이 메뉴 영역 안인지 확인한다.
    bool contains(CCPoint world) {
        auto const p = convertToNodeSpace(world);
        auto const s = getContentSize();
        return p.x >= 0.f && p.x <= s.width && p.y >= 0.f && p.y <= s.height;
    }
    // 필터를 통과한 카드를 순서대로 배치하고, 화면에 보이는 카드만 표시/애니메이션을 켠다.
    void positionCards() {
        auto const size = getContentSize();
        int index = 0;
        for (auto* card : m_cards) {
            bool const included = m_filter(card->getTag(), m_category);
            float const x = m_pad + index * step() - m_offset;
            if (included) ++index;
            card->setPosition({x + m_card / 2.f, size.height / 2.f});
            bool const visible = included && x + m_card > 0.f && x < size.width;
            if (visible != card->isVisible()) {
                card->setVisible(visible);
                setPreviewRunning(card, visible);
            }
        }
    }
    // 노드와 자식의 scheduler/액션을 재귀적으로 재개 또는 일시정지한다.
    static void setPreviewRunning(CCNode* node, bool running) {
        if (running)
            node->resumeSchedulerAndActions();
        else
            node->pauseSchedulerAndActions();
        for (auto* child : CCArrayExt<CCNode*>(node->getChildren())) setPreviewRunning(child, running);
    }
    // 스크롤 거리를 0부터 extent() 사이로 제한해 적용한다.
    void setOffset(float value) {
        m_offset = std::clamp(value, 0.f, extent());
        positionCards();
        if (m_onScroll) m_onScroll();
    }

public:
    // size: 보이는 영역, card: 카드 폭, gap: 카드 사이 간격.
    static AuraCarousel* create(CCSize size, float card = 86.f, float gap = 8.f) {
        auto* result = new AuraCarousel;
        if (!result->CCMenu::init()) {
            delete result;
            return nullptr;
        }
        result->autorelease();
        result->m_card = std::max(1.f, card);
        result->m_gap = std::max(0.f, gap);
        int const fits = std::max(1, static_cast<int>((size.width + result->m_gap) / result->step() + .001f));
        result->m_pad = std::max(0.f, (size.width - (fits * result->step() - result->m_gap)) / 2.f);
        result->setContentSize(size);
        result->setAnchorPoint({0.f, 0.f});
        result->ignoreAnchorPointForPosition(false);
        result->setPosition({0.f, 0.f});
        result->setMouseEnabled(true);
        result->setID("cosmetics-aura-list"_spr);
        return result;
    }
    void addCard(CCMenuItemSpriteExtra* card) {
        addChild(card);
        m_cards.push_back(card);
        positionCards();
    }
    void scrollBy(float amount) { setOffset(m_offset + amount); }
    // 한 화면에 다 보이는 카드 수.
    int perPage() const {
        return std::max(1, static_cast<int>((getContentSize().width - 2.f * m_pad + m_gap) / step() + .001f));
    }
    // direction(-1/+1) 쪽으로 한 페이지 넘긴다. 드래그로 어중간하게 멈춰 있었어도 카드 경계에 맞춘다.
    void page(int direction) {
        float const at = std::round(m_offset / step());
        setOffset((at + direction * perPage()) * step());
    }
    bool atStart() const { return m_offset <= .5f; }
    bool atEnd() const { return m_offset >= extent() - .5f; }
    void setOnScroll(std::function<void()> callback) {
        m_onScroll = std::move(callback);
        if (m_onScroll) m_onScroll();
    }
    void setFilter(Filter filter) {
        m_filter = filter ? filter : auraInCategory;
        setOffset(0.f);
    }
    void setCategory(AuraCategory category) {
        m_category = category;
        m_filterChanged = m_touchID != -1;
        setOffset(0.f);
    }
    // 해당 카드가 보이도록(왼쪽에서 두 번째 자리쯤, 카드 경계에 맞춰) 스크롤한다. 필터에 없으면 그대로.
    void reveal(int tag) {
        int index = 0;
        for (auto* card : m_cards) {
            if (!m_filter(card->getTag(), m_category)) continue;
            if (card->getTag() == tag) {
                setOffset((index - (perPage() - 1) / 2) * step());
                return;
            }
            ++index;
        }
    }
    void onEnter() override {
        CCMenu::onEnter();
        for (auto* card : m_cards) setPreviewRunning(card, card->isVisible());
    }
    // 터치 하나만 추적한다. 터치 시작 때 눌린 카드를 기억해 두고, 7px 이상 움직이면 드래그로
    // 보고 카드 누름은 취소한다.
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        if (m_touchID != -1 || !nodeIsVisible(this) || !contains(touch->getLocation())) return false;
        m_touchID = touch->getID();
        m_start = convertToNodeSpace(touch->getLocation());
        m_startOffset = m_offset;
        m_dragging = false;
        m_filterChanged = false;
        m_pressedCard = CCMenu::ccTouchBegan(touch, event);
        return true;
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (touch->getID() != m_touchID || m_filterChanged) return;
        auto const p = convertToNodeSpace(touch->getLocation());
        if (!m_dragging && (std::abs(p.x - m_start.x) > 7.f || std::abs(p.y - m_start.y) > 7.f)) {
            m_dragging = true;
            if (m_pressedCard) CCMenu::ccTouchCancelled(touch, event);
            m_pressedCard = false;
        }
        if (m_dragging) setOffset(m_startOffset + m_start.x - p.x);
    }
    // 드래그가 아니고 영역 안에서 뗐을 때만 카드를 활성화한다.
    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (touch->getID() != m_touchID) return;
        bool const activate = m_pressedCard && !m_dragging && !m_filterChanged && contains(touch->getLocation());
        bool const cancel = m_pressedCard && !activate;
        m_touchID = -1;
        m_pressedCard = false;
        m_dragging = false;
        if (activate)
            CCMenu::ccTouchEnded(touch, event);
        else if (cancel)
            CCMenu::ccTouchCancelled(touch, event);
    }
    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        if (touch->getID() != m_touchID) return;
        if (m_pressedCard) CCMenu::ccTouchCancelled(touch, event);
        m_touchID = -1;
        m_pressedCard = false;
        m_dragging = false;
    }
    // 마우스 휠: 가로 입력이 있으면 그것을, 없으면 세로 입력을 가로 스크롤로 쓴다.
    void scrollWheel(float y, float x) override {
        if (!nodeIsVisible(this) || !contains(getMousePos())) return;
        scrollBy((std::abs(x) > .01f ? x : y) * 2.f);
    }
};
