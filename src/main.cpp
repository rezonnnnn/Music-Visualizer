// Music Visualizer: a tiny desktop overlay that draws your speaker output as
// neon segments. One file, one thread, no runtime. See README.md.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcommon.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FFT_N 2048
#define RING 4096
#define MAXC 96
#define WM_TRAY (WM_APP + 1)
template <class T> static void rel(T*& p) { if (p) { p->Release(); p = 0; } }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// ---------------------------------------------------------------- settings
struct Cfg {
    float col[3], gcol[3];
    int cols, rows, top, fps, scan, readouts, brackets, glitch, peaks, hideFs;
    float heightPct, opacity;
};
static Cfg cfg;
static wchar_t iniPath[MAX_PATH];
static FILETIME iniTime;

static const char DEFAULT_INI[] =
    "; Music Visualizer settings. Save the file and changes apply within a second.\r\n"
    "[visualizer]\r\n"
    "color = 00F0FF          ; neon colour, hex RRGGBB\r\n"
    "glitch_color = FF2A6D   ; colour of the beat glitch line\r\n"
    "columns = 48            ; 8 to 96\r\n"
    "rows = 10               ; 4 to 16\r\n"
    "height = 6              ; bar height, percent of screen height (2 to 30)\r\n"
    "opacity = 100           ; 10 to 100\r\n"
    "position = bottom       ; bottom or top\r\n"
    "fps = 60                ; 15 to 60\r\n"
    "scanlines = 1\r\n"
    "readouts = 1            ; BPM, peak frequency and level text\r\n"
    "brackets = 1\r\n"
    "peaks = 1               ; hollow peak-hold markers\r\n"
    "glitch = 1              ; jitter on each beat\r\n"
    "hide_on_fullscreen = 1  ; hide while a fullscreen app or video is in front\r\n";

static int iniInt(const wchar_t* k, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(L"visualizer", k, def, iniPath);
    return v < lo ? lo : v > hi ? hi : v;
}
static void iniColor(const wchar_t* k, const wchar_t* def, float* out) {
    wchar_t b[32];
    GetPrivateProfileStringW(L"visualizer", k, def, b, 32, iniPath);
    unsigned v = wcstoul(b[0] == L'#' ? b + 1 : b, 0, 16);
    out[0] = ((v >> 16) & 255) / 255.f; out[1] = ((v >> 8) & 255) / 255.f; out[2] = (v & 255) / 255.f;
}
static FILETIME iniStamp() {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    GetFileAttributesExW(iniPath, GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}
static void loadCfg() {
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(iniPath, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, DEFAULT_INI, sizeof(DEFAULT_INI) - 1, &w, 0); CloseHandle(f); }
    }
    iniColor(L"color", L"00F0FF", cfg.col);
    iniColor(L"glitch_color", L"FF2A6D", cfg.gcol);
    cfg.cols = iniInt(L"columns", 48, 8, MAXC);
    cfg.rows = iniInt(L"rows", 10, 4, 16);
    cfg.heightPct = (float)iniInt(L"height", 6, 2, 30);
    cfg.opacity = iniInt(L"opacity", 100, 10, 100) / 100.f;
    cfg.fps = iniInt(L"fps", 60, 15, 60);
    cfg.scan = iniInt(L"scanlines", 1, 0, 1);
    cfg.readouts = iniInt(L"readouts", 1, 0, 1);
    cfg.brackets = iniInt(L"brackets", 1, 0, 1);
    cfg.peaks = iniInt(L"peaks", 1, 0, 1);
    cfg.glitch = iniInt(L"glitch", 1, 0, 1);
    cfg.hideFs = iniInt(L"hide_on_fullscreen", 1, 0, 1);
    wchar_t b[16];
    GetPrivateProfileStringW(L"visualizer", L"position", L"bottom", b, 16, iniPath);
    cfg.top = (b[0] == L't' || b[0] == L'T');
    iniTime = iniStamp();
}

// ---------------------------------------------------------------- audio (speaker loopback)
static IMMDeviceEnumerator* enm;
static IAudioClient* ac;
static IAudioCaptureClient* cc;
static int aFloat, aChans, aBits, aRate = 48000;
static wchar_t devId[300];
static float ring[RING];
static unsigned rpos;
static ULONGLONG lastPacket, lastLoud;

