// @Clxp - 7-band parametric EQ, VST2 for Equalizer APO
// Made by Clxp
// Single-file build: everything is in this one file.
//
// Build config lives in CMakeLists.txt at the repo root, and CI in
// .github/workflows/build.yml. Both point at THIS file only.

// ---- vst2.h ----
#include <stdint.h>

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (*AEffectDispatcherProc)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void  (*AEffectProcessProc)(AEffect*, float** inputs, float** outputs, int32_t sampleFrames);
typedef void  (*AEffectProcessDoubleProc)(AEffect*, double** inputs, double** outputs, int32_t sampleFrames);
typedef void  (*AEffectSetParameterProc)(AEffect*, int32_t index, float parameter);
typedef float (*AEffectGetParameterProc)(AEffect*, int32_t index);

struct AEffect
{
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float   ioRatio;
    void*   object;
    void*   user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char    future[56];
};

struct ERect { int16_t top, left, bottom, right; };

enum { kEffectMagic = 0x56737450 };

enum
{
    effFlagsHasEditor    = 1 << 0,
    effFlagsCanReplacing = 1 << 4
};

enum
{
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3,
    effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58
};

enum { audioMasterAutomate = 0, audioMasterVersion = 1 };

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>
// vst2.h merged above

using namespace Gdiplus;

static HINSTANCE gInst = nullptr;

// ============================================================ parameters
constexpr int NB = 7;
constexpr int NP = 1 + NB * 5;
constexpr int GUI_W = 980, GUI_H = 640;
enum { K_ON = 0, K_TYPE, K_FREQ, K_GAIN, K_Q };
enum { T_PEAK = 0, T_LOWSHELF, T_HIGHSHELF, T_HP, T_LP, T_BP, T_NOTCH };

static const wchar_t* kTypeNames[7] = { L"Peak", L"Low Shelf", L"High Shelf", L"High Pass", L"Low Pass", L"Band Pass", L"Notch" };
static const double kDefFreq[NB] = { 60, 150, 400, 1000, 2500, 6000, 12000 };

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline int bandOf(int i)  { return (i - 1) / 5; }
static inline int kindOf(int i)  { return (i - 1) % 5; }
static inline int pidx(int b, int k) { return 1 + b * 5 + k; }
static inline bool usesGain(int t) { return t <= T_HIGHSHELF; }

static double toNative(int i, float n)
{
    if (i == 0) return -24.0 + 48.0 * n;
    switch (kindOf(i))
    {
        case K_ON:   return n > 0.5f ? 1.0 : 0.0;
        case K_TYPE: return (int) (n * 6.0f + 0.5f);
        case K_FREQ: return 20.0 * std::pow(1000.0, n);
        case K_GAIN: return -24.0 + 48.0 * n;
        default:     return 0.1 * std::pow(180.0, n);
    }
}

static float toNorm(int i, double v)
{
    if (i == 0) return clamp01((float) ((v + 24.0) / 48.0));
    switch (kindOf(i))
    {
        case K_ON:   return v > 0.5 ? 1.0f : 0.0f;
        case K_TYPE: return clamp01((float) (v / 6.0));
        case K_FREQ: return clamp01((float) (std::log(v / 20.0) / std::log(1000.0)));
        case K_GAIN: return clamp01((float) ((v + 24.0) / 48.0));
        default:     return clamp01((float) (std::log(v / 0.1) / std::log(180.0)));
    }
}

static float defaultNorm(int i)
{
    if (i == 0) return 0.5f;
    int b = bandOf(i);
    switch (kindOf(i))
    {
        case K_ON:   return 1.0f;
        case K_TYPE: return toNorm(i, b == 0 ? T_LOWSHELF : (b == NB - 1 ? T_HIGHSHELF : T_PEAK));
        case K_FREQ: return toNorm(i, kDefFreq[b]);
        case K_GAIN: return 0.5f;
        default:     return toNorm(i, 0.707);
    }
}

static std::string dispA(int i, float n)
{
    char b[48];
    double v = toNative(i, n);
    if (i == 0) { snprintf(b, sizeof b, "%+.1f dB", v); return b; }
    switch (kindOf(i))
    {
        case K_ON:   return v > 0.5 ? "On" : "Off";
        case K_TYPE: { std::wstring w = kTypeNames[(int) v]; return std::string(w.begin(), w.end()); }
        case K_FREQ: if (v >= 1000) snprintf(b, sizeof b, "%.2f kHz", v / 1000.0); else snprintf(b, sizeof b, "%.0f Hz", v); return b;
        case K_GAIN: snprintf(b, sizeof b, "%+.1f dB", v); return b;
        default:     snprintf(b, sizeof b, "Q %.2f", v); return b;
    }
}

static std::string nameA(int i)
{
    if (i == 0) return "Preamp";
    static const char* k[5] = { "On", "Type", "Freq", "Gain", "Q" };
    return "B" + std::to_string(bandOf(i) + 1) + " " + k[kindOf(i)];
}

// ============================================================ DSP
struct Bq { double b0, b1, b2, a1, a2; };

