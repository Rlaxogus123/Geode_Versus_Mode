// Versus 진입 화면(VersusEntry)과 여기서 쓰는 보조 팝업 모음.
// - PatchNotesPopup: 서버에서 받은 패치 노트 표시
// - MembershipCodePopup / MembershipInfoPopup: 멤버십 안내와 활성화 코드 입력
// - ArgonInfoPopup / ArgonNoticePopup: Argon 계정 인증 상태 안내
// - VersusEntry: Public Room / Ranked 선택, 내 프로필 카드, 버전·점검·밴 확인, 스태프 알림 등
// Service 구현에 포함되는 파일이다.
//
// 패치 노트 팝업. 영어/한국어 본문을 서버에서 받아 보여 주고 선택한 언어를 저장한다.
class PatchNotesPopup final : public ::versus::DeferredPopup {
    ScrollLayer* m_scroll = nullptr;
    std::string m_en, m_ko;
    bool m_korean = true;
    // 간단한 마크다운 형식으로 본문을 그린다: "# " 버전 제목, "## " 소제목, "- " 글머리표.
    // 최신 5개 버전까지만 보여 준다.
    void render() {
        m_scroll->m_contentLayer->removeAllChildren();
        auto const& text = m_korean ? m_ko : m_en;
        float offset = 12.f;
        bool continuingBullet = false;
        std::vector<std::pair<CCNode*, float>> rows;
        constexpr int kMaxVersions = 5;
        int versions = 0;
        for (size_t start = 0; start < text.size();) {
            auto end = text.find('\n', start);
            auto line = text.substr(start, end == std::string::npos ? end : end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                offset += 9.f;
                continuingBullet = false;
                continue;
            }
            if (line.starts_with("# ") && ++versions > kMaxVersions) break;
            bool title = line.starts_with("# "), heading = line.starts_with("## ");
            bool bullet = line.starts_with("- ");
            if (title || heading || bullet) line.erase(0, heading ? 3 : 2);
            if (heading) offset += 5.f;
            continuingBullet = !title && !heading && (bullet || continuingBullet);
            float indent = continuingBullet ? 13.f : 0.f;
            auto* body = createSystemText(line,
                                          title     ? 19.f
                                          : heading ? 16.f
                                                    : 13.5f,
                                          366.f - indent, kCCTextAlignmentLeft,
                                          title || heading ? TextWeight::Bold : TextWeight::Regular);
            body->setColor(title ? ccc3(143, 214, 255) : heading ? ccc3(255, 190, 105) : ccc3(230, 234, 242));
            body->setAnchorPoint({0.f, 1.f});
            body->setPositionX(14.f + indent);
            rows.emplace_back(body, offset);
            m_scroll->m_contentLayer->addChild(body);
            if (bullet) {
                auto* dot = CCLayerColor::create(ccc4(255, 190, 105, 255), 3.f, 3.f);
                dot->setPositionX(16.f);
                rows.emplace_back(dot, offset + 8.f);
                m_scroll->m_contentLayer->addChild(dot);
            }
            offset += body->getContentSize().height + (heading ? 9.f : 7.f);
        }
        float height = std::max(190.f, offset + 10.f);
        m_scroll->m_contentLayer->setContentSize({396.f, height});
        for (auto const& [row, y] : rows) row->setPositionY(height - y);
        m_scroll->scrollToTop();
    }
    // 언어 버튼: 선택을 저장하고 다시 그린다. 다른 안내 팝업과 같은 저장값을 공유한다.
    void english(CCObject*) {
        m_korean = false;
        Mod::get()->setSavedValue("patch-notes-korean", false);
        render();
    }
    void korean(CCObject*) {
        m_korean = true;
        Mod::get()->setSavedValue("patch-notes-korean", true);
        render();
    }
    bool init() override {
        // 높이 280: 300이면 왼쪽 위 닫기 버튼이 화면(높이 320) 위로 살짝 나간다.
        if (!Popup::init(420.f, 280.f)) return false;
        setTitle("Patch Notes");
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({396.f, 190.f});
        paper->setPosition({210.f, 145.f});
        paper->setColor({18, 23, 34});
        paper->setOpacity(225);
        m_mainLayer->addChild(paper);
        m_scroll = ScrollLayer::create({396.f, 190.f});
        m_scroll->setPosition({12.f, 50.f});
        m_mainLayer->addChild(m_scroll);
        button(m_buttonMenu, this, menu_selector(PatchNotesPopup::english), "English", {137.f, 24.f}, .43f);
        button(m_buttonMenu, this, menu_selector(PatchNotesPopup::korean), "Korean", {283.f, 24.f}, .43f);
        // 서버 응답이 오기 전에는 "Loading..." 을 보여 주고, 성공하면 현재 버전의 패치 노트를 본 것으로 기록한다.
        m_korean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        m_en = m_ko = "Loading...";
        render();
        Service::get().fetchPatchNotes(
            [self = Weak<PatchNotesPopup>(this)](std::string en, std::string ko, std::string error) {
                if (auto owner = self.lock()) {
                    owner->m_en = error.empty() ? std::move(en) : error;
                    owner->m_ko = error.empty() ? std::move(ko) : error;
                    owner->render();
                    if (error.empty()) {
                        Mod::get()->setSavedValue("patch-notes-seen", std::string(firebase_config::CLIENT_VERSION));
                        (void)Mod::get()->saveData();
                    }
                }
            });
        return true;
    }

public:
    CREATE_FUNC(PatchNotesPopup);
};

// 제작자 YouTube 채널 / Versus Discord 링크
constexpr char const* kTipp7YouTube = "https://www.youtube.com/@GMDTipp7/featured";
constexpr char const* kVersusDiscord = "https://discord.gg/PXgvB6MddY";
// YouTube 아이콘 버튼을 만든다.
CCMenuItemSpriteExtra* youtubeButton(CCMenu* menu, CCObject* target, SEL_MenuHandler action, CCPoint position,
                                     float height) {
    auto* item = iconButton(menu, target, action, "gj_ytIcon_001.png", position, height);
    item->setID("tipp7-youtube"_spr);
    return item;
}

// 멤버십 코드 입력 팝업. 코드는 기본적으로 가려서 입력하며(눈 아이콘으로 표시 전환),
// 확인 요청 중에는 입력과 버튼을 잠근다. 성공하면 changed 콜백을 호출한다.
class MembershipCodePopup final : public ::versus::DeferredPopup {
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_message = nullptr;
    void onExit() override {
        if (m_input) m_input->defocus();
        Popup::onExit();
    }

