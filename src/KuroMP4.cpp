// KuroMP4 — MMDPlugin プラグイン: MP4 などを 変換なしで 背景動画として 読み込む (くろ作 2026-11-17)
//
//  しくみ
//   * MikuMikuDance.exe の AVIFIL32 の 関数 (AVIFileOpenW など) を IAT で 横取りする。
//   * 開く ファイルが .mp4/.m4v/.mov/.mkv/.webm/.wmv (または それに ".avi" を 足した 名前) なら、
//     Media Foundation (IMFSourceReader) で デコードして、AVI の ふりを して 1枚ずつ 渡す。
//   * それ以外 (ふつうの AVI) は そのまま 本物の 関数へ。
//   * ドラッグ&ドロップ: DragQueryFileW/A を 横取りして "x.mp4" → "x.mp4.avi" に 見せる
//     (MMD は .avi を 背景動画として 読む)。開く ときに ".avi" を はずして 本物の MP4 を 開く。

#define MAKE_MMD_PLUGIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <vfw.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <algorithm>
#include "mmd_plugin.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------- log
static HMODULE g_self = nullptr;
static std::wstring g_logPath;
static std::mutex g_logMx;
static void LOG(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static std::string U8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- extensions
static const wchar_t* kExt[] = { L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".wmv" };
static bool EndsWithI(const std::wstring& s, const wchar_t* e)
{
    size_t n = wcslen(e);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, e) == 0;
}
static bool IsMovieExt(const std::wstring& s)
{
    for (auto e : kExt) if (EndsWithI(s, e)) return true;
    return false;
}
// "x.mp4" or "x.mp4.avi" -> real movie path ("" if not ours)
static std::wstring RealMoviePath(const std::wstring& p)
{
    if (IsMovieExt(p)) return p;
    if (EndsWithI(p, L".avi")) {
        std::wstring q = p.substr(0, p.size() - 4);
        if (IsMovieExt(q)) return q;
    }
    return {};
}

// ---------------------------------------------------------------- MF movie
struct Movie
{
    IMFSourceReader* rd = nullptr;
    UINT32 w = 0, h = 0;          // output size
    LONG stride = 0;              // output default stride (bytes, may be negative)
    UINT32 fpsN = 30, fpsD = 1;
    LONGLONG dur100ns = 0;
    LONG frames = 0;
    LONG curIdx = -1;             // index of frame in cur
    std::vector<BYTE> cur;        // top-down BGRA w*h*4
    bool eof = false;
    std::mutex mx;

    LONG IdxOf(LONGLONG t) const { return (LONG)((t * (LONGLONG)fpsN + (LONGLONG)fpsD * 5000000) / ((LONGLONG)fpsD * 10000000)); }
    LONGLONG TimeOf(LONG i) const { return (LONGLONG)i * fpsD * 10000000 / fpsN; }