static void audioClose() { if (ac) ac->Stop(); rel(cc); rel(ac); }
static bool audioOpen() {
    audioClose();
    IMMDevice* dev = 0;
    if (FAILED(enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    LPWSTR id = 0;
    if (SUCCEEDED(dev->GetId(&id)) && id) { lstrcpynW(devId, id, 300); CoTaskMemFree(id); }
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&ac);
    dev->Release();
    if (FAILED(hr)) return false;
    WAVEFORMATEX* wf = 0;
    if (FAILED(ac->GetMixFormat(&wf))) { audioClose(); return false; }
    aFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
             (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1 == 3);
    aChans = wf->nChannels; aBits = wf->wBitsPerSample; aRate = wf->nSamplesPerSec;
    // 1 second buffer, so the idle loop can poll four times a second without losing audio
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wf, 0);
    CoTaskMemFree(wf);
    if (FAILED(hr) || FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)) || FAILED(ac->Start())) {
        audioClose(); return false;
    }
    return true;
}
static bool defaultDeviceChanged() {
    IMMDevice* dev = 0; bool ch = false;
    if (FAILED(enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    LPWSTR id = 0;
    if (SUCCEEDED(dev->GetId(&id)) && id) { ch = lstrcmpW(id, devId) != 0; CoTaskMemFree(id); }
    dev->Release();
    return ch;
}
static void audioRead() {
    if (!cc) return;
    UINT32 n = 0; HRESULT hr;
    while (SUCCEEDED(hr = cc->GetNextPacketSize(&n)) && n) {
        BYTE* d; UINT32 fr; DWORD fl;
        if (FAILED(cc->GetBuffer(&d, &fr, &fl, 0, 0))) break;
        float pk = 0;
        bool usable = !(fl & AUDCLNT_BUFFERFLAGS_SILENT) && ((aFloat && aBits == 32) || (!aFloat && aBits == 16));
        for (UINT32 i = 0; i < fr; i++) {
            float m = 0;
            if (usable) {
                if (aFloat) { const float* s = (const float*)d + i * aChans; for (int c = 0; c < aChans; c++) m += s[c]; }
                else { const short* s = (const short*)d + i * aChans; for (int c = 0; c < aChans; c++) m += s[c] / 32768.f; }
                m /= aChans;
            }
            ring[rpos++ & (RING - 1)] = m;
            float am = fabsf(m); if (am > pk) pk = am;
        }
        cc->ReleaseBuffer(fr);
        lastPacket = GetTickCount64();
        if (pk > 1e-4f) lastLoud = lastPacket;
    }
    if (FAILED(hr)) audioClose();  // device went away; the housekeeping tick reopens it
}

// ---------------------------------------------------------------- analysis
static float hannW[FFT_N], cosT[FFT_N / 2], sinT[FFT_N / 2], re[FFT_N], im[FFT_N];
static unsigned short brev[FFT_N];
static float edge[MAXC + 1], tilt[MAXC], centerHz[MAXC];
static float lv[MAXC], pk[MAXC];
static float peakDb = -40, rmsDb = -99, pulse, glitch, prevLow, bpm, sinceBeat = 9;
static float beatIv[8]; static int beatN;

static void fftInit() {
    for (int i = 0; i < FFT_N; i++) {
        hannW[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (FFT_N - 1));
        unsigned r = 0; for (int b = 0; b < 11; b++) if (i & (1 << b)) r |= 1 << (10 - b);
        brev[i] = (unsigned short)r;
    }
    for (int k = 0; k < FFT_N / 2; k++) { cosT[k] = cosf(6.2831853f * k / FFT_N); sinT[k] = -sinf(6.2831853f * k / FFT_N); }
}
static void bandsInit() {
    for (int i = 0; i <= cfg.cols; i++) edge[i] = 30.f * powf(16000.f / 30.f, (float)i / cfg.cols) * FFT_N / aRate;
    for (int i = 0; i < cfg.cols; i++) {
        centerHz[i] = 30.f * powf(16000.f / 30.f, (i + 0.5f) / cfg.cols);
        tilt[i] = 3.f * log2f(centerHz[i] / 1000.f);  // lift the highs so they read as loud as the bass
    }
}
static void fft() {
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1, step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len)
            for (int j = 0; j < half; j++) {
                float wr = cosT[j * step], wi = sinT[j * step];
                int a = i + j, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
            }
    }
}
static void analyze(float dt, bool live) {
    float raw[MAXC];
    int C = cfg.cols;
    if (!live) { for (int c = 0; c < C; c++) raw[c] = 0; rmsDb = -99; }
    else {
        unsigned start = rpos - FFT_N; float ss = 0;
        for (int i = 0; i < FFT_N; i++) {
            float s = ring[(start + i) & (RING - 1)];
            ss += s * s; re[brev[i]] = s * hannW[i]; im[brev[i]] = 0;
        }
        fft();
        float rms = sqrtf(ss / FFT_N);
        rmsDb = rms > 1e-5f ? 20.f * log10f(rms) : -99;
        const float norm = 1.f / ((FFT_N / 4.f) * (FFT_N / 4.f));  // full-scale sine reads 0 dB
        float mx = -200;
        for (int c = 0; c < C; c++) {
            float b0 = edge[c], b1 = edge[c + 1], p;
            if (b1 - b0 < 1.f) {  // narrower than one FFT bin: interpolate
                float m = (b0 + b1) * 0.5f; int k = (int)m; float f = m - k;
                p = (re[k] * re[k] + im[k] * im[k]) * (1 - f) + (re[k + 1] * re[k + 1] + im[k + 1] * im[k + 1]) * f;
            } else {
                int k0 = (int)b0, k1 = (int)ceilf(b1); if (k1 > FFT_N / 2) k1 = FFT_N / 2;
                p = 0; for (int k = k0; k < k1; k++) p += re[k] * re[k] + im[k] * im[k];
                p /= (k1 - k0);
            }
            raw[c] = 10.f * log10f(p * norm + 1e-20f) + tilt[c];
            if (raw[c] > mx) mx = raw[c];
        }
        // automatic gain: follow the loudest band, fall 6 dB a second, never below -60 dB
        peakDb = mx > peakDb - 6.f * dt ? mx : peakDb - 6.f * dt;
        if (peakDb < -60) peakDb = -60;
        for (int c = 0; c < C; c++) raw[c] = clampf((raw[c] - (peakDb - 42.f)) / 42.f, 0, 1);
    }
    for (int c = 0; c < C; c++) {  // fast rise, slow fall
        float k = dt / (raw[c] > lv[c] ? 0.008f : 0.25f);
        lv[c] += (raw[c] - lv[c]) * (k > 1 ? 1 : k);
        pk[c] = lv[c] > pk[c] - dt * 0.35f ? lv[c] : pk[c] - dt * 0.35f;
    }
    int nl = 0; float low = 0;
    for (int c = 0; c < C && centerHz[c] < 120.f; c++) { low += lv[c]; nl++; }
    if (nl) low /= nl;
    sinceBeat += dt;
    if (low - prevLow > 0.08f && sinceBeat > 0.25f) {
        if (sinceBeat < 2.f) {
            beatIv[beatN++ & 7] = sinceBeat;
            int n = beatN < 8 ? beatN : 8; float s[8];
            for (int i = 0; i < n; i++) s[i] = beatIv[i];
            for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j] < s[j - 1]; j--) { float t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
            float b = 60.f / s[n / 2];
            while (b > 180) b *= 0.5f; while (b < 70) b *= 2;
            bpm = b;
        }
        sinceBeat = 0; pulse = 1; glitch = 1;
    }
    prevLow = low;
    pulse -= dt / 0.15f; if (pulse < 0) pulse = 0;
    glitch -= dt / 0.09f; if (glitch < 0) glitch = 0;
}