static Bq makeBq(int type, double sr, double f, double q, double gDb)
{
    const double PI = 3.14159265358979323846;
    f = f < 10 ? 10 : (f > sr * 0.49 ? sr * 0.49 : f);
    double w0 = 2 * PI * f / sr, cs = std::cos(w0), sn = std::sin(w0);
    double al = sn / (2 * q), A = std::pow(10.0, gDb / 40.0), sA = std::sqrt(A);
    double b0, b1, b2, a0, a1, a2;
    switch (type)
    {
        case T_LOWSHELF:
            b0 = A * ((A + 1) - (A - 1) * cs + 2 * sA * al);
            b1 = 2 * A * ((A - 1) - (A + 1) * cs);
            b2 = A * ((A + 1) - (A - 1) * cs - 2 * sA * al);
            a0 = (A + 1) + (A - 1) * cs + 2 * sA * al;
            a1 = -2 * ((A - 1) + (A + 1) * cs);
            a2 = (A + 1) + (A - 1) * cs - 2 * sA * al; break;
        case T_HIGHSHELF:
            b0 = A * ((A + 1) + (A - 1) * cs + 2 * sA * al);
            b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 = A * ((A + 1) + (A - 1) * cs - 2 * sA * al);
            a0 = (A + 1) - (A - 1) * cs + 2 * sA * al;
            a1 = 2 * ((A - 1) - (A + 1) * cs);
            a2 = (A + 1) - (A - 1) * cs - 2 * sA * al; break;
        case T_HP: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
        case T_LP: b0 = (1 - cs) / 2; b1 = 1 - cs;    b2 = b0; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
        case T_BP: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
        case T_NOTCH: b0 = 1; b1 = -2 * cs; b2 = 1; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
        default:
            b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A;
            a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; break;
    }
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

static double magnitude(const Bq& c, double f, double sr)
{
    double w = 2 * 3.14159265358979323846 * f / sr;
    std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

// ============================================================ plugin
struct Editor;

struct Plugin
{
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> P[NP];
    std::atomic<float> peak { 0.0f };
    double sr = 48000.0;
    double z[2][NB][2] = {};
    Editor* ed = nullptr;
    ERect rect = { 0, 0, (int16_t) GUI_H, (int16_t) GUI_W };

    Plugin(audioMasterCallback m);
    Bq bandCoeffs(int b) const
    {
        return makeBq((int) toNative(pidx(b, K_TYPE), P[pidx(b, K_TYPE)]), sr,
                      toNative(pidx(b, K_FREQ), P[pidx(b, K_FREQ)]),
                      toNative(pidx(b, K_Q), P[pidx(b, K_Q)]),
                      toNative(pidx(b, K_GAIN), P[pidx(b, K_GAIN)]));
    }
    bool bandOn(int b) const { return P[pidx(b, K_ON)] > 0.5f; }

    void setParam(int i, float n)
    {
        n = clamp01(n);
        P[i] = n;
        if (master) master(&fx, audioMasterAutomate, i, 0, nullptr, n);
    }

    void process(float** in, float** out, int n)
    {
        Bq c[NB]; bool on[NB];
        for (int b = 0; b < NB; ++b) { on[b] = bandOn(b); c[b] = bandCoeffs(b); }
        const double pre = std::pow(10.0, toNative(0, P[0]) / 20.0);
        float pk = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < n; ++i)
            {
                double x = in[ch][i] * pre;
                for (int b = 0; b < NB; ++b)
                {
                    if (!on[b]) continue;
                    double* s = z[ch][b];
                    double y = c[b].b0 * x + s[0];
                    s[0] = c[b].b1 * x - c[b].a1 * y + s[1];
                    s[1] = c[b].b2 * x - c[b].a2 * y;
                    x = y;
                }
                out[ch][i] = (float) x;
                float a = std::fabs((float) x);
                if (a > pk) pk = a;
            }
        }
        peak = pk;
    }
};

// ============================================================ editor (Win32 + GDI+)
struct Widget { int kind; float x, y, w, h; int param; };   // 0 hslider 1 vfader 2 knob 3 toggle 4 type button
static const RectF G(12, 72, 956, 164);

static const Color cBlue(47, 184, 255), cText(216, 221, 227), cPanel(35, 40, 47);

static void txt(Graphics& g, const std::wstring& s, float x, float y, float w, float h, float sz, Color c,
                StringAlignment al = StringAlignmentCenter, bool bold = false, bool italic = false)
{
    FontFamily ff(L"Segoe UI");
    Font f(&ff, sz, (bold ? FontStyleBold : FontStyleRegular) | (italic ? FontStyleItalic : 0), UnitPixel);
    SolidBrush br(c);
    StringFormat sf; sf.SetAlignment(al); sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s.c_str(), -1, &f, RectF(x, y, w, h), &sf, &br);
}

static void roundRect(GraphicsPath& p, float x, float y, float w, float h, float r)
{
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}
static void fillRound(Graphics& g, Brush* b, float x, float y, float w, float h, float r)
{ GraphicsPath p; roundRect(p, x, y, w, h, r); g.FillPath(b, &p); }

