// 스프라이트 프레임 캐시 조회와 파일 스프라이트 생성 헬퍼.
#pragma once
#include <Geode/cocos/include/cocos2d.h>
#include <Geode/loader/Log.hpp>

namespace versus {

// 캐시에서 이름(또는 별칭)으로 프레임을 찾는다. 없으면 nullptr.
inline cocos2d::CCSpriteFrame* cachedFrame(char const* name) {
    auto* cache = cocos2d::CCSpriteFrameCache::sharedSpriteFrameCache();
    if (!cache || !name || !*name || !cache->m_pSpriteFrames) return nullptr;
    if (auto* frame = static_cast<cocos2d::CCSpriteFrame*>(cache->m_pSpriteFrames->objectForKey(name))) return frame;
    if (!cache->m_pSpriteFramesAliases) return nullptr;
    auto* key = static_cast<cocos2d::CCString*>(cache->m_pSpriteFramesAliases->objectForKey(name));
    return key ? static_cast<cocos2d::CCSpriteFrame*>(cache->m_pSpriteFrames->objectForKey(key->getCString()))
               : nullptr;
}

// 캐시에 없으면 sheet를 로드한 뒤 다시 찾는다. 그래도 없으면 경고 로그를 남긴다.
inline cocos2d::CCSpriteFrame* loadedFrame(char const* name, char const* sheet) {
    auto* frame = cachedFrame(name);
    if (!frame && sheet && *sheet) {
        cocos2d::CCSpriteFrameCache::sharedSpriteFrameCache()->addSpriteFramesWithFile(sheet);
        frame = cachedFrame(name);
        if (!frame) geode::log::warn("Versus: frame {} is not in {}", name, sheet);
    }
    return frame;
}

// 프레임으로 스프라이트를 만든다. 프레임이 없으면 nullptr.
inline cocos2d::CCSprite* loadedFrameSprite(char const* name, char const* sheet = nullptr) {
    auto* frame = loadedFrame(name, sheet);
    return frame ? cocos2d::CCSprite::createWithSpriteFrame(frame) : nullptr;
}

// 미니게임 버튼 그림. GD 2.2091 시트에서 Google Play 게임 버튼(GJ_gpgBtn_001.png)이 빠져서, 모양이 같은 Game Center
// 버튼(GJ_gkBtn_001.png)을 먼저 쓰고 없으면 예전 그림을 쓴다.
inline char const* miniGamesFrame() {
    static constexpr char const* kNames[] = {"GJ_gkBtn_001.png", "GJ_gpgBtn_001.png"};
    for (auto* name : kNames)
        if (cachedFrame(name)) return name;
    return kNames[0];
}

// 파일 스프라이트를 만든다. 파일을 못 찾아 대체 이미지가 쓰이면 nullptr.
inline cocos2d::CCSprite* fileSprite(char const* file) {
    auto* sprite = cocos2d::CCSprite::create(file);
    return sprite && !sprite->isUsingFallback() ? sprite : nullptr;
}

}