// ---------------------------------------------------------------- 5x7 pixel font
static const char GLYPHS[] = " 0123456789BDEGHIKLMNPSVZ.-/\x01\x02";
static const unsigned char FONT[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0, 0, 0, 0, 0, 0x0C, 0x0C}, {0, 0, 0, 0x0E, 0, 0, 0},
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
    {0, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0}, {0, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0},
};
static int setText(unsigned* out, const char* s) {
    int n = 0;
    for (; *s && n < 32; s++, n++) { const char* p = strchr(GLYPHS, *s); out[n] = p ? (unsigned)(p - GLYPHS) : 0; }
    for (int i = n; i < 32; i++) out[i] = 0;
    return n;
}

// ---------------------------------------------------------------- drawing (one pixel shader)
struct CB {
    float lv[MAXC], pk[MAXC];
    float accent[4];  // rgb, master opacity
    float gcol[4];    // glitch rgb, glitch amount
    float dims[4];    // width, height, bars top, bars height
    float grid[4];    // columns, rows, dpi scale, jitter px
    float txt[4];     // left text x, right text x, text y, font pixel size
    float flags[4];   // scanlines, brackets, peaks, readouts
    float fx[4];      // glitch line y, beat pulse
    unsigned tl[32], tr[32];
    unsigned font[320];
};
static CB cb;
static const char SHADER[] =
    "cbuffer C:register(b0){float4 lv[24];float4 pk[24];float4 accent;float4 gcol;float4 dims;float4 grid;"
    "float4 txt;float4 flags;float4 fx;uint4 tl[8];uint4 tr[8];uint4 font[80];};\n"
    "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==1?3.0:-1.0,id==2?-3.0:1.0,0,1);}\n"
    "float textAt(float2 p,float x0,bool right){float s=txt.w;float lx=p.x-x0,ly=p.y-txt.z;\n"
    " if(lx<0||ly<0||ly>=7*s)return 0;int ci=(int)(lx/(6*s));if(ci>=32)return 0;\n"
    " int cx=(int)((lx-ci*6*s)/s),cy=(int)(ly/s);if(cx>4)return 0;\n"
    " uint g=right?tr[ci>>2][ci&3]:tl[ci>>2][ci&3];uint i=g*8+cy;uint bits=font[i>>2][i&3];\n"
    " return (float)((bits>>(4-cx))&1);}\n"
    "float4 PS(float4 pos:SV_Position):SV_Target{float2 p=pos.xy;float W=dims.x,bt=dims.z,bh=dims.w,S=grid.z;\n"
    " float a=0;float3 col=accent.rgb;float cols=grid.x,rows=grid.y;\n"
    " if(p.y>=bt&&p.y<bt+bh){float cw=W/cols;float c=min(cols-1,floor(p.x/cw));\n"
    "  float lx=p.x-c*cw-(fmod(c,7)==3?grid.w:0);float rh=bh/rows;float ly=bt+bh-p.y;\n"
    "  float r=min(rows-1,floor(ly/rh));float ry=ly-r*rh;float gx=max(1,floor(2*S)),gy=max(1,floor(S));\n"
    "  if(lx>=gx&&lx<cw-gx&&ry>=gy&&ry<rh-gy){uint ci=(uint)c;float v=lv[ci>>2][ci&3],pv=pk[ci>>2][ci&3];\n"
    "   float lit=round(v*rows),pr=min(rows-1,round(pv*rows));\n"
    "   if(r<lit)a=(0.35+0.6*r/rows)*(1+0.2*fx.y);\n"
    "   else if(flags.z>0&&r==pr&&pv>0.04){float e=max(1,floor(S));\n"
    "    a=(lx<gx+e||lx>=cw-gx-e||ry<gy+e||ry>=rh-gy-e)?0.9:0.0;}\n"
    "   else a=0.04;}\n"
    "  if(flags.x>0&&fmod(floor(p.y),3)==0)a*=0.55;}\n"
    " if(flags.y>0){float t=bt-4*S,L=14*S,m=4*S,th=max(1,floor(S));\n"
    "  bool hl=p.y>=t&&p.y<t+th&&((p.x>=m&&p.x<m+L)||(p.x>=W-m-L&&p.x<W-m));\n"
    "  bool vl=p.y>=t&&p.y<t+L&&((p.x>=m&&p.x<m+th)||(p.x>=W-m-th&&p.x<W-m));\n"
    "  if(hl||vl)a=max(a,0.5);}\n"
    " if(flags.w>0){a=max(a,0.85*textAt(p,txt.x,false));a=max(a,0.55*textAt(p,txt.y,true));}\n"
    " if(gcol.a>0.3&&floor(p.y)==floor(fx.x)){col=gcol.rgb;a=max(a,0.5*gcol.a);}\n"
    " a=saturate(a)*accent.a;return float4(col*a,a);}\n";

