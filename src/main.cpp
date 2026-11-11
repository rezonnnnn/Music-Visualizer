// Music Visualizer: a tiny desktop widget that draws your speaker output as neon segments.
// One thread, no runtime. This file is the Windows side (audio capture, drawing, window, settings);
// the signal analysis is in analysis.h. See README.md.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
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
#include "analysis.h"

#define APP_VERSION L"0.3.0"
#define MAXC mv::MAXC
#define WM_TRAY (WM_APP + 1)
#define WM_OPENSETTINGS (WM_APP + 2)  // sent by a second launch: "open your settings window"
#define WM_REBUILDGFX (WM_APP + 3)    // used by the tests: behave as if the graphics device was lost
template <class T> static void rel(T*& p) { if (p) { p->Release(); p = 0; } }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static mv::Analyzer an;

// ---------------------------------------------------------------- settings
struct Cfg {
    float col[3], gcol[3];
    int cols, rows, opacity, fps, scan, readouts, brackets, glitch, peaks, hideFs, onTop, locked;
    int x, y, w, h;  // window rectangle in pixels; w <= 0 means "pick a default spot"
};
static Cfg cfg;
static wchar_t iniPath[MAX_PATH + 32];
static FILETIME iniTime;

static const char DEFAULT_INI[] =
    "; Music Visualizer settings. Use the Settings window (tray icon), or edit this file:\r\n"
    "; changes apply within a second of saving.\r\n"
    "[visualizer]\r\n"
    "color=00F0FF\r\n"
    "glitch_color=FF2A6D\r\n"
    "opacity=50\r\n"
    "columns=48\r\n"
    "rows=10\r\n"
    "fps=60\r\n"
    "scanlines=0\r\n"
    "readouts=1\r\n"
    "brackets=1\r\n"
    "peaks=1\r\n"
    "glitch=1\r\n"
    "hide_on_fullscreen=1\r\n"
    "always_on_top=0\r\n"
    "locked=0\r\n";

// Portable if a settings.ini sits next to the .exe; otherwise settings live in the user's AppData folder,
// which is always writable (a Program Files folder is not).
static void findIniPath() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(0, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    lstrcpyW(iniPath, dir); lstrcatW(iniPath, L"settings.ini");
    if (GetFileAttributesW(iniPath) != INVALID_FILE_ATTRIBUTES) return;
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH - 40);
    if (!n || n >= MAX_PATH - 40) return;  // no AppData: fall back to the folder of the .exe
    lstrcatW(dir, L"\\MusicVisualizer");
    CreateDirectoryW(dir, 0);
    lstrcpyW(iniPath, dir); lstrcatW(iniPath, L"\\settings.ini");
}
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
    cfg.opacity = iniInt(L"opacity", 50, 10, 100);
    cfg.cols = iniInt(L"columns", 48, 8, MAXC);
    cfg.rows = iniInt(L"rows", 10, 4, 16);
    cfg.fps = iniInt(L"fps", 60, 15, 60);
    cfg.scan = iniInt(L"scanlines", 0, 0, 1);
    cfg.readouts = iniInt(L"readouts", 1, 0, 1);
    cfg.brackets = iniInt(L"brackets", 1, 0, 1);
    cfg.peaks = iniInt(L"peaks", 1, 0, 1);
    cfg.glitch = iniInt(L"glitch", 1, 0, 1);
    cfg.hideFs = iniInt(L"hide_on_fullscreen", 1, 0, 1);
    cfg.onTop = iniInt(L"always_on_top", 0, 0, 1);
    cfg.locked = iniInt(L"locked", 0, 0, 1);
    cfg.x = iniInt(L"x", 0, -32000, 32000); cfg.y = iniInt(L"y", 0, -32000, 32000);
    cfg.w = iniInt(L"w", 0, 0, 16000); cfg.h = iniInt(L"h", 0, 0, 16000);
    iniTime = iniStamp();
}
static void putInt(const wchar_t* k, int v) {
    wchar_t b[16]; wsprintfW(b, L"%d", v);
    WritePrivateProfileStringW(L"visualizer", k, b, iniPath);
}
static void putColor(const wchar_t* k, const float* c) {
    wchar_t b[16];
    wsprintfW(b, L"%02X%02X%02X", (int)(c[0] * 255 + .5f), (int)(c[1] * 255 + .5f), (int)(c[2] * 255 + .5f));
    WritePrivateProfileStringW(L"visualizer", k, b, iniPath);
}
static void saveCfg() {
    putColor(L"color", cfg.col); putColor(L"glitch_color", cfg.gcol);
    putInt(L"opacity", cfg.opacity); putInt(L"columns", cfg.cols); putInt(L"rows", cfg.rows); putInt(L"fps", cfg.fps);
    putInt(L"scanlines", cfg.scan); putInt(L"readouts", cfg.readouts); putInt(L"brackets", cfg.brackets);
    putInt(L"peaks", cfg.peaks); putInt(L"glitch", cfg.glitch); putInt(L"hide_on_fullscreen", cfg.hideFs);
    putInt(L"always_on_top", cfg.onTop); putInt(L"locked", cfg.locked);
    putInt(L"x", cfg.x); putInt(L"y", cfg.y); putInt(L"w", cfg.w); putInt(L"h", cfg.h);
    iniTime = iniStamp();  // our own write is not an outside edit
}

