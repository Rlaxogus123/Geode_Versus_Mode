// 1.0.8: 맵의 노래 정보와 Jukebox(fleym.nongd) 연동. Ranked 매치 카드의 노래 칸(RankedLayer)과 1:1 / 그룹 방의
// 노래 줄(SongStrip)이 같이 쓴다. VersusUI.cpp 안(namespace versus)에서 한 번만 include한다.
namespace song {
// Jukebox 모드 ID. 켜져 있으면 커스텀 노래의 NONG(다른 음원)을 플레이어가 골라 받을 수 있다.
constexpr char const* kJukeboxID = "fleym.nongd";
struct MapSong {
    int id = 0;              // 커스텀 노래 ID (0: GD 공식 노래)
    bool official = false;
    int more = 0;            // 이 맵이 쓰는 다른 노래 수
    std::string name, artist;
    float sizeMB = 0.f;
};
// 캐시된 레벨에서 대표 노래를 찾는다. 노래 정보(이름 등)는 GD가 받아 둔 만큼만.
std::optional<MapSong> mapSong(int64_t levelID) {
    auto* level = cachedLevel(levelID);
    if (!level) return std::nullopt;
    MapSong song;
    song.id = level->m_songID;
    if (song.id <= 0) {
        song.official = true;
        song.id = 0;
        int const track = level->m_audioTrack;
        song.name = std::string(LevelTools::getAudioTitle(track));
        song.artist = std::string(LevelTools::nameForArtist(LevelTools::artistForAudio(track)));
    } else {
        auto* music = MusicDownloadManager::sharedState();
        SongInfoObject* info = song.id >= 10'000'000 ? music->getMusicObject(song.id) : nullptr;
        if (!info) info = music->getSongInfoObject(song.id);
        if (info) {
            song.name = std::string(info->m_songName);
            song.artist = std::string(info->m_artistName);
            song.sizeMB = info->m_fileSize;
        }
    }
    std::set<int> others;
    std::string const list(level->m_songIDs);
    for (size_t at = 0; at < list.size();) {
        size_t const end = std::min(list.find(',', at), list.size());
        int value = 0;
        auto const parsed = std::from_chars(list.data() + at, list.data() + end, value);
        if (parsed.ec == std::errc() && value > 0 && value != song.id) others.insert(value);
        at = end + 1;
    }
    song.more = static_cast<int>(others.size());
    return song;
}
// Jukebox로 다른 음원을 고를 수 있는 노래인지(Jukebox가 켜져 있고 커스텀 노래).
bool jukeboxFor(MapSong const& song) {
    return !song.official && song.id > 0 && Loader::get()->isModLoaded(kJukeboxID);
}
// 노래 이름. 정보가 아직 없으면 "Official song" / "Song <ID>".
std::string title(MapSong const& song) {
    if (!song.name.empty()) return song.name;
    return song.official ? std::string("Official song") : fmt::format("Song {}", song.id);
}
// 줄 하나를 폭에 맞춘다: minScale까지 줄이고, 그래도 넘치면 뒤를 잘라 "..."을 붙인다(긴 노래 이름).
void fitLine(CCLabelBMFont* line, float maxWidth, float scale, float minScale) {
    if (!line || maxWidth <= 0.f) return;
    line->setScale(scale);
    float const full = line->getContentSize().width;
    if (full * scale <= maxWidth) return;
    line->setScale(std::max(minScale, maxWidth / std::max(1.f, full)));
    std::string text = line->getString();
    while (text.size() > 1 && line->getContentSize().width * line->getScale() > maxWidth) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        while (!text.empty() && text.back() == ' ') text.pop_back();
        line->setString((text + "...").c_str());
    }
}
// 노래 칸 왼쪽 원판: Jukebox가 있으면 그 원판 그림(JB_PinDisc), 없으면 GD 음악 아이콘. 높이 size로 맞춘다.
CCSprite* disc(bool jukebox, float size) {
    CCSprite* sprite = nullptr;
    if (jukebox)
        if (auto* frame = cachedFrame("fleym.nongd/JB_PinDisc.png")) sprite = CCSprite::createWithSpriteFrame(frame);
    if (!sprite) sprite = loadedFrameSprite("GJ_musicOnBtn_001.png", "GJ_GameSheet03.plist");
    if (sprite) sprite->setScale(size / std::max(1.f, sprite->getContentSize().height));
    return sprite;
}
// Jukebox의 NONG 목록을 연다. Jukebox에는 다른 모드가 부를 수 있는 기능이 없어서, GD의 노래 위젯
// (CustomSongWidget)을 owner 아래에 보이지 않게 만들어 두고 Jukebox가 그 위젯에 붙이는 버튼(nong-button)을 대신
// 누른다(GD 레벨 화면에서 그 버튼을 누르는 것과 같다). 받은 NONG은 GD가 받은 노래로 치므로(Jukebox의 pathForSong)
// 맵 준비가 그대로 READY가 된다. 위젯은 노래가 바뀔 때만 새로 만든다(화면이 다시 그려져도 남는다).
class JukeboxOpener {
    geode::Ref<CustomSongWidget> m_widget;
    int m_song = 0;

public:
    bool open(CCNode* owner, int songID) {
        if (!owner || songID <= 0 || !Loader::get()->isModLoaded(kJukeboxID)) return false;
        if (!m_widget || m_song != songID || m_widget->getParent() != owner) {
            if (m_widget) m_widget->removeFromParent();
            auto* music = MusicDownloadManager::sharedState();
            bool const library = songID >= 10'000'000;
            SongInfoObject* info = library ? music->getMusicObject(songID) : nullptr;
            if (!info) info = music->getSongInfoObject(songID);
            if (!info) info = SongInfoObject::create(songID);
            auto* widget = info ? CustomSongWidget::create(info, nullptr, false, false, false, false, false, library, 0)
                                : nullptr;
            if (!widget) return false;
            widget->setVisible(false);
            owner->addChild(widget, -100);
            m_widget = widget;
            m_song = songID;
        }
        if (auto* opener = typeinfo_cast<CCMenuItemSpriteExtra*>(
                m_widget->getChildByIDRecursive(std::string(kJukeboxID) + "/nong-button"))) {
            opener->activate();
            return true;
        }
        Notification::create("Jukebox can't open this song yet. Try again in a moment.", NotificationIcon::Warning)
            ->show();
        return false;
    }
};
}