static HWND hwnd;
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static IDXGISwapChain1* sc;
static ID3D11RenderTargetView* rtv;
static ID3D11Buffer* cbuf;
static IDCompositionDevice* dcomp;
static IDCompositionTarget* dtarget;
static IDCompositionVisual* dvisual;
static int winW, winH;
static float dpiS = 1, fontPx = 2, barsTop, barsH;

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static void makeTarget() {
    ID3D11Texture2D* bb = 0;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    dev->CreateRenderTargetView(bb, 0, &rtv);
    bb->Release();
    D3D11_VIEWPORT vp = {0, 0, (float)winW, (float)winH, 0, 1};
    ctx->RSSetViewports(1, &vp);
}
static void layout() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    dpiS = GetDpiForSystem() / 96.f;
    fontPx = floorf(1.5f * dpiS + 0.5f); if (fontPx < 1) fontPx = 1;
    barsH = floorf((mi.rcMonitor.bottom - mi.rcMonitor.top) * cfg.heightPct / 100.f);
    float pad = floorf(2 * dpiS), gap = floorf(7 * dpiS);
    barsTop = pad + 7 * fontPx + gap;
    int w = mi.rcWork.right - mi.rcWork.left, h = (int)(barsTop + barsH + pad);
    int y = cfg.top ? mi.rcWork.top : mi.rcWork.bottom - h;
    SetWindowPos(hwnd, HWND_TOPMOST, mi.rcWork.left, y, w, h, SWP_NOACTIVATE);
    if (sc && (w != winW || h != winH)) {
        winW = w; winH = h;
        rel(rtv);
        sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        makeTarget();
    }
    winW = w; winH = h;
}
static bool gfxInit() {
    UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, fl, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx)) &&
        FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, fl, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx))) return false;
    IDXGIDevice1* xd = 0; IDXGIAdapter* ad = 0; IDXGIFactory2* fac = 0;
    dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&xd);
    xd->SetMaximumFrameLatency(1);
    xd->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = winW; d.Height = winH; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = fac->CreateSwapChainForComposition(dev, &d, 0, &sc);
    fac->Release(); ad->Release();
    if (FAILED(hr)) { xd->Release(); return false; }
    // the Windows compositor blends the transparent strip on the GPU
    hr = DCompositionCreateDevice(xd, __uuidof(IDCompositionDevice), (void**)&dcomp);
    xd->Release();
    if (FAILED(hr)) return false;
    dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget);
    dcomp->CreateVisual(&dvisual);
    dvisual->SetContent(sc);
    dtarget->SetRoot(dvisual);
    dcomp->Commit();
    makeTarget();

    // compile the shader once, then unload the compiler to give its memory back
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!comp) return false;
    D3DCompileFn compile = (D3DCompileFn)(void*)GetProcAddress(comp, "D3DCompile");
    ID3DBlob *vb = 0, *pb = 0;
    ID3D11VertexShader* vs = 0; ID3D11PixelShader* ps = 0;
    bool ok = compile && SUCCEEDED(compile(SHADER, sizeof(SHADER) - 1, 0, 0, 0, "VS", "vs_4_0", 0, 0, &vb, 0)) &&
              SUCCEEDED(compile(SHADER, sizeof(SHADER) - 1, 0, 0, 0, "PS", "ps_4_0", 0, 0, &pb, 0)) &&
              SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &ps));
    rel(vb); rel(pb);
    FreeLibrary(comp);
    if (!ok) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, 0, &cbuf);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState* rs = 0;
    dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs); rs->Release();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(ps, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    vs->Release(); ps->Release();
    for (int g = 0; g < (int)(sizeof(FONT) / 7); g++) for (int r = 0; r < 7; r++) cb.font[g * 8 + r] = FONT[g][r];
    return true;
}
static void draw(float alpha, double now) {
    static double lastText = -1; static int rightLen;
    if (now - lastText > 0.25) {  // refresh the readouts four times a second
        lastText = now;
        setText(cb.tl, fmod(now, 1.0) < 0.5 ? "\x01 MVIZ//SEG" : "\x02 MVIZ//SEG");
        int best = 0; for (int c = 1; c < cfg.cols; c++) if (lv[c] > lv[best]) best = c;
        char b[64], f[16], bp[8];
        float hz = centerHz[best];
        if (hz < 1000) snprintf(f, 16, "%d HZ", (int)(hz + 0.5f)); else snprintf(f, 16, "%.1f KHZ", hz / 1000);
        if (bpm > 0) snprintf(bp, 8, "%03d", (int)(bpm + 0.5f)); else strcpy(bp, "---");
        snprintf(b, 64, "BPM %s  PK %s  LVL %.0f DB", bp, f, rmsDb < -99 ? -99.f : rmsDb);
        rightLen = setText(cb.tr, b);
    }
    for (int c = 0; c < MAXC; c++) { cb.lv[c] = c < cfg.cols ? lv[c] : 0; cb.pk[c] = c < cfg.cols ? pk[c] : 0; }
    memcpy(cb.accent, cfg.col, 12); cb.accent[3] = alpha;
    memcpy(cb.gcol, cfg.gcol, 12); cb.gcol[3] = cfg.glitch ? glitch : 0;
    cb.dims[0] = (float)winW; cb.dims[1] = (float)winH; cb.dims[2] = barsTop; cb.dims[3] = barsH;
    cb.grid[0] = (float)cfg.cols; cb.grid[1] = (float)cfg.rows; cb.grid[2] = dpiS;
    cb.grid[3] = (cfg.glitch && glitch > 0.3f) ? floorf(((rand() % 7) - 3) * glitch * dpiS) : 0;
    cb.txt[0] = floorf(12 * dpiS); cb.txt[1] = winW - floorf(12 * dpiS) - rightLen * 6 * fontPx;
    cb.txt[2] = floorf(2 * dpiS); cb.txt[3] = fontPx;
    cb.flags[0] = (float)cfg.scan; cb.flags[1] = (float)cfg.brackets; cb.flags[2] = (float)cfg.peaks; cb.flags[3] = (float)cfg.readouts;
    cb.fx[0] = barsTop + barsH * (0.2f + 0.6f * (rand() % 100) / 100.f); cb.fx[1] = pulse;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(cbuf, 0); }
    ctx->OMSetRenderTargets(1, &rtv, 0);
    ctx->Draw(3, 0);
    sc->Present(1, 0);
}