// "Start with Windows" is the standard per-user Run entry; the registry is the single source of truth for it.
static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool autostartOn() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"MusicVisualizer", RRF_RT_REG_SZ, 0, 0, 0) == ERROR_SUCCESS;
}
static void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t v[MAX_PATH + 4] = L"\"";
        GetModuleFileNameW(0, v + 1, MAX_PATH);
        lstrcatW(v, L"\"");
        RegSetValueExW(k, L"MusicVisualizer", 0, REG_SZ, (const BYTE*)v, (lstrlenW(v) + 1) * sizeof(wchar_t));
    } else RegDeleteValueW(k, L"MusicVisualizer");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- audio (speaker loopback)
static IMMDeviceEnumerator* enm;
static IAudioClient* ac;
static IAudioCaptureClient* cc;
static int aFloat, aChans, aBits, aRate;
static wchar_t devId[300];
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
    aChans = wf->nChannels; aBits = wf->wBitsPerSample;
    int rate = (int)wf->nSamplesPerSec;
    // 1 second buffer, so the idle loop can poll four times a second without losing audio
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wf, 0);
    CoTaskMemFree(wf);
    if (FAILED(hr) || FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)) || FAILED(ac->Start())) {
        audioClose(); return false;
    }
    if (rate != aRate) { aRate = rate; an.init(cfg.cols, aRate); }  // a different device can run at a different rate
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
    static float mono[1024];
    while (SUCCEEDED(hr = cc->GetNextPacketSize(&n)) && n) {
        BYTE* d; UINT32 fr; DWORD fl;
        if (FAILED(cc->GetBuffer(&d, &fr, &fl, 0, 0))) break;
        float pk = 0;
        bool usable = !(fl & AUDCLNT_BUFFERFLAGS_SILENT) && ((aFloat && aBits == 32) || (!aFloat && aBits == 16));
        for (UINT32 i = 0; i < fr;) {  // mix down to mono in blocks and hand them to the analyzer
            int m = 0;
            for (; i < fr && m < 1024; i++, m++) {
                float s = 0;
                if (usable) {
                    if (aFloat) { const float* p = (const float*)d + (size_t)i * aChans; for (int c = 0; c < aChans; c++) s += p[c]; }
                    else { const short* p = (const short*)d + (size_t)i * aChans; for (int c = 0; c < aChans; c++) s += p[c] / 32768.f; }
                    s /= aChans;
                }
                mono[m] = s;
                float am = fabsf(s); if (am > pk) pk = am;
            }
            an.feed(mono, m);
        }
        cc->ReleaseBuffer(fr);
        lastPacket = GetTickCount64();
        if (pk > 1e-4f) lastLoud = lastPacket;
    }
    if (FAILED(hr)) audioClose();  // device went away; the once-a-second check reopens it
}