static const Color kBandCol[NB] = {
    Color(255, 92, 92), Color(255, 160, 60), Color(255, 220, 70), Color(110, 230, 110),
    Color(47, 184, 255), Color(150, 110, 255), Color(255, 110, 200) };

static Color withA(Color c, int a) { return Color((BYTE) a, c.GetR(), c.GetG(), c.GetB()); }

static void strokeRound(Graphics& g, Pen* p, float x, float y, float w, float h, float r)
{ GraphicsPath gp; roundRect(gp, x, y, w, h, r); g.DrawPath(p, &gp); }

static void drawPanel(Graphics& g, float x, float y, float w, float h, float r = 6)
{
    for (int i = 4; i >= 1; --i)
    { SolidBrush s(Color(14, 0, 0, 0)); fillRound(g, &s, x - i + 1, y + i, w + 2 * i - 2, h, r + i); }
    LinearGradientBrush gb(PointF(0, y), PointF(0, y + h), Color(48, 55, 64), Color(29, 33, 39));
    fillRound(g, &gb, x, y, w, h, r);
    Pen hi(Color(60, 255, 255, 255), 1), lo(Color(200, 8, 10, 12), 1);
    strokeRound(g, &lo, x, y, w, h, r);
    strokeRound(g, &hi, x + 1, y + 1, w - 2, h - 2, r - 1);
}

static void drawScrew(Graphics& g, float cx, float cy)
{
    LinearGradientBrush b(PointF(cx - 4, cy - 4), PointF(cx + 4, cy + 4), Color(190, 195, 200), Color(70, 75, 82));
    g.FillEllipse(&b, cx - 4, cy - 4, 8.0f, 8.0f);
    Pen p(Color(30, 32, 36), 1.3f);
    g.DrawLine(&p, cx - 2.5f, cy + 1.5f, cx + 2.5f, cy - 1.5f);
}

struct Editor
{
    Plugin* pl;
    HWND hwnd = nullptr;
    ULONG_PTR gdiTok = 0;
    std::vector<Widget> ws;
    float meter = 0;
    int dragW = -1, dragDot = -1;
    float startN = 0; int startX = 0, startY = 0;

    Editor(Plugin* p) : pl(p)
    {
        ws.push_back({ 0, 175, 14, 330, 28, 0 });
        for (int b = 0; b < NB; ++b)
        {
            float bx = 8.0f + b * 138, y0 = 252;
            ws.push_back({ 3, bx + 58, y0 + 10,  18,  18,  pidx(b, K_ON) });
            ws.push_back({ 1, bx + 49, y0 + 44,  36,  170, pidx(b, K_GAIN) });
            ws.push_back({ 2, bx + 14, y0 + 254, 48,  48,  pidx(b, K_FREQ) });
            ws.push_back({ 2, bx + 72, y0 + 254, 48,  48,  pidx(b, K_Q) });
            ws.push_back({ 4, bx + 8,  y0 + 326, 118, 26,  pidx(b, K_TYPE) });
        }
    }

    static const wchar_t* cls() { return L"ClxpEQWindow"; }

    bool open(HWND parent)
    {
        GdiplusStartupInput in; GdiplusStartup(&gdiTok, &in, nullptr);
        WNDCLASSEXW wc = { sizeof wc };
        wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndProc; wc.hInstance = gInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = cls();
        RegisterClassExW(&wc);
        hwnd = CreateWindowExW(0, cls(), L"", WS_CHILD | WS_VISIBLE, 0, 0, GUI_W, GUI_H, parent, nullptr, gInst, this);
        SetTimer(hwnd, 1, 33, nullptr);
        return hwnd != nullptr;
    }
    void close()
    {
        if (hwnd) { KillTimer(hwnd, 1); DestroyWindow(hwnd); hwnd = nullptr; }
        UnregisterClassW(cls(), gInst);
        if (gdiTok) GdiplusShutdown(gdiTok);
        gdiTok = 0;
    }

    float fx(double f) const { return G.X + G.Width * (float) (std::log(f / 20.0) / std::log(1000.0)); }
    float dy(double db) const { return G.Y + G.Height * (float) ((24.0 - db) / 48.0); }

    int hitWidget(int x, int y) const
    {
        for (int i = (int) ws.size() - 1; i >= 0; --i)
        {
            const Widget& w = ws[i];
            if (x >= w.x && x <= w.x + w.w && y >= w.y && y <= w.y + w.h) return i;
        }
        return -1;
    }
    int hitDot(int x, int y) const
    {
        for (int b = NB - 1; b >= 0; --b)
        {
            if (!pl->bandOn(b)) continue;
            int t = (int) toNative(pidx(b, K_TYPE), pl->P[pidx(b, K_TYPE)]);
            float px = fx(toNative(pidx(b, K_FREQ), pl->P[pidx(b, K_FREQ)]));
            float py = dy(usesGain(t) ? toNative(pidx(b, K_GAIN), pl->P[pidx(b, K_GAIN)]) : 0.0);
            if ((x - px) * (x - px) + (y - py) * (y - py) < 100) return b;
        }
        return -1;
    }