    bool Open(const std::wstring& path)
    {
        IMFAttributes* at = nullptr;
        MFCreateAttributes(&at, 2);
        at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), at, &rd);
        at->Release();
        if (FAILED(hr)) { LOG("MFCreateSourceReaderFromURL failed %08X", hr); return false; }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        IMFMediaType* mt = nullptr;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
        mt->Release();
        if (FAILED(hr)) { LOG("SetCurrentMediaType RGB32 failed %08X", hr); return false; }
        IMFMediaType* out = nullptr;
        rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &out);
        MFGetAttributeSize(out, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(out, MF_MT_FRAME_RATE, &fpsN, &fpsD);
        UINT32 st = 0;
        if (SUCCEEDED(out->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) stride = (LONG)st; else stride = (LONG)w * 4;
        out->Release();
        if (fpsN == 0 || fpsD == 0) { fpsN = 30; fpsD = 1; }
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) dur100ns = (LONGLONG)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
        frames = std::max<LONG>(1, IdxOf(dur100ns));
        cur.assign((size_t)w * h * 4, 0);
        LOG("open %s : %ux%u fps %u/%u dur %.3fs frames %ld stride %ld", U8(path).c_str(), w, h, fpsN, fpsD, dur100ns / 1e7, frames, stride);
        return w > 0 && h > 0;
    }
    ~Movie() { if (rd) rd->Release(); }

    // read next decoded frame into cur; returns its index or -1 on eof/error
    LONG ReadNext()
    {
        for (;;) {
            DWORD si = 0, fl = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
            HRESULT hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &s);
            if (FAILED(hr)) { LOG("ReadSample failed %08X", hr); eof = true; return -1; }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { if (s) s->Release(); eof = true; return -1; }
            if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                IMFMediaType* out = nullptr;
                if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &out))) {
                    UINT32 nw = 0, nh = 0; MFGetAttributeSize(out, MF_MT_FRAME_SIZE, &nw, &nh);
                    UINT32 st = 0; if (SUCCEEDED(out->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) stride = (LONG)st;
                    LOG("media type changed %ux%u stride %ld", nw, nh, stride);
                    out->Release();
                }
            }
            if (!s) continue;
            IMFMediaBuffer* b = nullptr;
            s->ConvertToContiguousBuffer(&b);
            s->Release();
            if (!b) continue;
            BYTE* p = nullptr; DWORD len = 0;
            IMF2DBuffer* b2 = nullptr;
            LONG pitch = stride;
            bool locked2d = false;
            if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&p, &pitch))) locked2d = true;
            else { b->Lock(&p, nullptr, &len); }
            // copy to top-down BGRA
            for (UINT32 y = 0; y < h; ++y) {
                const BYTE* src = pitch >= 0 ? p + (size_t)y * pitch : p + (size_t)(h - 1 - y) * (-pitch);
                memcpy(&cur[(size_t)y * w * 4], src, (size_t)w * 4);
            }
            if (locked2d) b2->Unlock2D(); else b->Unlock();
            if (b2) b2->Release();
            b->Release();
            curIdx = IdxOf(ts);
            return curIdx;
        }
    }

    // make cur hold frame idx (or the closest decodable one)
    void Seek(LONG idx)
    {
        if (idx < 0) idx = 0;
        if (idx >= frames) idx = frames - 1;
        if (idx == curIdx) return;
        if (idx > curIdx && idx - curIdx <= 45 && !eof) {          // short forward: just decode
            while (curIdx < idx) if (ReadNext() < 0) break;
            return;
        }
        PROPVARIANT pv; PropVariantInit(&pv);
        pv.vt = VT_I8; pv.hVal.QuadPart = TimeOf(idx);
        HRESULT hr = rd->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);
        if (FAILED(hr)) LOG("SetCurrentPosition failed %08X", hr);
        eof = false; curIdx = -1;
        LONG got;
        int guard = 0;
        do { got = ReadNext(); } while (got >= 0 && got < idx && ++guard < 100000);
    }
};

// ---------------------------------------------------------------- fake AVI objects
static const DWORD kMagicFile = 0x4B4D5046, kMagicStream = 0x4B4D5053, kMagicGF = 0x4B4D5047;
struct FakeFile   { DWORD magic = kMagicFile;   Movie* mv = nullptr; LONG ref = 1; std::wstring path; };
struct FakeStream { DWORD magic = kMagicStream; FakeFile* file = nullptr; LONG ref = 1; };
struct FakeGF     { DWORD magic = kMagicGF;     FakeStream* st = nullptr; WORD bpp = 24; std::vector<BYTE> dib; };
static std::mutex g_objMx;
static std::set<void*> g_files, g_streams, g_gfs;
static bool IsOurs(std::set<void*>& s, void* p) { std::lock_guard<std::mutex> lk(g_objMx); return s.count(p) != 0; }

