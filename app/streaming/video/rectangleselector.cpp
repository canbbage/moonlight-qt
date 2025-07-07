#include "rectangleselector.h"
#include <QDebug>

RectangleSelector::RectangleSelector(Overlay::OverlayManager* overlayManager)
    : m_Active(false)
    , m_Selecting(false)
    , m_StartPoint(0, 0)
    , m_EndPoint(0, 0)
    , m_CurrentRect(0, 0, 0, 0)
    , m_RectColor(Qt::red)
    , m_LineWidth(2)
    , m_OverlayManager(overlayManager)
    , m_RectangleOverlayId(Overlay::OverlayDebug)
    , m_InfoTextOverlayId(Overlay::OverlayStatusUpdate)
{
}

RectangleSelector::~RectangleSelector()
{
    deactivate();
}

void RectangleSelector::activate()
{
    if (!m_Active) {
        m_Active = true;
        m_Selecting = false;
        
        // 显示更明确的提示信息
        m_OverlayManager->updateOverlayText(m_InfoTextOverlayId, 
            "矩形选择模式已激活\n"
            "请使用鼠标左键拖动选择区域\n"
            "按ESC取消选择\n"
            "再次按Ctrl+Alt+Shift+R退出选择模式");
        m_OverlayManager->setOverlayState(m_InfoTextOverlayId, true);
        
        // 如果已有矩形，则显示它
        if (m_CurrentRect.width() > 0 && m_CurrentRect.height() > 0) {
            updateRectangleOverlay();
        }
    }
}

void RectangleSelector::deactivate()
{
    if (m_Active) {
        m_Active = false;
        m_Selecting = false;
        
        // 隐藏提示信息
        m_OverlayManager->setOverlayState(m_InfoTextOverlayId, false);
        
        // 保留矩形显示
        // updateRectangleOverlay();
    }
}

bool RectangleSelector::handleMouseButtonEvent(SDL_MouseButtonEvent* event, int windowWidth, int windowHeight)
{
    if (!m_Active) {
        return false;
    }
    
    if (event->button == SDL_BUTTON_LEFT) {
        if (event->state == SDL_PRESSED) {
            // 开始选择
            m_Selecting = true;
            m_StartPoint = screenToRelative(event->x, event->y, windowWidth, windowHeight);
            m_EndPoint = m_StartPoint;
            
            // 更新矩形
            m_CurrentRect = QRectF(m_StartPoint, QSizeF(0, 0));
            updateRectangleOverlay();
            updateInfoTextOverlay();
            
            return true;
        }
        else if (event->state == SDL_RELEASED && m_Selecting) {
            // 完成选择
            m_Selecting = false;
            m_EndPoint = screenToRelative(event->x, event->y, windowWidth, windowHeight);
            
            // 更新最终矩形
            m_CurrentRect = normalizeRect(m_StartPoint, m_EndPoint);
            updateRectangleOverlay();
            updateInfoTextOverlay();
            
            qDebug() << "矩形选择完成: " << m_CurrentRect;
            
            return true;
        }
    }
    
    return false;
}

bool RectangleSelector::handleMouseMotionEvent(SDL_MouseMotionEvent* event, int windowWidth, int windowHeight)
{
    if (!m_Active || !m_Selecting) {
        return false;
    }
    
    // 更新结束点
    m_EndPoint = screenToRelative(event->x, event->y, windowWidth, windowHeight);
    
    // 更新矩形
    m_CurrentRect = normalizeRect(m_StartPoint, m_EndPoint);
    updateRectangleOverlay();
    updateInfoTextOverlay();
    
    return true;
}

void RectangleSelector::setCurrentRectangle(const QRectF& rect)
{
    m_CurrentRect = rect;
    
    if (m_Active) {
        updateRectangleOverlay();
        updateInfoTextOverlay();
    }
}

QRect RectangleSelector::getAbsoluteRect(int windowWidth, int windowHeight) const
{
    return QRect(
        qRound(m_CurrentRect.x() * windowWidth),
        qRound(m_CurrentRect.y() * windowHeight),
        qRound(m_CurrentRect.width() * windowWidth),
        qRound(m_CurrentRect.height() * windowHeight)
    );
}

void RectangleSelector::setRectangleStyle(const QColor& color, int lineWidth)
{
    m_RectColor = color;
    m_LineWidth = lineWidth;
    
    if (m_Active && m_CurrentRect.width() > 0 && m_CurrentRect.height() > 0) {
        updateRectangleOverlay();
    }
}

QPointF RectangleSelector::screenToRelative(int x, int y, int windowWidth, int windowHeight) const
{
    return QPointF(
        static_cast<qreal>(x) / windowWidth,
        static_cast<qreal>(y) / windowHeight
    );
}

QRectF RectangleSelector::normalizeRect(const QPointF& start, const QPointF& end) const
{
    qreal x = qMin(start.x(), end.x());
    qreal y = qMin(start.y(), end.y());
    qreal width = qAbs(end.x() - start.x());
    qreal height = qAbs(end.y() - start.y());
    
    return QRectF(x, y, width, height);
}

void RectangleSelector::updateRectangleOverlay()
{
    if (m_CurrentRect.width() <= 0 || m_CurrentRect.height() <= 0) {
        m_OverlayManager->setOverlayState(m_RectangleOverlayId, false);
        return;
    }
    
    // 创建矩形描述文本
    QString rectText = QString("RECT:%1,%2,%3,%4")
        .arg(m_CurrentRect.x())
        .arg(m_CurrentRect.y())
        .arg(m_CurrentRect.width())
        .arg(m_CurrentRect.height());
    
    // 更新矩形覆盖层
    m_OverlayManager->updateOverlayText(m_RectangleOverlayId, rectText.toUtf8().constData());
    m_OverlayManager->setOverlayState(m_RectangleOverlayId, true);
}

void RectangleSelector::updateInfoTextOverlay()
{
    if (!m_Active) {
        return;
    }
    
    QString infoText;
    
    if (m_Selecting) {
        infoText = "正在选择区域...\n"
                   "释放鼠标左键完成选择\n"
                   "按ESC取消选择";
    }
    else if (m_CurrentRect.width() > 0 && m_CurrentRect.height() > 0) {
        infoText = QString("选定区域: 左上角(%.2f, %.2f) 右下角(%.2f, %.2f) 大小(%.2f, %.2f)\n"
                         "按Ctrl+Alt+Shift+R退出选择模式")
            .arg(m_CurrentRect.left())
            .arg(m_CurrentRect.top())
            .arg(m_CurrentRect.right())
            .arg(m_CurrentRect.bottom())
            .arg(m_CurrentRect.width())
            .arg(m_CurrentRect.height());
    }
    else {
        infoText = "矩形选择模式已激活\n"
                   "请使用鼠标左键拖动选择区域\n"
                   "按ESC取消选择\n"
                   "再次按Ctrl+Alt+Shift+R退出选择模式";
    }
    
    m_OverlayManager->updateOverlayText(m_InfoTextOverlayId, infoText.toUtf8().constData());
} 