    bool bandOn(int b) const { return pl->bandOn(b); }

    void paint(HDC hdc)
    {
        Bitmap bmp(GUI_W, GUI_H, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        LinearGradientBrush bg(Point(0, 0), Point(0, GUI_H), Color(40, 46, 54), Color(20, 23, 28));
        g.FillRectangle(&bg, 0, 0, GUI_W, GUI_H);
        Pen brush(Color(6, 255, 255, 255), 1);
        for (int y = 0; y < GUI_H; y += 3) g.DrawLine(&brush, 0, y, GUI_W, y);

        drawPanel(g, 8, 8, GUI_W - 16, 52);
        drawScrew(g, 20, 20); drawScrew(g, GUI_W - 20, 20);
        txt(g, L"@Clxp", 34, 8, 130, 52, 30, Color(240, 244, 248), StringAlignmentNear, true);
        txt(g, L"GAIN", 128, 8, 44, 52, 11, Color(140, 216, 221, 227), StringAlignmentNear, true);

        SolidBrush well(Color(10, 12, 15)); fillRound(g, &well, 610, 14, 340, 40, 5);
        Pen wl(Color(70, 0, 0, 0), 1.5f); strokeRound(g, &wl, 610, 14, 340, 40, 5);
        float lvl = 20.0f * std::log10(meter > 1e-4f ? meter : 1e-4f);
        int lit = (int) ((lvl + 60.0f) / 60.0f * 30.0f); if (lit < 0) lit = 0;
        for (int i = 0; i < 30; ++i)
        {
            Color c = i < 18 ? cBlue : (i < 25 ? Color(127, 224, 90) : Color(255, 77, 61));
            float x = 618.0f + i * 10.8f;
            if (i < lit) { SolidBrush gl(withA(c, 45)); g.FillRectangle(&gl, x - 2, 18.0f, 11.0f, 32.0f); }
            SolidBrush sb(i < lit ? c : withA(c, 38));
            g.FillRectangle(&sb, x, 21.0f, 7.0f, 26.0f);
        }

        paintGraph(g);

        for (int b = 0; b < NB; ++b)
        {
            float bx = 8.0f + b * 138, y0 = 252;
            bool on = bandOn(b);
            drawPanel(g, bx, y0, 134, 354, 5);

            LinearGradientBrush hd(PointF(0, y0), PointF(0, y0 + 36), Color(52, 60, 70), Color(36, 42, 50));
            fillRound(g, &hd, bx + 3, y0 + 3, 128, 36, 4);
            SolidBrush acc(withA(kBandCol[b], on ? 255 : 70));
            fillRound(g, &acc, bx + 6, y0 + 38, 122, 2, 1);
            txt(g, std::to_wstring(b + 1), bx + 10, y0 + 3, 40, 36, 20, withA(kBandCol[b], on ? 255 : 110), StringAlignmentNear, true);

            SolidBrush well2(Color(70, 10, 12, 15)); fillRound(g, &well2, bx + 40, y0 + 42, 54, 174, 5);

            txt(g, L"FREQ", bx + 14, y0 + 238, 48, 12, 9, Color(130, 216, 221, 227));
            txt(g, L"Q",    bx + 72, y0 + 238, 48, 12, 9, Color(130, 216, 221, 227));

            Pen dv(Color(40, 255, 255, 255), 1); g.DrawLine(&dv, bx + 10, y0 + 318, bx + 124, y0 + 318);
        }
        for (auto& w : ws) paintWidget(g, w);

        txt(g, L"Made by Clxp", GUI_W - 220, GUI_H - 30, 200, 22, 14, Color(130, 216, 221, 227), StringAlignmentFar, true, true);
        txt(g, L"7-BAND PARAMETRIC EQ", 20, GUI_H - 30, 300, 22, 11, Color(90, 216, 221, 227), StringAlignmentNear, true);

        Graphics out(hdc);
        out.DrawImage(&bmp, 0, 0, GUI_W, GUI_H);
    }

    void paintGraph(Graphics& g)
    {
        drawPanel(g, G.X - 4, G.Y - 4, G.Width + 8, G.Height + 8, 6);
        LinearGradientBrush gb(PointF(0, G.Y), PointF(0, G.Y + G.Height), Color(10, 14, 20), Color(18, 26, 36));
        fillRound(g, &gb, G.X, G.Y, G.Width, G.Height, 4);
        g.SetClip(RectF(G.X, G.Y, G.Width, G.Height));

        Pen grid(Color(16, 255, 255, 255), 1), minor(Color(8, 255, 255, 255), 1), zero(Color(70, 255, 255, 255), 1);
        for (double base : { 10., 100., 1000., 10000. })
            for (int m = 1; m < 10; ++m)
            {
                double f = base * m; if (f < 20 || f > 20000) continue;
                float x = fx(f);
                g.DrawLine((m == 1 || m == 5) ? &grid : &minor, x, G.Y, x, G.Y + G.Height);
            }
        for (double f : { 50., 100., 500., 1000., 5000., 10000. })
        {
            std::wstring s = f >= 1000 ? std::to_wstring((int) (f / 1000)) + L"k" : std::to_wstring((int) f);
            txt(g, s, fx(f) + 2, G.Y + G.Height - 16, 40, 14, 10, Color(110, 255, 255, 255), StringAlignmentNear);
        }
        for (int db = -18; db <= 18; db += 6)
        {
            float y = dy(db);
            g.DrawLine(db == 0 ? &zero : &grid, G.X, y, G.X + G.Width, y);
            txt(g, (db > 0 ? L"+" : L"") + std::to_wstring(db), G.X + 4, y - 7, 30, 14, 10, Color(110, 255, 255, 255), StringAlignmentNear);
        }
        txt(g, L"Made by Clxp", G.X, G.Y, G.Width, G.Height, 58, Color(14, 255, 255, 255), StringAlignmentCenter, true);

        Bq c[NB]; bool on[NB];
        for (int b = 0; b < NB; ++b) { on[b] = pl->bandOn(b); c[b] = pl->bandCoeffs(b); }
        double preDb = toNative(0, pl->P[0]);
        const int N = (int) G.Width;
        std::vector<PointF> pts;
        std::vector<PointF> bandPts[NB];
        for (int px = 0; px <= N; ++px)
        {
            double f = 20.0 * std::pow(1000.0, px / (double) N), m = 1.0;
            for (int b = 0; b < NB; ++b)
                if (on[b])
                {
                    double mb = magnitude(c[b], f, pl->sr); m *= mb;
                    double d = 20.0 * std::log10(mb > 1e-6 ? mb : 1e-6);
                    d = d > 24 ? 24 : (d < -24 ? -24 : d);
                    bandPts[b].push_back(PointF(G.X + (float) px, dy(d)));
                }
            double db = 20.0 * std::log10(m > 1e-6 ? m : 1e-6) + preDb;
            db = db > 24 ? 24 : (db < -24 ? -24 : db);
            pts.push_back(PointF(G.X + (float) px, dy(db)));
        }
        for (int b = 0; b < NB; ++b)
            if (on[b]) { Pen bp(withA(kBandCol[b], 70), 1.2f); g.DrawLines(&bp, bandPts[b].data(), (INT) bandPts[b].size()); }

        std::vector<PointF> fill = pts;
        fill.push_back(PointF(G.X + G.Width, dy(0))); fill.push_back(PointF(G.X, dy(0)));
        LinearGradientBrush fb(PointF(0, G.Y), PointF(0, G.Y + G.Height), Color(90, 47, 184, 255), Color(10, 47, 184, 255));
        g.FillPolygon(&fb, fill.data(), (INT) fill.size());
        for (int w = 9; w >= 3; w -= 3) { Pen glow(Color(22, 47, 184, 255), (float) w); g.DrawLines(&glow, pts.data(), (INT) pts.size()); }
        Pen cp(Color(150, 220, 255), 2.0f);
        g.DrawLines(&cp, pts.data(), (INT) pts.size());
        g.ResetClip();

        for (int b = 0; b < NB; ++b)
        {
            if (!on[b]) continue;
            int t = (int) toNative(pidx(b, K_TYPE), pl->P[pidx(b, K_TYPE)]);
            float px = fx(toNative(pidx(b, K_FREQ), pl->P[pidx(b, K_FREQ)]));
            float py = dy(usesGain(t) ? toNative(pidx(b, K_GAIN), pl->P[pidx(b, K_GAIN)]) : 0.0);
            SolidBrush gl(withA(kBandCol[b], 60)); g.FillEllipse(&gl, px - 13, py - 13, 26.0f, 26.0f);
            SolidBrush ring(kBandCol[b]); g.FillEllipse(&ring, px - 9, py - 9, 18.0f, 18.0f);
            SolidBrush core(Color(20, 24, 30)); g.FillEllipse(&core, px - 6.5f, py - 6.5f, 13.0f, 13.0f);
            txt(g, std::to_wstring(b + 1), px - 9, py - 9, 18, 18, 10, Color(255, 255, 255), StringAlignmentCenter, true);
        }
    }

    void paintWidget(Graphics& g, const Widget& w)
    {
        float n = pl->P[w.param];
        std::string d = dispA(w.param, n);
        std::wstring ds(d.begin(), d.end());
        Color accent = w.param == 0 ? cBlue : kBandCol[bandOf(w.param)];
        switch (w.kind)
        {
            case 0:
            {
                SolidBrush tr(Color(8, 10, 13)); fillRound(g, &tr, w.x, w.y + w.h / 2 - 5, w.w, 10, 5);
                Pen tp(Color(70, 255, 255, 255), 1); strokeRound(g, &tp, w.x, w.y + w.h / 2 - 5, w.w, 10, 5);
                float tx = w.x + n * (w.w - 30);
                SolidBrush fl(withA(cBlue, 200)); fillRound(g, &fl, w.x + 2, w.y + w.h / 2 - 3, tx - w.x + 14, 6, 3);
                for (int i = 0; i <= 8; ++i)
                { Pen t(Color(60, 255, 255, 255), 1); float x = w.x + 15 + i * (w.w - 30) / 8; g.DrawLine(&t, x, w.y + w.h - 2, x, w.y + w.h + 3); }
                LinearGradientBrush th(PointF(0, w.y), PointF(0, w.y + w.h), Color(245, 247, 249), Color(100, 106, 114));
                fillRound(g, &th, tx, w.y, 30, w.h, 4);
                Pen ol(Color(20, 22, 25), 1); strokeRound(g, &ol, tx, w.y, 30, w.h, 4);
                Pen gp(Color(55, 58, 63), 1.2f);
                for (int i = -1; i <= 1; ++i) g.DrawLine(&gp, tx + 15 + i * 5.0f, w.y + 6, tx + 15 + i * 5.0f, w.y + w.h - 6);
                SolidBrush vb(Color(10, 12, 15)); fillRound(g, &vb, w.x + w.w + 8, w.y, 76, w.h, 4);
                txt(g, ds, w.x + w.w + 8, w.y, 76, w.h, 13, cBlue, StringAlignmentCenter, true);
                break;
            }
            case 1:
            {
                float cx = w.x + w.w / 2;
                SolidBrush tr(Color(8, 10, 13)); fillRound(g, &tr, cx - 3, w.y, 6, w.h, 3);
                for (int i = 0; i <= 8; ++i)
                {
                    float y = w.y + 8 + i * (w.h - 16) / 8.0f; float len = (i == 4) ? 8.0f : 4.0f;
                    Pen t(Color(i == 4 ? 140 : 70, 255, 255, 255), 1);
                    g.DrawLine(&t, cx - 6 - len, y, cx - 6, y);
                    g.DrawLine(&t, cx + 6, y, cx + 6 + len, y);
                }
                float ty = w.y + (1 - n) * (w.h - 16);
                float mid = w.y + (w.h - 16) / 2 + 8;
                SolidBrush fl(withA(accent, 190));
                float y1 = ty + 8, top = y1 < mid ? y1 : mid;
                g.FillRectangle(&fl, cx - 1.5f, top, 3.0f, std::fabs(mid - y1));
                LinearGradientBrush th(PointF(w.x, 0), PointF(w.x + w.w, 0), Color(245, 247, 249), Color(110, 116, 124));
                fillRound(g, &th, w.x, ty, w.w, 16, 3);
                Pen ol(Color(20, 22, 25), 1); strokeRound(g, &ol, w.x, ty, w.w, 16, 3);
                Pen gp(Color(40, 43, 47), 1.6f); g.DrawLine(&gp, w.x + 4, ty + 8, w.x + w.w - 4, ty + 8);
                txt(g, ds, w.x - 8, w.y + w.h + 6, w.w + 16, 14, 11, cText);
                break;
            }
            case 2:
            {
                float cx = w.x + w.w / 2, cy = w.y + w.h / 2, r = w.w / 2 - 1;
                for (int i = 0; i <= 10; ++i)
                {
                    float a = (135.0f + 27.0f * i) * 3.14159265f / 180.0f;
                    Pen t(Color(i <= n * 10 + 0.01f ? 160 : 60, 255, 255, 255), 1);
                    g.DrawLine(&t, cx + std::cos(a) * (r + 1), cy + std::sin(a) * (r + 1), cx + std::cos(a) * (r + 4), cy + std::sin(a) * (r + 4));
                }
                float ar = r - 3;
                Pen bgp(Color(10, 12, 15), 4.0f); g.DrawArc(&bgp, cx - ar, cy - ar, ar * 2, ar * 2, 135.0f, 270.0f);
                if (n > 0.003f)
                {
                    Pen glow(withA(accent, 50), 7.0f); g.DrawArc(&glow, cx - ar, cy - ar, ar * 2, ar * 2, 135.0f, 270.0f * n);
                    Pen ap(accent, 3.5f); g.DrawArc(&ap, cx - ar, cy - ar, ar * 2, ar * 2, 135.0f, 270.0f * n);
                }
                float kr = ar * 0.78f;
                SolidBrush sh(Color(90, 0, 0, 0)); g.FillEllipse(&sh, cx - kr + 1, cy - kr + 3, kr * 2, kr * 2);
                LinearGradientBrush kb(PointF(cx - kr, cy - kr), PointF(cx + kr, cy + kr), Color(232, 235, 239), Color(58, 63, 71));
                g.FillEllipse(&kb, cx - kr, cy - kr, kr * 2, kr * 2);
                float ir = kr * 0.72f;
                LinearGradientBrush ib(PointF(0, cy - ir), PointF(0, cy + ir), Color(78, 85, 94), Color(34, 38, 44));
                g.FillEllipse(&ib, cx - ir, cy - ir, ir * 2, ir * 2);
                Pen op(Color(10, 12, 14), 1); g.DrawEllipse(&op, cx - kr, cy - kr, kr * 2, kr * 2);
                float a = (135.0f + 270.0f * n) * 3.14159265f / 180.0f;
                Pen pp(accent, 2.6f); pp.SetStartCap(LineCapRound); pp.SetEndCap(LineCapRound);
                g.DrawLine(&pp, cx + std::cos(a) * ir * 0.3f, cy + std::sin(a) * ir * 0.3f, cx + std::cos(a) * ir * 0.95f, cy + std::sin(a) * ir * 0.95f);
                txt(g, ds, w.x - 14, w.y + w.h + 3, w.w + 28, 14, 11, cText);
                break;
            }
            case 3:
            {
                bool on = n > 0.5f;
                if (on) { for (int i = 3; i >= 1; --i) { SolidBrush glow(withA(accent, 28)); fillRound(g, &glow, w.x - i * 2, w.y - i * 2, w.w + i * 4, w.h + i * 4, 4.0f + i); } }
                LinearGradientBrush bb(PointF(0, w.y), PointF(0, w.y + w.h), on ? accent : Color(40, 64, 82), on ? withA(accent, 150) : Color(24, 40, 52));
                fillRound(g, &bb, w.x, w.y, w.w, w.h, 4);
                Pen op(Color(8, 10, 12), 1.2f); strokeRound(g, &op, w.x, w.y, w.w, w.h, 4);
                Pen hi(Color(on ? 120 : 40, 255, 255, 255), 1); g.DrawLine(&hi, w.x + 4, w.y + 3, w.x + w.w - 4, w.y + 3);
                break;
            }
            case 4:
            {
                LinearGradientBrush bb(PointF(0, w.y), PointF(0, w.y + w.h), Color(30, 35, 42), Color(14, 17, 21));
                fillRound(g, &bb, w.x, w.y, w.w, w.h, 4);
                Pen op(Color(70, 78, 90), 1); strokeRound(g, &op, w.x, w.y, w.w, w.h, 4);
                txt(g, kTypeNames[(int) toNative(w.param, n)], w.x + 6, w.y, w.w - 24, w.h, 13, cText, StringAlignmentNear);
                PointF tri[3] = { PointF(w.x + w.w - 18, w.y + 10), PointF(w.x + w.w - 8, w.y + 10), PointF(w.x + w.w - 13, w.y + 16) };
                SolidBrush ab(accent); g.FillPolygon(&ab, tri, 3);
                break;
            }
        }
    }

    // ---- interaction
    void mouseDown(int x, int y)
    {
        dragW = dragDot = -1;
        int d = hitDot(x, y);
        if (d >= 0) { dragDot = d; SetCapture(hwnd); return; }
        int i = hitWidget(x, y);
        if (i < 0) return;
        const Widget& w = ws[i];
        if (w.kind == 3) { pl->setParam(w.param, pl->P[w.param] > 0.5f ? 0.0f : 1.0f); return; }
        if (w.kind == 4)
        {
            HMENU m = CreatePopupMenu();
            int cur = (int) toNative(w.param, pl->P[w.param]);
            for (int t = 0; t < 7; ++t) AppendMenuW(m, MF_STRING | (t == cur ? MF_CHECKED : 0), 1 + t, kTypeNames[t]);
            POINT pt = { (LONG) w.x, (LONG) (w.y + w.h) }; ClientToScreen(hwnd, &pt);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd > 0) pl->setParam(w.param, toNorm(w.param, cmd - 1));
            return;
        }
        dragW = i; startN = pl->P[w.param]; startX = x; startY = y; SetCapture(hwnd);
    }

