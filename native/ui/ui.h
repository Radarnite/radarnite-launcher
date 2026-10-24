// The launcher window's content: state from the core (api.h), layout + drawing with Direct2D (one immediate-mode
// pass that also records hit rects and the focus order), mouse/keyboard input and the native EDIT fields.
// Repaints only on events: input, core messages, the spinner while a launch runs, toast expiry, and the page's
// transitions (anim.h) while they move, each repainting only its own rect.
#pragma once
#include "anim.h"
#include "api.h"
#include "art.h"
#include "gfx.h"
#include "images.h"
#include "video.h"
#include <deque>
#include <memory>
#include <unordered_map>

namespace rn {

const UINT WM_APP_SINK = WM_APP + 1;     // queued core messages to apply
const UINT WM_APP_EDITKEY = WM_APP + 2;  // a key the EDIT subclass hands to the window (wParam = vk, lParam = edit id)
const UINT WM_APP_IMG = WM_APP + 3;      // cosmetics panel images decoded (Images): repaint

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
    void openSheet(bool open);                  // the match settings sheet (selftest: render a scenario with it open)
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
    struct Event { int type; State state; Session session; LogLine log; std::wstring text; Level level;
                   std::shared_ptr<const PortCatalog> cat; std::shared_ptr<const PortJob> job; PortLoad load; PortPreview pv; };
    struct Hit { int id; D2D1_RECT_F r; int flags; float radius; D2D1_RECT_F clip; int layer; };
    struct LogEntry { std::wstring ts, text, lower; int lvl; bool key; int wrapCols = 0, lines = 1; };
    struct Toast { std::wstring text; Level level; ULONGLONG until; uint32_t seq; };
    struct EditBox { HWND hwnd = nullptr; int id = 0; D2D1_RECT_F r{}; bool show = false; D2D1_RECT_F clip{}; HFONT font = nullptr; Font f; int textH = 16;
                     RECT applied{}; bool shown = false; bool clipped = false;
                     bool lazy = false, want = false;   // lazy: the EDIT only shows while it has the focus (drawn text otherwise)
                     uint32_t bg = C::bg2; };
    struct Thumb { std::wstring skin; ArtImage img; UINT req = 0; ID2D1Bitmap* bmp = nullptr; ID2D1BitmapBrush* brush = nullptr; };
    struct BgKey { float w, top, bottom, scale; const SeasonArt* sa; uint32_t artGen; const void* rt; bool bare; std::wstring ghost; };

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
    void frost(D2D1_RECT_F r, float radius);    // the dock's blurred glass
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
    void drawTop(float y, float W);                             // build switcher, +, Cosmetics, Activity, Build
    void drawPoster(float x, float right, float top, float bottom);   // the season's title (bottom left)
    float drawDock(float x, float w, float top, float bottom);  // summary or sheet, status line, Launch; returns its top
    float okline(float x, float y, float w);                    // the status line; returns the height
    float summary(float x, float y, float w, bool withChips);   // returns the height
    float drawSheet(float x, float w, float top, float bottom); // returns its top
    float sheetBody(float x, float y, float w);                 // returns the bottom
    void drawLaunch(D2D1_RECT_F pr, uint32_t glow);
    void stepBuild(int d);                                      // previous / next build (Q / E, ‹ ›, arrows)
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
    EditBox edits_[4];          // [3]: the cosmetics panel's search
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
    ID2D1Bitmap* blur_ = nullptr;           // the stage at 1/16, the frost's backdrop
    ID2D1BitmapBrush* blurBr_ = nullptr;
    BgKey bgKey_{};
    ID2D1LinearGradientBrush* modeBr_[2] = {};
    ID2D1LinearGradientBrush* rangeBr_ = nullptr;
    float tabScroll_ = 0, tabMax_ = 0;
    D2D1_RECT_F tabClip_{};
    int tabEnsure_ = -1;        // build index to scroll into view on the next frame
    D2D1_RECT_F playRect_{};
    float playRadius_ = 16;
    bool sheetOpen_ = false;
    D2D1_RECT_F sheetRect_{};
    bool playShown_ = false, playDisabled_ = false;
    float toastRight_ = 0, toastBottom_ = 0;
    HWND tip_ = nullptr;        // one comctl32 tooltip, its tool = the hovered control
    bool spinning_ = false;
    bool pendingEnsure_ = false;