// ---------------------------------------------------------------- tray icon and window
static NOTIFYICONDATAW nid;
static bool running = true, cfgDirty;
static const wchar_t* PRESET_NAME[] = {L"Cyan", L"Hot pink", L"Acid yellow", L"White", L"Red", L"Green"};
static const wchar_t* PRESET_HEX[] = {L"00F0FF", L"FF2A6D", L"E8FF00", L"FFFFFF", L"FF3030", L"39FF14"};

static HICON makeIcon() {
    unsigned px[256] = {};
    const int hts[4] = {6, 11, 8, 13};
    for (int b = 0; b < 4; b++) for (int y = 0; y < hts[b]; y++) for (int x = 0; x < 3; x++)
        px[(14 - y) * 16 + 1 + b * 4 + x] = 0xFF00F0FF;
    unsigned char mask[32] = {};
    ICONINFO ii = {}; ii.fIcon = TRUE;
    ii.hbmColor = CreateBitmap(16, 16, 1, 32, px); ii.hbmMask = CreateBitmap(16, 16, 1, 1, mask);
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmColor); DeleteObject(ii.hbmMask);
    return ic;
}
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            HMENU m = CreatePopupMenu();
            for (int i = 0; i < 6; i++) AppendMenuW(m, MF_STRING, 100 + i, PRESET_NAME[i]);
            AppendMenuW(m, MF_SEPARATOR, 0, 0);
            AppendMenuW(m, MF_STRING, 2, L"Edit settings...");
            AppendMenuW(m, MF_STRING, 1, L"Quit");
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN, p.x, p.y, 0, h, 0);
            DestroyMenu(m);
            if (cmd == 1) running = false;
            else if (cmd == 2) ShellExecuteW(0, L"open", L"notepad.exe", iniPath, 0, SW_SHOWNORMAL);
            else if (cmd >= 100) { WritePrivateProfileStringW(L"visualizer", L"color", PRESET_HEX[cmd - 100], iniPath); cfgDirty = true; }
        }
        return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: case WM_DPICHANGED:
        cfgDirty = true; return 0;
    case WM_DESTROY: running = false; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static bool fullscreenInFront() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd || fg == GetShellWindow() || fg == GetDesktopWindow()) return false;
    wchar_t cls[32]; GetClassNameW(fg, cls, 32);
    if (!lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"Shell_TrayWnd")) return false;
    RECT r; MONITORINFO mi = {sizeof(mi)};
    GetWindowRect(fg, &r);
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
static double nowSec() {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    CreateMutexW(0, TRUE, L"MusicVisualizer.single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GetModuleFileNameW(0, iniPath, MAX_PATH);
    wchar_t* slash = wcsrchr(iniPath, L'\\');
    lstrcpyW(slash ? slash + 1 : iniPath, L"settings.ini");
    loadCfg();

    WNDCLASSW wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"MusicVisualizer";
    RegisterClassW(&wc);
    // click-through, always on top, never takes focus, no taskbar button
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, L"Music Visualizer", WS_POPUP, 0, 0, 100, 100, 0, 0, inst, 0);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    layout();
    if (!gfxInit()) { MessageBoxW(0, L"Could not start the graphics device.", L"Music Visualizer", MB_ICONERROR); return 1; }

    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enm);
    if (enm) audioOpen();
    fftInit(); bandsInit();

    nid.cbSize = sizeof(nid); nid.hWnd = hwnd; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY; nid.hIcon = makeIcon();
    lstrcpyW(nid.szTip, L"Music Visualizer");
    Shell_NotifyIconW(NIM_ADD, &nid);

    HANDLE timer = CreateWaitableTimerExW(0, 0, 2 /*high resolution*/, TIMER_ALL_ACCESS);
    bool active = false, shown = false, hiddenFs = false;
    float vis = 0;
    double last = nowSec(), lastHouse = 0;
    int houseN = 0;

    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        double t = nowSec();
        if (t - lastHouse > 1.0) {  // once a second: settings file, audio device, fullscreen check
            lastHouse = t; houseN++;
            FILETIME ft = iniStamp();
            if (CompareFileTime(&ft, &iniTime)) cfgDirty = true;
            if (enm && (!ac || ((houseN & 1) && defaultDeviceChanged()))) { int r = aRate; audioOpen(); if (aRate != r) cfgDirty = true; }
            hiddenFs = cfg.hideFs && fullscreenInFront();
        }
        if (cfgDirty) { cfgDirty = false; loadCfg(); layout(); bandsInit(); }
        audioRead();
        ULONGLONG tick = GetTickCount64();
        bool live = tick - lastPacket < 150;                    // loopback sends nothing while nothing plays
        float quiet = live ? (tick - lastLoud) / 1000.f : 9;
        bool want = quiet < 1.5f && !hiddenFs;

        if (!active) {
            if (!want) { MsgWaitForMultipleObjects(0, 0, FALSE, 250, QS_ALLINPUT); continue; }  // asleep: 4 wake-ups a second
            active = true; last = nowSec();
            if (!shown) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); shown = true; }
        }
        t = nowSec();
        float dt = (float)(t - last); last = t;
        if (dt > 0.1f) dt = 0.1f;
        analyze(dt, live);
        float k = dt / (want ? 0.1f : (hiddenFs ? 0.05f : 0.6f));
        vis += ((want ? 1.f : 0.f) - vis) * (k > 1 ? 1 : k);
        if (!want && vis < 0.004f) {  // faded out: draw one empty frame, then stop drawing
            vis = 0; draw(0, t); active = false;
            memset(lv, 0, sizeof(lv)); memset(pk, 0, sizeof(pk));
            continue;
        }
        draw(vis * cfg.opacity, t);
        // Present waits for the screen refresh; this tops the wait up to the fps setting
        double left = 1.0 / cfg.fps - (nowSec() - t) - 0.0015;
        if (left > 0.001 && timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(left * 1e7);
            SetWaitableTimer(timer, &due, 0, 0, 0, FALSE);
            WaitForSingleObject(timer, 100);
        }
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    audioClose();
    return 0;
}