// ---------------------------------------------------------------- originals
using PFN_AVIFileOpenW         = HRESULT (WINAPI*)(PAVIFILE*, LPCWSTR, UINT, LPCLSID);
using PFN_AVIFileInfoA         = HRESULT (WINAPI*)(PAVIFILE, LPAVIFILEINFOA, LONG);
using PFN_AVIFileGetStream     = HRESULT (WINAPI*)(PAVIFILE, PAVISTREAM*, DWORD, LONG);
using PFN_AVIFileRelease       = ULONG   (WINAPI*)(PAVIFILE);
using PFN_AVIStreamInfoA       = HRESULT (WINAPI*)(PAVISTREAM, LPAVISTREAMINFOA, LONG);
using PFN_AVIStreamLength      = LONG    (WINAPI*)(PAVISTREAM);
using PFN_AVIStreamStart       = LONG    (WINAPI*)(PAVISTREAM);
using PFN_AVIStreamSampleToTime= LONG    (WINAPI*)(PAVISTREAM, LONG);
using PFN_AVIStreamTimeToSample= LONG    (WINAPI*)(PAVISTREAM, LONG);
using PFN_AVIStreamGetFrameOpen= PGETFRAME (WINAPI*)(PAVISTREAM, LPBITMAPINFOHEADER);
using PFN_AVIStreamGetFrame    = LPVOID  (WINAPI*)(PGETFRAME, LONG);
using PFN_AVIStreamGetFrameClose = HRESULT (WINAPI*)(PGETFRAME);
using PFN_AVIStreamRelease     = ULONG   (WINAPI*)(PAVISTREAM);
using PFN_DragQueryFileW       = UINT    (WINAPI*)(HDROP, UINT, LPWSTR, UINT);
using PFN_DragQueryFileA       = UINT    (WINAPI*)(HDROP, UINT, LPSTR, UINT);

static PFN_AVIFileOpenW o_AVIFileOpenW;
static PFN_AVIFileInfoA o_AVIFileInfoA;
static PFN_AVIFileGetStream o_AVIFileGetStream;
static PFN_AVIFileRelease o_AVIFileRelease;
static PFN_AVIStreamInfoA o_AVIStreamInfoA;
static PFN_AVIStreamLength o_AVIStreamLength;
static PFN_AVIStreamStart o_AVIStreamStart;
static PFN_AVIStreamSampleToTime o_AVIStreamSampleToTime;
static PFN_AVIStreamTimeToSample o_AVIStreamTimeToSample;
static PFN_AVIStreamGetFrameOpen o_AVIStreamGetFrameOpen;
static PFN_AVIStreamGetFrame o_AVIStreamGetFrame;
static PFN_AVIStreamGetFrameClose o_AVIStreamGetFrameClose;
static PFN_AVIStreamRelease o_AVIStreamRelease;
static PFN_DragQueryFileW o_DragQueryFileW;
static PFN_DragQueryFileA o_DragQueryFileA;

static bool g_mfReady = false;
static void EnsureMF()
{
    if (g_mfReady) return;
    HRESULT hc = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    LOG("MFStartup %08X (CoInit %08X)", hr, hc);
    g_mfReady = SUCCEEDED(hr);
}