    // ================================================================ the cosmetics panel and its preview (port_ui.cpp)
public:
    void onPortCatalog(std::shared_ptr<const PortCatalog> c) override;
    void onPortLoad(const PortLoad& l) override;
    void onPortJob(std::shared_ptr<const PortJob> j) override;
    void onPortPreview(const PortPreview& p) override;
    bool importing() const { return pj_ != nullptr; }
    bool portOpen() const { return pOpen_; }
    // Selftest: open the panel on the selected build and put it in a state: 1 open, 2 filters popup, 3 Import all
    // dialog, 4 preview of the second card, 5 three cards selected and one hovered, 6 scrolled to the end
    void portScene(int what);
    Images& images() { return img_; }
    void imagesArrived();                       // WM_APP_IMG
    void videoEvent(WPARAM w, LPARAM l) { video_.onEvent(w, l); invalidate(); }   // WM_APP_VIDEO
    bool videoShown() const { return video_.shown(); }
    size_t clipBytes() const { return video_.processBytes(); }   // the clip process (selftest)
    // Selftest: clicks and keys on the panel (needs a render target and the mock's catalog)
    std::vector<std::pair<std::wstring, bool>> portCheck(float width, float height);

private:
    struct PortRow { std::wstring stage; int64_t ms; bool done; ULONGLONG at; };
    struct RarBrush { std::string key; ID2D1LinearGradientBrush* bg; ID2D1LinearGradientBrush* shade; uint32_t dark; };
    float para(const std::wstring& s, const Font& f, float x, float top, float w, float lh, int maxLines, uint32_t c, bool draw = true);
    float chip(int id, float x, float y, float h, const std::wstring& label, int icon, const std::wstring& count, bool on, bool disabled, bool measure = false, bool pill = false);
    void portEvent(Event& e);                    // a queued onPort* (types 4..7)
    void portShow(bool open);
    void portLoad(bool refresh);
    void portRebuild();                          // the filtered, sorted list and the per-group counts
    void portRefilter() { pDirty_ = true; pScroll_ = 0; pCursor_ = 0; invalidate(); }
    bool portMatch(const PortItem& it, int skip) const;
    bool importable(const PortItem& it) const { return !it.path.empty() && (it.status == PortStatus::New || it.status == PortStatus::Failed); }
    void drawPort(float x, float y, float w, float h);
    float drawPortJob(float x, float y, float w);
    float drawPortLoad(float x, float y, float w, bool draw);
    void drawPortCard(int slot, int item, D2D1_RECT_F r);
    void drawPortOverlays();
    void drawPortPop();
    void drawPortDialog();
    void drawPortPreview();
    void portActivate(int id);
    bool portKey(UINT vk, bool ctrl, bool shift);
    bool portWheel(float x, float y, float delta);
    void portScrollbar(float y, bool down);      // mouse down on it / drag
    void portConfirm(const std::vector<int>& items, bool all);
    void portPreviewOpen(int item);
    void portCursor(int idx, bool keyboard);
    std::wstring portTip(int id) const;
    RarBrush* rarBrush(const std::string& rarity);
    void releasePortBrushes();

    Images img_;
    Video video_;               // the preview's clip (live window only)
    int layer_ = 0;             // overlay level of the hits being recorded (Tab stays inside the topmost one)
    bool pOpen_ = false, pLoading_ = false, pDirty_ = true, pLogOpen_ = false, pPvWait_ = false, pDlg_ = false, pDlgAll_ = false;
    std::wstring pBuild_, pQ_;
    std::shared_ptr<const PortCatalog> pCat_;
    std::shared_ptr<const PortJob> pj_;
    std::shared_ptr<const PortJob> pjEnd_;       // the last import, finished (done / stopped / failed): its card until dismissed
    std::wstring pjEndFor_;                      // the build (name) the last import was started on: its finished card shows only on that build's panel
    ULONGLONG pjAt_ = 0, pLoadT0_ = 0;
    std::vector<PortRow> pRows_;
    uint32_t pTypes_ = 0, pSts_ = 0, pRars_ = 0;   // filter bit sets (PortType, PortStatus, rarity index)
    int pSort_ = 0;                              // 0 newest, 1 oldest, 2 A to Z, 3 rarity
    std::vector<uint8_t> pSel_;                  // per catalog item
    int pSelN_ = 0;
    std::vector<int> pList_;                     // filtered + sorted catalog indices
    int pCntT_[16] = {}, pCntS_[4] = {}, pCntR_[32] = {}, pTodo_ = 0;
    int pNewAll_ = -1;                           // items of the catalog not imported yet (-1 = not listed); the lobby's badge
    bool glassChips_ = false;                    // chips drawn on the season art (the panel's bars): glass like the lobby
    int pPop_ = 0;                               // 0 none, 1 category, 2 filters, 3 sort
    D2D1_RECT_F pPopAnchor_{}, pGrid_{}, pBody_{}, pJobR_{}, pLoadR_{};
    std::vector<int> pPending_;                  // the dialog's items
    int pPv_ = -1;                               // catalog index in the preview
    PortPreview pPvData_;
    float pScroll_ = 0, pTotal_ = 0, pView_ = 0;
    int pCursor_ = 0;                            // keyboard position in pList_
    std::vector<int> pSlot_;                     // visible card slot -> pList_ index
    std::vector<RarBrush> rarBrushes_;
    std::vector<int8_t> pRi_;                    // per catalog item: its rarity's index in the filter list (-1 none)
    std::unordered_map<std::wstring, int> pPath_;   // path -> catalog index, built while an import is shown
};

} // namespace rn