// ---------------------------------------------------------------- 5x7 pixel font
static const char GLYPHS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.-/\x01\x02";
static const unsigned char FONT[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 0 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 2 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 4 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 6 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 8 9
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // A B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},  // C D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // E F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // G H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // I J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // K L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // M N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // O P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // Q R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // S T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // U V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // W X
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Y Z
    {0, 0, 0, 0, 0, 0x0C, 0x0C}, {0, 0, 0, 0x0E, 0, 0, 0},                                   // . -
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},                                              // /
    {0, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0}, {0, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0},              // filled and hollow square
};
#define NGLYPH ((int)(sizeof(FONT) / 7))
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
    float flags[4];   // scanlines, brackets, peaks, text
    float fx[4];      // glitch line y, unused, unlocked
    unsigned tl[32], tr[32];
    unsigned font[352];
};
static CB cb;
// Cells and outlines are solid: every lit pixel has the same strength, and the opacity setting scales the lot.
static const char SHADER[] =
    "cbuffer C:register(b0){float4 lv[24];float4 pk[24];float4 accent;float4 gcol;float4 dims;float4 grid;"
    "float4 txt;float4 flags;float4 fx;uint4 tl[8];uint4 tr[8];uint4 font[88];};\n"
    "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==1?3.0:-1.0,id==2?-3.0:1.0,0,1);}\n"
    "float textAt(float2 p,float x0,bool right){float s=txt.w;float lx=p.x-x0,ly=p.y-txt.z;\n"
    " if(lx<0||ly<0||ly>=7*s)return 0;int ci=(int)(lx/(6*s));if(ci>=32)return 0;\n"
    " int cx=(int)((lx-ci*6*s)/s),cy=(int)(ly/s);if(cx>4)return 0;\n"
    " uint g=right?tr[ci>>2][ci&3]:tl[ci>>2][ci&3];uint i=g*8+cy;uint bits=font[i>>2][i&3];\n"
    " return (float)((bits>>(4-cx))&1);}\n"
    "float4 PS(float4 pos:SV_Position):SV_Target{float2 p=pos.xy;float W=dims.x,H=dims.y,bt=dims.z,bh=dims.w,S=grid.z;\n"
    " float a=0;float3 col=accent.rgb;float cols=grid.x,rows=grid.y;float th=max(1,floor(S));\n"
    " if(p.y>=bt&&p.y<bt+bh){float cw=W/cols;float c=min(cols-1,floor(p.x/cw));\n"
    "  float lx=p.x-c*cw-(fmod(c,7)==3?grid.w:0);float rh=bh/rows;float ly=bt+bh-p.y;\n"
    "  float r=min(rows-1,floor(ly/rh));float ry=ly-r*rh;float gx=max(1,floor(2*S)),gy=max(1,floor(S));\n"
    "  if(lx>=gx&&lx<cw-gx&&ry>=gy&&ry<rh-gy){uint ci=(uint)c;float v=lv[ci>>2][ci&3],pv=pk[ci>>2][ci&3];\n"
    "   float lit=round(v*rows),pr=min(rows-1,round(pv*rows));\n"
    "   if(r<lit)a=1;\n"
    "   else if(flags.z>0&&r==pr&&pv>0.04)a=(lx<gx+th||lx>=cw-gx-th||ry<gy+th||ry>=rh-gy-th)?1.0:0.0;\n"
    "   else a=0.06;}\n"
    "  if(flags.x>0&&fmod(floor(p.y),3)==0)a*=0.55;}\n"
    " if(flags.y>0){float t=bt-4*S,L=14*S,m=4*S;\n"
    "  bool hl=p.y>=t&&p.y<t+th&&((p.x>=m&&p.x<m+L)||(p.x>=W-m-L&&p.x<W-m));\n"
    "  bool vl=p.y>=t&&p.y<t+L&&((p.x>=m&&p.x<m+th)||(p.x>=W-m-th&&p.x<W-m));\n"
    "  if(hl||vl)a=1;}\n"
    " if(flags.w>0){a=max(a,textAt(p,txt.x,false));a=max(a,0.7*textAt(p,txt.y,true));}\n"
    " if(fx.z>0){bool edge=p.x<th||p.x>=W-th||p.y<th||p.y>=H-th;a=max(a,edge?1.0:0.15);}\n"
    " if(gcol.a>0.3&&floor(p.y)==floor(fx.x)){col=gcol.rgb;a=max(a,gcol.a);}\n"
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
static int winW = 1, winH = 1;
static float dpiS = 1, fontPx = 2, barsTop, barsH;
static bool forceDraw = true, gfxLost;

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static void makeTarget() {
    ID3D11Texture2D* bb = 0;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { gfxLost = true; return; }
    if (FAILED(dev->CreateRenderTargetView(bb, 0, &rtv))) gfxLost = true;
    bb->Release();
    D3D11_VIEWPORT vp = {0, 0, (float)winW, (float)winH, 0, 1};
    ctx->RSSetViewports(1, &vp);
}
// Fits the picture to the window: a text row on top, the bars filling the rest.
static void layout() {
    RECT r; GetClientRect(hwnd, &r);
    int w = r.right > 1 ? r.right : 1, h = r.bottom > 1 ? r.bottom : 1;
    dpiS = GetDpiForWindow(hwnd) / 96.f;
    fontPx = floorf(1.5f * dpiS + 0.5f); if (fontPx < 1) fontPx = 1;
    float pad = floorf(3 * dpiS), gap = floorf(7 * dpiS);
    barsTop = pad + 7 * fontPx + gap;
    barsH = h - barsTop - pad; if (barsH < 4) barsH = 4;
    if (sc && (w != winW || h != winH)) {
        winW = w; winH = h;
        rel(rtv);
        if (FAILED(sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) gfxLost = true;
        else makeTarget();
    }
    winW = w; winH = h;
    forceDraw = true;
}
static void gfxShutdown() {
    if (ctx) ctx->ClearState();
    rel(rtv); rel(cbuf); rel(dvisual); rel(dtarget); rel(dcomp); rel(sc); rel(ctx); rel(dev);
}
static bool gfxInit() {
    UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, fl, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx)) &&
        FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, fl, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx))) return false;
    IDXGIDevice1* xd = 0; IDXGIAdapter* ad = 0; IDXGIFactory2* fac = 0;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&xd))) return false;
    xd->SetMaximumFrameLatency(1);
    HRESULT hr = xd->GetAdapter(&ad);
    if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = winW; d.Height = winH; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = fac->CreateSwapChainForComposition(dev, &d, 0, &sc);
    }
    // the Windows compositor blends the transparent widget on the GPU
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(xd, __uuidof(IDCompositionDevice), (void**)&dcomp);
    rel(fac); rel(ad); rel(xd);
    if (FAILED(hr)) return false;
    if (FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget)) || FAILED(dcomp->CreateVisual(&dvisual))) return false;
    dvisual->SetContent(sc);
    dtarget->SetRoot(dvisual);
    dcomp->Commit();
    gfxLost = false;
    makeTarget();
    if (gfxLost) return false;

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
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState* rs = 0;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, 0, &cbuf)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &rs));
    if (ok) {
        ctx->RSSetState(rs);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(ps, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
    }
    rel(rs); rel(vs); rel(ps);
    for (int g = 0; g < NGLYPH; g++) for (int r = 0; r < 7; r++) cb.font[g * 8 + r] = FONT[g][r];
    forceDraw = true;
    return ok;
}
static void draw(float alpha, double now) {
    static double lastText = -1; static int rightLen; static int lastLocked = -1;
    if (now - lastText > 0.25 || lastLocked != cfg.locked) {  // refresh the text four times a second
        lastText = now; lastLocked = cfg.locked;
        if (!cfg.locked) {
            setText(cb.tl, "UNLOCKED - DRAG TO MOVE");
            rightLen = setText(cb.tr, "DOUBLE-CLICK TO LOCK");
        } else {
            setText(cb.tl, fmod(now, 1.0) < 0.5 ? "\x01 MVIZ//SEG" : "\x02 MVIZ//SEG");
            int best = 0; for (int c = 1; c < cfg.cols; c++) if (an.lv[c] > an.lv[best]) best = c;
            char b[64], f[16], bp[8];
            float hz = an.centerHz[best];
            if (hz < 1000) snprintf(f, 16, "%d HZ", (int)(hz + 0.5f)); else snprintf(f, 16, "%.1f KHZ", hz / 1000);
            if (an.bpm > 0) snprintf(bp, 8, "%03d", (int)(an.bpm + 0.5f)); else strcpy(bp, "---");
            snprintf(b, 64, "BPM %s  PK %s  LVL %.0f DB", bp, f, an.rmsDb < -99 ? -99.f : an.rmsDb);
            rightLen = setText(cb.tr, b);
        }
    }
    // Everything is snapped to the steps the picture can actually show (whole cells, 1/255 opacity),
    // so an unchanged picture gives identical bytes and the frame is skipped.
    float R = (float)cfg.rows;
    bool gl = cfg.glitch && an.glitch > 0.3f;
    for (int c = 0; c < MAXC; c++) {
        bool on = c < cfg.cols;
        cb.lv[c] = on ? floorf(an.lv[c] * R + 0.5f) / R : 0;
        cb.pk[c] = (on && an.pk[c] > 0.04f) ? fmaxf(0.05f, floorf(an.pk[c] * R + 0.5f) / R) : 0;
    }
    memcpy(cb.accent, cfg.col, 12); cb.accent[3] = floorf(clampf(alpha, 0, 1) * 255 + 0.5f) / 255;
    memcpy(cb.gcol, cfg.gcol, 12); cb.gcol[3] = gl ? floorf(an.glitch * 8) / 8 : 0;
    cb.dims[0] = (float)winW; cb.dims[1] = (float)winH; cb.dims[2] = barsTop; cb.dims[3] = barsH;
    cb.grid[0] = (float)cfg.cols; cb.grid[1] = (float)cfg.rows; cb.grid[2] = dpiS;
    cb.grid[3] = gl ? floorf(((rand() % 7) - 3) * an.glitch * dpiS) : 0;
    cb.txt[0] = floorf(12 * dpiS); cb.txt[1] = winW - floorf(12 * dpiS) - rightLen * 6 * fontPx;
    cb.txt[2] = floorf(3 * dpiS); cb.txt[3] = fontPx;
    cb.flags[0] = (float)cfg.scan; cb.flags[1] = (float)cfg.brackets; cb.flags[2] = (float)cfg.peaks;
    cb.flags[3] = (cfg.readouts || !cfg.locked) ? 1.f : 0.f;
    cb.fx[0] = gl ? barsTop + barsH * (0.2f + 0.6f * (rand() % 100) / 100.f) : 0;
    cb.fx[2] = cfg.locked ? 0.f : 1.f;
    static CB shownCb;
    if (!forceDraw && !memcmp(&cb, &shownCb, sizeof(cb))) return;
    if (!rtv || !cbuf) return;
    shownCb = cb; forceDraw = false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { gfxLost = true; return; }
    memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(cbuf, 0);
    ctx->OMSetRenderTargets(1, &rtv, 0);
    ctx->Draw(3, 0);
    // a driver update, a GPU switch or waking from sleep can take the device away: it is rebuilt on the next frame
    HRESULT hr = sc->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) gfxLost = true;
}

