/*
    SPDX-FileCopyrightText: 2026 Miryu Gaming <dev@miryugaming.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "miryuduo.h"

#include "effect/effecthandler.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"

#if MIRYUDUO_NEW_KWIN_API
// 新版 effect.h 只前向声明 RenderViewport，用到它的成员函数时必须补上头文件
#include "core/renderviewport.h"
#include "opengl/eglcontext.h"
#endif

#include <QDebug>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QVector2D>

#include <algorithm>
#include <chrono>
#include <cmath>

Q_LOGGING_CATEGORY(lcMiryuDuo, "kwin_effect_miryu_duo")

namespace KWin
{

namespace
{
// 折叠/展开总时长（毫秒）
constexpr int kDuration = 1800;
// 完全折起时的模糊半径 = 屏幕宽度 × 该比例
constexpr double kBlurWidthRatio = 0.035;
// 模糊半径上限（设备像素），防止超宽屏上采样半径大到没有意义
constexpr double kBlurRadiusMax = 140.0;
// 判为"完全展平"的阈值
constexpr double kFlatEpsilon = 0.001;
// 无阻塞时，注销开始（KWin Quitting）后等多久再开始折叠动画（毫秒）。
// 这段时间让保存阶段快速完成，折叠动画在 plasmashell 退出前播放。
constexpr int kPreFoldDelay = 3000;
// 兜底：注销开始后若一直没有 Plasma 面板（Dock 窗口）关闭，
// 到这个时间仍触发折叠，避免没有面板时动画永不播放。
// 注意：不使用 KWin Normal 判断阻塞是否完成，因为实测 Normal 可能在阻塞
// （保存对话框）期间提前到达；面板关闭才是 SmsDie 已发、阻塞已处理完的可靠信号。
constexpr int kLogoutFallbackDelay = 60000;
} // namespace

MiryuDuoEffect::MiryuDuoEffect()
{
    // 注销计时器：先跑 3 秒预折叠阶段。
    // 3 秒到点时若面板已关闭（无阻塞，SmsDie 已发）则折叠；
    // 若面板未关闭（可能有阻塞）则切换为 60 秒兜底，继续等面板关闭。
    m_logoutDelayTimer.setSingleShot(true);
    connect(&m_logoutDelayTimer, &QTimer::timeout, this, &MiryuDuoEffect::startLogoutFold);

    connect(effects, &EffectsHandler::sessionStateChanged,
            this, &MiryuDuoEffect::slotSessionStateChanged);
    connect(effects, &EffectsHandler::screenLockingChanged,
            this, &MiryuDuoEffect::slotScreenLockingChanged);
    connect(effects, &EffectsHandler::windowClosed,
            this, &MiryuDuoEffect::slotWindowClosed);

    // 会话开始时桌面是展平的：开机画面（ksplash）不应该被折叠，
    // 登入动画在 ksplash 关闭或锁屏解锁时才播放。
    m_progress = 1.0;
}

MiryuDuoEffect::~MiryuDuoEffect() = default;

bool MiryuDuoEffect::supported()
{
#if MIRYUDUO_NEW_KWIN_API
    // KWin 6.6 起 GLFramebuffer::blitSupported() 被移除，
    // 改由 OpenGL 上下文自己报告是否支持帧缓冲 Blit
    return effects->openglContext() && effects->openglContext()->supportsBlits();
#else
    return effects->isOpenGLCompositing() && GLFramebuffer::blitSupported();
#endif
}

bool MiryuDuoEffect::isActive() const
{
    // 正在播放动画、处于折叠状态、或需要抓取一帧冻结画面时接管绘制。
    // 最后一种情况用于：面板在 3 秒预折叠阶段内关闭时，立即把当前桌面
    // （窗口尚未全部关闭）冻结下来，等计时器到点再开始折叠动画。
    return m_running || m_progress < 1.0 - kFlatEpsilon
        || (m_freezeFrame && !m_hasFrozenFrame);
}

int MiryuDuoEffect::requestedEffectChainPosition() const
{
    // 尽量排在特效链末端，这样折叠作用在其它特效（含模糊）合成后的完整画面上
    return 90;
}

void MiryuDuoEffect::slotSessionStateChanged()
{
    // KWin 6.x 的 SessionManager::setState(uint) 把 D-Bus 参数
    // 0/1/2 分别映射成 Saving/Quitting/Normal，而 ksmserver 发的是
    // Normal/Saving/Quitting，于是特效看到的状态整体错开了一格：
    //
    //   ksmserver Saving   （开始注销，客户端开始回应）→ KWin Quitting
    //   ksmserver Quitting （清场，刚发完 exitkde 通知）→ KWin Normal
    //   ksmserver Normal   （取消注销）                → KWin Saving
    //
    // 时序设计（用面板关闭而非 KWin Normal 判断阻塞是否完成，因为 Normal
    // 实测可能在保存对话框期间提前到达，不可靠）：
    //   1. KWin Quitting（注销开始）时启动 3 秒计时器，m_panelClosed=false。
    //   2. 3 秒到点时：
    //      - 若面板已关闭（无阻塞，SmsDie 已发）→ 开始折叠。
    //      - 若面板未关闭（可能有阻塞）→ 不折叠，切换为 60 秒兜底。
    //   3. 面板在 3 秒阶段内关闭：立即抓取冻结帧（窗口尚未全部关闭），
    //      等 3 秒计时器到点再开始折叠动画。
    //   4. 面板在 60 秒兜底阶段内关闭：立即折叠。
    //   5. 60 秒兜底到点仍无面板关闭：直接折叠（极端情况）。
    switch (effects->sessionState()) {
    case SessionState::Quitting:
        // ksmserver Saving 阶段：注销开始，可能正在阻塞。
        // 启动 3 秒预折叠计时器，重置状态标记。
        m_foldingForLogout = true;
        m_panelClosed = false;
        m_preFoldPhase = true;
        if (!m_logoutDelayTimer.isActive()) {
            m_logoutDelayTimer.start(kPreFoldDelay);
        }
        break;
    case SessionState::Normal:
        // ksmserver Quitting 阶段。注意：此信号可能在阻塞期间提前到达，
        // 不能作为"阻塞已完成"的依据，因此不在这里触发折叠。
        // 折叠决策统一由面板关闭和计时器驱动。
        m_foldingForLogout = true;
        break;
    case SessionState::Saving:
        // 注销被取消，复位并展开
        if (m_foldingForLogout) {
            m_foldingForLogout = false;
            m_panelClosed = false;
            m_preFoldPhase = true;
            m_logoutDelayTimer.stop();
            playUnfold(false);
        }
        break;
    }
}

void MiryuDuoEffect::startLogoutFold()
{
    if (!m_foldingForLogout || m_running) {
        return;
    }

    if (m_preFoldPhase) {
        // 3 秒预折叠阶段到点
        if (m_panelClosed) {
            // 面板已关闭：SmsDie 已发、阻塞已处理完（或无阻塞）。
            // 冻结帧已在面板关闭时抓取，现在开始折叠动画。
            m_foldingForLogout = false;
            m_preFoldPhase = false;
            playFold(true);
        } else {
            // 面板未关闭：可能有阻塞（保存对话框等），此时不折叠。
            // 切换为 60 秒兜底，继续等待面板关闭。
            m_preFoldPhase = false;
            m_logoutDelayTimer.start(kLogoutFallbackDelay);
        }
    } else {
        // 60 秒兜底阶段到点：仍未检测到面板关闭，直接折叠。
        m_foldingForLogout = false;
        playFold(true);
    }
}

void MiryuDuoEffect::slotScreenLockingChanged(bool locked)
{
    if (locked) {
        // 锁屏不播放任何折叠动画
        m_unlockAnimationPending = false;
        return;
    }

    if (m_foldingForLogout) {
        return;
    }

    // 解锁时锁屏界面不一定已经被销毁：有时进程先退出（窗口先关闭），
    // 有时 KWin 先收到解锁信号再销毁客户端连接，两种顺序都要能播出登入动画。
    if (m_unlockAnimationPending) {
        m_unlockAnimationPending = false;
        playUnfold(true);
        return;
    }

    // 兜底：桌面若本来就处于折叠状态，解锁时接着展开回来
    playUnfold(false);
}

void MiryuDuoEffect::slotWindowClosed(EffectWindow *window)
{
    if (!window) {
        return;
    }

    const QString windowClass = window->windowClass();

    // 注销进行中：plasmashell 面板（Dock 窗口）关闭意味着 ksmserver 已发
    // SmsDie、阻塞必然已处理完。
    // - 3 秒预折叠阶段内：立即抓取冻结帧（此时其他窗口尚未全部关闭），
    //   等 3 秒计时器到点再开始折叠动画，保证动画在 plasmashell 退出前播放。
    // - 60 秒兜底阶段内：立即折叠。
    // 异常退出（plasmashell 崩溃但不在注销流程中）时 m_foldingForLogout 为
    // false，不会触发折叠。
    if (m_foldingForLogout) {
        if (window->isDock() && !m_running) {
            m_panelClosed = true;
            if (m_preFoldPhase) {
                // 3 秒阶段：抓取冻结帧，等计时器到点再播动画。
                // isActive() 会因 (m_freezeFrame && !m_hasFrozenFrame) 返回 true，
                // 下一帧 paintFoldOverlay 把当前桌面复制到离屏纹理。
                // 抓帧时临时隐藏 Plasma 桌面图标。
                m_freezeFrame = true;
                m_hasFrozenFrame = false;
                m_hidingDesktopForFold = true;
                effects->addRepaintFull();
            } else {
                // 60 秒兜底阶段：立即折叠。
                m_foldingForLogout = false;
                m_preFoldPhase = true;
                m_logoutDelayTimer.stop();
                playFold(true);
            }
        }
        return;
    }

    // ksplash 关闭的瞬间桌面才露出来，登入动画正好发生在"桌面出现"的那一刻
    if (windowClass.contains(QStringLiteral("ksplash"))) {
        playUnfold(true);
        return;
    }

    // 锁屏界面（greeter）关闭同理。isLockScreen() 由 KWin 直接标记锁屏客户端，
    // 比窗口 class 更可靠；class 判断留作老版本 / X11 的兜底。
    if (window->isLockScreen() || windowClass.contains(QStringLiteral("kscreenlocker"))) {
        if (effects->isScreenLocked()) {
            // 锁屏还没真正结束（窗口先于解锁信号关闭），等解锁信号再播
            m_unlockAnimationPending = true;
        } else {
            playUnfold(true);
        }
    }
}

void MiryuDuoEffect::playFold(bool freezeFrame)
{
    if (!m_running && m_progress <= kFlatEpsilon) {
        return; // 已经折起
    }

    // 已经在冻结折叠途中（Saving 之后又收到 Quitting）时，保留最初抓到的画面
    if (!(m_freezeFrame && m_hasFrozenFrame)) {
        m_freezeFrame = freezeFrame;
        m_hasFrozenFrame = false;
        // 需要抓取新的冻结帧，在抓帧那一帧临时隐藏桌面图标
        m_hidingDesktopForFold = freezeFrame;
    }

    animateTo(0.0, kDuration);
}

void MiryuDuoEffect::playUnfold(bool snapToFolded)
{
    if (m_running && m_to >= 1.0) {
        return; // 已经在展开
    }

    m_foldingForLogout = false;
    m_panelClosed = false;
    m_preFoldPhase = true;
    m_logoutDelayTimer.stop();
    // 展开必须使用实时画面，否则解锁后会先看到一张冻结的旧桌面
    m_freezeFrame = false;
    m_hasFrozenFrame = false;
    m_hidingDesktopForFold = false;

    if (snapToFolded) {
        m_progress = 0.0;
    } else if (m_progress >= 1.0 - kFlatEpsilon) {
        return; // 本来就是展平的，没有可播放的动画
    }

    animateTo(1.0, kDuration);
}

void MiryuDuoEffect::animateTo(double target, int durationMs)
{
    m_from = m_progress;
    m_to = target;
    m_duration = std::max(1, durationMs);
    // 置空，让第一帧绘制时才开始计时
    m_animationStart = std::chrono::steady_clock::time_point();
    m_running = true;
    effects->addRepaintFull();
}

void MiryuDuoEffect::updateProgress()
{
    if (!m_running) {
        return;
    }

    // 第一帧到达时才开始计时，避免合成器排队造成的延迟被算进动画时长
    if (m_animationStart == std::chrono::steady_clock::time_point()) {
        m_animationStart = std::chrono::steady_clock::now();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_animationStart);
    const double t = std::clamp(double(elapsed.count()) / double(m_duration), 0.0, 1.0);

    // 三次缓出：开头快、结尾慢，接近折叠设备的开合节奏
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);
    m_progress = m_from + (m_to - m_from) * eased;

    if (t >= 1.0) {
        m_progress = m_to;
        m_running = false;
    }
}

#if MIRYUDUO_NEW_KWIN_API
void MiryuDuoEffect::prePaintScreen(ScreenPrePaintData &data)
#else
void MiryuDuoEffect::prePaintScreen(ScreenPrePaintData &data,
                                    std::chrono::milliseconds presentTime)
#endif
{
    if (m_running) {
        updateProgress();
        // 折叠会改变整屏内容，每帧都需要完整重绘
        effects->addRepaintFull();
    }

#if MIRYUDUO_NEW_KWIN_API
    effects->prePaintScreen(data);
#else
    effects->prePaintScreen(data, presentTime);
#endif
}

void MiryuDuoEffect::postPaintScreen()
{
    if (m_running) {
        effects->addRepaintFull();
    }

    effects->postPaintScreen();
}

bool MiryuDuoEffect::ensureResources(const QSize &deviceSize)
{
    if (m_shaderBroken) {
        return false;
    }

    if (!m_shader) {
        m_shader = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture,
            QString(),
            QStringLiteral(":/miryu-duo/fold.frag"));
        if (!m_shader) {
            m_shaderBroken = true;
            qCWarning(lcMiryuDuo) << "无法加载 fold.frag，折叠动画已停用";
            return false;
        }
    }

    if (!m_texture || m_textureSize != deviceSize) {
        m_texture = GLTexture::allocate(GL_RGBA16F, deviceSize);
        if (!m_texture) {
            m_shaderBroken = true;
            qCWarning(lcMiryuDuo) << "无法分配离屏纹理，尺寸为" << deviceSize;
            return false;
        }
        m_texture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_texture->setFilter(GL_LINEAR);
        m_framebuffer = std::make_unique<GLFramebuffer>(m_texture.get());
        m_textureSize = deviceSize;
        // 新纹理里还没有画面，冻结状态必须重新抓取
        m_hasFrozenFrame = false;
    }

    if (!m_framebuffer || !m_framebuffer->valid()) {
        m_shaderBroken = true;
        qCWarning(lcMiryuDuo) << "无法创建离屏帧缓冲，折叠动画已停用";
        return false;
    }

    return true;
}

void MiryuDuoEffect::paintFoldOverlay(const RenderTarget &renderTarget,
                                      const RenderViewport &viewport)
{
    if (!isActive() || m_shaderBroken) {
        return;
    }

    // renderRect() 在不同版本里可能是 QRectF 或 KWin::RectF，
    // 两者都提供 width()/height()/x()/y() 与 toRect()，这里用 auto 承接
    const auto renderRect = viewport.renderRect();
    const double scale = viewport.scale();
    const QSize deviceSize(qRound(renderRect.width() * scale),
                           qRound(renderRect.height() * scale));
    if (deviceSize.isEmpty() || !ensureResources(deviceSize)) {
        return;
    }

    // 把刚合成好的整屏内容复制到离屏纹理，再对它做折叠 + 模糊。
    // 目标矩形坐标是纹理局部坐标，所以从 (0, 0) 开始。
    // 注销折叠只抓第一帧：之后再怎么关闭窗口，折叠的始终是注销瞬间的画面。
    if (!(m_freezeFrame && m_hasFrozenFrame)) {
        if (!m_framebuffer->blitFromRenderTarget(renderTarget, viewport,
                                                 renderRect.toRect(),
                                                 QRect(QPoint(0, 0), deviceSize))) {
            return;
        }
        m_hasFrozenFrame = true;
        // 冻结帧已抓取（不含桌面图标），不再需要隐藏桌面窗口
        m_hidingDesktopForFold = false;
    }

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(float(renderRect.x() * scale), float(renderRect.y() * scale));

    ShaderManager::instance()->pushShader(m_shader.get());
    m_shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    m_shader->setUniform("duoSize",
                         QVector2D(float(deviceSize.width()), float(deviceSize.height())));
    m_shader->setUniform("duoProgress", float(m_progress));
    m_shader->setUniform("duoFoldLeft", m_foldLeft ? 1.0f : 0.0f);
    m_shader->setUniform("duoBlurRadius",
                         float(std::min(kBlurRadiusMax, deviceSize.width() * kBlurWidthRatio)));
    m_texture->render(QSizeF(deviceSize));
    ShaderManager::instance()->popShader();
}

#if MIRYUDUO_NEW_KWIN_API
bool MiryuDuoEffect::paintScreen(const RenderTarget &renderTarget,
                                 const RenderViewport &viewport,
                                 int mask, const Region &deviceRegion,
                                 LogicalOutput *screen)
{
    // 抓取冻结帧的那一帧，由 paintWindow() 把 Plasma 桌面图标窗口的不透明度设为 0，
    // 这样合成出来的画面不含桌面图标；合成结束后无需手动恢复（每个窗口的 data 是局部的）。

    // 先让 KWin 把整个桌面（含壁纸、所有窗口）正常合成到当前帧缓冲
    const bool painted = effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (!painted) {
        // GPU 出错时其它特效会停止渲染，这里同样直接放弃本帧
        return false;
    }

    paintFoldOverlay(renderTarget, viewport);
    return true;
}
#else
void MiryuDuoEffect::paintScreen(const RenderTarget &renderTarget,
                                 const RenderViewport &viewport,
                                 int mask, const QRegion &region, Output *screen)
{
    // 抓取冻结帧的那一帧，由 paintWindow() 把 Plasma 桌面图标窗口的不透明度设为 0，
    // 这样合成出来的画面不含桌面图标；合成结束后无需手动恢复（每个窗口的 data 是局部的）。

    // 先让 KWin 把整个桌面（含壁纸、所有窗口）正常合成到当前帧缓冲
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

    paintFoldOverlay(renderTarget, viewport);
}
#endif

bool MiryuDuoEffect::paintWindow(const RenderTarget &renderTarget,
                                 const RenderViewport &viewport,
                                 EffectWindow *w, int mask,
#if MIRYUDUO_NEW_KWIN_API
                                 const Region &deviceRegion,
#else
                                 const QRegion &deviceRegion,
#endif
                                 WindowPaintData &data)
{
    // 抓取冻结帧的那一帧，把 Plasma 桌面图标窗口（isDesktop）渲染为全透明，
    // 使抓帧画面不含桌面图标。opacity 只作用于本次 data，不会影响窗口本身。
    if (m_hidingDesktopForFold && !m_hasFrozenFrame && w->isDesktop()) {
        data.setOpacity(0.0);
    }
    return effects->paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}

} // namespace KWin