// 1.0.8: 1:1 / 그룹 방의 노래 줄. 왼쪽 원판(Jukebox가 있으면 그 노래의 NONG 목록을 여는 버튼), 노래 이름과
// "by 작곡가 | ID | 크기", 오른쪽 받기 상태(READY / % / WAITING / SKIPPED / FAILED). 방 화면은 상태가 바뀔 때마다
// 다시 그려지므로 Jukebox 위젯은 방 화면이 들고 있고, 이 줄은 원판을 누르면 onJukebox(노래 ID)만 부른다.
// refresh()는 노래 정보가 늦게 오거나(GD의 노래 정보 요청) 받기가 진행되면 그 자리에서 고친다. 노래를 못 받은
// 맵(FAILED)이면 Jukebox 원판이 천천히 커졌다 작아지며 다른 음원을 골라 보라고 알린다.
class SongStrip final : public CCNode {
    static constexpr int kPulseTag = 0x5306;
    int64_t m_level = 0;
    CCSize m_size;
    std::function<void(int)> m_onJukebox;
    std::string m_key;
    int m_song = 0;
    bool m_jukebox = false, m_pulsing = false;
    CCLabelBMFont* m_state = nullptr;
    CCSprite* m_disc = nullptr;
    float m_discScale = 1.f;