    void mouseMove(int x, int y)
    {
        if (dragDot >= 0)
        {
            int b = dragDot;
            pl->setParam(pidx(b, K_FREQ), (x - G.X) / G.Width);
            int t = (int) toNative(pidx(b, K_TYPE), pl->P[pidx(b, K_TYPE)]);
            if (usesGain(t))
            {
                double db = 24.0 - (y - G.Y) / G.Height * 48.0;
                pl->setParam(pidx(b, K_GAIN), toNorm(pidx(b, K_GAIN), db < -24 ? -24 : (db > 24 ? 24 : db)));
            }
        }
        else if (dragW >= 0)
        {
            const Widget& w = ws[dragW];
            float n;
            if (w.kind == 0) n = startN + (x - startX) / (w.w - 30);
            else if (w.kind == 1) n = startN + (startY - y) / (w.h - 16);
            else n = startN + (startY - y) / 150.0f;
            pl->setParam(w.param, n);
        }
    }

    void mouseUp() { dragW = dragDot = -1; ReleaseCapture(); }

    void dblClick(int x, int y)
    {
        int i = hitWidget(x, y);
        if (i >= 0 && (ws[i].kind <= 2)) pl->setParam(ws[i].param, defaultNorm(ws[i].param));
    }

    void wheel(int x, int y, int delta)
    {
        int i = hitWidget(x, y);
        if (i < 0) return;
        const Widget& w = ws[i];
        float steps = delta / 120.0f;
        if (w.kind <= 2) pl->setParam(w.param, pl->P[w.param] + steps * 0.02f);
        else if (w.kind == 4)
        {
            int t = (int) toNative(w.param, pl->P[w.param]) + (steps > 0 ? 1 : -1);
            t = t < 0 ? 0 : (t > 6 ? 6 : t);
            pl->setParam(w.param, toNorm(w.param, t));
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        if (m == WM_CREATE) SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR) ((CREATESTRUCT*) lp)->lpCreateParams);
        Editor* e = (Editor*) GetWindowLongPtr(h, GWLP_USERDATA);
        if (!e) return DefWindowProc(h, m, wp, lp);
        int x = (short) LOWORD(lp), y = (short) HIWORD(lp);
        switch (m)
        {
            case WM_PAINT:      { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); e->paint(dc); EndPaint(h, &ps); return 0; }
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:      { float pk = e->pl->peak; e->meter = pk > e->meter * 0.85f ? pk : e->meter * 0.85f; InvalidateRect(h, nullptr, FALSE); return 0; }
            case WM_LBUTTONDOWN:   e->mouseDown(x, y); return 0;
            case WM_LBUTTONDBLCLK: e->dblClick(x, y); return 0;
            case WM_MOUSEMOVE:     if (wp & MK_LBUTTON) e->mouseMove(x, y); return 0;
            case WM_LBUTTONUP:     e->mouseUp(); return 0;
            case WM_MOUSEWHEEL:    { POINT p = { (short) LOWORD(lp), (short) HIWORD(lp) }; ScreenToClient(h, &p); e->wheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp)); return 0; }
        }
        return DefWindowProc(h, m, wp, lp);
    }
};

