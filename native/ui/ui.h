// The launcher window's content: state from the core (api.h), layout + drawing with Direct2D (one immediate-mode
// pass that also records hit rects and the focus order), mouse/keyboard input and the native EDIT fields.
// Repaints only on events: input, core messages, the spinner while a launch runs, toast expiry, and the page's
// transitions (anim.h) while they move, each repainting only its own rect.
#pragma once
#include "anim.h"
#include "api.h"
#include "art.h"
#include "gfx.h"
#include <deque>

namespace rn {

const UINT WM_APP_SINK = WM_APP + 1;     // queued core messages to apply
const UINT WM_APP_EDITKEY = WM_APP + 2;  // a key the EDIT subclass hands to the window (wParam = vk, lParam = edit id)

class App final : public UiSink {
public:
    App(Gfx& g, Core* core);
    ~App();

    // hwnd = the window (null when rendering offscreen only: no EDIT controls, texts drawn by Direct2D).
    void attach(HWND hwnd);
    void start();                        // core->ready(this)
    void drain();                        // apply the queued core messages (UI thread)

    // Render target lifetime (brushes and the logo bitmap live on it).
    void setTarget(ID2D1RenderTarget* rt, float dpi);
    void paint(float width, float height);   // DIPs

    // Window state the page used to get from the host
    void setMaximized(bool m) { if (maximized_ != m) { maximized_ = m; invalidate(); } }
    void setActive(bool a) { if (active_ != a) { active_ = a; invalidate(); } }
    void setDpi(float dpi);
    bool busy() const;
    bool sessionRunning() const;

    // Input (client coordinates in DIPs)
    int ncHit(float x, float y) const;   // HTCAPTION over the title bar, HTCLIENT elsewhere
    void mouseMove(float x, float y);
    void mouseLeave();
    void mouseDown(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, float delta);
    bool key(UINT vk, bool ctrl, bool shift);   // true = handled
    bool cursor(float x, float y);              // sets the cursor; false = default arrow
    void timer(UINT_PTR id);
    void editCommand(int id, int code);         // WM_COMMAND from an EDIT
    void editKey(int id, UINT vk, bool ctrl, bool shift);   // keys the EDIT subclass hands over
    HBRUSH editBrush(HWND edit, HDC dc);       // WM_CTLCOLOREDIT: the field's colors (the EDITs are opaque)
    void openActivity(bool open);
    void openMenu(int m) { if (menu_ != m) { menu_ = m; invalidate(); } }   // selftest: render a scenario with a menu open; 0 closes
    // Transitions on/off (Windows "Show animations"; off without a window, so offscreen renders are the rest state)
    void setMotion(bool on);
    bool motion() const { return anim_.motion; }
    bool animating() const { return anim_.running(); }
    bool artPending() const { return art_.pending(); }   // season art still downloading or decoding (selftest)
    int animTicks = 0;                          // animation timer ticks (selftest)
    D2D1_RECT_F actButtonRect() const;          // selftest: where the Activity button is (hover check)
    // Selftest in the live window: frame hit-testing (WM_NCHITTEST), no system frame, Escape in the search EDIT.
    std::vector<std::pair<std::wstring, bool>> liveCheck();

    // Selftest: drives the window through clicks and keys on the current target and checks the state after each
    // step. Returns { name, ok } pairs. Needs a render target (setTarget) and a scenario with the match server.
    std::vector<std::pair<std::wstring, bool>> selfCheck(float width, float height);

    // Selftest: text clipped in controls that must not clip (the page's "clipped" checks)
    std::vector<std::wstring> issues;

    // UiSink (any thread)
    void onState(const State& s) override;
    void onSession(const Session& s) override;
    void onLog(const LogLine& l) override;
    void onToast(const std::wstring& text, Level level) override;

private:
    struct Event { int type; State state; Session session; LogLine log; std::wstring text; Level level; };
    struct Hit { int id; D2D1_RECT_F r; int flags; float radius; D2D1_RECT_F clip; };
    struct LogEntry { std::wstring ts, text, lower; int lvl; bool key; int wrapCols = 0, lines = 1; };
    struct Toast { std::wstring text; Level level; ULONGLONG until; uint32_t seq; };
    struct EditBox { HWND hwnd = nullptr; int id = 0; D2D1_RECT_F r{}; bool show = false; D2D1_RECT_F clip{}; HFONT font = nullptr; Font f; int textH = 16;
                     RECT applied{}; bool shown = false; bool clipped = false;
                     bool lazy = false, want = false;   // lazy: the EDIT only shows while it has the focus (drawn text otherwise)
                     uint32_t bg = C::bg2; };
    struct Thumb { std::wstring skin; ArtImage img; UINT req = 0; ID2D1Bitmap* bmp = nullptr; ID2D1BitmapBrush* brush = nullptr; };
    struct BgKey { float w, top, bottom, scale; const SeasonArt* sa; uint32_t artGen; const void* rt; };

