// Minimal animation engine of the native UI, with the page's motion (ui/index.html: 120/180 ms; `ease` for most
// transitions, var(--ease) = cubic-bezier(.2,.7,.2,1) for the switch, the Launch press, the sheen and the toasts).
// A tween moves one scalar per key (0..1 amounts the drawing code turns into opacity, colors and offsets). Only
// tweens in flight are stored: a key without one is at rest and draws its rest value, so nothing runs when nothing
// moves. The owner keeps a timer only while running() and repaints only the rects that tick() hands over.
//
// Two ways to start one:
//  - explicit: to(key, from, to, ...) on an event (hover enter starts the sheen, a toast arrives);
//  - implicit, like a CSS transition: the drawing code asks fx()/fxColor() for the value of a key with its target for
//    this frame; when the target differs from the last one the engine animates from the value on screen to it.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <math.h>
#include <stdint.h>
#include <algorithm>
#include <vector>

namespace rn {

const float T1 = 120, T2 = 180;   // --t1, --t2

inline double clockMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

enum class Curve { Css, Ui, Linear };   // CSS `ease`, var(--ease), linear

// cubic-bezier(x1,y1,x2,y2) as CSS evaluates it: the t where x(t) = x (bisection, x is monotonic), then y(t).
inline float bezier(float x, float x1, float y1, float x2, float y2) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    auto b = [](float t, float p1, float p2) { float u = 1 - t; return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t; };
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 20; i++) { t = (lo + hi) / 2; if (b(t, x1, x2) < x) lo = t; else hi = t; }
    return b(t, y1, y2);
}
inline float ease(float x, Curve c) {
    return c == Curve::Linear ? std::min(1.f, std::max(0.f, x)) : c == Curve::Ui ? bezier(x, .2f, .7f, .2f, 1.f) : bezier(x, .25f, .1f, .25f, 1.f);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
// sRGB channel interpolation, like a CSS color transition
inline uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) o |= (uint32_t)lroundf(lerp((float)((a >> s) & 255), (float)((b >> s) & 255), t)) << s;
    return o;
}

class Anim {
public:
    bool motion = true;   // false: every change is instant (offscreen, or Windows "Show animations" is off)

    // Move key from `from` to `to` in `ms` after `delay`, `loops` times (each loop eased, like a CSS animation;
    // loops <= 0 = until stop()). `r` = where the key is drawn now (client DIPs), repainted while it moves.
    void to(uint32_t key, float from, float to, float ms, Curve c = Curve::Css, D2D1_RECT_F r = {}, float delay = 0, int loops = 1) {
        stop(key);
        if (!motion || ms <= 0 || (from == to && loops == 1)) return;
        run_.push_back({ key, from, to, ms, delay, loops, c, clockMs(), r });
    }
    void stop(uint32_t key) { run_.erase(std::remove_if(run_.begin(), run_.end(), [&](const Tween& t) { return t.key == key; }), run_.end()); }
    void clear() { run_.clear(); fx_.clear(); }
    void frame() { frame_++; }   // call once per paint: a key not drawn in the previous one shows up without a transition

    // The value to draw: the one in flight, else `rest`. `r` (client DIPs) updates where the key is drawn now.
    float get(uint32_t key, float rest, D2D1_RECT_F r) {
        for (auto& t : run_) if (t.key == key) { t.r = r; return value(t, clockMs()); }
        return rest;
    }
    float get(uint32_t key, float rest) const {
        for (auto& t : run_) if (t.key == key) return value(t, clockMs());
        return rest;
    }
    bool running(uint32_t key) const { for (auto& t : run_) if (t.key == key) return true; return false; }
    bool running() const { return !run_.empty(); }

    // Implicit transition of a scalar: `target` is this frame's value; a change animates from the value on screen.
    float fx(uint32_t key, float target, D2D1_RECT_F r, float ms = T1, Curve c = Curve::Css) {
        Fx& f = slot(key, target);
        if (f.target != target) { to(key, get(key, f.target), target, ms, c, r); f.target = target; }
        return get(key, target, r);
    }
    // Implicit transition of a color (the tween moves 0..1 between the color on screen and the new target).
    uint32_t fxColor(uint32_t key, uint32_t target, D2D1_RECT_F r, float ms = T1, Curve c = Curve::Css) {
        Fx& f = slot(key, (float)target);
        if (f.c1 != target) {
            f.c0 = mix(f.c0, f.c1, get(key, 1));
            f.c1 = target; f.target = (float)target;
            to(key, 0, 1, ms, c, r);
        }
        return mix(f.c0, f.c1, get(key, 1, r));
    }

    // Timer tick: hands the rects whose value changes to `repaint` and drops the finished tweens (their last repaint
    // draws the rest value).
    template <class F> void tick(F&& repaint) {
        double now = clockMs();
        for (auto& t : run_) if (now >= t.t0 + t.delay && t.r.right > t.r.left) repaint(t.r);
        run_.erase(std::remove_if(run_.begin(), run_.end(), [&](const Tween& t) { return t.loops > 0 && now >= t.t0 + t.delay + t.ms * t.loops; }), run_.end());
    }

private:
    struct Tween { uint32_t key; float from, to, ms, delay; int loops; Curve curve; double t0; D2D1_RECT_F r; };
    struct Fx { uint32_t key; float target; uint32_t c0, c1, seen; };
    std::vector<Tween> run_;
    std::vector<Fx> fx_;   // last target of every implicit key (a few dozen controls)
    uint32_t frame_ = 0;

    // First sight (or back after a frame without it): no transition, like a newly rendered element.
    Fx& slot(uint32_t key, float target) {
        uint32_t c = (uint32_t)target;
        for (auto& f : fx_) if (f.key == key) {
            if (f.seen + 1 < frame_) { stop(key); f.target = target; f.c0 = f.c1 = c; }
            f.seen = frame_;
            return f;
        }
        fx_.push_back({ key, target, c, c, frame_ });
        return fx_.back();
    }
    static float value(const Tween& t, double now) {
        double e = now - t.t0 - t.delay;
        if (e <= 0) return t.from;
        if (t.loops > 0 && e >= (double)t.ms * t.loops) return t.to;
        return lerp(t.from, t.to, ease((float)(fmod(e, (double)t.ms) / t.ms), t.curve));
    }
};

} // namespace rn
