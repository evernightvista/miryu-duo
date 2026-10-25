#version 140

// KWin 的着色器预处理器会丢弃 #version 行，并自行写入真正的版本声明与精度限定。
// 这里保留它只是为了文件能脱离 KWin 被 glslang 之类的工具单独检查。

uniform sampler2D sampler;

uniform vec2 duoSize;        // 当前屏幕的设备像素尺寸
uniform float duoProgress;   // 0.0 = 完全合拢，1.0 = 完全展平
uniform float duoFoldLeft;   // 1.0 = 折叠左半屏，0.0 = 折叠右半屏
uniform float duoBlurRadius; // 完全合拢时的模糊半径（设备像素）

in vec2 texcoord0;
out vec4 fragColor;

const float DUO_PI = 3.14159265359;
const float GOLDEN_ANGLE = 2.39996323;
const int BLUR_TAPS = 24;

// 黄金角螺旋采样：用较少的采样点得到平滑的大半径模糊，
// 避免规则网格内核在大半径下出现网格状重影。
vec4 blurRadial(vec2 uv, float radiusPixels)
{
    if (radiusPixels < 0.5) {
        return texture(sampler, uv);
    }

    vec2 texel = 1.0 / duoSize;
    vec4 sum = texture(sampler, uv);

    for (int i = 1; i < BLUR_TAPS; ++i) {
        float t = float(i) / float(BLUR_TAPS - 1);
        // sqrt 让采样点按圆盘面积均匀分布，避免中心过亮
        float r = radiusPixels * sqrt(t);
        float a = float(i) * GOLDEN_ANGLE;
        vec2 offset = vec2(cos(a), sin(a)) * r * texel;
        sum += texture(sampler, clamp(uv + offset, vec2(0.0), vec2(1.0)));
    }

    return sum / float(BLUR_TAPS);
}

void main()
{
    vec2 uv = texcoord0;
    float halfWidth = duoSize.x * 0.5;
    // 以铰链（屏幕中线）为原点的横坐标，向右为正
    float u = uv.x * duoSize.x - halfWidth;

    // 合拢程度：0 = 展平，1 = 完全合拢
    float foldAmount = 1.0 - duoProgress;

    // 书本式合拢：折面绕铰链转过 0 → 180°。
    // 角度小于 90° 时折面向内收；超过 90° 后它越过铰链，
    // 最终（180°）正好整个盖在另一半上，与另一半完全重合。
    float angle = foldAmount * DUO_PI;
    float cosAngle = cos(angle);

    // 折叠面板初始所在的一侧：-1 = 左半屏，+1 = 右半屏
    float side = (duoFoldLeft > 0.5) ? -1.0 : 1.0;

    float blurRadius = duoBlurRadius * foldAmount;

    // 折面上横向位置 uSrc 的点，旋转后投影到 u = uSrc * cos(angle)。
    // 反过来，屏幕位置 u 在折面上对应的原始位置就是 u / cos(angle)；
    // 只有仍落在折叠那一半（符号与 side 相同）且没超出半屏宽度的位置才属于折面。
    // |cos| 极小时折面侧对视线，投影宽度趋近于零，此时屏幕上看不到它。
    float srcU = (abs(cosAngle) > 1e-4) ? (u / cosAngle) : 0.0;
    bool onFoldPanel = (abs(cosAngle) > 1e-4)
                       && (srcU * side >= 0.0)
                       && (abs(srcU) <= halfWidth);

    if (onFoldPanel) {
        // ---- 折叠面：内容随旋转被压缩，过了 90° 后叠到另一半之上 ----
        vec2 srcUv = vec2(srcU / duoSize.x + 0.5, uv.y);
        vec4 color = blurRadial(srcUv, blurRadius);

        // 折面越接近合拢越暗：转过 90° 后背对视线，只剩约四成亮度
        color.rgb *= mix(1.0, 0.40, foldAmount);

        // 铰链处的折痕阴影，只在折起时出现
        color.rgb *= mix(1.0, mix(0.35, 1.0, smoothstep(0.0, 14.0, abs(u))), foldAmount);

        // 折面外侧那条边略暗一点，让"折过来的板子"有厚度感。
        // 折起时外缘位于 |u| = halfWidth * |cos|，展平时该效果完全关掉，
        // 否则会把屏幕最外侧一圈也压暗。
        float edge = smoothstep(0.0, 8.0, halfWidth * abs(cosAngle) - abs(u));
        color.rgb *= mix(1.0, mix(0.60, 1.0, edge), foldAmount);

        color.a = 1.0;
        fragColor = color;
    } else if (u * side > 0.0) {
        // ---- 折叠侧的空区域：折面已经从这里转走，露出背景 ----
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        // ---- 静止面：不旋转，只带六成失焦 ----
        vec4 color = blurRadial(uv, blurRadius * 0.6);

        // 铰链附近的阴影，随展开消失
        color.rgb *= mix(1.0, mix(0.80, 1.0, smoothstep(0.0, 26.0, -u * side)), foldAmount);

        color.a = 1.0;
        fragColor = color;
    }
}
