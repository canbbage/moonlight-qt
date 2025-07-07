#include "rectangleselector.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"
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
            "按ESC取消选择但保留矩形\n"
            "再次按Ctrl+Alt+Shift+R退出选择模式");
        m_OverlayManager->setOverlayState(m_InfoTextOverlayId, true);
        
        // 如果已有矩形，则显示它
        if (m_CurrentRect.width() > 0 && m_CurrentRect.height() > 0) {
            updateRectangleOverlay();
            m_OverlayManager->setOverlayState(m_RectangleOverlayId, true);
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
            // 计算相对坐标
            QPointF relativePos = screenToRelative(event->x, event->y, windowWidth, windowHeight);
            
            // 检查是否在视频区域内
            if (relativePos.x() < 0 || relativePos.y() < 0) {
                // 点击在视频区域外，忽略
                qDebug() << "点击在视频区域外，忽略";
                return true;
            }
            
            // 开始选择
            m_Selecting = true;
            m_StartPoint = relativePos;
            m_EndPoint = m_StartPoint;
            
            // 更新矩形
            m_CurrentRect = QRectF(m_StartPoint, QSizeF(0, 0));
            updateRectangleOverlay();
            updateInfoTextOverlay();
            
            return true;
        }
        else if (event->state == SDL_RELEASED && m_Selecting) {
            // 计算相对坐标
            QPointF relativePos = screenToRelative(event->x, event->y, windowWidth, windowHeight);
            
            // 如果释放在视频区域外，使用边界值
            if (relativePos.x() < 0 || relativePos.y() < 0) {
                // 使用最后一个有效的位置
                relativePos = m_EndPoint;
            }
            
            // 完成选择
            m_Selecting = false;
            m_EndPoint = relativePos;
            
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
    
    // 计算相对坐标
    QPointF relativePos = screenToRelative(event->x, event->y, windowWidth, windowHeight);
    
    // 如果鼠标在视频区域外，使用边界值
    if (relativePos.x() < 0 || relativePos.y() < 0) {
        // 使用最后一个有效的位置
        relativePos = m_EndPoint;
    }
    
    // 更新结束点
    m_EndPoint = relativePos;
    
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
    // 获取视频区域的位置和尺寸
    SDL_Rect src, dst;
    
    // 原始视频分辨率
    src.x = src.y = 0;
    src.w = Session::get()->getActiveVideoWidth();
    src.h = Session::get()->getActiveVideoHeight();
    
    // 窗口尺寸
    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;
    
    // 计算视频在窗口中的实际显示区域（考虑黑边和拉伸）
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    
    qDebug() << "窗口尺寸:" << windowWidth << "x" << windowHeight 
             << "视频原始分辨率:" << src.w << "x" << src.h 
             << "视频显示区域:" << dst.x << "," << dst.y << "," << dst.w << "x" << dst.h;
    
    // 检查鼠标是否在视频显示区域内
    if (x < dst.x || x >= dst.x + dst.w || y < dst.y || y >= dst.y + dst.h) {
        qDebug() << "鼠标在视频区域外: (" << x << "," << y << ")";
        // 鼠标在视频区域外，返回无效坐标
        return QPointF(-1, -1);
    }
    
    // 计算鼠标在视频显示区域内的相对位置 (0.0-1.0)
    QPointF result(
        static_cast<qreal>(x - dst.x) / dst.w,
        static_cast<qreal>(y - dst.y) / dst.h
    );
    
    qDebug() << "坐标转换: 窗口(" << x << "," << y << ") -> "
             << "视频显示区域(" << dst.x << "," << dst.y << "," << dst.w << "," << dst.h << ") -> "
             << "相对坐标" << result;
    
    return result;
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
    
    // 打印调试信息
    qDebug() << "更新矩形覆盖层: " << m_CurrentRect 
             << " 视频尺寸: " << Session::get()->getActiveVideoWidth() << "x" << Session::get()->getActiveVideoHeight();
    
    // 创建矩形描述文本，使用特殊格式以便渲染器能够识别并绘制矩形
    // 格式: DRAW_RECT:x,y,width,height,r,g,b,a,lineWidth
    QString rectText = QString("DRAW_RECT:%1,%2,%3,%4,%5,%6,%7,%8,%9")
        .arg(m_CurrentRect.x(), 0, 'f', 6)  // 使用更高精度
        .arg(m_CurrentRect.y(), 0, 'f', 6)
        .arg(m_CurrentRect.width(), 0, 'f', 6)
        .arg(m_CurrentRect.height(), 0, 'f', 6)
        .arg(m_RectColor.red())
        .arg(m_RectColor.green())
        .arg(m_RectColor.blue())
        .arg(m_RectColor.alpha())
        .arg(m_LineWidth);
    
    // 更新矩形覆盖层
    m_OverlayManager->updateOverlayText(m_RectangleOverlayId, rectText.toUtf8().constData());
    m_OverlayManager->setOverlayState(m_RectangleOverlayId, true);
    
    // 打印调试信息
    qDebug() << "更新矩形: " << m_CurrentRect << " 文本: " << rectText;
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
        infoText = QString("选定区域: 左上角(%1, %2) 右下角(%3, %4) 大小(%5, %6)\n"
                         "按ESC取消选择但保留矩形\n"
                         "按Ctrl+Alt+Shift+R退出选择模式")
            .arg(m_CurrentRect.left(), 0, 'f', 4)
            .arg(m_CurrentRect.top(), 0, 'f', 4)
            .arg(m_CurrentRect.right(), 0, 'f', 4)
            .arg(m_CurrentRect.bottom(), 0, 'f', 4)
            .arg(m_CurrentRect.width(), 0, 'f', 4)
            .arg(m_CurrentRect.height(), 0, 'f', 4);
    }
    else {
        infoText = "矩形选择模式已激活\n"
                   "请使用鼠标左键拖动选择区域\n"
                   "按ESC取消选择\n"
                   "再次按Ctrl+Alt+Shift+R退出选择模式";
    }
    
    m_OverlayManager->updateOverlayText(m_InfoTextOverlayId, infoText.toUtf8().constData());
} 