// ============================================================ VST2 glue
static void copyStr(void* dst, const char* s, size_t maxLen)
{
    strncpy((char*) dst, s, maxLen - 1);
    ((char*) dst)[maxLen - 1] = 0;
}

static intptr_t dispatch(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    Plugin* p = (Plugin*) e->object;
    switch (op)
    {
        case effClose:
            if (p->ed) { p->ed->close(); delete p->ed; }
            delete p; return 0;
        case effGetProgram: return 0;
        case effGetProgramName: copyStr(ptr, "Default", 24); return 0;
        case effGetParamLabel:
        {
            const char* l = (idx == 0 || (idx > 0 && kindOf(idx) == K_GAIN)) ? "dB" : (idx > 0 && kindOf(idx) == K_FREQ ? "Hz" : "");
            copyStr(ptr, l, 8); return 0;
        }
        case effGetParamDisplay: copyStr(ptr, dispA(idx, p->P[idx]).c_str(), 8); return 0;
        case effGetParamName:    copyStr(ptr, nameA(idx).c_str(), 8); return 0;
        case effSetSampleRate:   p->sr = opt > 0 ? opt : 48000.0; return 0;
        case effMainsChanged:    memset(p->z, 0, sizeof p->z); return 0;
        case effEditGetRect:     *(ERect**) ptr = &p->rect; return 1;
        case effEditOpen:
            if (!p->ed) p->ed = new Editor(p);
            return p->ed->open((HWND) ptr) ? 1 : 0;
        case effEditClose:
            if (p->ed) { p->ed->close(); delete p->ed; p->ed = nullptr; }
            return 0;
        case effEditIdle:        return 0;
        case effGetEffectName:
        case effGetProductString: copyStr(ptr, "@Clxp", 32); return 1;
        case effGetVendorString:  copyStr(ptr, "Clxp", 32); return 1;
        case effGetVendorVersion: return 1000;
        case effGetVstVersion:    return 2400;
        case effCanDo:            return 0;
    }
    return 0;
}