// ---------------------------------------------------------------- the widget window: place, lock, menu
static NOTIFYICONDATAW nid;
static UINT msgTaskbarCreated;
static bool running = true, cfgDirty, layoutDirty, windowDirty;
static HWND setWnd;
static double tick();
static void openSettings();
static void syncSettings();

static HICON appIcon(int size) {
    return (HICON)LoadImageW(GetModuleHandleW(0), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0);
}
static void defaultSpot() {  // bottom centre of the main screen, above the taskbar
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    float s = GetDpiForSystem() / 96.f;
    int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
    cfg.w = (int)(760 * s) < ww ? (int)(760 * s) : ww; cfg.h = (int)(150 * s) < wh ? (int)(150 * s) : wh;
    cfg.x = mi.rcWork.left + (ww - cfg.w) / 2; cfg.y = mi.rcWork.bottom - cfg.h - (int)(12 * s);
    if (cfg.y < mi.rcWork.top) cfg.y = mi.rcWork.top;
}
// False when the saved spot is no longer on any screen (a monitor was unplugged or the layout changed).
static bool onScreen() {
    RECT r = {cfg.x, cfg.y, cfg.x + cfg.w, cfg.y + cfg.h}, hit;
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!m) return false;
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    return IntersectRect(&hit, &r, &mi.rcWork) && hit.right - hit.left >= 48 && hit.bottom - hit.top >= 48;
}
// Applies position, size, layer and lock state to the window.
static void applyWindow() {
    if (cfg.w <= 0 || cfg.h <= 0 || !onScreen()) defaultSpot();
    // locked: clicks pass through and it never takes focus. unlocked: it can be dragged and resized.
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex = cfg.locked ? (ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE) : (ex & ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    // while unlocked it floats on top so it can be found and placed; locked, it drops to the desktop
    bool top = cfg.onTop || !cfg.locked;
    SetWindowPos(hwnd, top ? HWND_TOPMOST : HWND_BOTTOM, cfg.x, cfg.y, cfg.w, cfg.h, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    layoutDirty = true;
}
static void setLocked(int v) {
    cfg.locked = v;
    applyWindow(); saveCfg(); syncSettings();
}
static void trayTip(const wchar_t* tip) {
    if (!lstrcmpW(nid.szTip, tip)) return;
    lstrcpynW(nid.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
static void showMenu(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (cfg.locked ? MF_CHECKED : 0), 3, L"Lock position");
    AppendMenuW(m, MF_STRING, 2, L"Settings...");
    AppendMenuW(m, MF_STRING | (autostartOn() ? MF_CHECKED : 0), 4, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING, 1, L"Quit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(h);
    SetTimer(h, 1, 16, 0);  // keeps the picture moving while the menu is open
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, 0);
    KillTimer(h, 1);
    DestroyMenu(m);
    if (cmd == 1) running = false;
    else if (cmd == 2) openSettings();
    else if (cmd == 3) setLocked(!cfg.locked);
    else if (cmd == 4) { setAutostart(!autostartOn()); syncSettings(); }
}
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == msgTaskbarCreated && msg) { Shell_NotifyIconW(NIM_ADD, &nid); return 0; }  // Explorer restarted: put the icon back
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) showMenu(h);
        return 0;
    case WM_OPENSETTINGS: openSettings(); return 0;
    case WM_REBUILDGFX: gfxLost = true; return 0;
    case WM_NCHITTEST: {  // unlocked: the whole widget is a drag handle, its edges resize
        if (cfg.locked) return HTTRANSPARENT;
        RECT r; GetWindowRect(h, &r);
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), b = (int)(8 * dpiS);
        bool L = x < r.left + b, R = x >= r.right - b, T = y < r.top + b, B = y >= r.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
        return HTCAPTION;
    }
    case WM_NCLBUTTONDBLCLK: setLocked(1); return 0;
    case WM_NCRBUTTONUP: showMenu(h); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = (LONG)(240 * dpiS); mm->ptMinTrackSize.y = (LONG)(70 * dpiS);
        return 0;
    }
    case WM_ENTERSIZEMOVE: SetTimer(h, 1, 16, 0); return 0;  // keep drawing while being dragged
    case WM_EXITSIZEMOVE: {
        KillTimer(h, 1);
        RECT r; GetWindowRect(h, &r);
        cfg.x = r.left; cfg.y = r.top; cfg.w = r.right - r.left; cfg.h = r.bottom - r.top;
        saveCfg();
        return 0;
    }
    case WM_TIMER: tick(); return 0;
    case WM_SIZE: layoutDirty = true; return 0;
    case WM_WINDOWPOSCHANGING:  // locked on the desktop: stay behind every other window
        if (cfg.locked && !cfg.onTop) { WINDOWPOS* p = (WINDOWPOS*)lp; p->hwndInsertAfter = HWND_BOTTOM; p->flags &= ~SWP_NOZORDER; }
        break;
    case WM_DISPLAYCHANGE: windowDirty = true; return 0;  // screens changed: make sure it is still on one
    case WM_DPICHANGED: layoutDirty = true; return 0;
    case WM_DESTROY: running = false; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- settings window