// ---------------------------------------------------------------- hooks
static HRESULT WINAPI h_AVIFileOpenW(PAVIFILE* ppf, LPCWSTR file, UINT mode, LPCLSID h)
{
    std::wstring real = file ? RealMoviePath(file) : std::wstring();
    if (real.empty() || (mode & (OF_WRITE | OF_CREATE | OF_READWRITE))) return o_AVIFileOpenW(ppf, file, mode, h);
    LOG("AVIFileOpenW %s (mode %u)", U8(file).c_str(), mode);
    if (GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES) { LOG(" -> file not found"); return AVIERR_FILEOPEN; }
    EnsureMF();
    auto mv = new Movie();
    if (!g_mfReady || !mv->Open(real)) { delete mv; LOG(" -> open failed"); return AVIERR_BADFORMAT; }
    auto f = new FakeFile(); f->mv = mv; f->path = real;
    { std::lock_guard<std::mutex> lk(g_objMx); g_files.insert(f); }
    *ppf = (PAVIFILE)f;
    return AVIERR_OK;
}
static HRESULT WINAPI h_AVIFileInfoA(PAVIFILE pf, LPAVIFILEINFOA fi, LONG sz)
{
    if (!IsOurs(g_files, pf)) return o_AVIFileInfoA(pf, fi, sz);
    auto f = (FakeFile*)pf; AVIFILEINFOA x = {};
    x.dwMaxBytesPerSec = 0; x.dwFlags = AVIFILEINFO_HASINDEX; x.dwCaps = AVIFILECAPS_CANREAD;
    x.dwStreams = 1; x.dwSuggestedBufferSize = f->mv->w * f->mv->h * 4;
    x.dwWidth = f->mv->w; x.dwHeight = f->mv->h; x.dwScale = f->mv->fpsD; x.dwRate = f->mv->fpsN; x.dwLength = f->mv->frames;
    strcpy_s(x.szFileType, "KuroMP4");
    memcpy(fi, &x, std::min<size_t>(sizeof(x), (size_t)sz));
    return AVIERR_OK;
}
static HRESULT WINAPI h_AVIFileGetStream(PAVIFILE pf, PAVISTREAM* pps, DWORD type, LONG idx)
{
    if (!IsOurs(g_files, pf)) return o_AVIFileGetStream(pf, pps, type, idx);
    LOG("AVIFileGetStream type %08X idx %ld", type, idx);
    if (!(type == 0 || type == streamtypeVIDEO) || idx != 0) { *pps = nullptr; return AVIERR_NODATA; }
    auto f = (FakeFile*)pf; auto s = new FakeStream(); s->file = f; InterlockedIncrement(&f->ref);
    { std::lock_guard<std::mutex> lk(g_objMx); g_streams.insert(s); }
    *pps = (PAVISTREAM)s;
    return AVIERR_OK;
}
static void FreeFile(FakeFile* f)
{
    { std::lock_guard<std::mutex> lk(g_objMx); g_files.erase(f); }
    LOG("close %s", U8(f->path).c_str());
    delete f->mv; delete f;
}
static ULONG WINAPI h_AVIFileRelease(PAVIFILE pf)
{
    if (!IsOurs(g_files, pf)) return o_AVIFileRelease(pf);
    auto f = (FakeFile*)pf; LONG r = InterlockedDecrement(&f->ref);
    if (r <= 0) FreeFile(f);
    return (ULONG)std::max<LONG>(r, 0);
}
static HRESULT WINAPI h_AVIStreamInfoA(PAVISTREAM ps, LPAVISTREAMINFOA si, LONG sz)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamInfoA(ps, si, sz);
    auto mv = ((FakeStream*)ps)->file->mv; AVISTREAMINFOA x = {};
    x.fccType = streamtypeVIDEO; x.fccHandler = mmioFOURCC('K', 'M', 'P', '4');
    x.dwScale = mv->fpsD; x.dwRate = mv->fpsN; x.dwStart = 0; x.dwLength = mv->frames;
    x.dwSuggestedBufferSize = mv->w * mv->h * 4; x.dwQuality = (DWORD)-1;
    SetRect(&x.rcFrame, 0, 0, (int)mv->w, (int)mv->h);
    strcpy_s(x.szName, "KuroMP4 video");
    memcpy(si, &x, std::min<size_t>(sizeof(x), (size_t)sz));
    return AVIERR_OK;
}
static LONG WINAPI h_AVIStreamLength(PAVISTREAM ps)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamLength(ps);
    return ((FakeStream*)ps)->file->mv->frames;
}
static LONG WINAPI h_AVIStreamStart(PAVISTREAM ps)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamStart(ps);
    return 0;
}
static LONG WINAPI h_AVIStreamSampleToTime(PAVISTREAM ps, LONG s)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamSampleToTime(ps, s);
    auto mv = ((FakeStream*)ps)->file->mv;
    return (LONG)((LONGLONG)s * 1000 * mv->fpsD / mv->fpsN);
}
static LONG WINAPI h_AVIStreamTimeToSample(PAVISTREAM ps, LONG t)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamTimeToSample(ps, t);
    auto mv = ((FakeStream*)ps)->file->mv;
    LONG s = (LONG)((LONGLONG)t * mv->fpsN / ((LONGLONG)1000 * mv->fpsD));
    return std::min(s, mv->frames - 1);
}
static PGETFRAME WINAPI h_AVIStreamGetFrameOpen(PAVISTREAM ps, LPBITMAPINFOHEADER want)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamGetFrameOpen(ps, want);
    auto s = (FakeStream*)ps; auto g = new FakeGF(); g->st = s; InterlockedIncrement(&s->ref);
    WORD bpp = 24;
    if (want && (ULONG_PTR)want > 0x10 && want->biBitCount) bpp = want->biBitCount;
    if (bpp != 24 && bpp != 32) bpp = 24;
    g->bpp = bpp;
    auto mv = s->file->mv;
    DWORD rowB = ((mv->w * bpp / 8) + 3) & ~3u;
    g->dib.assign(sizeof(BITMAPINFOHEADER) + (size_t)rowB * mv->h, 0);
    auto bi = (BITMAPINFOHEADER*)g->dib.data();
    bi->biSize = sizeof(BITMAPINFOHEADER); bi->biWidth = (LONG)mv->w; bi->biHeight = (LONG)mv->h;
    bi->biPlanes = 1; bi->biBitCount = bpp; bi->biCompression = BI_RGB; bi->biSizeImage = rowB * mv->h;
    { std::lock_guard<std::mutex> lk(g_objMx); g_gfs.insert(g); }
    LOG("GetFrameOpen want=%p (bpp %u comp %u w %ld h %ld) -> %u bpp", want,
        (want && (ULONG_PTR)want > 0x10) ? want->biBitCount : 0, (want && (ULONG_PTR)want > 0x10) ? want->biCompression : 0,
        (want && (ULONG_PTR)want > 0x10) ? want->biWidth : 0, (want && (ULONG_PTR)want > 0x10) ? want->biHeight : 0, bpp);
    return (PGETFRAME)g;
}
static LPVOID WINAPI h_AVIStreamGetFrame(PGETFRAME pg, LONG pos)
{
    if (!IsOurs(g_gfs, pg)) return o_AVIStreamGetFrame(pg, pos);
    auto g = (FakeGF*)pg; auto mv = g->st->file->mv;
    std::lock_guard<std::mutex> lk(mv->mx);
    static int nlog = 0;
    DWORD t0 = GetTickCount();
    mv->Seek(pos);
    DWORD dt = GetTickCount() - t0;
    if (nlog < 40 || dt > 300) { LOG("GetFrame %ld -> cur %ld (%lu ms)", pos, mv->curIdx, dt); ++nlog; }
    // top-down BGRA -> bottom-up DIB
    auto bi = (BITMAPINFOHEADER*)g->dib.data(); BYTE* dst = g->dib.data() + sizeof(BITMAPINFOHEADER);
    DWORD rowB = ((mv->w * g->bpp / 8) + 3) & ~3u;
    for (UINT32 y = 0; y < mv->h; ++y) {
        const BYTE* s = &mv->cur[(size_t)(mv->h - 1 - y) * mv->w * 4];
        BYTE* d = dst + (size_t)y * rowB;
        if (g->bpp == 32) { memcpy(d, s, (size_t)mv->w * 4); for (UINT32 x = 0; x < mv->w; ++x) d[x * 4 + 3] = 255; }
        else for (UINT32 x = 0; x < mv->w; ++x) { d[x * 3] = s[x * 4]; d[x * 3 + 1] = s[x * 4 + 1]; d[x * 3 + 2] = s[x * 4 + 2]; }
    }
    return bi;
}
static void ReleaseStream(FakeStream* s)
{
    if (InterlockedDecrement(&s->ref) <= 0) {
        { std::lock_guard<std::mutex> lk(g_objMx); g_streams.erase(s); }
        auto f = s->file; delete s;
        if (InterlockedDecrement(&f->ref) <= 0) FreeFile(f);
    }
}
static HRESULT WINAPI h_AVIStreamGetFrameClose(PGETFRAME pg)
{
    if (!IsOurs(g_gfs, pg)) return o_AVIStreamGetFrameClose(pg);
    auto g = (FakeGF*)pg;
    { std::lock_guard<std::mutex> lk(g_objMx); g_gfs.erase(g); }
    auto s = g->st; delete g; ReleaseStream(s);
    return AVIERR_OK;
}
static ULONG WINAPI h_AVIStreamRelease(PAVISTREAM ps)
{
    if (!IsOurs(g_streams, ps)) return o_AVIStreamRelease(ps);
    ReleaseStream((FakeStream*)ps);
    return 0;
}
// drag & drop: movie -> movie + ".avi"
static UINT WINAPI h_DragQueryFileW(HDROP hd, UINT i, LPWSTR buf, UINT cch)
{
    if (i == 0xFFFFFFFF) return o_DragQueryFileW(hd, i, buf, cch);
    UINT n = o_DragQueryFileW(hd, i, nullptr, 0);
    std::wstring s(n + 1, L'\0');
    o_DragQueryFileW(hd, i, &s[0], n + 1); s.resize(n);
    if (IsMovieExt(s)) { s += L".avi"; LOG("drop %s", U8(s).c_str()); }
    if (!buf) return (UINT)s.size();
    if (cch == 0) return 0;
    UINT c = std::min<UINT>((UINT)s.size(), cch - 1);
    memcpy(buf, s.c_str(), c * sizeof(wchar_t)); buf[c] = 0;
    return c;
}
static UINT WINAPI h_DragQueryFileA(HDROP hd, UINT i, LPSTR buf, UINT cch)
{
    if (i == 0xFFFFFFFF) return o_DragQueryFileA(hd, i, buf, cch);
    UINT n = o_DragQueryFileW(hd, i, nullptr, 0);
    std::wstring s(n + 1, L'\0');
    o_DragQueryFileW(hd, i, &s[0], n + 1); s.resize(n);
    if (!IsMovieExt(s)) return o_DragQueryFileA(hd, i, buf, cch);
    s += L".avi";
    int need = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!buf) return (UINT)(need - 1);
    std::string a(need, 0); WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, &a[0], need, nullptr, nullptr);
    UINT c = std::min<UINT>((UINT)(need - 1), cch ? cch - 1 : 0);
    memcpy(buf, a.c_str(), c); if (cch) buf[c] = 0;
    return c;
}