static void setParameterCb(AEffect* e, int32_t i, float v) { if (i >= 0 && i < NP) ((Plugin*) e->object)->P[i] = clamp01(v); }
static float getParameterCb(AEffect* e, int32_t i)        { return (i >= 0 && i < NP) ? ((Plugin*) e->object)->P[i].load() : 0.0f; }
static void processReplacingCb(AEffect* e, float** in, float** out, int32_t n) { ((Plugin*) e->object)->process(in, out, n); }
static void processCb(AEffect* e, float** in, float** out, int32_t n) { processReplacingCb(e, in, out, n); }

Plugin::Plugin(audioMasterCallback m) : master(m)
{
    for (int i = 0; i < NP; ++i) P[i] = defaultNorm(i);
    memset(&fx, 0, sizeof fx);
    fx.magic = kEffectMagic;
    fx.dispatcher = dispatch;
    fx.process = processCb;
    fx.setParameter = setParameterCb;
    fx.getParameter = getParameterCb;
    fx.processReplacing = processReplacingCb;
    fx.numPrograms = 1;
    fx.numParams = NP;
    fx.numInputs = 2;
    fx.numOutputs = 2;
    fx.flags = effFlagsHasEditor | effFlagsCanReplacing;
    fx.uniqueID = ('C' << 24) | ('l' << 16) | ('x' << 8) | 'E';
    fx.version = 1000;
    fx.object = this;
}

extern "C" __declspec(dllexport) AEffect* VSTPluginMain(audioMasterCallback master)
{
    Plugin* p = new Plugin(master);
    return &p->fx;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) gInst = h;
    return TRUE;
}