enum { ID_COL = 10, ID_GCOL, ID_SW_COL, ID_SW_GCOL, ID_OPA, ID_COLS, ID_ROWS, ID_FPS, ID_SCAN, ID_READ, ID_BRACK,
       ID_PEAK, ID_GLITCH, ID_HIDEFS, ID_ONTOP, ID_LOCK, ID_RESET, ID_CLOSE, ID_AUTOSTART, ID_VAL = 100 };
static HFONT uiFont;
static HBRUSH brCol, brGcol;
static int sx(int v) { return MulDiv(v, GetDpiForSystem(), 96); }
static HBRUSH brushOf(const float* c) { return CreateSolidBrush(RGB((int)(c[0] * 255 + .5f), (int)(c[1] * 255 + .5f), (int)(c[2] * 255 + .5f))); }
static HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, sx(x), sx(y), sx(w), sx(h), setWnd, (HMENU)(INT_PTR)id, GetModuleHandleW(0), 0);
    SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
    return c;
}
static void slider(const wchar_t* label, int y, int id, int lo, int hi, int val) {
    ctl(L"STATIC", label, 0, 16, y + 4, 92, 20, -1);
    HWND t = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 108, y, 186, 26, id);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(t, TBM_SETPOS, TRUE, val);
    wchar_t b[8]; wsprintfW(b, L"%d", val);
    ctl(L"STATIC", b, 0, 300, y + 4, 40, 20, ID_VAL + id);
}
static void check(const wchar_t* label, int x, int y, int id, int val) {
    HWND c = ctl(L"BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, 160, 22, id);
    SendMessageW(c, BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
}
static void syncSettings() {  // these can change from the menu or a double-click while the window is open
    if (!setWnd) return;
    CheckDlgButton(setWnd, ID_LOCK, cfg.locked ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(setWnd, ID_AUTOSTART, autostartOn() ? BST_CHECKED : BST_UNCHECKED);
}
static void pickColor(float* c, HBRUSH* br, int swatch) {
    typedef BOOL(WINAPI* ChooseFn)(CHOOSECOLORW*);
    static ChooseFn choose; static COLORREF custom[16];
    if (!choose) { HMODULE m = LoadLibraryW(L"comdlg32.dll"); if (m) choose = (ChooseFn)(void*)GetProcAddress(m, "ChooseColorW"); }
    if (!choose) return;
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = setWnd; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    cc.rgbResult = RGB((int)(c[0] * 255 + .5f), (int)(c[1] * 255 + .5f), (int)(c[2] * 255 + .5f));
    SetTimer(hwnd, 1, 16, 0);
    BOOL ok = choose(&cc);
    KillTimer(hwnd, 1);
    if (!ok || !setWnd) return;
    c[0] = GetRValue(cc.rgbResult) / 255.f; c[1] = GetGValue(cc.rgbResult) / 255.f; c[2] = GetBValue(cc.rgbResult) / 255.f;
    DeleteObject(*br); *br = brushOf(c);
    InvalidateRect(GetDlgItem(setWnd, swatch), 0, TRUE);
    saveCfg();
}
static LRESULT CALLBACK setProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)lp);
        if (id == ID_SW_COL) return (LRESULT)brCol;
        if (id == ID_SW_GCOL) return (LRESULT)brGcol;
        break;
    }
    case WM_HSCROLL: {  // sliders apply live and save when released
        HWND t = (HWND)lp; int id = GetDlgCtrlID(t), v = (int)SendMessageW(t, TBM_GETPOS, 0, 0);
        if (id == ID_OPA) cfg.opacity = v; else if (id == ID_COLS) cfg.cols = v;
        else if (id == ID_ROWS) cfg.rows = v; else if (id == ID_FPS) cfg.fps = v; else break;
        wchar_t b[8]; wsprintfW(b, L"%d", v);
        SetDlgItemTextW(h, ID_VAL + id, b);
        layoutDirty = true;
        if (LOWORD(wp) != TB_THUMBTRACK) saveCfg();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), on = IsDlgButtonChecked(h, id) == BST_CHECKED;
        switch (id) {
        case ID_COL: pickColor(cfg.col, &brCol, ID_SW_COL); return 0;
        case ID_GCOL: pickColor(cfg.gcol, &brGcol, ID_SW_GCOL); return 0;
        case ID_SCAN: cfg.scan = on; break;
        case ID_READ: cfg.readouts = on; break;
        case ID_BRACK: cfg.brackets = on; break;
        case ID_PEAK: cfg.peaks = on; break;
        case ID_GLITCH: cfg.glitch = on; break;
        case ID_HIDEFS: cfg.hideFs = on; break;
        case ID_ONTOP: cfg.onTop = on; applyWindow(); break;
        case ID_LOCK: setLocked(on); return 0;
        case ID_AUTOSTART: setAutostart(on); syncSettings(); return 0;
        case ID_RESET: cfg.w = 0; applyWindow(); break;
        case ID_CLOSE: case IDCANCEL: DestroyWindow(h); return 0;  // Esc closes too
        default: return 0;
        }
        saveCfg();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: setWnd = 0; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void openSettings() {
    if (setWnd) { ShowWindow(setWnd, SW_RESTORE); SetForegroundWindow(setWnd); return; }
    static bool once;
    HINSTANCE inst = GetModuleHandleW(0);
    if (!once) {
        once = true;
        // the slider control lives in comctl32; loaded only when the settings window is first opened
        typedef BOOL(WINAPI* InitFn)(const INITCOMMONCONTROLSEX*);
        HMODULE m = LoadLibraryW(L"comctl32.dll");
        InitFn init = m ? (InitFn)(void*)GetProcAddress(m, "InitCommonControlsEx") : 0;
        INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_BAR_CLASSES};
        if (init) init(&ic);
        NONCLIENTMETRICSW ncm = {sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
        WNDCLASSEXW wc = {sizeof(wc)}; wc.lpfnWndProc = setProc; wc.hInstance = inst; wc.lpszClassName = L"MusicVisualizerSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorW(0, IDC_ARROW);
        wc.hIcon = appIcon(GetSystemMetrics(SM_CXICON)); wc.hIconSm = appIcon(GetSystemMetrics(SM_CXSMICON));
        RegisterClassExW(&wc);
    }
    if (brCol) DeleteObject(brCol); if (brGcol) DeleteObject(brGcol);
    brCol = brushOf(cfg.col); brGcol = brushOf(cfg.gcol);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, sx(356), sx(446)};
    AdjustWindowRect(&r, style, FALSE);
    setWnd = CreateWindowExW(0, L"MusicVisualizerSettings", L"Music Visualizer settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    if (!setWnd) return;
    ctl(L"STATIC", L"Colour", 0, 16, 18, 92, 20, -1);
    ctl(L"STATIC", L"", 0, 112, 15, 60, 24, ID_SW_COL);
    ctl(L"BUTTON", L"Choose...", WS_TABSTOP, 182, 14, 90, 26, ID_COL);
    ctl(L"STATIC", L"Glitch colour", 0, 16, 50, 92, 20, -1);
    ctl(L"STATIC", L"", 0, 112, 47, 60, 24, ID_SW_GCOL);
    ctl(L"BUTTON", L"Choose...", WS_TABSTOP, 182, 46, 90, 26, ID_GCOL);
    slider(L"Opacity %", 86, ID_OPA, 10, 100, cfg.opacity);
    slider(L"Columns", 118, ID_COLS, 8, MAXC, cfg.cols);
    slider(L"Rows", 150, ID_ROWS, 4, 16, cfg.rows);
    slider(L"Frames / sec", 182, ID_FPS, 15, 60, cfg.fps);
    check(L"Lock position", 16, 222, ID_LOCK, cfg.locked);
    check(L"Always on top", 184, 222, ID_ONTOP, cfg.onTop);
    check(L"Readouts", 16, 248, ID_READ, cfg.readouts);
    check(L"Peak markers", 184, 248, ID_PEAK, cfg.peaks);
    check(L"Brackets", 16, 274, ID_BRACK, cfg.brackets);
    check(L"Beat glitch", 184, 274, ID_GLITCH, cfg.glitch);
    check(L"Scanlines", 16, 300, ID_SCAN, cfg.scan);
    check(L"Hide on fullscreen", 184, 300, ID_HIDEFS, cfg.hideFs);
    check(L"Start with Windows", 16, 326, ID_AUTOSTART, autostartOn());
    ctl(L"BUTTON", L"Reset position", WS_TABSTOP, 16, 362, 130, 28, ID_RESET);
    ctl(L"BUTTON", L"Close", WS_TABSTOP, 250, 362, 90, 28, ID_CLOSE);
    HWND note = ctl(L"STATIC", L"Version " APP_VERSION L". Sound is analysed on this PC only. Nothing is recorded or sent anywhere.", 0, 16, 402, 324, 36, -1);
    EnableWindow(note, FALSE);  // greyed: it is a footnote
    ShowWindow(setWnd, SW_SHOW);
    SetForegroundWindow(setWnd);
}