    void invalidate();
    void dirty(D2D1_RECT_F r);             // repaint only r (client DIPs)
    void animate();                        // animation timer on while something moves, off at rest
    void greet();                          // the Launch button's sheen, twice (page: greet())
    void setToastTimer();
    // Values of the transitions for this frame (r in the current drawing coordinates; rest value while measuring)
    float fx(uint32_t key, float target, D2D1_RECT_F r, float ms = T1, Curve c = Curve::Css);
    uint32_t fxc(uint32_t key, uint32_t target, D2D1_RECT_F r, float ms = T1, Curve c = Curve::Css);
    float tw(uint32_t key, float rest, D2D1_RECT_F r);
    void apply(Event& e);
    void addLog(const LogLine& l);
    void toast(const std::wstring& text, Level level);
    void setConfig(uint32_t fields, const Config& v);
    void select(const std::wstring& name, bool click = false);
    void activate(int id);
    void moveFocus(int dir);
    void setFocus(int id, bool keyboard);
    void setHover(int id);
    std::wstring tooltip(int id) const;   // the page's title attributes
    void updateTip();
    void ensureVisible(int id);
    void syncEdits();
    void layoutEdits();
    void createEdits();
    void updateSpinTimer();
    void commitEdit(int id);
    const Build* sel() const;
    int level(const Build& b) const;       // 0 ok, 1 warn, 2 bad
    bool matchOk(const Build* b) const;

    // drawing primitives
    ID2D1SolidColorBrush* br(uint32_t c, float a = 1.f);
    void fill(D2D1_RECT_F r, uint32_t c, float a = 1.f, float radius = 0);
    void box(D2D1_RECT_F r, float radius, uint32_t fillC, float fillA, uint32_t border, float borderA = 1.f);
    void hline(float x0, float x1, float y, uint32_t c);
    void vline(float x, float y0, float y1, uint32_t c);
    void dot(float cx, float cy, float d, uint32_t c);
    void icon(Icon i, float x, float y, float size, uint32_t c, float a = 1.f);
    void spinner(float x, float y, float size);
    void skeleton(int i, D2D1_RECT_F r);
    void shadow(D2D1_RECT_F r, float radius, float blur, float oy, float alpha, uint32_t color = 0x000000);
    void glass(D2D1_RECT_F r, float radius, float fillA = .62f, float borderA = .12f);
    float pill(float x, float y, const std::wstring& s, int dot, bool measure = false);   // .badge; dot -1 none, 0-2 level, 3 spinner
    float gbutton(int id, float x, float y, float h, const wchar_t* label, Icon ic, bool iconOnly, bool measure = false);
    float textW(const std::wstring& s, const Font& f, float tracking = 0);
    bool text(const std::wstring& s, const Font& f, float x, float top, float w, float lh, uint32_t c, int align = 0, float tracking = 0, float a = 1.f);
    float rich(const std::wstring& s, const Font& f, float x, float top, float w, float lh, uint32_t c, uint32_t boldC, int align = 0, bool draw = true);
    void hit(int id, D2D1_RECT_F r, int flags, float radius = 8);
    const Hit* findHit(float x, float y) const;
    const Hit* hitById(int id) const;
    D2D1_RECT_F rectOf(int id) const;

    // parts
    void drawTitle(float W);
    void drawStage(float top, float bottom);                   // season gradient, rays, skin and fades (cached bitmap)
    void drawTop(float y, float W);                             // build tabs, +, Cosmetics, Build
    void drawHero(float x, float right, float top, float bottom);
    void drawLaunch(D2D1_RECT_F pr, uint32_t glow);
    void drawPanel(float x, float y, float w, float h);
    float panelBody(float x, float y, float w);                 // returns the height
    void drawActivity(float x, float y, float w, float h);
    // season art
    void ensureArt(const wchar_t* skin, UINT px);
    void takeArt();
    ID2D1BitmapBrush* thumb(const wchar_t* skin);
    void releaseArt();                                          // the Direct2D side (target change)
    void drawMenu();
    void drawToasts(float W, float H);
    void drawScrollbar(int id, float x, float y, float h, float scroll, float total, float view);
    float button(int id, float x, float y, const wchar_t* label, Icon ic, bool sm, bool ghost, bool iconOnly, bool hasIcon = true, bool measure = false);