    CCMenuItemSpriteExtra* m_submit = nullptr;
    CCMenuItemSpriteExtra* m_visibility = nullptr;
    std::function<void()> m_changed;
    bool m_busy = false;
    bool m_codeVisible = false;
    void toggleVisibility(CCObject*) {
        if (m_busy || !m_input) return;
        m_codeVisible = !m_codeVisible;
        m_input->setPasswordMode(!m_codeVisible);
        m_visibility->setOpacity(m_codeVisible ? 255 : 150);
    }
    void youtube(CCObject*) { web::openLinkInBrowser(kTipp7YouTube); }
    // 입력한 코드로 멤버십 활성화를 서버에 요청한다. 결과 메시지를 팝업 안에 보여 준다.
    void submit(CCObject*) {
        if (m_busy) return;
        m_input->defocus();
        auto code = std::string(m_input->getString());
        if (code.empty()) {
            m_message->setString("Enter your membership code.");
            return;
        }
        m_busy = true;
        enabled(m_submit, false);
        enabled(m_visibility, false);
        m_input->setEnabled(false);
        m_message->setString("VERIFYING...");
        Service::get().redeemMembership(std::move(code),
                                        [self = Weak<MembershipCodePopup>(this)](bool ok, std::string detail) {
                                            if (auto popup = self.lock()) {
                                                popup->m_busy = false;
                                                enabled(popup->m_submit, true);
                                                enabled(popup->m_visibility, true);
                                                popup->m_input->setEnabled(true);
                                                if (ok) {
                                                    popup->m_input->setString("");
                                                    popup->m_codeVisible = false;
                                                    popup->m_input->setPasswordMode(true);
                                                }
                                                popup->m_visibility->setOpacity(popup->m_codeVisible ? 255 : 150);
                                                popup->m_message->setString(ok ? "MEMBERSHIP ACTIVE!" : detail.c_str());
                                                popup->m_message->limitLabelWidth(300.f, .3f, .16f);
                                                if (ok && popup->m_changed) popup->m_changed();
                                            }
                                        });
    }
    // 코드 입력창은 공백을 제외한 출력 가능한 ASCII 문자만 받고 최대 128자까지 허용한다.
    bool init(std::function<void()> changed) {
        if (!Popup::init(340.f, 215.f, "GJ_square02.png")) return false;
        m_changed = std::move(changed);
        setTitle("Membership Code", "bigFont.fnt", .57f);
        label(m_mainLayer, "Active until the administrator changes the code.", {170.f, 158.f}, .4f, 310.f, kIce, false,
              "chatFont.fnt");
        m_input = TextInput::create(244.f, "Membership code");
        std::string accepted;
        for (char c = 33; c <= 126; ++c) accepted += c;
        m_input->setFilter(accepted);
        m_input->setMaxCharCount(128);
        m_input->setPasswordMode(true);
        m_input->setPosition({151.f, 114.f});
        styleInput(m_input);
        m_mainLayer->addChild(m_input);
        m_visibility = iconButton(m_buttonMenu, this, menu_selector(MembershipCodePopup::toggleVisibility),
                                  "hideBtn_001.png", {299.f, 114.f}, 26.f);
        m_visibility->setID("membership-code-visibility"_spr);
        m_visibility->setOpacity(150);
        m_message = label(m_mainLayer, "Case-sensitive. No spaces.", {170.f, 76.f}, .28f, 302.f, kIce);
        m_submit =
            button(m_buttonMenu, this, menu_selector(MembershipCodePopup::submit), "Activate", {170.f, 32.f}, .57f);
        youtubeButton(m_buttonMenu, this, menu_selector(MembershipCodePopup::youtube), {42.f, 32.f}, 28.f);
        return true;
    }

public:
    static MembershipCodePopup* create(std::function<void()> changed) {
        auto* popup = new MembershipCodePopup;
        if (popup->init(std::move(changed))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// 멤버십 안내 팝업(혜택과 활성화 방법). 닫으면 next 콜백으로 코드 입력 팝업으로 이어진다.
class MembershipInfoPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 420.f, H = 280.f, PaperW = 396.f, PaperH = 196.f, PaperY = 46.f;  // (H 300이면 닫기 버튼이 화면 밖)
    // 영어 본문. 줄 앞 표식: "> " 안내 상자, "## " 소제목, "1. " 번호 항목, "!" 빨간 강조 경고.
    static constexpr char const* kEnglish =
        "> This membership is not made for commercial purposes. It is offered for a short time only, to keep the benefits of my existing YouTube viewers. Playing Versus without it brings no restriction or disadvantage of any kind.\n"
        "\n"
        "## What do members get?\n"
        "1. More finisher (execution) animations\n"
        "2. More emotes to use\n"
        "3. Exclusive achievements & badges + stylish nickname decorations\n"
        "\n"
        "## How does activation work?\n"
        "1. Join Level 3, \"\xEB\xAA\xA8\xEB\x93\x9C\xEC\x93\xB8\xEB\x9E\x98\", of the Tipp7 YouTube channel membership.\n"
        "2. A post that only Level 3 members can see is on the channel's YouTube Community tab.\n"
        "3. Enter the activation code from that post to activate the membership.\n"
        "!4. The activation code changes from time to time. Each time it changes, you need to enter the new code again.";
    // 한국어 본문(UTF-8 바이트 이스케이프로 적혀 있음)
    static constexpr char const* kKorean =
        "> \xED\x95\xB4\xEB\x8B\xB9 \xEB\xA9\xA4\xEB\xB2\x84\xEC\x8B\xAD \xEA\xB8\xB0\xEB\x8A\xA5\xEC\x9D\x80 \xEC\x83\x81\xEC\x97\x85\xEC\xA0\x81\xEC\x9D\xB8 \xEC\x9A\xA9\xEB\x8F\x84\xEB\xA1\x9C \xEB\xA7\x8C\xEB\x93\xA4\xEC\x96\xB4\xEC\xA7\x84 \xEA\xB8\xB0\xEB\x8A\xA5\xEC\x9D\xB4 \xEC\x95\x84\xEB\x8B\x88\xEB\xA9\xB0, \xEA\xB8\xB0\xEC\xA1\xB4 \xEC\xA0\x9C \xEC\x9C\xA0\xED\x8A\x9C\xEB\xB8\x8C \xEC\x8B\x9C\xEC\xB2\xAD\xEC\x9E\x90\xEB\xB6\x84\xEB\x93\xA4\xEC\x9D\x98 \xED\x98\x9C\xED\x83\x9D \xEC\x9C\xA0\xEC\xA7\x80\xEB\xA5\xBC \xEC\x9C\x84\xED\x95\xB4 \xEC\xA7\xA7\xEA\xB2\x8C \xEC\xA0\x9C\xEA\xB3\xB5\xEB\x90\x98\xEB\x8A\x94 \xEA\xB8\xB0\xEB\x8A\xA5\xEC\x9E\x85\xEB\x8B\x88\xEB\x8B\xA4. Versus\xEB\xA5\xBC \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEC\x8B\x9C\xEB\x8A\x94 \xEB\x8D\xB0\xEC\x97\x90\xEB\x8A\x94 \xEA\xB7\xB8 \xEC\x96\xB4\xEB\x96\xA0\xED\x95\x9C \xEC\xA7\x80\xEC\x9E\xA5\xEB\x8F\x84 \xEB\xB6\x88\xEC\x9D\xB4\xEC\x9D\xB5\xEB\x8F\x84 \xEB\xB0\x9C\xEC\x83\x9D\xED\x95\x98\xEC\xA7\x80 \xEC\x95\x8A\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "\n"
        "## \xEB\xA9\xA4\xEB\xB2\x84\xEC\x8B\xAD \xED\x98\x9C\xED\x83\x9D\xEC\x9D\x80 \xEB\xAC\xB4\xEC\x97\x87\xEC\x9D\xB4 \xEC\x9E\x88\xEB\x82\x98\xEC\x9A\x94?\n"
        "1. \xEB\x8D\x94\xEC\x9A\xB1 \xEB\x8B\xA4\xEC\x96\x91\xED\x95\x9C \xEC\xB2\x98\xED\x98\x95 \xEB\xAA\xA8\xEC\x85\x98\n"
        "2. \xEB\x8D\x94\xEC\x9A\xB1 \xEB\x8B\xA4\xEC\x96\x91\xED\x95\x9C \xEC\x9D\xB4\xEB\xAA\xA8\xED\x8B\xB0\xEC\xBD\x98 \xEC\x82\xAC\xEC\x9A\xA9\n"
        "3. \xEC\xA0\x84\xEC\x9A\xA9 \xEB\x8F\x84\xEC\xA0\x84\xEA\xB3\xBC\xEC\xA0\x9C & \xEB\xB1\x83\xEC\xA7\x80 + \xEB\xA9\x8B\xEC\x9E\x88\xEB\x8A\x94 \xEC\x9D\xB4\xEB\xA6\x84 \xEA\xBE\xB8\xEB\xAF\xB8\xEA\xB8\xB0\n"
        "\n"
        "## \xEB\xA9\xA4\xEB\xB2\x84\xEC\x8B\xAD \xED\x99\x9C\xEC\x84\xB1\xED\x99\x94 \xEA\xB5\xAC\xEC\xA1\xB0\xEA\xB0\x80 \xEC\x96\xB4\xEB\x96\xBB\xEA\xB2\x8C \xEB\x90\x98\xEB\x82\x98\xEC\x9A\x94?\n"
        "1. Tipp7 \xEC\x9C\xA0\xED\x8A\x9C\xEB\xB8\x8C \xEB\xA9\xA4\xEB\xB2\x84\xEC\x8B\xAD\xEC\x9D\x98 Level3, '\xEB\xAA\xA8\xEB\x93\x9C\xEC\x93\xB8\xEB\x9E\x98'\xEC\x97\x90 \xEA\xB0\x80\xEC\x9E\x85\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "2. \xEC\x9C\xA0\xED\x8A\x9C\xEB\xB8\x8C \xEC\xBB\xA4\xEB\xAE\xA4\xEB\x8B\x88\xED\x8B\xB0 \xEA\xB2\x8C\xEC\x8B\x9C\xEA\xB8\x80\xEC\x97\x90 Level3 \xEA\xB0\x80\xEC\x9E\x85\xEC\x9E\x90\xEA\xB0\x80 \xEB\xB3\xBC \xEC\x88\x98 \xEC\x9E\x88\xEB\x8A\x94 \xEA\xB2\x8C\xEC\x8B\x9C\xEA\xB8\x80\xEC\x9D\x84 \xEC\x98\xAC\xEB\xA0\xA4\xEB\x91\x90\xEC\x97\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "3. \xED\x95\xB4\xEB\x8B\xB9 \xEA\xB2\x8C\xEC\x8B\x9C\xEA\xB8\x80 \xEC\x86\x8D \xED\x99\x9C\xEC\x84\xB1\xED\x99\x94 \xEC\xBD\x94\xEB\x93\x9C\xEB\xA5\xBC \xEC\x9E\x85\xEB\xA0\xA5\xED\x95\x98\xEB\xA9\xB4 \xED\x99\x9C\xEC\x84\xB1\xED\x99\x94\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "!4. \xEB\xA9\xA4\xEB\xB2\x84\xEC\x8B\xAD \xED\x99\x9C\xEC\x84\xB1\xED\x99\x94 \xEC\xBD\x94\xEB\x93\x9C\xEB\x8A\x94 \xEB\xB9\x84\xEC\xA0\x95\xEA\xB8\xB0\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xEB\xB3\x80\xEA\xB2\xBD\xEB\x90\x98\xEB\xA9\xB0, \xEB\xB3\x80\xEA\xB2\xBD\xEB\x90\xA0 \xEB\x95\x8C\xEB\xA7\x88\xEB\x8B\xA4 \xEC\xBD\x94\xEB\x93\x9C\xEB\xA5\xBC \xEB\x8B\xA4\xEC\x8B\x9C \xEC\x9E\x85\xEB\xA0\xA5\xED\x95\x98\xEC\x85\x94\xEC\x95\xBC \xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4.";
    ScrollLayer* m_scroll = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    std::function<void()> m_next;
    bool m_isKorean = true;
    // 내용이 영역을 넘으면 글자 크기를 단계적으로 줄여 가며 다시 배치한다.
    void render() {
        float height = 0.f;
        for (float scale : {1.f, .92f, .85f, .8f}) {
            height = layout(scale);
            if (height <= PaperH) break;
        }
        m_scroll->m_contentLayer->setContentSize({PaperW, std::max(PaperH, height)});
        m_scroll->scrollToTop();
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(m_isKorean ? 130 : 255);
        m_korean->setOpacity(m_isKorean ? 255 : 130);
    }
    // 본문을 줄 단위로 파싱해 scale 배율로 배치하고 필요한 높이를 반환한다.
    float layout(float scale) {
        m_scroll->m_contentLayer->removeAllChildren();
        std::string_view const text = m_isKorean ? kKorean : kEnglish;
        float offset = 12.f * scale;
        std::vector<std::pair<CCNode*, float>> rows;
        auto place = [&](CCNode* node, float x, float y) {
            node->setAnchorPoint({0.f, 1.f});
            node->setPositionX(x);
            rows.emplace_back(node, y);
            m_scroll->m_contentLayer->addChild(node);
        };
        for (size_t start = 0; start < text.size();) {
            auto end = text.find('\n', start);
            std::string line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            start = end == std::string_view::npos ? text.size() : end + 1;
            if (line.empty()) {
                offset += 9.f * scale;
                continue;
            }
            if (line.starts_with("> ")) {
                auto* notice = createSystemText(line.substr(2), 13.5f * scale, PaperW - 40.f, kCCTextAlignmentLeft,
                                                TextWeight::Bold);
                notice->setColor(ccc3(150, 235, 255));
                place(notice, 20.f, offset + 4.f * scale);
                auto* box = NineSlice::create("square02b_001.png");
                box->setColor({30, 70, 110});
                box->setOpacity(200);
                box->setContentSize({PaperW - 16.f, notice->getContentSize().height + 12.f * scale});
                box->setAnchorPoint({0.f, 1.f});
                box->setPositionX(8.f);
                rows.emplace_back(box, offset - 2.f * scale);
                m_scroll->m_contentLayer->addChild(box, -1);
                offset += notice->getContentSize().height + 14.f * scale;
                continue;
            }
            if (line.starts_with("## ")) {
                auto* heading = createSystemText(line.substr(3), 16.f * scale, PaperW - 28.f, kCCTextAlignmentLeft,
                                                 TextWeight::Bold);
                heading->setColor(ccc3(255, 190, 105));
                place(heading, 14.f, offset);
                offset += heading->getContentSize().height + 6.f * scale;
                continue;
            }
            bool const warning = line.starts_with("!");
            if (warning) line.erase(0, 1);
            auto const dot = line.find(". ");
            std::string number = dot != std::string::npos && dot <= 2 ? line.substr(0, dot + 1) : "";
            if (!number.empty()) line.erase(0, dot + 2);
            auto const weight = warning ? TextWeight::Bold : TextWeight::Regular;
            float const size = (warning ? 14.f : 13.f) * scale;
            auto const color = warning ? ccc3(255, 70, 70) : ccc3(230, 234, 242);
            if (!number.empty()) {
                auto* index = createSystemText(number, size, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
                index->setColor(warning ? color : ccc3(255, 190, 105));
                place(index, 18.f, offset);
            }
            auto* body = createSystemText(line, size, PaperW - 56.f, kCCTextAlignmentLeft, weight);
            body->setColor(color);
            place(body, 36.f, offset);
            offset += body->getContentSize().height + 5.f * scale;
        }
        float const needed = offset + 8.f * scale, height = std::max(PaperH, needed);
        for (auto const& [row, y] : rows) row->setPositionY(height - y);
        return needed;
    }
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    void youtube(CCObject*) { web::openLinkInBrowser(kTipp7YouTube); }
    void next(CCObject*) {
        auto next = m_next;
        onClose(nullptr);
        if (next) next();
    }
    bool init(std::function<void()> next) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_next = std::move(next);
        setTitle("Membership", "bigFont.fnt", .62f);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({PaperW, PaperH});
        paper->setPosition({W / 2.f, PaperY + PaperH / 2.f});
        paper->setColor({18, 23, 34});
        paper->setOpacity(225);
        m_mainLayer->addChild(paper);
        m_scroll = ScrollLayer::create({PaperW, PaperH});
        m_scroll->setPosition({(W - PaperW) / 2.f, PaperY});
        m_mainLayer->addChild(m_scroll);
        m_english =
            button(m_buttonMenu, this, menu_selector(MembershipInfoPopup::english), "English", {58.f, 24.f}, .42f);
        m_korean =
            button(m_buttonMenu, this, menu_selector(MembershipInfoPopup::korean), "Korean", {132.f, 24.f}, .42f);
        youtubeButton(m_buttonMenu, this, menu_selector(MembershipInfoPopup::youtube), {196.f, 24.f}, 26.f);
        auto* channel = CCLabelBMFont::create("GMDTipp7", "goldFont.fnt");
        channel->limitLabelWidth(98.f, .5f, .3f);
        auto* link = CCMenuItemSpriteExtra::create(channel, this, menu_selector(MembershipInfoPopup::youtube));
        link->setPosition({213.f + channel->getScaledContentSize().width / 2.f, 24.f});
        m_buttonMenu->addChild(link);
        button(m_buttonMenu, this, menu_selector(MembershipInfoPopup::next), "Next", {W - 52.f, 24.f}, .55f,
               "GJ_button_02.png")
            ->setID("membership-next"_spr);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }

public:
    static MembershipInfoPopup* create(std::function<void()> next) {
        auto* popup = new MembershipInfoPopup;
        if (popup->init(std::move(next))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// Argon 인증 상태 코드(verified/pending/checking/failed/unlinked/off)를 사용자용 문구로 바꾼다.
std::string argonStatusText(std::string const& state, bool korean) {
    if (state == "verified") return korean ? "\xEC\x9D\xB8\xEC\xA6\x9D\xEB\x90\xA8" : "Verified";
    if (state == "pending")
        return korean
                   ? "\xED\x99\x95\xEC\x9D\xB8 \xEB\x8C\x80\xEA\xB8\xB0 (\xEB\x8B\xA4\xEC\x9D\x8C \xEC\xA0\x91\xEC\x86\x8D \xEB\x95\x8C \xEB\x8B\xA4\xEC\x8B\x9C \xED\x99\x95\xEC\x9D\xB8)"
                   : "Pending (checked again on a later sign-in)";
    if (state == "checking") return korean ? "\xED\x99\x95\xEC\x9D\xB8 \xEC\xA4\x91..." : "Checking...";
    if (state == "failed")
        return korean
                   ? "\xEC\x9D\xB8\xEC\xA6\x9D \xEC\x8B\xA4\xED\x8C\xA8 - GD\xEC\x97\x90\xEC\x84\x9C \xEA\xB3\x84\xEC\xA0\x95\xEC\x97\x90 \xEB\x8B\xA4\xEC\x8B\x9C \xEB\xA1\x9C\xEA\xB7\xB8\xEC\x9D\xB8\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94"
                   : "Failed - log in to your GD account again in GD";
    if (state == "unlinked")
        return korean
                   ? "\xEC\x9D\xB4 \xEA\xB8\xB0\xEA\xB8\xB0\xEC\x97\x90 \xEA\xB3\x84\xEC\xA0\x95 \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x9D\x84 \xEC\x95\x84\xEC\xA7\x81 \xEB\xAA\xBB \xEB\xB6\x88\xEB\x9F\xAC\xEC\x98\xB4 - \xEB\x8B\xA4\xEC\x9D\x8C \xEC\xA0\x91\xEC\x86\x8D \xEB\x95\x8C \xEB\x8B\xA4\xEC\x8B\x9C \xEC\x8B\x9C\xEB\x8F\x84"
                   : "Your account's progress is not loaded on this device yet - tried again next time";
    if (state == "off")
        return korean
                   ? "\xEC\x9D\xB4 \xEB\xB9\x8C\xEB\x93\x9C\xEC\x97\x90\xEB\x8A\x94 Argon\xEC\x9D\xB4 \xEC\x97\x86\xEC\x9D\x8C"
                   : "Not in this build";
    return korean ? "\xEC\x95\x84\xEC\xA7\x81 \xEC\x9D\xB8\xEC\xA6\x9D \xEC\x95\x88 \xEB\x90\xA8" : "Not verified yet";
}
// Argon 인증 안내 팝업. 현재 상태와, Argon 이 무엇을 확인하는지, 라이선스 크레딧을 영어/한국어로 보여 준다.
class ArgonInfoPopup final : public ::versus::DeferredPopup {
    CCNode* m_body = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    bool m_isKorean = true;
    // 상태 문구와 설명 줄들을 만들고, 영역에 맞을 때까지 글자 크기를 줄여 가며 배치한다.
    void render() {
        m_body->removeAllChildren();
        bool const ko = m_isKorean;
        auto const state = Service::get().argonState();
        std::vector<std::pair<std::string, int>> lines;
        lines.push_back({ko ? "\xEC\x83\x81\xED\x83\x9C: " + argonStatusText(state, true)
                            : "Status: " + argonStatusText(state, false),
                         2});
        if (ko) {
            lines.push_back(
                {"Versus\xEB\x8A\x94 Argon\xEC\x9C\xBC\xEB\xA1\x9C \xEC\xA7\x80\xEA\xB8\x88 GD \xEA\xB3\x84\xEC\xA0\x95\xEC\x97\x90 \xEC\x8B\xA4\xEC\xA0\x9C\xEB\xA1\x9C \xEB\xA1\x9C\xEA\xB7\xB8\xEC\x9D\xB8\xED\x95\x9C \xEC\x82\xAC\xEB\x9E\x8C\xEC\x9D\xB8\xEC\xA7\x80 \xED\x99\x95\xEC\x9D\xB8\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4. \xEB\x8B\xA4\xEB\xA5\xB8 \xEC\x82\xAC\xEB\x9E\x8C\xEC\x9D\xB4 \xEB\x82\xB4 \xEA\xB3\x84\xEC\xA0\x95\xEC\x9D\xB8 \xEC\xB2\x99 \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\x8A\x94 \xEA\xB2\x83\xEC\x9D\x84 \xEB\xA7\x89\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
                 0});
            lines.push_back(
                {"- \xEC\x9D\xB4 \xEA\xB8\xB0\xEA\xB8\xB0\xEA\xB0\x80 GD \xEA\xB3\x84\xEC\xA0\x95\xEC\x9D\x84 \xEC\xB2\x98\xEC\x9D\x8C \xEC\x97\xB0\xEA\xB2\xB0\xED\x95\x98\xEA\xB1\xB0\xEB\x82\x98 \xEA\xB7\xB8 \xEA\xB3\x84\xEC\xA0\x95\xEC\x9C\xBC\xEB\xA1\x9C \xEB\xB0\x94\xEA\xBF\x80 \xEB\x95\x8C\xEB\xA7\x8C \xED\x99\x95\xEC\x9D\xB8\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4. \xED\x8F\x89\xEC\x86\x8C \xEC\xA0\x91\xEC\x86\x8D\xEC\x9D\x80 \xEA\xB8\xB0\xEB\x8B\xA4\xEB\xA6\xAC\xEC\xA7\x80 \xEC\x95\x8A\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
                 0});
            lines.push_back(
                {"- Argon\xEC\x9D\xB4 \xEB\xB0\x94\xEC\x81\x98\xEA\xB1\xB0\xEB\x82\x98 \xEB\xA9\x88\xEC\xB6\xB0 \xEC\x9E\x88\xEC\x96\xB4\xEB\x8F\x84 \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\xA0 \xEC\x88\x98 \xEC\x9E\x88\xEA\xB3\xA0, \xEB\x8B\xA4\xEC\x9D\x8C \xEC\xA0\x91\xEC\x86\x8D \xEB\x95\x8C \xEB\x8B\xA4\xEC\x8B\x9C \xED\x99\x95\xEC\x9D\xB8\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
                 0});
            lines.push_back(
                {"- Versus \xEC\x84\x9C\xEB\xB2\x84\xEB\x8A\x94 GD \xEB\xB9\x84\xEB\xB0\x80\xEB\xB2\x88\xED\x98\xB8\xEB\xA5\xBC \xEB\xB0\x9B\xEC\xA7\x80 \xEC\x95\x8A\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4. Argon\xEC\x9D\xB4 \xEB\xA7\x8C\xEB\x93\xA0 \xED\x99\x95\xEC\x9D\xB8\xEC\x9A\xA9 \xED\x86\xA0\xED\x81\xB0\xEB\xA7\x8C \xEB\xB0\x9B\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
                 0});
        } else {
            lines.push_back(
                {"Versus uses Argon to check that you are really logged in to your GD account. It stops anyone from playing as your account.",
                 0});
            lines.push_back(
                {"- It checks only when this device first links your GD account or switches to it. Ordinary sign-ins never wait for it.",
                 0});
            lines.push_back(
                {"- If Argon is busy or down you can still play; Versus checks again on a later sign-in.", 0});
            lines.push_back({"- The Versus server never gets your GD password, only the check token Argon makes.", 0});
        }
        lines.push_back({ko ? "\xEC\xA0\x9C\xEA\xB3\xB5" : "Credits", 1});
        lines.push_back(
            {"Argon by the Globed Team (github.com/GlobedGD/argon) - MIT License, Copyright (c) 2025 Globed Team.", 3});
        lines.push_back(
            {ko ? "\xEC\xA0\x84\xEC\xB2\xB4 \xEB\x9D\xBC\xEC\x9D\xB4\xEC\x84\xA0\xEC\x8A\xA4 \xEB\xAC\xB8\xEA\xB5\xAC\xEB\x8A\x94 Versus \xEB\xAA\xA8\xEB\x93\x9C \xED\x8C\x8C\xEC\x9D\xBC\xEC\x9D\x98 argon-LICENSE.txt\xEC\x97\x90 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4."
                : "The full license text is in the Versus mod files (argon-LICENSE.txt).",
             3});
        for (float scale : {1.f, .92f, .85f, .78f}) {
            m_body->removeAllChildren();
            float y = 254.f;
            for (auto const& [text, kind] : lines) {
                float const size = (kind == 1 || kind == 2 ? 14.f : kind == 3 ? 11.5f : 12.5f) * scale;
                auto* node = createSystemText(text, size, 368.f, kCCTextAlignmentLeft,
                                              kind == 1 || kind == 2 ? TextWeight::Bold : TextWeight::Regular);
                node->setColor(kind == 1   ? ccc3(255, 190, 105)
                               : kind == 2 ? (state == "verified" ? ccc3(120, 255, 160)
                                              : state == "failed" ? ccc3(255, 110, 90)
                                                                  : ccc3(255, 214, 60))
                               : kind == 3 ? kMuted
                                           : ccc3(230, 234, 242));
                node->setAnchorPoint({0.f, 1.f});
                node->setPosition({26.f, y});
                m_body->addChild(node);
                y -= node->getContentSize().height + (kind == 1 ? 4.f : 6.f) * scale;
            }
            if (y >= 54.f) break;
        }
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(ko ? 130 : 255);
        m_korean->setOpacity(ko ? 255 : 130);
    }
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    bool init() override {
        if (!Popup::init(420.f, 300.f, "GJ_square02.png")) return false;
        setTitle("Argon Verification", "goldFont.fnt", .7f);
        setID("versus-argon-info"_spr);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({396.f, 210.f});
        paper->setPosition({210.f, 155.f});
        paper->setColor({18, 23, 34});
        paper->setOpacity(225);
        m_mainLayer->addChild(paper);
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 2);
        m_english = button(m_buttonMenu, this, menu_selector(ArgonInfoPopup::english), "English", {58.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(ArgonInfoPopup::korean), "Korean", {132.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(ArgonInfoPopup::onClose), "OK", {362.f, 24.f}, .55f);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }

public:
    CREATE_FUNC(ArgonInfoPopup);
};

// Argon 확인이 되지 않아 계정 기록을 불러오지 못했을 때 띄우는 안내 팝업.
// 이 경우 랭크 경기가 평소 기록이 아닌 새 기록에 저장된다는 점을 알린다.
class ArgonNoticePopup final : public ::versus::DeferredPopup {
    CCNode* m_body = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    bool m_isKorean = true;
    void render() {
        m_body->removeAllChildren();
        bool const ko = m_isKorean;
        std::vector<std::string> const lines = ko ? std::vector<std::string>{
            "GD \xEA\xB3\x84\xEC\xA0\x95 \xED\x99\x95\xEC\x9D\xB8(Argon)\xEC\x9D\x84 \xED\x95\x98\xEC\xA7\x80 \xEB\xAA\xBB\xED\x95\xB4\xEC\x84\x9C \xEC\x9D\xB4 \xEA\xB3\x84\xEC\xA0\x95\xEC\x9D\x98 Versus \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x9D\x84 \xEC\x95\x84\xEC\xA7\x81 \xEB\xB6\x88\xEB\x9F\xAC\xEC\x98\xA4\xEC\xA7\x80 \xEB\xAA\xBB\xED\x96\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
            "- \xEC\xA7\x80\xEA\xB8\x88\xEB\x8F\x84 \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\xA0 \xEC\x88\x98 \xEC\x9E\x88\xEC\xA7\x80\xEB\xA7\x8C, \xEC\x9D\xB4\xEB\xB2\x88\xEC\x97\x90 \xED\x95\x98\xEB\x8A\x94 \xEB\x9E\xAD\xED\x81\xAC \xEA\xB2\xBD\xEA\xB8\xB0\xEB\x8A\x94 \xEC\x9B\x90\xEB\x9E\x98 \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x9D\xB4 \xEC\x95\x84\xEB\x8B\x88\xEB\x9D\xBC \xEC\x83\x88 \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x97\x90 \xEC\xA0\x80\xEC\x9E\xA5\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
            "- \xEC\x9B\x90\xEB\x9E\x98 \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x9D\x80 \xEC\x84\x9C\xEB\xB2\x84\xEC\x97\x90 \xEA\xB7\xB8\xEB\x8C\x80\xEB\xA1\x9C \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4. GD\xEC\x97\x90\xEC\x84\x9C \xEA\xB3\x84\xEC\xA0\x95 \xEB\xA1\x9C\xEA\xB7\xB8\xEC\x9D\xB8\xEC\x9D\x84 \xED\x99\x95\xEC\x9D\xB8\xED\x95\x98\xEA\xB3\xA0 Versus\xEB\xA5\xBC \xEB\x8B\xA4\xEC\x8B\x9C \xEC\x97\xB4\xEB\xA9\xB4 \xEB\xB6\x88\xEB\x9F\xAC\xEC\x98\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
        } : std::vector<std::string>{
            "Versus could not verify your GD account (Argon), so this account's Versus progress is not loaded yet.",
            "- You can still play, but Ranked games played now are saved to a new record, not your usual one.",
            "- Your progress is safe on the server. Check your GD login and reopen Versus to load it.",
        };
        float y = 196.f;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto* node = createSystemText(lines[i], i == 0 ? 13.5f : 12.5f, 344.f, kCCTextAlignmentLeft,
                                          i == 0 ? TextWeight::Bold : TextWeight::Regular);
            node->setColor(i == 0 ? ccc3(255, 214, 60) : ccc3(230, 234, 242));
            node->setAnchorPoint({0.f, 1.f});
            node->setPosition({28.f, y});
            m_body->addChild(node);
            y -= node->getContentSize().height + 8.f;
        }
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(ko ? 130 : 255);
        m_korean->setOpacity(ko ? 255 : 130);
    }
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    bool init() override {
        if (!Popup::init(400.f, 240.f, "GJ_square02.png")) return false;
        setTitle("GD Account Not Loaded", "goldFont.fnt", .65f);
        setID("versus-argon-notice"_spr);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({372.f, 160.f});
        paper->setPosition({200.f, 126.f});
        paper->setColor({18, 23, 34});
        paper->setOpacity(225);
        m_mainLayer->addChild(paper);
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 2);
        m_english = button(m_buttonMenu, this, menu_selector(ArgonNoticePopup::english), "English", {58.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(ArgonNoticePopup::korean), "Korean", {132.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(ArgonNoticePopup::onClose), "OK", {340.f, 24.f}, .55f);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }

public:
    CREATE_FUNC(ArgonNoticePopup);
};
// Versus 진입 화면. 서버 버전/점검 상태를 확인한 뒤 Public Room 과 Ranked(Season 2 포함)로 들어가는 관문이다.
// 내 프로필 카드(랭크, 꾸미기 버튼), 멤버십·밴·신고 알림·레이팅 환급·스태프 메일 확인,
// 디버그 스위치를 함께 다룬다. 주기적 확인은 update() 에서 한다.
class VersusEntry final : public SceneLayer {
    CCMenuItemSpriteExtra* m_argon = nullptr;
    std::string m_argonShown = "-";
    // Argon 인증 상태에 맞는 아이콘 스프라이트
    static CCSprite* argonStatusIcon(std::string const& state) {
        char const* frame = state == "verified"                         ? "GJ_completesIcon_001.png"
                            : state == "pending" || state == "checking" ? "GJ_timeIcon_001.png"
                            : state == "failed"                         ? "GJ_deleteIcon_001.png"
                                                                        : "exMark_001.png";
        auto* icon = loadedFrameSprite(frame, "GJ_GameSheet03.plist");
        if (!icon) icon = loadedFrameSprite("GJ_infoIcon_001.png", "GJ_GameSheet03.plist");
        return icon;
    }
    // Argon 상태를 보여 주는 배지 스프라이트(아이콘 + 상태 문구, 상태별 색)
    static CCSprite* argonBadgeSprite(std::string const& state) {
        ccColor3B const color = state == "verified"                                                ? ccc3(120, 255, 160)
                                : state == "pending" || state == "checking" || state == "unlinked" ? ccc3(255, 214, 60)
                                : state == "failed"                                                ? ccc3(255, 110, 90)
                                                                                                   : kMuted;
        auto* text = CCLabelBMFont::create(state == "verified"   ? "ARGON VERIFIED"
                                           : state == "pending"  ? "ARGON PENDING"
                                           : state == "checking" ? "ARGON CHECKING"
                                           : state == "failed"   ? "ARGON FAILED"
                                           : state == "unlinked" ? "ARGON NOT LINKED"
                                                                 : "ARGON NOT VERIFIED",
                                           "bigFont.fnt");
        text->setScale(.3f);
        text->setColor(color);
        auto const size = text->getScaledContentSize();
        auto* sprite = CCSprite::create();
        sprite->setContentSize({size.width + 20.f, std::max(16.f, size.height + 6.f)});
        if (auto* mark = argonStatusIcon(state)) {
            mark->setScale(13.f / std::max(1.f, std::max(mark->getContentSize().width, mark->getContentSize().height)));
            mark->setPosition({7.f, sprite->getContentSize().height / 2.f});
            sprite->addChild(mark);
        }
        text->setAnchorPoint({0.f, .5f});
        text->setPosition({17.f, sprite->getContentSize().height / 2.f});
        sprite->addChild(text);
        return sprite;
    }
    // 상태가 바뀐 경우에만 Argon 배지를 갱신해 화면 오른쪽 위에 맞춘다. Argon 이 꺼진 빌드이거나 서버에 연결되지 않았으면 숨긴다.
    void refreshArgon() {
        if (!m_argon) return;
        auto const state = Service::get().argonState();
        if (state == m_argonShown) return;
        m_argonShown = state;
        m_argon->setVisible(state != "off" && Service::get().connected());
        if (state == "off") return;
        m_argon->setSprite(argonBadgeSprite(state));
        m_argon->setPosition(
            {m_window.width - 14.f - m_argon->getScaledContentSize().width / 2.f, m_window.height - 60.f});
    }
    void onArgon(CCObject*) {
        if (auto* popup = ArgonInfoPopup::create()) popup->show();
    }
    CCLabelBMFont* m_online = nullptr;
    CCDrawNode* m_onlineDot = nullptr;
    int m_onlineShown = -2;
    // 접속자 수 표시를 갱신한다. 값이 바뀔 때만 다시 그리며 음수(알 수 없음)는 회색으로 표시한다.
    void refreshOnline() {
        if (!m_online) return;
        int const count = Service::get().onlineCount();
        if (count == m_onlineShown) return;
        m_onlineShown = count;
        m_online->setString(count < 0 ? "-- ONLINE" : fmt::format("{} ONLINE", count).c_str());
        m_online->limitLabelWidth(110.f, .36f, .2f);
        m_online->setColor(count < 0 ? kMuted : ccc3(150, 255, 170));
        m_onlineDot->clear();
        m_onlineDot->drawDot({0.f, 0.f}, 3.2f,
                             count < 0 ? ccColor4F{.45f, .5f, .6f, 1.f} : ccColor4F{.3f, 1.f, .45f, 1.f});
    }
    CCMenuItemSpriteExtra* m_public = nullptr;
    CCMenuItemSpriteExtra* m_ranked = nullptr;
    CCMenuItemSpriteExtra* m_season2 = nullptr;
    CCLabelBMFont* m_rankedCaption = nullptr;
    bool m_season2Checked = false;
    int m_season2Ready = -1;
    // 이 화면이 현재 실행 중인 씬이고 전환이 진행 중이지 않은지 확인
    bool sceneSettled() {
        auto* director = CCDirector::sharedDirector();
        return getParent() && director->getRunningScene() == getParent() && !director->getNextScene();
    }
    // Season 2 진입 버튼과 기존 Ranked 안내 문구 중 무엇을 보일지 정한다. 버튼이 보이게 되는 첫 시점에 라운지 맵 준비를 시작한다.
    // Season 2 가 공개된 뒤에는 이 버튼이 Season 1 진입으로 쓰인다(m_season2IsS1).
    void showSeason2Entry(bool shown) {
        if (placeSeasonPicker()) refreshProfile();
        if (!m_season2 || !m_rankedCaption) return;
        if (shown && !m_season2Checked) {
            m_season2Checked = true;
            lounge::prepareMap();
        }
        bool const button = shown && !m_season2IsS1;
        m_season2->setVisible(button);
        m_rankedCaption->setVisible(!button);
        refreshSeason2Entry();
    }
    static bool season2Public() { return Service::get().season() >= 2; }
    bool m_season2IsS1 = false;
    // Season 2 가 공개됐거나 라운지 맵이 준비됐을 때만 버튼을 활성화한다.
    void refreshSeason2Entry() {
        if (!m_season2 || !m_season2->isVisible()) return;
        int const ready = season2Public() || lounge::mapReady() ? 1 : 0;
        if (ready == m_season2Ready) return;
        m_season2Ready = ready;
        enabled(m_season2, ready == 1);
    }
    CCLabelBMFont* m_season = nullptr;
    std::string m_seasonShown;
    // 랭크 카드의 시즌 남은 시간 문구를 갱신한다.
    void refreshSeason() {
        if (!m_season) return;
        auto const& service = Service::get();
        auto text = seasonTimeLeft(service.season(), service.seasonEndsAt(), service.serverNow());
        if (text == m_seasonShown) return;
        m_seasonShown = text;
        m_season->setString(text.c_str());
        m_season->limitLabelWidth(m_seasonWidth, .3f, .18f);
    }
    float m_seasonWidth = 150.f;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_integrity = nullptr;
    CCMenuItemSpriteExtra* m_update = nullptr;
    CCMenuItemSpriteExtra* m_admin = nullptr;
    std::optional<BanInfo> m_ban;
    bool m_banNoticeShown = false;
    int m_noticeTicks = 0;
    CCLabelBMFont* m_membership = nullptr;
    float m_membershipTimer = 0.f;
    float m_integrityTimer = 0.f;
    bool m_membershipLoaded = false;
    bool m_releaseCurrent = false, m_autoNotes = false;
    bool m_maintenanceShown = false;
    Weak<AccountChoicePopup> m_choicePopup;
    bool m_choiceQueued = false;
    std::vector<std::function<void()>> m_pendingPopups;
    CCNode* m_profilePanel = nullptr;
    CCNode* m_profileContent = nullptr;
    float m_profileW = 280.f, m_profileH = 180.f, m_profileRight = 560.f;
    std::optional<RankedProfile> m_rank;
    static constexpr float kCardRatingY = 94.f;
    std::string m_profileCup = Mod::get()->getSavedValue<std::string>("profile-season", "");
    std::map<std::string, RankedProfile> m_seasonRanks;
    std::set<std::string> m_seasonLoading;
    SeasonPicker* m_seasonPicker = nullptr;
    // 프로필 카드가 보여 줄 컵(시즌). 저장된 값이 현재 시즌 목록에 없으면 빈 값(Season 1 방식)이 된다.
    std::string shownCup() const {
        for (auto const& season : profileSeasons())
            if (season.cup == m_profileCup) return m_profileCup;
        return {};
    }
    // 프로필 카드의 시즌 선택기를 만들거나, 시즌 수가 바뀌었을 때 다시 만든다. 선택은 저장한다.
    bool placeSeasonPicker() {
        if (!m_profilePanel) return false;
        auto seasons = profileSeasons();
        if (m_seasonPicker && m_seasonPicker->count() == seasons.size()) return false;
        if (m_seasonPicker) m_seasonPicker->removeFromParent();
        size_t index = 0;
        std::string const cup = shownCup();
        for (size_t i = 0; i < seasons.size(); ++i)
            if (seasons[i].cup == cup) index = i;
        m_seasonPicker = SeasonPicker::create({m_profileW - 50.f, m_profileH - 121.f}, std::move(seasons), index, 48.f,
                                              [self = Weak<VersusEntry>(this)](ProfileSeason const& season) {
                                                  auto owner = self.lock();
                                                  if (!owner) return;
                                                  owner->m_profileCup = season.cup;
                                                  Mod::get()->setSavedValue("profile-season", season.cup);
                                                  owner->refreshProfile();
                                              });
        if (m_seasonPicker) m_profilePanel->addChild(m_seasonPicker, 3);
        return true;
    }
    // 해당 컵의 랭크 프로필을 서버에서 한 번만 받아 캐시한다. 지금 보고 있는 컵이면 카드를 갱신한다.
    void loadSeasonRank(std::string const& cup) {
        if (m_seasonRanks.count(cup) || !m_seasonLoading.insert(cup).second) return;
        Service::get().fetchRankedProfileIn(
            cup, {}, [self = Weak<VersusEntry>(this), cup](std::optional<RankedProfile> rank, std::string) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_seasonLoading.erase(cup);
                if (!rank) return;
                owner->m_seasonRanks[cup] = std::move(*rank);
                if (owner->shownCup() == cup) owner->refreshProfile();
            });
    }
    // Season 2 컵 선택 시 프로필 카드의 랭크 영역(티어 얼굴, 이름, 레이팅 또는 배치 진행도)을 그린다.
    void season2ProfileRank(std::string const& cup, float w, float h) {
        auto found = m_seasonRanks.find(cup);
        if (found == m_seasonRanks.end()) loadSeasonRank(cup);
        RankedProfile const* rank = found == m_seasonRanks.end() ? nullptr : &found->second;
        std::string const tier = rank && rank->placed ? rank->tier : std::string("Unranked");
        CCPoint const face{50.f, h - 92.f};
        rankedGlow(m_profileContent, season2PaletteKey(tier), face, {78.f, 56.f});
        s_season2FaceLook = true;
        rankedAura(m_profileContent, tier, face);
        s_season2FaceLook = false;
        season2Face(m_profileContent, tier, face, .9f);
        float const textX = 100.f, textW = w - textX - 12.f;
        if (!rank)
            label(m_profileContent, "LOADING", {textX, h - 80.f}, .5f, textW, ccWHITE, true, "gjFont46.fnt");
        else if (auto* name = Season2TierText::create()) {
            name->setPosition({textX, h - 80.f});
            m_profileContent->addChild(name);
            name->set(rank->placed && !rank->tierName.empty() ? rank->tierName : std::string("Placement"), tier, .5f,
                      textW, true);
        }
        std::string rating = "R----";
        if (rank && rank->placed)
            rating = rank->globalRank > 0 ? fmt::format("R{:.0f}  #{}", rank->rating, rank->globalRank)
                                          : fmt::format("R{:.0f}", rank->rating);
        else if (rank)
            rating = fmt::format("{} / {}", rank->placementCompleted,
                                 rank->placementRequired > 0 ? rank->placementRequired : 5);
        auto* ratingText =
            label(m_profileContent, rating, {textX, h - kCardRatingY}, .5f, textW, ccWHITE, true, "bigFont.fnt");
        rankedGradient(ratingText, season2PaletteKey(tier));
    }
    // 내 프로필 카드를 다시 그린다: 아이콘, 닉네임, 칭호, 랭크 티어 얼굴과 레이팅/순위.
    // 컵을 선택한 상태면 season2ProfileRank 로 넘기고, 아니면 기본 랭크 정보로 그린다.
    void refreshProfile() {
        if (!m_profileContent) return;
        m_profileContent->removeAllChildrenWithCleanup(true);
        auto const& me = Service::get().profile();
        float const w = m_profileW, h = m_profileH;
        player(m_profileContent, me, {40.f, h - 30.f}, 1.15f);
        float const nameX = 78.f + (w - 86.f) / 2.f;
        auto* name = label(m_profileContent, me.name.empty() ? "Player" : me.name, {nameX, h - 22.f}, .55f, w - 122.f);
        styleNickname(name, me.membership, me.cosmetics);
        titleBadge(m_profileContent, me.cosmetics.title, {nameX, h - 44.f}, 1.15f, w - 122.f);
        auto* divider = CCLayerColor::create(ccc4(255, 255, 255, 45), w - 20.f, 1.f);
        divider->setPosition({10.f, h - 58.f});
        m_profileContent->addChild(divider);
        auto* block = NineSlice::create("square02b_001.png");
        block->setColor({0, 0, 0});
        block->setOpacity(80);
        block->setContentSize({w - 16.f, 69.f});
        block->setAnchorPoint({.5f, .5f});
        block->setPosition({w / 2.f, h - 97.f});
        block->setID("versus-my-ranked-panel"_spr);
        m_profileContent->addChild(block, -1);
        if (auto const cup = shownCup(); !cup.empty()) {
            season2ProfileRank(cup, w, h);
            return;
        }
        std::string const tier =
            m_rank ? (m_rank->placed ? m_rank->tier : std::string("Unranked")) : std::string("Unranked");
        CCPoint const face{50.f, h - 92.f};
        rankedGlow(m_profileContent, tier, face, {78.f, 56.f});
        rankedAura(m_profileContent, tier, face);
        rankedFace(m_profileContent, tier, face, .9f);
        float const textX = 100.f, textW = w - textX - 12.f;
        std::string tierText = m_rank ? tier : std::string("LOADING");
        std::transform(tierText.begin(), tierText.end(), tierText.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        auto* tierLabel =
            label(m_profileContent, tierText, {textX, h - 80.f}, .5f, textW, ccWHITE, true, "gjFont46.fnt");
        rankedGradient(tierLabel, tier);
        auto* ratingText =
            label(m_profileContent,
                  m_rank ? (m_rank->globalRank > 0 ? fmt::format("R{:.0f}  #{}", m_rank->rating, m_rank->globalRank)
                                                   : fmt::format("R{:.0f}", m_rank->rating))
                         : std::string("R----"),
                  {textX, h - kCardRatingY}, .5f, textW, ccWHITE, true, "bigFont.fnt");
        rankedGradient(ratingText, tier);
    }
    // 랭크 프로필과 업적 정보를 받아 카드와 칭호 목록에 반영한다.
    void loadProfileData() {
        Service::get().fetchRankedProfileIn(
            {}, {}, [self = Weak<VersusEntry>(this)](std::optional<RankedProfile> rank, std::string) {
                if (auto owner = self.lock(); owner && rank) {
                    owner->m_rank = std::move(rank);
                    owner->refreshProfile();
                }
            });
        Service::get().fetchAchievements({}, [](std::optional<AchievementState> state, std::string) {
            if (state) noteTitles(*state);
        });
    }
    auto saved() {
        return [self = Weak<VersusEntry>(this)] {
            if (auto owner = self.lock()) owner->refreshProfile();
        };
    }
    void onAura(CCObject*) {
        if (auto* popup = AuraPopup::create(saved())) popup->show();
    }
    void onVersusProfile(CCObject*) { showPlayer(Service::get().profile()); }
    // 디버그 계정에만 보이는 DEBUG 스위치
    CCMenuItemSpriteExtra* m_debugSwitch = nullptr;
    CCMenu* m_debugMenu = nullptr;
    float m_debugLeft = 0.f;
    // 디버그 계정이면 DEBUG 버튼을 만든다.
    void addDebugButtons() {
        if (!m_debugMenu) return;
        if (!m_debugSwitch && debug::debugAccountNow()) {
            m_debugSwitch = button(m_debugMenu, this, menu_selector(VersusEntry::onDebugSwitch), "DEBUG",
                                   {m_debugLeft + 34.f, m_window.height - 60.f}, .4f);
            m_debugSwitch->setID("versus-debug-switch"_spr);
            paintDebugSwitch();
        }
    }
    // 스위치 상태에 맞게 DEBUG 버튼의 문구와 색을 바꾼다.
    void paintDebugSwitch() {
        if (!m_debugSwitch) return;
        bool const on = debug::enabled();
        auto* sprite = ButtonSprite::create(on ? "DEBUG: ON" : "DEBUG: OFF", "goldFont.fnt",
                                            on ? "GJ_button_02.png" : "GJ_button_04.png");
        sprite->setScale(.4f);
        m_debugSwitch->setSprite(sprite);
    }
    // 디버그 모드를 켜고 끈다.
    void onDebugSwitch(CCObject*) {
        if (!debug::debugAccountNow()) return;
        debug::setEnabled(!debug::enabled());
        paintDebugSwitch();
        placeDebugBadge();
        Notification::create(debug::enabled() ? "Debug mode on" : "Debug mode off", NotificationIcon::Info, 1.5f)
            ->show();
    }
    // 내 프로필 꾸미기 버튼 콜백들(Emotes / Finisher / Nickname / Titles)
    void onEmotes(CCObject*) {
        if (auto* popup = EmotePickerPopup::create({}, {})) popup->show();
    }
    void onFinisher(CCObject*) {
        if (auto* popup = FinisherPopup::create(saved())) popup->show();
    }
    void onNickname(CCObject*) {
        if (auto* popup = NicknamePopup::create(saved())) popup->show();
    }
    void onTitles(CCObject*) {
        log::info("Versus: opening achievements & titles");
        auto* popup = TitlesPopup::create([self = Weak<VersusEntry>(this)] {
            if (auto owner = self.lock()) owner->refreshProfile();
        });
        if (popup)
            popup->show();
        else
            log::error("Versus: achievements window failed to initialize");
    }
    // 멤버십 표시 문구를 현재 프로필 상태로 갱신
    void showMembership() {
        m_membership->setString(Service::get().profile().membership ? "MEMBERSHIP ACTIVE" : "STANDARD MEMBER");
        m_membership->limitLabelWidth(m_window.width - 120.f, .46f, .3f);
    }
    // 서버에서 멤버십 상태를 다시 확인한다. 실패하면 확인 불가 문구를 보여 준다.
    void checkMembership() {
        m_membershipTimer = 0.f;
        Service::get().refreshMembership([self = Weak<VersusEntry>(this)](bool ok, std::string) {
            if (auto owner = self.lock()) {
                owner->m_membershipLoaded = true;
                owner->showMembership();
                owner->refreshProfile();
                if (!ok) owner->m_membership->setString("MEMBERSHIP CHECK UNAVAILABLE");
            }
        });
    }
    // 멤버십 안내 팝업을 먼저 보이고, 이어서 코드 입력 팝업을 연다.
    void onMembership(CCObject*) {
        auto code = [self = Weak<VersusEntry>(this)] {
            if (!self.lock()) return;
            if (auto* popup = MembershipCodePopup::create([self] {
                    if (auto owner = self.lock()) {
                        owner->m_membershipLoaded = true;
                        owner->showMembership();
                    }
                }))
                popup->show();
        };
        if (auto* popup = MembershipInfoPopup::create(code))
            popup->show();
        else
            code();
    }
    void onDiscord(CCObject*) { web::openLinkInBrowser(kVersusDiscord); }
    // 이 화면이 지금 실제로 화면에 표시되고 있는지 확인
    bool onScreen() {
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        return isRunning() && scene == CCDirector::sharedDirector()->getRunningScene();
    }
    // 팝업 표시를 대기열에 넣는다. 화면이 전환 중이거나 보이지 않으면 보일 수 있게 될 때까지 미룬다.
    void present(std::function<void()> show) {
        m_pendingPopups.push_back(std::move(show));
        presentPending();
    }
    void presentPending() {
        if (m_pendingPopups.empty() || m_transitioning || !onScreen()) return;
        auto queued = std::move(m_pendingPopups);
        m_pendingPopups.clear();
        for (auto& show : queued)
            if (show) show();
    }
    // 밴 정보를 반영한다. 밴 상태면 모드 버튼을 비활성화하고 상태 문구를 바꾸며, 안내 팝업은 한 번만 띄운다.
    void applyBan(std::optional<BanInfo> ban) {
        m_ban = std::move(ban);
        bool const open = !m_ban && m_releaseCurrent && Service::get().connected();
        enabled(m_public, open);
        enabled(m_ranked, open);
        if (m_ban) {
            m_status->setString(
                m_ban->permanent
                    ? "BANNED FROM VERSUS MATCHES (PERMANENT)"
                    : fmt::format("BANNED FROM VERSUS MATCHES UNTIL {}", localDateTime(m_ban->until)).c_str());
            m_status->setColor(kAlert);
            if (!m_banNoticeShown) {
                m_banNoticeShown = true;
                present([ban = *m_ban] {
                    if (auto* popup = BanNoticePopup::create(ban)) popup->show();
                });
            }
        } else if (Service::get().connected() && m_releaseCurrent) {
            m_status->setString("CHOOSE YOUR MATCH");
            m_status->setColor(kIce);
        }
        m_status->limitLabelWidth(m_window.width - 50.f, .30f, .18f);
    }
    // 자신의 밴 상태를 서버에서 확인한다.
    void checkBan() {
        Service::get().fetchOwnBan([self = Weak<VersusEntry>(this)](bool ok, std::optional<BanInfo> ban, std::string) {
            if (auto owner = self.lock(); owner && ok) owner->applyBan(std::move(ban));
        });
    }
    // 관리자 상태를 확인해 Reports 버튼 표시, 대기 중 신고 수와 스태프 메일 수를 반영한다.
    // 제작자 계정은 수신함 수, 서브 관리자는 받은 답장 수를 메일 수로 본다.
    void checkAdmin() {
        Service::get().fetchAdminStatus([self = Weak<VersusEntry>(this)](AdminStatus status, std::string error) {
            auto owner = self.lock();
            if (!owner || !owner->m_admin || !error.empty()) return;
            owner->m_admin->setVisible(status.admin);
            owner->showSeason2Entry(Service::get().season2Access());
            if (status.admin) {
                int const mail = status.role == "owner" ? status.inbox : status.replies;
                std::string text =
                    status.reports > 0 ? fmt::format("Reports ({})", status.reports) : std::string("Reports");
                if (mail > 0) text += fmt::format(" | Mail {}", mail);
                auto* sprite =
                    ButtonSprite::create(text.c_str(), "goldFont.fnt",
                                         status.reports > 0 || mail > 0 ? "GJ_button_06.png" : "GJ_button_04.png");
                sprite->setScale(.48f);
                owner->m_admin->setSprite(sprite);
                owner->staffMail(status.role == "owner", mail);
            }
        });
    }
    int m_mailSeen = 0;
    // 새 스태프 메일이 이전에 본 수보다 늘었을 때만 알림 팝업을 띄운다.
    void staffMail(bool isOwner, int count) {
        if (count <= m_mailSeen) {
            m_mailSeen = count;
            return;
        }
        m_mailSeen = count;
        present([isOwner, self = Weak<VersusEntry>(this)] {
            createQuickPopup("Staff Mail",
                             isOwner ? "A sub-admin sent you a message."
                                     : "You have a new message or answer from Tipp7.",
                             "Later", "Open", [isOwner, self](auto*, bool yes) {
                                 if (!yes) return;
                                 if (auto* popup = StaffMessagesPopup::create(isOwner, [self] {
                                         if (auto owner = self.lock()) owner->checkAdmin();
                                     }))
                                     popup->show();
                             });
        });
    }
    // 레이팅 환급 알림을 받아 한 번 보여 주고 확인 처리한다.
    void checkRatingRefunds() {
        Service::get().fetchRatingRefunds(
            [self = Weak<VersusEntry>(this)](std::vector<RatingRefund> refunds, std::string error) {
                auto owner = self.lock();
                if (!owner || !error.empty() || refunds.empty()) return;
                owner->loadProfileData();
                owner->present([refunds = std::move(refunds)] {
                    showRatingRefunds(refunds);
                    std::vector<int64_t> ids;
                    for (auto const& refund : refunds) ids.push_back(refund.id);
                    Service::get().acknowledgeRatingRefunds(std::move(ids));
                });
            });
    }
    // 신고 처리 결과 알림을 받아 한 번 보여 주고 확인 처리한다.
    void checkReportNotices() {
        Service::get().fetchReportNotices(
            [self = Weak<VersusEntry>(this)](std::vector<ReportNotice> notices, std::string error) {
                auto owner = self.lock();
                if (!owner || !error.empty() || notices.empty()) return;
                owner->present([notices = std::move(notices)] {
                    showReportResults(notices);
                    std::vector<int64_t> ids;
                    for (auto const& notice : notices) ids.push_back(notice.id);
                    Service::get().acknowledgeReportNotices(std::move(ids));
                });
            });
    }
    void onMyReports(CCObject*) {
        if (auto* popup = MyReportsPopup::create()) popup->show();
    }
    void onAdmin(CCObject*) {
        if (auto* popup = AdminPanelPopup::create([self = Weak<VersusEntry>(this)] {
                if (auto owner = self.lock()) owner->checkAdmin();
            }))
            popup->show();
    }
    // 감지된 외부 치트 도구가 있으면 화면 구석에 ANTI-CHEAT 경고와 기능 요약을 표시한다.
    void refreshIntegrityNotice() {
        auto statuses = integrity::activeExternalCheats(false);
        m_integrity->setVisible(!statuses.empty());
        if (!statuses.empty()) {
            std::string text = "ANTI-CHEAT DETECTING";
            for (auto const& status : statuses) {
                auto summary = integrity::featureSummary(status.code, status.features);
                if (!summary.empty()) text += fmt::format("\n{}: {}", status.source, summary);
            }
            m_integrity->setString(text.c_str());
            m_integrity->limitLabelWidth(std::min(220.f, m_window.width - 20.f), .34f, .22f);
        }
    }
    // 매 프레임 호출. 팝업 대기열 처리와 Argon 알림/배지 갱신을 하고, 0.25초마다 안티치트 문구, 접속자 수, 시즌 시간을 갱신한다.
    // 연결된 상태에서는 계정 선택 팝업, Season 2 대기 진입을 처리하고, 15초마다 멤버십/밴/관리자 상태를 다시 확인한다
    // (알림류는 4번에 한 번).
    void update(float dt) override {
        showAutoNotes();
        presentPending();
        if (Service::get().takeArgonNotice())
            present([] {
                if (auto* popup = ArgonNoticePopup::create()) popup->show();
            });
        if (m_argon) {
            auto const state = Service::get().argonState();
            if (state != m_argonShown || m_argon->isVisible() != (state != "off" && Service::get().connected())) {
                m_argonShown = "-";
                refreshArgon();
            }
        }
        m_integrityTimer += std::clamp(dt, 0.f, .25f);
        if (m_integrityTimer >= .25f) {
            m_integrityTimer = 0.f;
            refreshIntegrityNotice();
            refreshOnline();
            refreshSeason();
            if (!m_debugSwitch) addDebugButtons();
        }
        refreshSeason2Entry();
        if (!m_releaseCurrent || !Service::get().connected() || m_transitioning) return;
        if (!Service::get().accountChoices().empty() && !m_choiceQueued && !m_choicePopup.lock()) askAccountChoice();
        if (editorplaza::waiting() && sceneSettled() &&
            (season2Public() ? Service::get().season2Access() && lounge::mapReady()
                             : m_season2 && m_season2->isVisible() && m_season2Ready == 1)) {
            openSeason2();
            if (m_transitioning)
                editorplaza::entered();
            else
                editorplaza::cancel();
            if (m_transitioning) return;
        }
        m_membershipTimer += std::clamp(dt, 0.f, .25f);
        if (m_membershipTimer >= 15.f) {
            checkMembership();
            if (m_ban) checkBan();
            if (m_admin && m_admin->isVisible()) checkAdmin();
            if (++m_noticeTicks >= 4) {
                m_noticeTicks = 0;
                checkReportNotices();
                checkRatingRefunds();
            }
        }
    }
    // 계정 기록이 여러 개라 선택이 필요할 때 모드 버튼을 막고 선택 팝업을 대기열에 넣는다.
    void askAccountChoice() {
        enabled(m_public, false);
        enabled(m_ranked, false);
        m_status->setString("CHOOSE THE VERSUS RECORD TO KEEP");
        m_status->setColor(kAlert);
        m_status->limitLabelWidth(m_window.width - 50.f, .30f, .18f);
        if (m_choiceQueued || m_choicePopup.lock()) return;
        m_choiceQueued = true;
        present([self = Weak<VersusEntry>(this)] {
            if (auto owner = self.lock()) owner->showAccountChoice();
        });
    }
    // 계정 선택 팝업을 연다. 선택이 끝나면 릴리스 확인부터 다시 하고, "Later" 면 이 화면에서 나간다.
    void showAccountChoice() {
        m_choiceQueued = false;
        if (Service::get().accountChoices().empty() || m_choicePopup.lock()) return;
        auto* popup = AccountChoicePopup::create(
            Service::get().accountChoices(),
            [self = Weak<VersusEntry>(this)] {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_status->setString("LOADING YOUR VERSUS RECORD...");
                owner->m_status->setColor(kIce);
                owner->checkRelease(0);
            },
            [self = Weak<VersusEntry>(this)] {
                if (auto owner = self.lock()) owner->goBack();
            });
        if (!popup) return;
        m_choicePopup = popup;
        popup->show();
    }
    // Public Room / Ranked 로 진입한다. 서버 미연결, 계정 선택 대기, 밴 상태면 진입하지 않는다.
    void choose(bool ranked) {
        if (m_transitioning || !m_releaseCurrent || !Service::get().connected()) return;
        if (!Service::get().accountChoices().empty()) {
            askAccountChoice();
            return;
        }
        if (m_ban) {
            if (auto* popup = BanNoticePopup::create(*m_ban)) popup->show();
            return;
        }
        m_transitioning = true;
        Service::get().setRankedCup("");
        if (ranked)
            showRanked();
        else
            showLobby();
    }
    void onPublic(CCObject*) { choose(false); }
    // Ranked 버튼. Season 2 가 공개돼 있으면 Season 2 로, 아니면 기존 랭크 화면으로 간다.
    void onRanked(CCObject*) {
        if (season2Public()) {
            openSeason2();
            return;
        }
        choose(true);
    }
    void onSeason2(CCObject*) {
        if (m_season2IsS1) {
            choose(true);
            return;
        }
        openSeason2();
    }
    // Season 2 로 들어간다. 라운지 맵이 아직 준비되지 않았으면 안내 후 대기한다.
    void openSeason2() {
        if (m_transitioning || !m_releaseCurrent || !Service::get().connected() || !Service::get().season2Access())
            return;
        if (!lounge::mapReady()) {
            Notification::create("Season 2's lounge is loading - a moment...", NotificationIcon::Loading, 1.5f)->show();
            return;
        }
        if (!Service::get().accountChoices().empty()) {
            askAccountChoice();
            return;
        }
        if (m_ban) {
            if (auto* popup = BanNoticePopup::create(*m_ban)) popup->show();
            return;
        }
        m_transitioning = true;
        showSeason2();
    }
    void onNotes(CCObject*) {
        if (auto* popup = PatchNotesPopup::create()) popup->show();
    }
    void onUpdate(CCObject*) { update::showUpdateCheck(); }
    // 버전이 바뀐 뒤 처음 열었을 때 패치 노트를 한 번 자동으로 보여 주고 본 버전을 기록한다.
    void showAutoNotes() {
        if (!m_autoNotes || !m_releaseCurrent || m_transitioning) return;
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (scene != CCDirector::sharedDirector()->getRunningScene()) return;
        m_autoNotes = false;
        Mod::get()->setSavedValue("patch-notes-seen", std::string(firebase_config::CLIENT_VERSION));
        (void)Mod::get()->saveData();
        log::info("Versus {}: showing the patch notes once", firebase_config::CLIENT_VERSION);
        onNotes(nullptr);
    }
    // 서버 점검 중이면 모드 버튼을 막고 남은 시간을 표시하며 점검 공지를 한 번 띄운다.
    // 5초 후 릴리스 확인을 다시 시도한다.
    void waitMaintenance() {
        auto const message = Service::get().maintenanceMessage();
        if (message.empty()) return;
        enabled(m_public, false);
        enabled(m_ranked, false);
        auto const& info = Service::get().maintenance();
        std::string status = "SERVER MAINTENANCE - " + maintenanceTimeLeft(*info, Service::get().serverNow());
        if (!status.empty() && status.back() == '.') status.pop_back();
        for (auto& c : status) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        m_status->setString(status.c_str());
        m_status->setColor(kIce);
        m_status->limitLabelWidth(m_window.width - 50.f, .30f, .18f);
        if (!std::exchange(m_maintenanceShown, true)) present([message] { showMaintenanceNotice(message); });
        runAction(CCSequence::create(CCDelayTime::create(5.f), CallFuncExt::create([self = Weak<VersusEntry>(this)] {
                                         if (auto entry = self.lock()) entry->checkRelease(0);
                                     }),
                                     nullptr));
    }
    // 서버의 릴리스 버전을 확인한 뒤 서버에 접속한다.
    // 실패하면 최대 3번까지 간격을 늘려 가며 재시도하고, 점검 중이면 점검 대기로 넘어간다.
    // 빌드 버전이 서버와 다르면(디버그 모드가 아닌 경우) 업데이트 안내를 띄우고 플레이를 막는다.
    // 접속에 성공하면 멤버십, 프로필, 밴, 관리자 상태, 알림을 한꺼번에 불러온다.
    void checkRelease(int attempt) {
        Service::get().fetchRelease([self = Weak<VersusEntry>(this), attempt](std::string version, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            if (version.empty() && Service::get().maintenance()) {
                owner->waitMaintenance();
                return;
            }
            if (version.empty()) {
                if (attempt < 3) {
                    owner->m_status->setString(
                        fmt::format("VERSION CHECK FAILED - RETRYING ({}/3)", attempt + 1).c_str());
                    owner->runAction(CCSequence::create(CCDelayTime::create(1.5f * (attempt + 1)),
                                                        CallFuncExt::create([self, attempt] {
                                                            if (auto entry = self.lock())
                                                                entry->checkRelease(attempt + 1);
                                                        }),
                                                        nullptr));
                    return;
                }
                owner->m_releaseCurrent = false;
                owner->m_update->setVisible(true);
                owner->m_status->setString("VERSION CHECK FAILED - REOPEN TO RETRY");
                return;
            }
            debug::noteServer(version);
            owner->m_releaseCurrent = version == firebase_config::CLIENT_VERSION || debug::activate(version);
            owner->placeDebugBadge();
            if (!owner->m_releaseCurrent) {
                owner->m_update->setVisible(true);
                owner->m_status->setString("UPDATE VERSUS TO PLAY");
                owner->present([] {
                    FLAlertLayer::create("Update Required",
                                         "Download the latest Versus from the server using the update button below.",
                                         "OK")
                        ->show();
                });
                return;
            }
            Service::get().connect([self](bool ok, std::string error) {
                if (auto entry = self.lock()) {
                    if (!ok && isMaintenanceMessage(error)) {
                        entry->waitMaintenance();
                        return;
                    }
                    enabled(entry->m_public, ok);
                    enabled(entry->m_ranked, ok);
                    entry->m_status->setString(ok ? "CHOOSE YOUR MATCH" : error.c_str());
                    if (!ok && Service::get().profile().accountId <= 0) {
                        entry->m_status->setString("LOG IN TO A GD ACCOUNT TO PLAY VERSUS");
                        entry->present([] {
                            FLAlertLayer::create(
                                "Geometry Dash Account",
                                "Versus saves your progress on your <cy>Geometry Dash account</c>, so every device "
                                "you log into shares it.\nLog in from <cg>Settings > Account</c>, then open Versus again.",
                                "OK")
                                ->show();
                        });
                    }
                    if (ok && !Service::get().accountChoices().empty()) {
                        entry->askAccountChoice();
                        return;
                    }
                    if (ok) {
                        entry->checkMembership();
                        entry->loadProfileData();
                        entry->checkBan();
                        entry->checkAdmin();
                        entry->checkReportNotices();
                        entry->checkRatingRefunds();
                    }
                }
            });
        });
    }
    // 뒤로 가기: CreatorLayer 로 돌아간다.
    void goBack() override {
        if (m_transitioning) return;
        m_transitioning = true;
        editorplaza::cancel();
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, CreatorLayer::scene()));
    }

public:
    // 화면 구성: 접속자 수, Argon 배지, 왼쪽의 Public Room / Ranked 카드, 오른쪽의 내 프로필 카드와
    // 꾸미기 버튼, 아래쪽의 패치 노트/멤버십/Discord/업데이트/신고 버튼, 상태 줄과 안티치트 표시.
    // 마지막에 릴리스 확인(checkRelease)을 시작한다.
    bool init() override {
        if (!initScene("VERSUS")) return false;
        editorplaza::homeReached();
        Service::get().setRankedCup("");
        addLiveButton(m_window.width - 36.f);
        m_onlineDot = CCDrawNode::create();
        m_onlineDot->setPosition({49.f, m_window.height - 24.f});
        addChild(m_onlineDot, 5);
        m_onlineDot->runAction(
            CCRepeatForever::create(CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(.9f, 1.35f)),
                                                       CCEaseSineInOut::create(CCScaleTo::create(.9f, 1.f)), nullptr)));
        m_online = label(this, "-- ONLINE", {57.f, m_window.height - 24.f}, .36f, 110.f, kMuted, true, "bigFont.fnt");
        m_online->setID("versus-online-count"_spr);
        refreshOnline();
        auto* actions = menu(this);
        m_argon = CCMenuItemSpriteExtra::create(argonBadgeSprite(""), this, menu_selector(VersusEntry::onArgon));
        m_argon->setID("versus-argon-badge"_spr);
        m_argon->setVisible(false);
        actions->addChild(m_argon);
        refreshArgon();
        float const top = m_window.height - 76.f, bottom = 62.f, half = m_window.width / 2.f;
        float const leftX = (half - std::min(236.f, half - 24.f)) / 2.f + 4.f, cardH = (top - bottom - 10.f) / 2.f;
        float const profileLeft = m_window.width - 12.f - std::min(300.f, m_window.width - half - 20.f);
        float const leftW = std::max(std::min(236.f, half - 24.f), profileLeft - 10.f - leftX);
        for (int i = 0; i < 2; ++i) {
            float const y = i ? bottom : bottom + cardH + 10.f;
            auto* card = panel(this, {leftX, y}, {leftW, cardH}, false);
            auto* icon = CCSprite::createWithSpriteFrameName(i ? "GJ_achBtn_001.png" : "GJ_versusBtn_001.png");
            float iconSize = std::max(icon->getContentSize().width, icon->getContentSize().height);
            icon->setScale(iconSize > 0.f ? std::min(1.f, (cardH - 26.f) / iconSize) : .5f);
            icon->setPosition({34.f, cardH / 2.f + 6.f});
            card->addChild(icon);
            auto* item =
                button(actions, this, i ? menu_selector(VersusEntry::onRanked) : menu_selector(VersusEntry::onPublic),
                       i ? "Ranked Match" : "Public Room", {leftX + leftW / 2.f + 28.f, y + cardH / 2.f + 8.f}, .6f);
            item->setID(i ? "ranked-mode-button"_spr : "public-mode-button"_spr);
            if (i)
                m_ranked = item;
            else
                m_public = item;
            if (i) {
                m_seasonWidth = leftW - 70.f;
                m_season =
                    label(card, "", {leftW / 2.f + 28.f, cardH / 2.f - 12.f}, .3f, m_seasonWidth, ccc3(255, 205, 126));
                m_season->setID("ranked-season-countdown"_spr);
                refreshSeason();
            }
            auto* caption = label(card, i ? "Find your rival. Climb the ranks." : "Create a room.\nChallenge a friend.",
                                  {leftW / 2.f + 28.f, i ? 15.f : cardH / 2.f - 17.f}, .32f, leftW - 70.f, kIce, false,
                                  "bigFont.fnt");
            if (!i) caption->setAlignment(kCCTextAlignmentCenter);
            if (i) {
                m_rankedCaption = caption;
                m_season2IsS1 = season2Public();
                m_season2 =
                    button(actions, this, menu_selector(VersusEntry::onSeason2),
                           m_season2IsS1 ? "Season 1" : "Season 2", {leftX + leftW / 2.f + 28.f, y + 16.f}, .5f);
                m_season2->setID("season2-mode-button"_spr);
                showSeason2Entry(Service::get().season2Access());
            }
        }
        m_profileW = std::min(300.f, m_window.width - half - 20.f);
        m_profileH = top - bottom;
        m_profileRight = m_window.width - 12.f;
        m_profilePanel = panel(this, {m_profileRight - m_profileW, bottom}, {m_profileW, m_profileH}, false);
        m_profilePanel->setID("versus-my-player"_spr);
        m_profileContent = CCNode::create();
        m_profilePanel->addChild(m_profileContent, 2);
        auto* custom = menu(m_profilePanel);
        auto* info = iconButton(custom, this, menu_selector(VersusEntry::onVersusProfile), "GJ_infoIcon_001.png",
                                {m_profileW - 17.f, m_profileH - 17.f}, 18.f);
        info->setID("versus-my-profile-button"_spr);
        char const* names[] = {"Aura", "Finisher", "Nickname", "Titles", "Emotes"};
        SEL_MenuHandler handlers[] = {menu_selector(VersusEntry::onAura), menu_selector(VersusEntry::onFinisher),
                                      menu_selector(VersusEntry::onNickname), menu_selector(VersusEntry::onTitles),
                                      menu_selector(VersusEntry::onEmotes)};
        constexpr int buttons = 5;
        float const slot = (m_profileW - 16.f) / buttons;
        for (int i = 0; i < buttons; ++i) {
            CCNode* sprite = nullptr;
            if (i == 0)
                sprite = CircleButtonSprite::createWithSpriteFrameName("GJ_sMagicIcon_001.png", 1.f,
                                                                       CircleBaseColor::Cyan, CircleBaseSize::Medium);
            else if (i == 1)
                sprite = CircleButtonSprite::createWithSpriteFrameName("GJ_demonIcon_001.png", 1.f,
                                                                       CircleBaseColor::Pink, CircleBaseSize::Medium);
            else if (i == 2)
                sprite = CCSprite::createWithSpriteFrameName("GJ_profileButton_001.png");
            else if (i == 3)
                sprite = CCSprite::createWithSpriteFrameName("GJ_achBtn_001.png");
            else {
                auto* face = emoteArt("smile");
                face->setScale(.62f);
                sprite = CircleButtonSprite::create(face, CircleBaseColor::Green, CircleBaseSize::Medium);
            }
            if (!sprite)
                sprite = ButtonSprite::create(names[i], 50, true, "goldFont.fnt", "GJ_button_01.png", 30.f, .6f);
            sprite->setScale(32.f / std::max(1.f, sprite->getContentSize().height));
            auto* item = CCMenuItemSpriteExtra::create(sprite, this, handlers[i]);
            item->setPosition({8.f + slot * (i + .5f), 29.f});
            item->setID(fmt::format("customize-{}", i));
            custom->addChild(item);
            label(m_profilePanel, names[i], {8.f + slot * (i + .5f), 8.f}, .32f, slot - 6.f, ccc3(255, 221, 120), false,
                  "goldFont.fnt");
        }
        placeSeasonPicker();
        refreshProfile();
        enabled(m_public, false);
        enabled(m_ranked, false);
        m_membership = label(this, "CHECKING MEMBERSHIP...", {m_window.width / 2.f, m_window.height - 46.f}, .43f,
                             m_window.width - 120.f, ccc3(255, 178, 77));
        auto* notesButton =
            iconButton(actions, this, menu_selector(VersusEntry::onNotes), "GJ_menuBtn_001.png", {}, 32.f);
        auto* membershipButton =
            button(actions, this, menu_selector(VersusEntry::onMembership), "Membership Code", {}, .48f);
        auto* discordButton =
            iconButton(actions, this, menu_selector(VersusEntry::onDiscord), "gj_discordIcon_001.png", {}, 30.f);
        discordButton->setID("versus-discord-button"_spr);
        membershipButton->setID("membership-code-button"_spr);
        m_update = iconButton(actions, this, menu_selector(VersusEntry::onUpdate), "GJ_shareBtn_001.png", {}, 32.f);
        // 아래 줄(오른쪽부터 Discord, 패치 노트, 멤버십 코드, 업데이트): 실제 폭과 화면 비례 간격으로 놓고,
        // 프로필 판넬 폭을 넘으면 같이 줄인다. 업데이트 버튼은 숨겨져 있어도 자리를 미리 잡아 둔다.
        float const gap = rowGap(m_window.width);
        layoutRowRight({discordButton, notesButton, membershipButton, m_update}, m_profileRight, 43.f, gap, m_profileW);
        m_update->setVisible(false);
        auto* mine = button(actions, this, menu_selector(VersusEntry::onMyReports), "My Reports", {leftX + 44.f, 43.f},
                            .44f, "GJ_button_04.png");
        mine->setID("my-reports-button"_spr);
        m_admin =
            button(actions, this, menu_selector(VersusEntry::onAdmin), "Reports",
                   {leftX + 44.f + mine->getScaledContentSize().width / 2.f + 50.f, 43.f}, .48f, "GJ_button_04.png");
        m_admin->setID("admin-reports-button"_spr);
        m_admin->setVisible(false);
        m_debugMenu = actions;
        m_debugLeft = leftX;
        addDebugButtons();
        // 패치 노트 자동 표시 설정이 켜져 있고 이 버전의 패치 노트를 아직 보지 않았으면 자동으로 보여 준다.
        m_autoNotes = firebase_config::AUTO_PATCH_NOTES &&
                      Mod::get()->getSavedValue<std::string>("patch-notes-seen") != firebase_config::CLIENT_VERSION;
        m_status = label(this, "CHECKING VERSION...", {m_window.width / 2.f, 13.f}, .30f, m_window.width - 50.f, kIce);
        m_integrity = label(this, "ANTI-CHEAT DETECTING", {10.f, 10.f}, .34f, std::min(220.f, m_window.width - 20.f),
                            ccc3(255, 92, 74), true, "chatFont.fnt");
        m_integrity->setID("anti-cheat-detecting"_spr);
        m_integrity->setAnchorPoint({0.f, 0.f});
        m_integrity->setAlignment(kCCTextAlignmentLeft);
        m_integrity->setVisible(false);
        refreshIntegrityNotice();
        checkRelease(0);
        scheduleUpdate();
        return true;
    }
    CREATE_FUNC(VersusEntry);
};
