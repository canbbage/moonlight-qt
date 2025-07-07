#pragma once

#include "overlaymanager.h"
#include <SDL_events.h>
#include <QRectF>
#include <QColor>

class RectangleSelector {
public:
    RectangleSelector(Overlay::OverlayManager* overlayManager);
    ~RectangleSelector();
    
    void activate();
    void deactivate();
    bool isActive() const { return m_Active; }
    
    bool handleMouseButtonEvent(SDL_MouseButtonEvent* event, int windowWidth, int windowHeight);
    bool handleMouseMotionEvent(SDL_MouseMotionEvent* event, int windowWidth, int windowHeight);
    
    QRectF getCurrentRectangle() const { return m_CurrentRect; }
    void setCurrentRectangle(const QRectF& rect);
    
    // 获取矩形的绝对坐标（像素）
    QRect getAbsoluteRect(int windowWidth, int windowHeight) const;
    
    // 设置矩形颜色和线宽
    void setRectangleStyle(const QColor& color, int lineWidth);
    
private:
    bool m_Active;
    bool m_Selecting;
    QPointF m_StartPoint;
    QPointF m_EndPoint;
    QRectF m_CurrentRect; // 存储为相对坐标 (0.0-1.0)
    
    QColor m_RectColor;
    int m_LineWidth;
    
    Overlay::OverlayManager* m_OverlayManager;
    Overlay::OverlayType m_RectangleOverlayId;
    Overlay::OverlayType m_InfoTextOverlayId;
    
    void updateRectangleOverlay();
    void updateInfoTextOverlay();
    
    // 将屏幕坐标转换为相对坐标 (0.0-1.0)
    QPointF screenToRelative(int x, int y, int windowWidth, int windowHeight) const;
    
    // 规范化矩形（确保宽度和高度为正）
    QRectF normalizeRect(const QPointF& start, const QPointF& end) const;
}; 