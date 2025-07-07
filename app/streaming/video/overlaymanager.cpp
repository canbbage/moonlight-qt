#include "overlaymanager.h"
#include "path.h"

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr)
{
    // 不再从文件加载字体，而是使用系统字体
    
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    strncpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    m_Overlays[type].text[getOverlayMaxTextLength() - 1] = '\0';

    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        // 使用系统字体，尝试加载支持中文的字体
        #ifdef Q_OS_WIN
        // Windows 上使用微软雅黑
        m_Overlays[type].font = TTF_OpenFont("C:\\Windows\\Fonts\\msyh.ttc", m_Overlays[type].fontSize);
        #elif defined(Q_OS_MAC)
        // macOS 上使用苹方
        m_Overlays[type].font = TTF_OpenFont("/System/Library/Fonts/PingFang.ttc", m_Overlays[type].fontSize);
        #else
        // Linux 上尝试使用 Noto Sans CJK
        m_Overlays[type].font = TTF_OpenFont("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            // 尝试其他常见位置
            m_Overlays[type].font = TTF_OpenFont("/usr/share/fonts/noto/NotoSans-Regular.ttf", m_Overlays[type].fontSize);
        }
        #endif
        
        // 如果系统字体加载失败，回退到内置字体
        if (m_Overlays[type].font == nullptr) {
            if (m_FontData.isEmpty()) {
                m_FontData = Path::readDataFile("ModeSeven.ttf");
            }
            
            if (!m_FontData.isEmpty()) {
                m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                                   1,
                                                   m_Overlays[type].fontSize);
            }
        }
        
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    if (m_Overlays[type].enabled) {
        // 使用 TTF_RenderUTF8_Blended_Wrapped 来支持 UTF-8 编码的文本
        SDL_Surface* surface = TTF_RenderUTF8_Blended_Wrapped(m_Overlays[type].font,
                                                              m_Overlays[type].text,
                                                              m_Overlays[type].color,
                                                              1024);
        SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);
    }

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);
}