// ---------------------------------------------------------------- main loop
// The shell's own answer to "is a fullscreen app, game or presentation in front?"
static bool fullscreenInFront() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}
static double nowSec() {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}
// One frame of work. Returns how many seconds to wait before the next one, or -1 when asleep.
static double tick() {
    static bool active, shownWin, hiddenFs, tempoCleared;
    static float vis;
    static double last, lastHouse;
    static int houseN;
    double t = nowSec();
    if (t - lastHouse > 1.0) {  // once a second: settings file, audio device, fullscreen check, window layer
        lastHouse = t; houseN++;
        FILETIME ft = iniStamp();
        if (CompareFileTime(&ft, &iniTime)) cfgDirty = true;
        if (enm && (!ac || ((houseN & 1) && defaultDeviceChanged()))) { audioOpen(); layoutDirty = true; }
        trayTip(ac ? L"Music Visualizer" : L"Music Visualizer - no audio device found");
        hiddenFs = cfg.hideFs && fullscreenInFront();
        // Windows can drop or refuse the on-top layer (seen right after start-up): put it back if it is wrong
        bool wantTop = cfg.onTop || !cfg.locked, isTop = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (shownWin && wantTop != isTop) windowDirty = true;
        if (gfxLost) { gfxShutdown(); gfxInit(); }  // on failure gfxLost stays set and this retries next second
    }
    if (cfgDirty) { cfgDirty = false; windowDirty = true; loadCfg(); }
    if (windowDirty) { windowDirty = false; applyWindow(); }
    if (layoutDirty) { layoutDirty = false; an.setColumns(cfg.cols); if (!gfxLost) layout(); }
    audioRead();
    ULONGLONG tk = GetTickCount64();
    bool live = tk - lastPacket < 150;  // loopback sends nothing while nothing plays
    float quiet = live ? (tk - lastLoud) / 1000.f : 9;
    if (quiet < 2) tempoCleared = false;
    else if (!tempoCleared) { tempoCleared = true; an.resetTempo(); }  // the next song starts from a clean slate
    bool want = !cfg.locked || (quiet < 1.5f && !hiddenFs);  // unlocked is always visible so it can be placed

    if (!active) {
        if (!want || gfxLost) return -1;
        active = true; last = nowSec();
        if (!shownWin) {  // the layer set while hidden does not always stick, so set it again once shown
            ShowWindow(hwnd, SW_SHOWNOACTIVATE); shownWin = true;
            applyWindow();
        }
    }
    t = nowSec();
    float dt = (float)(t - last); last = t;
    if (dt > 0.1f) dt = 0.1f;
    an.frame(dt, live);
    float k = dt / (want ? 0.1f : (hiddenFs ? 0.05f : 0.6f));
    vis += ((want ? 1.f : 0.f) - vis) * (k > 1 ? 1 : k);
    if (gfxLost) return 0.25;
    if (!want && vis < 0.004f) {  // faded out: draw one empty frame, then stop drawing
        vis = 0; draw(0, t); active = false;
        memset(an.lv, 0, sizeof(an.lv)); memset(an.pk, 0, sizeof(an.pk));
        return -1;
    }
    draw(vis * cfg.opacity / 100.f, t);
    return 1.0 / cfg.fps - (nowSec() - t) - 0.0015;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    CreateMutexW(0, TRUE, L"MusicVisualizer.single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // already running: ask that copy to open its settings
        HWND other = FindWindowW(L"MusicVisualizer", 0);
        if (other) PostMessageW(other, WM_OPENSETTINGS, 0, 0);
        return 0;
    }
    findIniPath();
    loadCfg();
    if (autostartOn()) setAutostart(true);  // keeps the start-up entry pointing at this file if it was moved
    aRate = 48000; an.init(cfg.cols, aRate);

    WNDCLASSW wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"MusicVisualizer";
    wc.hCursor = LoadCursorW(0, IDC_SIZEALL); wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // no taskbar button; drawn by the compositor, so the window itself has no bitmap
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TOOLWINDOW | ((cfg.onTop || !cfg.locked) ? WS_EX_TOPMOST : 0),
                           wc.lpszClassName, L"Music Visualizer", WS_POPUP, 0, 0, 100, 100, 0, 0, inst, 0);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    applyWindow();
    layoutDirty = false; layout();
    if (!gfxInit()) {
        MessageBoxW(0, L"Music Visualizer could not start its graphics.\nIt needs Direct3D 11, which every PC with Windows 10 or 11 should have.",
                    L"Music Visualizer", MB_ICONERROR);
        return 1;
    }

    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enm);
    if (enm) audioOpen();

    nid.cbSize = sizeof(nid); nid.hWnd = hwnd; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY; nid.hIcon = appIcon(GetSystemMetrics(SM_CXSMICON));
    lstrcpyW(nid.szTip, L"Music Visualizer");
    Shell_NotifyIconW(NIM_ADD, &nid);

    HANDLE timer = CreateWaitableTimerExW(0, 0, 2 /*high resolution*/, TIMER_ALL_ACCESS);
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (setWnd && IsDialogMessageW(setWnd, &msg)) continue;  // Tab and arrow keys in the settings window
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        double wait = tick();
        if (wait < 0) MsgWaitForMultipleObjects(0, 0, FALSE, 250, QS_ALLINPUT);  // asleep: 4 wake-ups a second
        else if (wait > 0.001) {  // sleep out the rest of the frame, waking early for input
            if (timer) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(wait * 1e7);
                SetWaitableTimer(timer, &due, 0, 0, 0, FALSE);
                MsgWaitForMultipleObjects(1, &timer, FALSE, 300, QS_ALLINPUT);
            } else MsgWaitForMultipleObjects(0, 0, FALSE, (DWORD)(wait * 1000), QS_ALLINPUT);
        }
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    audioClose();
    gfxShutdown();
    return 0;
}