    float stateWidth() const { return std::clamp(m_size.width * .22f, 30.f, 52.f); }
    bool init(int64_t levelID, CCSize size, std::function<void(int)> onJukebox) {
        if (!CCNode::init()) return false;
        m_level = levelID;
        m_size = size;
        m_onJukebox = std::move(onJukebox);
        setContentSize(size);
        ignoreAnchorPointForPosition(false);
        setAnchorPoint({.5f, .5f});
        return true;
    }
    // 판, 원판, 두 줄, 상태 글자를 새로 만든다(노래 정보나 Jukebox 여부가 바뀌었을 때).
    void build(std::optional<song::MapSong> const& value) {
        removeAllChildrenWithCleanup(true);
        m_state = nullptr;
        m_disc = nullptr;
        m_pulsing = false;
        float const w = m_size.width, h = m_size.height;
        auto* plate = NineSlice::create("square02b_001.png");
        plate->setContentSize(m_size);
        plate->setPosition({w / 2.f, h / 2.f});
        plate->setColor(ccc3(6, 22, 52));
        plate->setOpacity(190);
        addChild(plate, -1);
        float const discSize = std::min(18.f, h - 4.f), discX = 4.f + discSize / 2.f;
        m_jukebox = value && song::jukeboxFor(*value);
        m_song = value && !value->official ? value->id : 0;
        if ((m_disc = song::disc(m_jukebox, discSize))) {
            m_discScale = m_disc->getScale();
            if (m_jukebox) {
                // 원판은 칸(holder) 가운데에 두고 그것만 맥박처럼 키운다(버튼 자신의 눌림 크기 연출과 겹치지 않게).
                auto* face = CCNode::create();
                face->setContentSize({discSize, discSize});
                m_disc->setPosition({discSize / 2.f, discSize / 2.f});
                face->addChild(m_disc);
                auto* holder = CCMenu::create();
                holder->setPosition({0.f, 0.f});
                addChild(holder, 2);
                auto* button = CCMenuItemSpriteExtra::create(face, this, menu_selector(SongStrip::onDisc));
                button->setID("room-song-jukebox"_spr);
                button->setPosition({discX, h / 2.f});
                holder->addChild(button);
            } else {
                m_disc->setPosition({discX, h / 2.f});
                addChild(m_disc);
            }
        }
        float const left = discX + discSize / 2.f + 5.f;
        float const textW = std::max(20.f, w - left - stateWidth() - 8.f);
        m_state = label(this, "", {w - 5.f, h / 2.f}, .24f, 0.f, kIce, false, "bigFont.fnt");
        m_state->setAnchorPoint({1.f, .5f});
        m_state->setID("room-song-state"_spr);
        if (!value) {
            label(this, "SONG", {left, h / 2.f + 4.f}, .24f, textW, kMuted, true, "bigFont.fnt");
            label(this, "Shown once the map is downloaded", {left, h / 2.f - 5.f}, .3f, textW, kMuted, true,
                  "chatFont.fnt");
            return;
        }
        auto* name = label(this, song::title(*value), {left, h / 2.f + 4.f}, .24f, 0.f, ccWHITE, true, "bigFont.fnt");
        name->setID("room-song-name"_spr);
        song::fitLine(name, textW, .24f, .16f);
        std::string detail = "by " + (value->artist.empty() ? std::string("Unknown") : value->artist);
        detail += value->official ? std::string("  |  GD song") : fmt::format("  |  ID {}", value->id);
        if (!value->official && value->sizeMB > 0.f) detail += fmt::format("  {:.1f}MB", value->sizeMB);
        if (value->more > 0) detail += fmt::format("  +{}", value->more);
        song::fitLine(label(this, detail, {left, h / 2.f - 5.f}, .3f, 0.f, ccc3(255, 214, 120), true, "chatFont.fnt"),
                      textW, .3f, .22f);
    }
    void onDisc(CCObject*) {
        if (m_song > 0 && m_onJukebox) m_onJukebox(m_song);
    }

public:
    static SongStrip* create(int64_t levelID, CCSize size, std::function<void(int)> onJukebox) {
        auto* strip = new SongStrip;
        if (strip->init(levelID, size, std::move(onJukebox))) {
            strip->autorelease();
            return strip;
        }
        delete strip;
        return nullptr;
    }
    // failed: 방의 맵 받기가 실패했음(레벨은 받았으니 노래 쪽일 가능성이 크다).
    void refresh(bool failed) {
        auto const value = song::mapSong(m_level);
        bool const jukebox = value && song::jukeboxFor(*value);
        std::string const key = !value ? std::string("map")
                                       : fmt::format("{}|{}|{}|{:.2f}|{}|{}", value->id, value->name, value->artist,
                                                     value->sizeMB, value->more, jukebox);
        if (key != m_key) {
            m_key = key;
            build(value);
        }
        if (!m_state) return;
        auto* music = MusicDownloadManager::sharedState();
        std::string text = "WAITING";
        ccColor3B color = kMuted;
        bool help = false;
        if (!value)
            text = "...";
        else if (value->official || music->isSongDownloaded(value->id)) {
            text = "READY";
            color = ccc3(135, 255, 175);
        } else if (mapAudioSkipped(m_level)) {
            text = "SKIPPED";
            color = ccc3(255, 190, 110);
        } else if (failed) {
            text = "FAILED";
            color = kNoMusicColor;
            help = true;
        } else if (int const percent = music->getDownloadProgress(value->id); percent > 0) {
            text = fmt::format("{}%", std::clamp(percent, 0, 99));
            color = kIce;
        }
        if (text != m_state->getString()) {
            m_state->setString(text.c_str());
            m_state->setScale(.24f);
            m_state->limitLabelWidth(stateWidth(), .24f, .12f);
        }
        m_state->setColor(color);
        bool const pulse = help && m_jukebox && m_disc;
        if (pulse == m_pulsing || !m_disc) return;
        m_pulsing = pulse;
        m_disc->stopActionByTag(kPulseTag);
        m_disc->setScale(m_discScale);
        if (!pulse) return;
        auto* action = CCRepeatForever::create(
            CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(.55f, m_discScale * 1.18f)),
                               CCEaseSineInOut::create(CCScaleTo::create(.55f, m_discScale)), nullptr));
        action->setTag(kPulseTag);
        m_disc->runAction(action);
    }
};
