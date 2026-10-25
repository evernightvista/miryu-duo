/*
    SPDX-FileCopyrightText: 2026 Miryu Gaming <dev@miryugaming.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "effect/effect.h"

#include <QSize>
#include <QTimer>

#include <chrono>
#include <memory>

// KWin 6.6 之后的特效 API 做了较大改动：
//   paintScreen() 返回 bool，区域类型由 QRegion 换成 Region，
//   Output 换成 LogicalOutput，prePaintScreen() 不再接收 presentTime。
// CMake 会读取已安装的 KWin 头文件并据此设置这个宏；
// 这里给出 0 作为兜底默认值，方便不经 CMake 单独编译时也成立。
#ifndef MIRYUDUO_NEW_KWIN_API
#define MIRYUDUO_NEW_KWIN_API 0
#endif

namespace KWin
{

class GLFramebuffer;
class GLShader;
class GLTexture;

/**
 * Miryu Duo —— 整屏折叠 + 渐进模糊。
 *
 * 注销时桌面像折叠手机一样合拢，登入时再展开。整屏（含壁纸）都会被折叠，
 * 因为特效先让 KWin 把画面正常合成到目标帧缓冲，再把整屏复制到离屏纹理，
 * 最后用自定义着色器重绘。
 */
class MiryuDuoEffect : public Effect
{
    Q_OBJECT

public:
    MiryuDuoEffect();
    ~MiryuDuoEffect() override;

    static bool supported();

    bool isActive() const override;
    int requestedEffectChainPosition() const override;

#if MIRYUDUO_NEW_KWIN_API
    void prePaintScreen(ScreenPrePaintData &data) override;
    [[nodiscard]] bool paintScreen(const RenderTarget &renderTarget,
                                   const RenderViewport &viewport,
                                   int mask, const Region &deviceRegion,
                                   LogicalOutput *screen) override;
    // 抓取冻结帧时临时把 Plasma 桌面图标窗口的不透明度设为 0，
    // 这样合成画面不含桌面图标；抓帧结束后自然恢复（每个窗口都走本方法）。
    [[nodiscard]] bool paintWindow(const RenderTarget &renderTarget,
                                   const RenderViewport &viewport,
                                   EffectWindow *w, int mask,
                                   const Region &deviceRegion,
                                   WindowPaintData &data) override;
#else
    void prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime) override;
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                     int mask, const QRegion &region, Output *screen) override;
    [[nodiscard]] bool paintWindow(const RenderTarget &renderTarget,
                                   const RenderViewport &viewport,
                                   EffectWindow *w, int mask,
                                   const QRegion &region,
                                   WindowPaintData &data) override;
#endif

    void postPaintScreen() override;

private Q_SLOTS:
    void slotSessionStateChanged();
    void slotScreenLockingChanged(bool locked);
    void slotWindowClosed(EffectWindow *window);
    void startLogoutFold();

private:
    void playFold(bool freezeFrame);
    void playUnfold(bool snapToFolded);
    void animateTo(double target, int durationMs);
    void updateProgress();
    void paintFoldOverlay(const RenderTarget &renderTarget, const RenderViewport &viewport);
    bool ensureResources(const QSize &deviceSize);

    // 0.0 = 完全折起，1.0 = 完全展平
    double m_progress = 1.0;
    double m_from = 0.0;
    double m_to = 0.0;
    int m_duration = 1800;
    bool m_running = false;
    // 注销流程已开始：此时不再自动展开，避免和注销抢状态
    bool m_foldingForLogout = false;
    // 注销过程中 Plasma 面板（Dock 窗口）是否已关闭。
    // 面板关闭意味着 ksmserver 已发 SmsDie、阻塞必然已处理完，
    // 是判断"无阻塞/阻塞已结束"的可靠信号（KWin Normal 不可靠）。
    bool m_panelClosed = false;
    // 是否处于 3 秒预折叠阶段。true=3 秒预折叠，false=60 秒兜底阶段。
    bool m_preFoldPhase = true;
    // 注销计时器：先跑 3 秒预折叠（面板已关闭则到点折叠），
    // 若面板未关闭则切换为 60 秒兜底。
    QTimer m_logoutDelayTimer;
    // 锁屏界面关闭与解锁信号的先后顺序不固定，用这个标记等到合适的时机再播登入动画
    bool m_unlockAnimationPending = false;
    bool m_foldLeft = true;
    // 着色器或离屏纹理创建失败后不再重试，直接退化为不折叠（比黑屏安全）
    bool m_shaderBroken = false;

    // 折叠开始时把当前画面冻结在离屏纹理里。
    // 注销时客户端会被逐个关闭，若不冻结，折叠动画很快就只剩下黑色背景。
    bool m_freezeFrame = false;
    bool m_hasFrozenFrame = false;
    // 抓取冻结帧时是否临时隐藏 Plasma 桌面图标窗口。
    // 仅在 m_freezeFrame && !m_hasFrozenFrame 的那一帧生效，抓帧后即复位。
    bool m_hidingDesktopForFold = false;

    // 用单调时钟计时，避免依赖不同 KWin 版本的帧时间参数
    std::chrono::steady_clock::time_point m_animationStart;
    QSize m_textureSize;

    std::unique_ptr<GLShader> m_shader;
    std::unique_ptr<GLTexture> m_texture;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
};

} // namespace KWin