    Gfx& g_;
    Core* core_;
    HWND hwnd_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1SolidColorBrush* brush2_ = nullptr;
    ID2D1Bitmap* logo_ = nullptr;
    ID2D1BitmapBrush* logoBrush_ = nullptr;
    ID2D1LinearGradientBrush* sheen_ = nullptr;     // .play::after
    ID2D1LinearGradientBrush* shimmer_ = nullptr;   // .sk while loading
    Anim anim_;
    bool animTimer_ = false;
    uint32_t toastSeq_ = 0;
    std::vector<D2D1_RECT_F> toastRects_;
    bool measuring_ = false, noHit_ = false, inContent_ = false;
    std::vector<D2D1_RECT_F> overlays_;   // popup/toast rects of this frame (they hide the EDIT controls)
    D2D1_RECT_F contentClip_{};
    float dpi_ = 96, scale_ = 1;
    float alpha_ = 1;
    D2D1_MATRIX_3X2_F base_ = D2D1::Matrix3x2F::Identity();
    float oy_ = 0;
    D2D1_RECT_F clip_ = { -1e9f, -1e9f, 1e9f, 1e9f };
    float W_ = 0, H_ = 0;

    CRITICAL_SECTION cs_;
    std::vector<Event> queue_;

    // state
    bool loaded_ = false;
    State st_;
    Session se_;
    std::wstring selected_;
    std::deque<LogEntry> logs_;
    int filter_ = 0;            // 0 all, 1 warn, 2 bad
    std::wstring q_;
    int unseen_ = 0, unseenLevel_ = 0;
    bool actOpen_ = false;
    float actH_ = 260;
    bool logFollow_ = true;
    float logScroll_ = 0, logMax_ = 0, logView_ = 0;
    std::vector<int> rows_;     // filtered log indices
    std::vector<float> rowY_;   // prefix heights
    bool rowsDirty_ = true;
    float contentScroll_ = 0, contentH_ = 0, contentView_ = 0;
    std::vector<Toast> toasts_;
    std::vector<D2D1_RECT_F> spinRects_;   // where spinners were drawn (the spin timer repaints only those)
    int menu_ = 0;              // 0 none, 1 add, 2 build, 3 cosmetics
    D2D1_RECT_F menuAnchor_{};
    int botsDrag_ = -1;

    // interaction
    std::vector<Hit> hits_;
    int hover_ = 0, press_ = 0, focus_ = 0;
    bool focusVisible_ = false;
    bool maximized_ = false, active_ = true;
    int drag_ = 0;              // id being dragged (range, grip, scrollbars)
    float dragY0_ = 0, dragV0_ = 0;
    EditBox edits_[3];
    HBRUSH editBrush_ = nullptr, fieldBrush_ = nullptr;
    // lobby screen
    ArtLoader art_;
    ArtImage artImg_;           // the selected build's skin, decoded at the size it is drawn (the only full art kept)
    std::wstring artWant_;
    UINT artReqPx_ = 0;
    uint32_t artGen_ = 0;
    std::vector<Thumb> thumbs_; // the tabs' small skins
    ID2D1BitmapRenderTarget* bgRT_ = nullptr;
    ID2D1Bitmap* bg_ = nullptr;
    BgKey bgKey_{};
    ID2D1LinearGradientBrush* modeBr_[2] = {};
    ID2D1LinearGradientBrush* rangeBr_ = nullptr;
    float tabScroll_ = 0, tabMax_ = 0;
    D2D1_RECT_F tabClip_{};
    int tabEnsure_ = -1;        // build index to scroll into view on the next frame
    D2D1_RECT_F playRect_{};
    bool playShown_ = false, playDisabled_ = false;
    float toastRight_ = 0, toastBottom_ = 0;
    HWND tip_ = nullptr;        // one comctl32 tooltip, its tool = the hovered control
    bool spinning_ = false;
    bool pendingEnsure_ = false;
};

} // namespace rn