// file-existence checks: "x.mp4.avi" (doesn't exist) -> "x.mp4"
static std::wstring RedirectFake(const wchar_t* p)
{
    if (!p || !EndsWithI(p, L".avi")) return {};
    std::wstring s(p);
    std::wstring real = RealMoviePath(s);
    if (real.empty() || real == s) return {};
    if (GetFileAttributesW(s.c_str()) != INVALID_FILE_ATTRIBUTES) return {};
    if (GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return real;
}
using PFN_CreateFileW = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_wfopen_s = errno_t (__cdecl*)(FILE**, const wchar_t*, const wchar_t*);
using PFN_wsopen_s = errno_t (__cdecl*)(int*, const wchar_t*, int, int, int);
static PFN_CreateFileW o_CreateFileW;
static PFN_wfopen_s o_wfopen_s;
static PFN_wsopen_s o_wsopen_s;
static HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t)
{
    if (disp == OPEN_EXISTING && !(acc & (GENERIC_WRITE | FILE_WRITE_DATA))) {
        std::wstring r = RedirectFake(n);
        if (!r.empty()) { LOG("CreateFileW redirect %s", U8(r).c_str()); return o_CreateFileW(r.c_str(), acc, sh, sa, disp, fl, t); }
    }
    return o_CreateFileW(n, acc, sh, sa, disp, fl, t);
}
static errno_t __cdecl h_wfopen_s(FILE** f, const wchar_t* n, const wchar_t* m)
{
    if (m && !wcschr(m, L'w') && !wcschr(m, L'a') && !wcschr(m, L'+')) {
        std::wstring r = RedirectFake(n);
        if (!r.empty()) { LOG("_wfopen_s redirect %s", U8(r).c_str()); return o_wfopen_s(f, r.c_str(), m); }
    }
    return o_wfopen_s(f, n, m);
}
static errno_t __cdecl h_wsopen_s(int* fh, const wchar_t* n, int of, int sh, int pm)
{
    if ((of & 3) == 0 /*_O_RDONLY*/) {
        std::wstring r = RedirectFake(n);
        if (!r.empty()) { LOG("_wsopen_s redirect %s", U8(r).c_str()); return o_wsopen_s(fh, r.c_str(), of, sh, pm); }
    }
    return o_wsopen_s(fh, n, of, sh, pm);
}

// ---------------------------------------------------------------- IAT patch
static void* PatchIAT(HMODULE mod, const char* dll, const char* fn, void* nf)
{
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto oft = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            void* old = (void*)ft->u1.Function;
            DWORD prot; VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            ft->u1.Function = (ULONG_PTR)nf;
            VirtualProtect(&ft->u1.Function, sizeof(void*), prot, &prot);
            return old;
        }
    }
    return nullptr;
}
template<class T> static void Hook(const char* dll, const char* fn, T nf, T& orig)
{
    void* o = PatchIAT(GetModuleHandleW(nullptr), dll, fn, (void*)nf);
    if (!o) { // not imported by exe: fall back to GetProcAddress (so our wrappers can still forward)
        HMODULE m = LoadLibraryA(dll); o = m ? (void*)GetProcAddress(m, fn) : nullptr;
        LOG("hook %s!%s: NOT in exe IAT (orig %p)", dll, fn, o);
    } else LOG("hook %s!%s ok", dll, fn);
    orig = (T)o;
}
static void InstallHooks()
{
    Hook("AVIFIL32.dll", "AVIFileOpenW", h_AVIFileOpenW, o_AVIFileOpenW);
    Hook("AVIFIL32.dll", "AVIFileInfoA", h_AVIFileInfoA, o_AVIFileInfoA);
    Hook("AVIFIL32.dll", "AVIFileGetStream", h_AVIFileGetStream, o_AVIFileGetStream);
    Hook("AVIFIL32.dll", "AVIFileRelease", h_AVIFileRelease, o_AVIFileRelease);
    Hook("AVIFIL32.dll", "AVIStreamInfoA", h_AVIStreamInfoA, o_AVIStreamInfoA);
    Hook("AVIFIL32.dll", "AVIStreamLength", h_AVIStreamLength, o_AVIStreamLength);
    Hook("AVIFIL32.dll", "AVIStreamStart", h_AVIStreamStart, o_AVIStreamStart);
    Hook("AVIFIL32.dll", "AVIStreamSampleToTime", h_AVIStreamSampleToTime, o_AVIStreamSampleToTime);
    Hook("AVIFIL32.dll", "AVIStreamTimeToSample", h_AVIStreamTimeToSample, o_AVIStreamTimeToSample);
    Hook("AVIFIL32.dll", "AVIStreamGetFrameOpen", h_AVIStreamGetFrameOpen, o_AVIStreamGetFrameOpen);
    Hook("AVIFIL32.dll", "AVIStreamGetFrame", h_AVIStreamGetFrame, o_AVIStreamGetFrame);
    Hook("AVIFIL32.dll", "AVIStreamGetFrameClose", h_AVIStreamGetFrameClose, o_AVIStreamGetFrameClose);
    Hook("AVIFIL32.dll", "AVIStreamRelease", h_AVIStreamRelease, o_AVIStreamRelease);
    Hook("SHELL32.dll", "DragQueryFileW", h_DragQueryFileW, o_DragQueryFileW);
    Hook("SHELL32.dll", "DragQueryFileA", h_DragQueryFileA, o_DragQueryFileA);
    Hook("KERNEL32.dll", "CreateFileW", h_CreateFileW, o_CreateFileW);
    Hook("MSVCR90.dll", "_wfopen_s", h_wfopen_s, o_wfopen_s);
    Hook("MSVCR90.dll", "_wsopen_s", h_wsopen_s, o_wsopen_s);
}

// ---------------------------------------------------------------- plugin
class KuroMP4Plugin : public MMDPluginDLL3
{
public:
    explicit KuroMP4Plugin(IDirect3DDevice9*) {}
    const char* getPluginTitle() const override { return "KuroMP4"; }
};

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h; DisableThreadLibraryCalls(h);
        wchar_t p[MAX_PATH]; GetModuleFileNameW(h, p, MAX_PATH);
        PathRemoveExtensionW(p); g_logPath = std::wstring(p) + L".log";
    }
    return TRUE;
}

static bool g_hooked = false;
extern "C" MMD_PLUGIN_API int version() { return 3; }   // MMDPlugin calls create<version>() → must be 3 for create3
extern "C" MMD_PLUGIN_API MMDPluginDLL3* create3(IDirect3DDevice9* d)
{
    { FILE* f = nullptr; if (_wfopen_s(&f, g_logPath.c_str(), L"w") == 0 && f) fclose(f); }
    LOG("KuroMP4 create3 (build %s %s)", __DATE__, __TIME__);
    if (!g_hooked) { InstallHooks(); g_hooked = true; }
    return new KuroMP4Plugin(d);
}
extern "C" MMD_PLUGIN_API void destroy3(MMDPluginDLL3* p) { delete p; }
