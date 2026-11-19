#include "raylib.h"
#include "rlgl.h" // Для корректной работы rlBegin/rlEnd
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <cctype>
#include <cstdlib>
#include <sstream> // Для безопасного чтения настроек

namespace fs = std::filesystem;

// =====================================================================
//  BASS (объявления вручную, чтобы не конфликтовал с raylib)
// =====================================================================
typedef unsigned long DWORD;
typedef unsigned __int64 QWORD;
typedef DWORD HSTREAM;
typedef DWORD HFX;

#define BASS_SAMPLE_FLOAT   0x100
#define BASS_UNICODE        0x80000000
#define BASS_ACTIVE_STOPPED 0
#define BASS_ACTIVE_PLAYING 1
#define BASS_ATTRIB_VOL     2
#define BASS_POS_BYTE       0
#define BASS_POS_FLUSH      0x1000000
#define BASS_DATA_FFT2048   0x80000003

#define BASS_STREAM_PRESCAN 0x20000

#define BASS_TAG_ID3V2          5
#define BASS_TAG_FLAC_PICTURE   0x12000

// Эффекты DX8, встроенные прямо в bass.dll
#define BASS_FX_DX8_COMPRESSOR 1
#define BASS_FX_DX8_PARAMEQ    7

struct BASS_DX8_PARAMEQ {
    float fCenter;
    float fBandwidth;
    float fGain;
};

struct BASS_DX8_COMPRESSOR {
    float fGain;
    float fAttack;
    float fRelease;
    float fThreshold;
    float fRatio;
    float fPredelay;
};

// Правильная структура FLAC-тега из документации BASS
struct TAG_FLAC_PICTURE {
    DWORD apic;
    const char* mime;
    const char* desc;
    DWORD width;
    DWORD height;
    DWORD depth;
    DWORD colors;
    DWORD length;
    const void* data;
};

extern "C" {
    __declspec(dllimport) int     BASS_Init(int device, DWORD freq, DWORD flags, void* win, const void* clsid);
    __declspec(dllimport) int     BASS_Free(void);
    __declspec(dllimport) HSTREAM BASS_StreamCreateFile(int mem, const void* file, QWORD offset, QWORD length, DWORD flags);
    __declspec(dllimport) int     BASS_StreamFree(HSTREAM handle);
    __declspec(dllimport) int     BASS_ChannelPlay(DWORD handle, int restart);
    __declspec(dllimport) int     BASS_ChannelPause(DWORD handle);
    __declspec(dllimport) DWORD   BASS_ChannelIsActive(DWORD handle);
    __declspec(dllimport) int     BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value);
    __declspec(dllimport) QWORD   BASS_ChannelGetLength(DWORD handle, DWORD mode);
    __declspec(dllimport) QWORD   BASS_ChannelGetPosition(DWORD handle, DWORD mode);
    __declspec(dllimport) int     BASS_ChannelSetPosition(DWORD handle, QWORD pos, DWORD mode);
    __declspec(dllimport) double  BASS_ChannelBytes2Seconds(DWORD handle, QWORD pos);
    __declspec(dllimport) QWORD   BASS_ChannelSeconds2Bytes(DWORD handle, double pos);
    __declspec(dllimport) DWORD   BASS_ChannelGetLevel(DWORD handle);
    __declspec(dllimport) DWORD   BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length);
    __declspec(dllimport) HFX     BASS_ChannelSetFX(DWORD handle, DWORD type, int priority);
    __declspec(dllimport) int     BASS_ChannelRemoveFX(DWORD handle, HFX fx);
    __declspec(dllimport) int     BASS_FXSetParameters(HFX handle, const void* params);
    __declspec(dllimport) const char* BASS_ChannelGetTags(DWORD handle, DWORD tags);
    __declspec(dllimport) DWORD   BASS_PluginLoad(const void* file, DWORD flags);
    __declspec(dllimport) int     BASS_ErrorGetCode(void);
}
// --- Дополнения BASS для настроек звука ---
#define BASS_ATTRIB_PAN     3
#define BASS_STREAM_DECODE  0x200000

typedef DWORD HDSP;
typedef void(__stdcall* DSPPROC)(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user);

#pragma pack(push, 4)
struct BASS_CHANNELINFO {
    DWORD freq;
    DWORD chans;
    DWORD flags;
    DWORD ctype;
    DWORD origres;
    DWORD plugin;
    DWORD sample;
    const char* filename;
};
#pragma pack(pop)

extern "C" {
    __declspec(dllimport) HDSP BASS_ChannelSetDSP(DWORD handle, DSPPROC proc, void* user, int priority);
    __declspec(dllimport) int  BASS_ChannelGetInfo(DWORD handle, BASS_CHANNELINFO* info);
}
// =====================================================================
//  Цвета
// =====================================================================
static const Color C_BG = { 10, 12, 22, 255 };
static const Color C_PANEL = { 24, 26, 44, 225 };
static const Color C_PANEL2 = { 44, 48, 78, 255 };
static const Color C_POP = { 20, 22, 38, 248 };
static const Color C_ACCENT = { 124, 92, 255, 255 };
static const Color C_ACC2 = { 64, 196, 255, 255 };
static const Color C_PINK = { 255, 92, 170, 255 };
static const Color C_TEXT = { 240, 242, 255, 255 };
static const Color C_DIM = { 150, 156, 190, 255 };

// =====================================================================
//  Структуры данных библиотеки
// =====================================================================
struct Track {
    std::wstring fullPath;
    std::string  title;
    std::string  artist;
};

struct FolderEntry {
    std::wstring path;
    std::string  name;
    std::vector<Track> tracks;
};

// =====================================================================
//  Состояние программы
// =====================================================================
static const int SPEC_BANDS = 72; // Количество точек спектра по ширине экрана

struct App {
    int   screen = 0;      // 0 плеер, 1 эквалайзер, 2 папки, 3 альбомы, 4 настройки
    bool  menuOpen = false, volOpen = false, presetOpen = false;
    float menuAnim = 0.0f, volAnim = 0.0f, presetAnim = 0.0f;
    int   viewMode = 0;    // кнопка справа вверху: 0 всё, 1 без панели плеера, 2 ещё и без волны, 3 только обложка
    float viewSidebar = 1.0f;
    float viewBar = 1.0f;
    float viewWave = 1.0f;
    float viewGlow = 1.0f;
    float rawSidebar = 1.0f, rawBar = 1.0f, rawWave = 1.0f, rawGlow = 1.0f;   // линейный прогресс анимаций (0..1)
    float viewDots[3] = { 0, 0, 0 };    // подсветка трёх точек кнопки режимов
    float viewPulse = 0.0f;             // «нажатие» кнопки режимов
    bool  lyricsOpen = false;           // открыто окно с текстом песни
    float rawLyrics = 0.0f;             // прогресс перехода к тексту песни
    bool  seeking = false; // сейчас тянем ползунок перемотки
    float seekGain = 1.0f; // громкость при перемотке (1 = звук, 0 = тишина)
    float volume = 0.7f;
    bool  playing = false;
    float pos = 0, total = 0;
    float eq[11] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // [0] преамп, [1..10] полосы
    bool  eqOn = true, toneOn = true, limitOn = false;
    float bass = 0, treble = 0;
    int   presetIdx = 0;
    float amp = 0.15f;     // общая энергия для фоновых градиентов
    float spectrum[SPEC_BANDS] = { 0 }; // сглаженные значения частот (слева НЧ -> справа ВЧ)
    int   W = 0, H = 0;
    float playerHover = 0.0f;                      // 0..1 анимация увеличения окна плеера
    int   themeIdx = 0;                            // номер готовой темы, -1 = свой градиент
    float hue[3] = { 0.72f, 0.55f, 0.93f };        // свои цвета (оттенок 0..1)

    // Библиотека папок и треков
    std::vector<FolderEntry> folders;
    int   selectedFolder = -1;
    int   playingFolder = -1;
    int   playingTrack = -1;
    float trackScroll = 0.0f;
    float folderScroll = 0.0f;
    std::string currentTitle = "Нет трека";
    std::string currentArtist = "Перетащите папку во вкладку «Папки»";
};

static App       A;
static HSTREAM   g_stream = 0;
static HFX       g_fxEq[10] = { 0 }; // 10 полос эквалайзера
static HFX       g_fxToneBass = 0;   // Ручка Бас
static HFX       g_fxToneTreb = 0;   // Ручка Высокие
static HFX       g_fxLimiter = 0;   // Лимитер

static Font      g_font;
static Texture2D g_glowTex;
static Texture2D g_coverTex = { 0 }; // Текстура обложки текущего трека
static bool      g_lock = false;
static int       g_drag = -1;
static bool      g_bassOk = false;
static Rectangle g_presetBtn = { 0, 0, 0, 0 };

// Тема оформления: текущие цвета (плавно меняются) и целевые
static Color     g_bgCol = { 10, 12, 22, 255 };
static Color     g_glow[3] = { { 124, 92, 255, 255 },{ 64, 196, 255, 255 },{ 255, 92, 170, 255 } };
static Color     g_tgtBg = { 10, 12, 22, 255 };
static Color     g_tgtGlow[3] = { { 124, 92, 255, 255 },{ 64, 196, 255, 255 },{ 255, 92, 170, 255 } };

// Пересчёт мыши, пока окно плеера увеличено при наведении
static bool      g_xOn = false;
static float     g_xS = 1.0f;
static Vector2   g_xC = { 0, 0 };

static const float EQ_FREQS[10] = { 31.0f, 62.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 15500.0f };
static const char* MENU_ITEMS[5] = { "Плеер", "Эквалайзер", "Папки", "Альбомы", "Настройки" };
static const char* PRESET_NAMES[8] = { "Плоский", "Рок", "Поп", "Бас", "Вокал", "Классика", "Электроника", "Джаз" };
static const float PRESETS[8][10] = {
    { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    { 5,  4,  3,  1, -1, -1,  1,  3,  4,  5 },
    { -1,  3,  4,  5,  3,  0, -1, -1, -1, -1 },
    { 7,  6,  5,  3,  1,  0,  0,  0,  0,  0 },
    { -2, -3, -3,  1,  4,  4,  3,  1,  0, -1 },
    { 0,  0,  0,  0,  0,  0, -3, -3, -3, -4 },
    { 5,  4,  1,  0, -2,  2,  1,  1,  4,  5 },
    { 3,  2,  1,  2, -2, -2,  0,  1,  2,  3 }
};

// =====================================================================
//  Вспомогательные функции
// =====================================================================
static Rectangle R(float x, float y, float w, float h) { return { x, y, w, h }; }
static Vector2   V(float x, float y) { return { x, y }; }
static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float EaseOutCubic(float t) { float f = 1.0f - t; return 1.0f - f * f * f; }
static float EaseInOutCubic(float t) { return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f; }
static float Lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float Approach(float cur, float target, float step) { return cur < target ? fminf(cur + step, target) : fmaxf(cur - step, target); }
static Color Ma(Color c, float a) { c.a = (unsigned char)(c.a * Clampf(a, 0.0f, 1.0f)); return c; }

static std::string PathToUtf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

static void  Txt(const char* s, float x, float y, float size, Color c) { DrawTextEx(g_font, s, V(x, y), size, size * 0.04f, c); }
static float TxtW(const char* s, float size) { return MeasureTextEx(g_font, s, size, size * 0.04f).x; }
static void  TxtC(const char* s, float cx, float y, float size, Color c) { Txt(s, cx - TxtW(s, size) / 2, y, size, c); }

static void TxtClipped(const std::string& s, float x, float y, float maxW, float size, Color c) {
    if (TxtW(s.c_str(), size) <= maxW) {
        Txt(s.c_str(), x, y, size, c);
        return;
    }
    std::string out = s;
    while (!out.empty()) {
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty()) out.pop_back();
        std::string test = out + "...";
        if (TxtW(test.c_str(), size) <= maxW) {
            Txt(test.c_str(), x, y, size, c);
            return;
        }
    }
}

static Vector2 MousePos() {
    Vector2 m = GetMousePosition();
    if (g_xOn) {
        m.x = g_xC.x + (m.x - g_xC.x) / g_xS;
        m.y = g_xC.y + (m.y - g_xC.y) / g_xS;
    }
    return m;
}
static bool Hit(Rectangle r) { return !g_lock && CheckCollisionPointRec(MousePos(), r); }
static bool Pressed(Rectangle r) { return Hit(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }
static bool BeginDrag(int id, Rectangle hit) {
    if (Pressed(hit)) g_drag = id;
    return g_drag == id && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
}

static std::string FmtTime(float s) {
    if (s < 0) s = 0;
    int m = (int)s / 60, sec = (int)s % 60;
    char b[16]; snprintf(b, sizeof b, "%d:%02d", m, sec);
    return b;
}

static bool Button(Rectangle r, const char* label, bool active, float size = 20) {
    Color base = active ? C_ACCENT : C_PANEL2;
    if (Hit(r)) base = ColorBrightness(base, 0.15f);
    DrawRectangleRounded(r, 0.5f, 12, base);
    TxtC(label, r.x + r.width / 2, r.y + (r.height - size) / 2 - 2, size, C_TEXT);
    return Pressed(r);
}

// =====================================================================
//  Обложки и текст песни: чтение из тегов файла, из папки и из интернета
// =====================================================================
typedef std::vector<unsigned char> Bytes;

static bool        g_onlineLyrics = true;  // искать текст песни в интернете (сервис lrclib.net)
static std::string g_coverNote;            // короткая причина, если обложки нет
static bool        g_adaptiveBg = false;   // подстраивать цвет фона под обложку песни
static Color       g_coverPal[3] = { { 124, 92, 255, 255 },{ 64, 196, 255, 255 },{ 255, 92, 170, 255 } };   // цвета обложки
static bool        g_coverPalOk = false;   // для текущей песни цвета обложки найдены
static bool        g_flacPlugin = false;   // плагин bassflac.dll загружен (точная перемотка FLAC)
static bool        g_curIsFlac = false;    // сейчас играет FLAC
static int         g_seekErr = 0;          // код ошибки BASS, если перемотка не удалась (0 = всё хорошо, -1 = не применилась)
static double      g_seekVerifyAt = 0.0;   // когда проверить, что перемотка действительно сработала
static float       g_seekTarget = 0.0f;    // куда перематывали
// --- Настройки звука ---
static float          g_pan = 0.0f;              // баланс: -1 влево ... +1 вправо
static volatile float g_stereoWidth = 1.0f;      // ширина стерео: 0 моно, 1 как есть, 2 шире
static bool           g_loudness = false;        // тонокомпенсация
static bool           g_crossfade = false;       // плавный переход между песнями
static float          g_xfadeSec = 4.0f;         // длительность перехода, сек
static bool           g_normalize = false;       // нормализация громкости треков
static int            g_outMode = 0;             // вывод: 0 обычный, 1 WASAPI Exclusive, 2 ASIO
static HFX            g_fxLoudLow = 0, g_fxLoudHigh = 0;   // эффекты тонокомпенсации
static HSTREAM        g_fadeStream = 0;          // уходящая песня во время перехода
static float          g_fadeGain = 0.0f;         // её громкость (1 -> 0)
static float          g_xfadeGain = 1.0f;        // нарастание новой песни (0 -> 1)
static float          g_normGainTarget = 1.0f;   // нужная поправка громкости трека
static float          g_normGain = 1.0f;         // текущая поправка (плавно догоняет нужную)
static std::wstring   g_curPath;                 // путь к играющему файлу
// --- 8D Аудио ---
static float g_8dX = 0.0f, g_8dY = 0.0f;       // откуда идёт звук: x от -1 (слева) до 1 (справа), y от -1 (спереди) до 1 (сзади)
static HFX   g_fx8dBack = 0, g_fx8dFront = 0;  // эффекты: сзади звук глуше, спереди ярче
// ---- PURE-BEGIN ----
static unsigned int BE32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}
static unsigned int LE32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int SyncSafe32(const unsigned char* p) {
    return ((unsigned int)(p[0] & 0x7F) << 21) | ((unsigned int)(p[1] & 0x7F) << 14) | ((unsigned int)(p[2] & 0x7F) << 7) | (unsigned int)(p[3] & 0x7F);
}

// Читает кусок файла (работает и с русскими путями)
static bool ReadChunk(const fs::path& path, size_t offset, size_t size, Bytes& out) {
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff total = f.tellg();
    if (total <= 0 || (size_t)total <= offset) return false;
    size_t avail = (size_t)total - offset;
    if (size > avail) size = avail;
    out.resize(size);
    f.seekg((std::streamoff)offset, std::ios::beg);
    f.read((char*)out.data(), (std::streamsize)size);
    out.resize((size_t)f.gcount());
    return !out.empty();
}

// Ищет начало картинки (JPEG или PNG) внутри блока данных
static size_t FindImageStart(const unsigned char* d, size_t n) {
    for (size_t i = 0; i + 8 <= n; i++) {
        if (d[i] == 0xFF && d[i + 1] == 0xD8 && d[i + 2] == 0xFF) return i;
        if (d[i] == 0x89 && d[i + 1] == 0x50 && d[i + 2] == 0x4E && d[i + 3] == 0x47) return i;
    }
    return (size_t)-1;
}

// ---------- Кодировки текста ----------
static void AppendUtf8(std::string& s, unsigned int cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

static bool IsValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 0)));
        if (len == 0 || i + len > n) return false;
        for (size_t k = 1; k < len; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static std::string Cp1251ToUtf8(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { r += (char)c; continue; }
        unsigned int cp = '?';
        if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
        else switch (c) {
        case 0xA8: cp = 0x401; break;  case 0xB8: cp = 0x451; break;
        case 0xB2: cp = 0x406; break;  case 0xB3: cp = 0x456; break;
        case 0xAF: cp = 0x407; break;  case 0xBF: cp = 0x457; break;
        case 0xAA: cp = 0x404; break;  case 0xBA: cp = 0x454; break;
        case 0xA5: cp = 0x490; break;  case 0xB4: cp = 0x491; break;
        case 0xB9: cp = 0x2116; break; case 0x96: cp = 0x2013; break;
        case 0x97: cp = 0x2014; break; case 0x91: cp = 0x2018; break;
        case 0x92: cp = 0x2019; break; case 0x93: cp = 0x201C; break;
        case 0x94: cp = 0x201D; break; case 0x85: cp = 0x2026; break;
        case 0xAB: cp = 0xAB; break;   case 0xBB: cp = 0xBB; break;
        case 0xA0: cp = 0x20; break;
        default: break;
        }
        AppendUtf8(r, cp);
    }
    return r;
}

static std::string Utf16ToUtf8(const unsigned char* d, size_t n, bool be) {
    std::string r;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned int u = be ? ((unsigned int)d[i] << 8 | d[i + 1]) : ((unsigned int)d[i + 1] << 8 | d[i]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            unsigned int lo = be ? ((unsigned int)d[i + 2] << 8 | d[i + 3]) : ((unsigned int)d[i + 3] << 8 | d[i + 2]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (u == 0) continue;
        AppendUtf8(r, u);
    }
    return r;
}

// Приводит любой текст к UTF-8 (убирает BOM, понимает UTF-16 и cp1251)
static std::string NormalizeText(const std::string& in) {
    if (in.size() >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF) return in.substr(3);
    if (in.size() >= 2 && (unsigned char)in[0] == 0xFF && (unsigned char)in[1] == 0xFE) return Utf16ToUtf8((const unsigned char*)in.data() + 2, in.size() - 2, false);
    if (in.size() >= 2 && (unsigned char)in[0] == 0xFE && (unsigned char)in[1] == 0xFF) return Utf16ToUtf8((const unsigned char*)in.data() + 2, in.size() - 2, true);
    if (IsValidUtf8(in)) return in;
    return Cp1251ToUtf8(in);
}

// ---------- ID3v2 ----------
struct Id3Frame { std::string id; size_t off; size_t size; };

static bool Id3Parse(const Bytes& tag, Bytes& body, std::vector<Id3Frame>& frames) {
    frames.clear();
    if (tag.size() < 10 || memcmp(tag.data(), "ID3", 3) != 0) return false;
    int ver = tag[3];
    if (ver < 2 || ver > 4) return false;
    unsigned int flags = tag[5];
    size_t size = SyncSafe32(&tag[6]);
    size_t bodyLen = tag.size() - 10;
    if (size < bodyLen) bodyLen = size;
    body.assign(tag.begin() + 10, tag.begin() + 10 + bodyLen);

    if (flags & 0x80) { // несинхронизированный тег: FF 00 -> FF
        Bytes t; t.reserve(body.size());
        for (size_t i = 0; i < body.size(); i++) {
            t.push_back(body[i]);
            if (body[i] == 0xFF && i + 1 < body.size() && body[i + 1] == 0x00) i++;
        }
        body.swap(t);
    }

    size_t p = 0;
    if ((flags & 0x40) && body.size() > 4) { // расширенный заголовок
        if (ver == 3) p = 4 + (size_t)BE32(&body[0]);
        else if (ver == 4) p = (size_t)SyncSafe32(&body[0]);
    }

    size_t hdr = (ver == 2) ? 6 : 10;
    while (p + hdr <= body.size()) {
        if (body[p] == 0) break; // дальше только пустое место
        size_t fsz;
        if (ver == 2) fsz = ((size_t)body[p + 3] << 16) | ((size_t)body[p + 4] << 8) | (size_t)body[p + 5];
        else if (ver == 3) fsz = BE32(&body[p + 4]);
        else fsz = SyncSafe32(&body[p + 4]);
        size_t dataStart = p + hdr;
        if (fsz == 0 || dataStart + fsz > body.size()) break;

        Id3Frame f;
        f.id.assign((const char*)&body[p], ver == 2 ? 3 : 4);
        f.off = dataStart; f.size = fsz;
        if (ver == 4 && (body[p + 9] & 0x01) && f.size > 4) { f.off += 4; f.size -= 4; } // индикатор длины данных
        frames.push_back(f);
        p = dataStart + fsz;
    }
    return true;
}

static bool CoverFromId3(const Bytes& tag, Bytes& img) {
    Bytes body; std::vector<Id3Frame> fr;
    if (!Id3Parse(tag, body, fr)) return false;
    for (size_t i = 0; i < fr.size(); i++) {
        if (fr[i].id != "APIC" && fr[i].id != "PIC") continue;
        size_t off = FindImageStart(&body[fr[i].off], fr[i].size);
        if (off != (size_t)-1) {
            img.assign(body.begin() + fr[i].off + off, body.begin() + fr[i].off + fr[i].size);
            return true;
        }
    }
    return false;
}

static std::string DecodeId3Text(unsigned char enc, const unsigned char* d, size_t n) {
    if (enc == 1 || enc == 2) {
        bool be = (enc == 2);
        if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { be = false; d += 2; n -= 2; }
        else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { be = true; d += 2; n -= 2; }
        return Utf16ToUtf8(d, n, be);
    }
    std::string s((const char*)d, n);
    if (enc == 3) return s;
    return IsValidUtf8(s) ? s : Cp1251ToUtf8(s);
}

// Текст песни из кадра USLT
static bool LyricsFromId3(const Bytes& tag, std::string& out) {
    Bytes body; std::vector<Id3Frame> fr;
    if (!Id3Parse(tag, body, fr)) return false;
    for (size_t i = 0; i < fr.size(); i++) {
        if (fr[i].id != "USLT" && fr[i].id != "ULT") continue;
        const unsigned char* d = &body[fr[i].off];
        size_t n = fr[i].size;
        if (n < 5) continue;
        unsigned char enc = d[0];
        size_t p = 4; // кодировка + язык (3 байта), дальше описание
        if (enc == 1 || enc == 2) { while (p + 1 < n && !(d[p] == 0 && d[p + 1] == 0)) p += 2; p += 2; }
        else { while (p < n && d[p] != 0) p++; p++; }
        if (p >= n) continue;
        out = DecodeId3Text(enc, d + p, n - p);
        if (out.size() > 10) return true;
    }
    return false;
}

// ---------- FLAC ----------
// Открывает FLAC и ставит чтение на первый блок метаданных
static bool FlacOpen(std::ifstream& f, const fs::path& path) {
    f.open(path, std::ios::binary);
    if (!f) return false;
    unsigned char hd[10];
    f.read((char*)hd, 4);
    if (f.gcount() < 4) return false;
    if (memcmp(hd, "ID3", 3) == 0) { // перед FLAC может стоять тег ID3
        f.read((char*)hd + 4, 6);
        if (f.gcount() < 6) return false;
        size_t sz = (size_t)SyncSafe32(hd + 6) + 10;
        f.seekg((std::streamoff)sz, std::ios::beg);
        f.read((char*)hd, 4);
        if (f.gcount() < 4) return false;
    }
    return memcmp(hd, "fLaC", 4) == 0;
}

static bool CoverFromFlac(const fs::path& path, Bytes& img) {
    std::ifstream f;
    if (!FlacOpen(f, path)) return false;
    bool found = false;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char bh[4];
        f.read((char*)bh, 4);
        if (f.gcount() < 4) break;
        bool last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | (size_t)bh[3];

        if (type == 6 && len > 40 && len < 40u * 1024 * 1024) {
            Bytes blk(len);
            f.read((char*)blk.data(), (std::streamsize)len);
            if ((size_t)f.gcount() < len) break;
            size_t p = 0;
            unsigned int picType = BE32(&blk[p]); p += 4;
            unsigned int ml = BE32(&blk[p]); p += 4 + (size_t)ml;
            if (p + 4 > len) break;
            unsigned int dl = BE32(&blk[p]); p += 4 + (size_t)dl;
            p += 16; // ширина, высота, глубина, число цветов
            if (p + 4 > len) break;
            size_t imgLen = BE32(&blk[p]); p += 4;
            if (p + imgLen <= len && imgLen > 8) {
                if (picType == 3 || !found) {
                    img.assign(blk.begin() + p, blk.begin() + p + imgLen);
                    found = true;
                }
                if (picType == 3) break; // лицевая обложка - то, что нужно
            }
        }
        else {
            f.seekg((std::streamoff)len, std::ios::cur);
        }
        if (last) break;
    }
    return found;
}

// Текст песни из комментариев Vorbis (поля LYRICS, UNSYNCEDLYRICS)
static bool LyricsFromFlac(const fs::path& path, std::string& out) {
    std::ifstream f;
    if (!FlacOpen(f, path)) return false;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char bh[4];
        f.read((char*)bh, 4);
        if (f.gcount() < 4) break;
        bool last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | (size_t)bh[3];

        if (type == 4 && len > 8 && len < 16u * 1024 * 1024) {
            Bytes blk(len);
            f.read((char*)blk.data(), (std::streamsize)len);
            if ((size_t)f.gcount() < len) break;
            size_t p = 0;
            unsigned int vl = LE32(&blk[p]); p += 4 + (size_t)vl;
            if (p + 4 > len) break;
            unsigned int cnt = LE32(&blk[p]); p += 4;
            std::string best;
            for (unsigned int i = 0; i < cnt && p + 4 <= len; i++) {
                size_t cl = LE32(&blk[p]); p += 4;
                if (p + cl > len) break;
                std::string kv((const char*)&blk[p], cl);
                p += cl;
                size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string key = kv.substr(0, eq);
                for (size_t k = 0; k < key.size(); k++) key[k] = (char)toupper((unsigned char)key[k]);
                if (key == "LYRICS" || key == "UNSYNCEDLYRICS" || key == "UNSYNCED LYRICS" || key == "SYNCEDLYRICS") {
                    std::string val = kv.substr(eq + 1);
                    if (val.size() > best.size()) best = val;
                }
            }
            if (best.size() > 10) { out = best; return true; }
        }
        else {
            f.seekg((std::streamoff)len, std::ios::cur);
        }
        if (last) break;
    }
    return false;
}

// ---------- M4A / MP4: атом covr ----------
static bool CoverFromM4a(const fs::path& path, Bytes& img) {
    Bytes all;
    if (!ReadChunk(path, 0, 100u * 1024 * 1024, all)) return false;
    for (size_t i = 4; i + 24 < all.size(); i++) {
        if (all[i] != 'c' || memcmp(&all[i], "covr", 4) != 0) continue;
        if (memcmp(&all[i + 8], "data", 4) != 0) continue;
        size_t dsz = BE32(&all[i + 4]);
        size_t start = i + 20;
        if (dsz < 24) continue;
        size_t len = dsz - 16;
        if (start + len > all.size()) len = all.size() - start;
        if (FindImageStart(&all[start], 16) == 0) {
            img.assign(all.begin() + start, all.begin() + start + len);
            return true;
        }
    }
    return false;
}

// =====================================================================
//  Текст песни: разбор LRC, список строк
// =====================================================================
struct LyricLine {
    float t = -1.0f;                 // время начала строки, сек (-1 = без времени)
    std::string text;
    std::vector<std::string> seg;    // строка, разбитая по ширине окна
    float h = 0.0f, y = 0.0f;        // высота и положение в списке
};

struct Lyrics {
    std::vector<LyricLine> lines;
    std::vector<float> hl;           // подсветка каждой строки 0..1 (плавно меняется)
    bool synced = false;             // есть метки времени
    int  status = 0;                 // 0 нет трека, 1 ищем, 2 готово, 3 не найден, 4 интернет-поиск выключен
    float scroll = 0.0f, totalH = 0.0f, layoutW = -1.0f;
    double freeUntil = 0.0;          // до этого момента не прокручиваем сами (пользователь крутил колёсиком)
    bool snap = true;
    std::string source;              // откуда взяли текст
};
static Lyrics g_ly;

static std::string TrimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// Метка времени вида 01:23.45 / 1:23 / 0:01:23.45
static bool ParseTimeTag(const std::string& tag, float& t) {
    if (tag.empty() || !isdigit((unsigned char)tag[0])) return false;
    double parts[3] = { 0, 0, 0 };
    int n = 0;
    size_t p = 0;
    while (n < 3 && p < tag.size()) {
        char* endp = nullptr;
        double v = strtod(tag.c_str() + p, &endp);
        if (endp == tag.c_str() + p) return false;
        parts[n++] = v;
        p = (size_t)(endp - tag.c_str());
        if (p < tag.size() && tag[p] == ':') p++; else break;
    }
    if (p < tag.size()) return false;
    if (n == 2) t = (float)(parts[0] * 60.0 + parts[1]);
    else if (n == 3) t = (float)(parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]);
    else return false;
    return true;
}

static void ParseLrc(const std::string& text, std::vector<LyricLine>& out, bool& timed) {
    out.clear(); timed = false;
    int offsetMs = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);

        std::vector<float> times;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            std::string tag = line.substr(i + 1, close - i - 1);
            float tv;
            if (ParseTimeTag(tag, tv)) times.push_back(tv);
            i = close + 1;
            if (tag.size() > 7 && (tag[0] == 'o' || tag[0] == 'O') && (tag[1] == 'f' || tag[1] == 'F') && (tag[2] == 'f' || tag[2] == 'F') && (tag[3] == 's' || tag[3] == 'S') && (tag[4] == 'e' || tag[4] == 'E') && (tag[5] == 't' || tag[5] == 'T') && tag[6] == ':')
                offsetMs = atoi(tag.c_str() + 7);
        }
        if (times.empty()) continue;

        std::string txt = line.substr(i);
        // убираем пословные метки <00:12.34>
        std::string clean;
        for (size_t k = 0; k < txt.size(); k++) {
            if (txt[k] == '<') {
                size_t c2 = txt.find('>', k);
                if (c2 != std::string::npos && c2 - k < 14) { k = c2; continue; }
            }
            clean += txt[k];
        }
        clean = TrimStr(clean);
        if (clean.empty()) continue;
        for (size_t k = 0; k < times.size(); k++) {
            LyricLine L; L.t = times[k]; L.text = clean;
            out.push_back(L);
            timed = true;
        }
    }
    if (timed) {
        for (size_t k = 0; k < out.size(); k++) {
            out[k].t -= offsetMs / 1000.0f;
            if (out[k].t < 0) out[k].t = 0;
        }
        std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    }
    else out.clear();
}

// Текст без меток времени: просто строки
static void ParsePlain(const std::string& text, std::vector<LyricLine>& out) {
    out.clear();
    size_t pos = 0;
    bool lastEmpty = true;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = TrimStr(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty()) {
            if (lastEmpty) continue;
            lastEmpty = true;
        }
        else lastEmpty = false;
        LyricLine L; L.text = line;
        out.push_back(L);
    }
    while (!out.empty() && out.back().text.empty()) out.pop_back();
}

static bool ApplyLyricsText(const std::string& raw, const char* source) {
    std::string text = NormalizeText(raw);
    std::vector<LyricLine> lines; bool timed = false;
    ParseLrc(text, lines, timed);
    if (!timed) ParsePlain(text, lines);
    if (lines.empty()) return false;
    g_ly.lines.swap(lines);
    g_ly.synced = timed;
    g_ly.status = 2;
    g_ly.source = source;
    g_ly.layoutW = -1.0f;
    g_ly.hl.clear();
    g_ly.snap = true;
    g_ly.scroll = 0.0f;
    g_ly.freeUntil = 0.0;
    return true;
}

// ---------- JSON (ответ lrclib.net) ----------
static size_t JsonParseString(const std::string& j, size_t p, std::string& out) {
    out.clear();
    if (p >= j.size() || j[p] != '"') return p;
    p++;
    while (p < j.size()) {
        char c = j[p];
        if (c == '"') return p + 1;
        if (c == '\\' && p + 1 < j.size()) {
            char e = j[p + 1];
            p += 2;
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (p + 4 > j.size()) return j.size();
                unsigned int cp = (unsigned int)strtoul(j.substr(p, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= j.size() && j[p] == '\\' && j[p + 1] == 'u') {
                    unsigned int lo = (unsigned int)strtoul(j.substr(p + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out += e; break; // \" \\ \/
            }
        }
        else { out += c; p++; }
    }
    return p;
}

// Ищет "key": значение внутри одного объекта
static size_t JsonFindValue(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = obj.find(pat, p)) != std::string::npos) {
        if (p > 0 && obj[p - 1] == '\\') { p += pat.size(); continue; }
        size_t q = p + pat.size();
        while (q < obj.size() && isspace((unsigned char)obj[q])) q++;
        if (q < obj.size() && obj[q] == ':') {
            q++;
            while (q < obj.size() && isspace((unsigned char)obj[q])) q++;
            return q;
        }
        p += pat.size();
    }
    return std::string::npos;
}

static bool JsonGetString(const std::string& obj, const char* key, std::string& out) {
    size_t q = JsonFindValue(obj, key);
    if (q == std::string::npos || q >= obj.size() || obj[q] != '"') return false;
    JsonParseString(obj, q, out);
    return true;
}

static double JsonGetNumber(const std::string& obj, const char* key, double def) {
    size_t q = JsonFindValue(obj, key);
    if (q == std::string::npos || q >= obj.size()) return def;
    char* endp = nullptr;
    double v = strtod(obj.c_str() + q, &endp);
    return (endp == obj.c_str() + q) ? def : v;
}

// Делит ответ на верхнеуровневые объекты { ... }
static std::vector<std::string> JsonSplitObjects(const std::string& j) {
    std::vector<std::string> v;
    int depth = 0; bool inStr = false; size_t start = 0;
    for (size_t i = 0; i < j.size(); i++) {
        char c = j[i];
        if (inStr) { if (c == '\\') i++; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '{') { if (depth == 0) start = i; depth++; }
        else if (c == '}') { depth--; if (depth == 0) v.push_back(j.substr(start, i - start + 1)); }
    }
    return v;
}

// Выбирает лучший вариант текста: по длительности, сначала с метками времени
static bool PickBestLyrics(const std::string& json, double dur, std::string& text) {
    std::vector<std::string> objs = JsonSplitObjects(json);
    double bestSyncDiff = 1e9, bestPlainDiff = 1e9;
    std::string bestSync, bestPlain;
    for (size_t i = 0; i < objs.size(); i++) {
        double d = JsonGetNumber(objs[i], "duration", -1.0);
        double diff = (d > 0 && dur > 0) ? fabs(d - dur) : 3.0;
        std::string s, p;
        if (JsonGetString(objs[i], "syncedLyrics", s) && s.size() > 10 && diff < bestSyncDiff) { bestSyncDiff = diff; bestSync = s; }
        if (JsonGetString(objs[i], "plainLyrics", p) && p.size() > 10 && diff < bestPlainDiff) { bestPlainDiff = diff; bestPlain = p; }
    }
    if (!bestSync.empty() && bestSyncDiff <= 10.0) { text = bestSync; return true; }
    if (!bestPlain.empty() && bestPlainDiff <= 10.0) { text = bestPlain; return true; }
    return false;
}

static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

// Имя файла в кэше по исполнителю, названию и длительности
static std::string LyricsCacheKey(const std::string& artist, const std::string& title, double dur) {
    std::string k = artist + "|" + title + "|" + std::to_string((int)(dur + 0.5));
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < k.size(); i++) { h ^= (unsigned char)tolower((unsigned char)k[i]); h *= 1099511628211ULL; }
    char buf[32]; snprintf(buf, sizeof buf, "%016llx", h);
    return buf;
}

// ---------- Цвета обложки ----------
struct Rgb { unsigned char r, g, b; };

static void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d < 1e-6f) h = 0.0f;
    else if (mx == r) h = fmodf((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
}

static Rgb HsvToRgb(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    Rgb o;
    o.r = (unsigned char)std::min(255.0f, (r + m) * 255.0f + 0.5f);
    o.g = (unsigned char)std::min(255.0f, (g + m) * 255.0f + 0.5f);
    o.b = (unsigned char)std::min(255.0f, (b + m) * 255.0f + 0.5f);
    return o;
}

// Делает цвет «светящимся»: насыщенным и достаточно ярким
static Rgb BoostColor(Rgb c) {
    float h, s, v;
    RgbToHsv(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, h, s, v);
    s = std::min(1.0f, std::max(0.55f, s * 1.1f));
    v = std::max(0.9f, v);
    return HsvToRgb(h, s, v);
}

// Находит три главных цвета картинки (RGBA, w x h): самый заметный, второй и третий
static bool ExtractPalette(const unsigned char* rgba, int w, int h, Rgb out[3]) {
    const int BINS = 12;
    double wsum[BINS], rs[BINS], gs[BINS], bs[BINS];
    for (int i = 0; i < BINS; i++) wsum[i] = rs[i] = gs[i] = bs[i] = 0.0;
    double totalW = 0.0, ar = 0.0, ag = 0.0, ab = 0.0;
    int n = 0;
    int step = std::max(1, std::min(w, h) / 48);
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const unsigned char* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (p[3] < 128) continue;   // прозрачные уголки
            float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f, hh, ss, vv;
            RgbToHsv(r, g, b, hh, ss, vv);
            ar += r; ag += g; ab += b; n++;
            double wgt = (vv < 0.15f) ? 0.0 : (double)ss * (double)ss * (double)vv;   // яркие насыщенные пиксели важнее
            int bin = (int)(hh / 30.0f) % BINS;
            wsum[bin] += wgt; rs[bin] += r * wgt; gs[bin] += g * wgt; bs[bin] += b * wgt;
            totalW += wgt;
        }
    }
    if (n == 0) return false;

    // Почти чёрно-белая обложка: берём спокойные нейтральные цвета
    if (totalW / n < 0.02) {
        float h, s, v;
        RgbToHsv((float)(ar / n), (float)(ag / n), (float)(ab / n), h, s, v);
        float base = std::max(0.55f, std::min(0.9f, v + 0.25f));
        out[0] = HsvToRgb(220.0f, 0.22f, base);
        out[1] = HsvToRgb(235.0f, 0.18f, std::min(1.0f, base + 0.1f));
        out[2] = HsvToRgb(205.0f, 0.20f, std::max(0.45f, base - 0.1f));
        return true;
    }

    // Сглаживаем по соседним оттенкам и выбираем три самых сильных, не похожих друг на друга
    double score[BINS];
    for (int i = 0; i < BINS; i++) score[i] = wsum[i] + 0.5 * (wsum[(i + 1) % BINS] + wsum[(i + BINS - 1) % BINS]);
    auto dist = [&](int a, int b) { int d = std::abs(a - b); return std::min(d, BINS - d); };
    int pick[3] = { -1, -1, -1 };
    for (int k = 0; k < 3; k++) {
        int best = -1;
        for (int i = 0; i < BINS; i++) {
            bool far = true;
            for (int j = 0; j < k; j++) if (dist(i, pick[j]) < 2) far = false;
            if (!far || wsum[i] <= 0.0) continue;
            if (k > 0 && score[i] < 0.18 * score[pick[0]]) continue;
            if (best < 0 || score[i] > score[best]) best = i;
        }
        pick[k] = best;
    }
    if (pick[0] < 0) return false;

    float baseH = 0.0f;
    for (int k = 0; k < 3; k++) {
        Rgb c;
        if (pick[k] >= 0) {
            double ws = wsum[pick[k]];
            c.r = (unsigned char)(rs[pick[k]] / ws * 255.0 + 0.5);
            c.g = (unsigned char)(gs[pick[k]] / ws * 255.0 + 0.5);
            c.b = (unsigned char)(bs[pick[k]] / ws * 255.0 + 0.5);
            c = BoostColor(c);
            if (k == 0) { float s, v; RgbToHsv(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, baseH, s, v); }
        }
        else {   // второго или третьего заметного цвета нет: берём соседние оттенки главного
            c = HsvToRgb(baseH + (k == 1 ? 38.0f : -38.0f), 0.75f, 0.95f);
        }
        out[k] = c;
    }
    return true;
}
// ---- PURE-END ----

// ---------- Картинки: raylib, а если не получилось - Windows GDI+ ----------
#pragma comment(lib, "gdiplus.lib")
struct GdipStartupInput { unsigned int version; void* debugCallback; int suppressBackgroundThread; int suppressExternalCodecs; };
struct GdipRect { int x, y, w, h; };
struct GdipBitmapData { unsigned int width, height; int stride; int pixelFormat; void* scan0; size_t reserved; };
extern "C" {
    __declspec(dllimport) int GdiplusStartup(size_t* token, const GdipStartupInput* input, void* output);
    __declspec(dllimport) int GdipLoadImageFromFile(const wchar_t* filename, void** image);
    __declspec(dllimport) int GdipGetImageWidth(void* image, unsigned int* width);
    __declspec(dllimport) int GdipGetImageHeight(void* image, unsigned int* height);
    __declspec(dllimport) int GdipBitmapLockBits(void* bitmap, const GdipRect* rect, unsigned int flags, int format, GdipBitmapData* data);
    __declspec(dllimport) int GdipBitmapUnlockBits(void* bitmap, GdipBitmapData* data);
    __declspec(dllimport) int GdipDisposeImage(void* image);
}

static bool DecodeWithGdiplus(const fs::path& file, Image& out) {
    static bool started = false;
    static size_t token = 0;
    if (!started) {
        GdipStartupInput in = { 1, nullptr, 0, 0 };
        if (GdiplusStartup(&token, &in, nullptr) != 0) return false;
        started = true;
    }
    std::wstring wp = file.wstring();
    void* bmp = nullptr;
    if (GdipLoadImageFromFile(wp.c_str(), &bmp) != 0 || !bmp) return false;
    unsigned int w = 0, h = 0;
    GdipGetImageWidth(bmp, &w);
    GdipGetImageHeight(bmp, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) { GdipDisposeImage(bmp); return false; }
    GdipRect r = { 0, 0, (int)w, (int)h };
    GdipBitmapData bd = { 0, 0, 0, 0, nullptr, 0 };
    // 0x26200A = 32 бита ARGB, 1 = только чтение
    if (GdipBitmapLockBits(bmp, &r, 1, 0x26200A, &bd) != 0 || bd.scan0 == nullptr) { GdipDisposeImage(bmp); return false; }
    Image img = GenImageColor((int)w, (int)h, BLANK);
    Color* dst = (Color*)img.data;
    for (unsigned int y = 0; y < h; y++) {
        const unsigned char* row = (const unsigned char*)bd.scan0 + (ptrdiff_t)y * bd.stride;
        for (unsigned int x = 0; x < w; x++) {
            Color c = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0], 255 };
            dst[y * w + x] = c;
        }
    }
    GdipBitmapUnlockBits(bmp, &bd);
    GdipDisposeImage(bmp);
    out = img;
    return true;
}

static bool DecodeBytesWithGdiplus(const Bytes& b, Image& out) {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) return false;
    tmp /= "musicplayer_cover.tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write((const char*)b.data(), (std::streamsize)b.size());
    }
    bool ok = DecodeWithGdiplus(tmp, out);
    fs::remove(tmp, ec);
    return ok;
}

static Image LoadImageFromBuffer(const unsigned char* data, int size) {
    Image none = { 0 };
    if (size < 8) return none;
    size_t off = FindImageStart(data, (size_t)size);
    if (off == (size_t)-1) return none;
    const unsigned char* d = data + off;
    int n = size - (int)off;
    const char* types[3];
    int count;
    if (d[0] == 0xFF) { types[0] = ".jpg"; types[1] = ".jpeg"; types[2] = "jpg"; count = 3; }
    else { types[0] = ".png"; types[1] = "png"; types[2] = ".PNG"; count = 3; }
    for (int i = 0; i < count; i++) {
        Image img = LoadImageFromMemory(types[i], d, n);
        if (img.data != nullptr) return img;
    }
    return none;
}

// Квадратная обложка со скруглёнными углами -> текстура
static Texture2D MakeCoverTexture(Image img) {
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) return Texture2D{ 0 };
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int side = (img.width < img.height) ? img.width : img.height;
    Rectangle crop = { (img.width - side) / 2.0f, (img.height - side) / 2.0f, (float)side, (float)side };
    ImageCrop(&img, crop);
    const int S = 256;
    ImageResize(&img, S, S);

    Rgb pal[3];
    bool palOk = ExtractPalette((const unsigned char*)img.data, S, S, pal);

    Color* px = (Color*)img.data;
    float rad = S * 0.06f;
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float fx = x + 0.5f, fy = y + 0.5f;
            float nx = fx < rad ? rad : (fx > S - rad ? S - rad : fx);
            float ny = fy < rad ? rad : (fy > S - rad ? S - rad : fy);
            float d = sqrtf((fx - nx) * (fx - nx) + (fy - ny) * (fy - ny));
            float a = Clampf(rad - d + 0.5f, 0.0f, 1.0f);
            if (a < 1.0f) px[y * S + x].a = (unsigned char)(px[y * S + x].a * a);
        }
    }

    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (tex.id != 0) {
        if (palOk) {
            for (int i = 0; i < 3; i++) { Color c = { pal[i].r, pal[i].g, pal[i].b, 255 }; g_coverPal[i] = c; }
            g_coverPalOk = true;
        }
        GenTextureMipmaps(&tex);
        SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    }
    return tex;
}

static Texture2D CoverFromBytes(const Bytes& b) {
    if (b.size() < 8) return Texture2D{ 0 };
    Image img = LoadImageFromBuffer(b.data(), (int)b.size());
    if (img.data == nullptr) {
        size_t off = FindImageStart(b.data(), b.size());
        if (off != (size_t)-1) {
            Bytes sub(b.begin() + off, b.end());
            DecodeBytesWithGdiplus(sub, img);   // запасной путь через Windows
        }
    }
    return MakeCoverTexture(img);
}

static bool IsImageExt(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (size_t i = 0; i < e.size(); i++) if (e[i] < 128) e[i] = (wchar_t)tolower((int)e[i]);
    return e == L".jpg" || e == L".jpeg" || e == L".png";
}

static Texture2D LoadCoverFromFile(const fs::path& imgPath) {
    std::error_code ec;
    if (!fs::exists(imgPath, ec)) return Texture2D{ 0 };
    Bytes b;
    if (!ReadChunk(imgPath, 0, 30u * 1024 * 1024, b)) return Texture2D{ 0 };
    return CoverFromBytes(b);
}

static Texture2D GetCover(HSTREAM stream, const std::wstring& trackPath) {
    (void)stream;
    g_coverNote.clear();
    g_coverPalOk = false;
    fs::path track(trackPath);
    std::string ext = track.extension().string();
    for (size_t i = 0; i < ext.size(); i++) ext[i] = (char)tolower((unsigned char)ext[i]);

    // 1. Обложка, встроенная в сам файл
    Bytes img;
    bool got = false;
    if (ext == ".flac") {
        got = CoverFromFlac(track, img);
    }
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac") {
        got = CoverFromM4a(track, img);
    }
    else {
        Bytes head;
        if (ReadChunk(track, 0, 10, head) && head.size() >= 10 && memcmp(head.data(), "ID3", 3) == 0) {
            size_t sz = (size_t)SyncSafe32(&head[6]) + 10;
            if (sz > 40u * 1024 * 1024) sz = 40u * 1024 * 1024;
            Bytes tag;
            if (ReadChunk(track, 0, sz, tag)) got = CoverFromId3(tag, img);
        }
    }
    bool decodeFailed = false;
    if (got) {
        Texture2D tex = CoverFromBytes(img);
        if (tex.id != 0) return tex;
        decodeFailed = true;
    }

    // 2. Картинка в папке с треком
    fs::path dir = track.parent_path();
    const char* names[] = { "cover", "folder", "front", "album", "artwork" };
    for (size_t n = 0; n < 5; n++) {
        const char* exts[] = { ".jpg", ".jpeg", ".png" };
        for (size_t k = 0; k < 3; k++) {
            Texture2D tex = LoadCoverFromFile(dir / (std::string(names[n]) + exts[k]));
            if (tex.id != 0) return tex;
        }
    }
    {
        const char* exts[] = { ".jpg", ".jpeg", ".png" };
        for (size_t k = 0; k < 3; k++) {
            fs::path cand = dir / track.stem();
            cand += exts[k];
            Texture2D tex = LoadCoverFromFile(cand);
            if (tex.id != 0) return tex;
        }
    }
    // 3. Любая картинка в папке (берём самую большую)
    {
        std::error_code ec;
        fs::path best; uintmax_t bestSize = 0; int seen = 0;
        fs::directory_iterator it(dir, ec), endIt;
        while (!ec && it != endIt && seen < 400) {
            std::error_code ec2;
            if (it->is_regular_file(ec2) && IsImageExt(it->path())) {
                uintmax_t sz = it->file_size(ec2);
                if (!ec2 && sz > bestSize) { bestSize = sz; best = it->path(); }
            }
            it.increment(ec);
            seen++;
        }
        if (!best.empty()) {
            Texture2D tex = LoadCoverFromFile(best);
            if (tex.id != 0) return tex;
            decodeFailed = true;
        }
    }
    g_coverNote = decodeFailed ? "не читается" : "нет в файле";
    return Texture2D{ 0 };
}


// Плагин bassflac.dll: без него FLAC играет через Windows и не перематывается
static void LoadBassPlugins() {
    std::vector<fs::path> found;
    std::error_code ec;
    found.push_back(fs::path(GetApplicationDirectory()) / "bassflac.dll");
    // запасной вариант: папка с библиотеками, версия x64
    fs::path root("C:/libs/bassflac24");
    if (fs::exists(root, ec)) {
        fs::recursive_directory_iterator it(root, ec), endIt;
        int seen = 0;
        while (!ec && it != endIt && seen < 400) {
            fs::path p = it->path();
            std::wstring name = p.filename().wstring(), dir = p.parent_path().filename().wstring();
            for (size_t i = 0; i < name.size(); i++) if (name[i] < 128) name[i] = (wchar_t)tolower((int)name[i]);
            for (size_t i = 0; i < dir.size(); i++) if (dir[i] < 128) dir[i] = (wchar_t)tolower((int)dir[i]);
            if (name == L"bassflac.dll" && dir == L"x64") found.push_back(p);
            it.increment(ec);
            seen++;
        }
    }
    for (size_t i = 0; i < found.size(); i++) {
        if (!fs::exists(found[i], ec)) continue;
        std::wstring w = found[i].wstring();
        if (BASS_PluginLoad(w.c_str(), BASS_UNICODE) != 0) { g_flacPlugin = true; return; }
    }
}

// =====================================================================
//  Текст песни: поиск (файл .lrc, кэш, теги, интернет), загрузка, показ
// =====================================================================
#pragma comment(lib, "winhttp.lib")
extern "C" {
    __declspec(dllimport) void* WinHttpOpen(const wchar_t* agent, unsigned int accessType, const wchar_t* proxy, const wchar_t* bypass, unsigned int flags);
    __declspec(dllimport) void* WinHttpConnect(void* session, const wchar_t* server, unsigned short port, unsigned int reserved);
    __declspec(dllimport) void* WinHttpOpenRequest(void* connect, const wchar_t* verb, const wchar_t* path, const wchar_t* version, const wchar_t* referrer, const wchar_t** acceptTypes, unsigned int flags);
    __declspec(dllimport) int WinHttpSendRequest(void* request, const wchar_t* headers, unsigned int headersLen, void* optional, unsigned int optionalLen, unsigned int totalLen, size_t context);
    __declspec(dllimport) int WinHttpReceiveResponse(void* request, void* reserved);
    __declspec(dllimport) int WinHttpQueryHeaders(void* request, unsigned int infoLevel, const wchar_t* name, void* buffer, unsigned int* bufferLen, unsigned int* index);
    __declspec(dllimport) int WinHttpReadData(void* request, void* buffer, unsigned int toRead, unsigned int* read);
    __declspec(dllimport) int WinHttpSetTimeouts(void* handle, int resolve, int connect, int send, int receive);
    __declspec(dllimport) int WinHttpCloseHandle(void* handle);
}

static bool HttpGetText(const std::wstring& host, const std::string& pathAndQuery, std::string& out) {
    out.clear();
    std::wstring wpath(pathAndQuery.begin(), pathAndQuery.end());   // после кодирования там только ASCII
    void* ses = WinHttpOpen(L"MusicPlayer/1.0 (personal player)", 0, nullptr, nullptr, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 4000, 5000, 5000, 8000);
    bool ok = false;
    void* con = WinHttpConnect(ses, host.c_str(), 443, 0);
    if (con) {
        void* req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, nullptr, nullptr, 0x00800000);   // HTTPS
        if (req) {
            if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                unsigned int status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(req, 19 | 0x20000000, nullptr, &status, &sz, nullptr);   // код ответа
                if (status == 200) {
                    char buf[8192];
                    unsigned int got = 0;
                    while (WinHttpReadData(req, buf, sizeof buf, &got) && got > 0) {
                        out.append(buf, got);
                        if (out.size() > 8u * 1024 * 1024) break;
                    }
                    ok = true;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

struct LyricsJob { unsigned int gen; std::string artist, title; double dur; fs::path cacheDir; std::string cacheKey; };
struct LyricsResult { bool ready = false; unsigned int gen = 0; bool ok = false; std::string text; };

// Общие данные для фонового потока живут до конца программы (специально не удаляются)
static std::mutex* g_lyMutex = new std::mutex();
static LyricsResult* g_lyResult = new LyricsResult();
static unsigned int g_lyGen = 0;

static void LyricsWorker(LyricsJob job) {
    LyricsResult res;
    res.gen = job.gen;
    std::string json;
    std::vector<std::string> queries;
    if (!job.artist.empty()) queries.push_back("/api/search?track_name=" + UrlEncode(job.title) + "&artist_name=" + UrlEncode(job.artist));
    else queries.push_back("/api/search?track_name=" + UrlEncode(job.title));
    queries.push_back("/api/search?q=" + UrlEncode(job.artist.empty() ? job.title : job.artist + " " + job.title));
    for (size_t i = 0; i < queries.size() && !res.ok; i++) {
        if (HttpGetText(L"lrclib.net", queries[i], json) && PickBestLyrics(json, job.dur, res.text)) res.ok = true;
    }
    if (res.ok) {   // запоминаем найденное, чтобы в следующий раз не искать
        std::error_code ec;
        fs::create_directories(job.cacheDir, ec);
        bool synced = res.text.find('[') != std::string::npos;
        fs::path f = job.cacheDir / (job.cacheKey + (synced ? ".lrc" : ".txt"));
        std::ofstream o(f, std::ios::binary);
        if (o) o.write(res.text.data(), (std::streamsize)res.text.size());
    }
    std::lock_guard<std::mutex> lock(*g_lyMutex);
    *g_lyResult = res;
    g_lyResult->ready = true;
}

static void LoadLyricsForTrack(const fs::path& track, const std::string& artistIn, const std::string& title, double dur) {
    g_ly = Lyrics();
    ++g_lyGen;
    g_ly.status = 3;
    std::string artist = artistIn;
    if (artist == "Неизвестный исполнитель") artist.clear();

    // 1. Файл .lrc рядом с песней
    Bytes b;
    fs::path lrc = track;
    lrc.replace_extension(".lrc");
    if (ReadChunk(lrc, 0, 2u * 1024 * 1024, b) && ApplyLyricsText(std::string(b.begin(), b.end()), "файл .lrc")) return;

    // 2. Кэш (то, что уже скачивали)
    fs::path cacheDir = fs::path(GetApplicationDirectory()) / "lyrics_cache";
    std::string key = LyricsCacheKey(artist, title, dur);
    const char* cexts[] = { ".lrc", ".txt" };
    for (size_t i = 0; i < 2; i++) {
        if (ReadChunk(cacheDir / (key + cexts[i]), 0, 2u * 1024 * 1024, b) && ApplyLyricsText(std::string(b.begin(), b.end()), "кэш")) return;
    }

    // 3. Текст внутри файла
    std::string ext = track.extension().string();
    for (size_t i = 0; i < ext.size(); i++) ext[i] = (char)tolower((unsigned char)ext[i]);
    std::string emb;
    if (ext == ".flac") {
        if (LyricsFromFlac(track, emb) && ApplyLyricsText(emb, "внутри файла")) return;
    }
    else {
        Bytes head;
        if (ReadChunk(track, 0, 10, head) && head.size() >= 10 && memcmp(head.data(), "ID3", 3) == 0) {
            size_t sz = (size_t)SyncSafe32(&head[6]) + 10;
            if (sz > 40u * 1024 * 1024) sz = 40u * 1024 * 1024;
            Bytes tag;
            if (ReadChunk(track, 0, sz, tag) && LyricsFromId3(tag, emb) && ApplyLyricsText(emb, "внутри файла")) return;
        }
    }

    // 4. Интернет (в фоне, чтобы окно не зависало)
    if (!g_onlineLyrics) { g_ly.status = 4; return; }
    if (title.empty()) return;
    g_ly.status = 1;
    LyricsJob job;
    job.gen = g_lyGen; job.artist = artist; job.title = title; job.dur = dur;
    job.cacheDir = cacheDir; job.cacheKey = key;
    std::thread(LyricsWorker, job).detach();
}

// Раз в кадр: забираем результат поиска из фонового потока
static void PollLyricsResult() {
    LyricsResult res;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(*g_lyMutex);
        if (g_lyResult->ready) { res = *g_lyResult; g_lyResult->ready = false; have = true; }
    }
    if (!have || res.gen != g_lyGen) return;
    if (!(res.ok && ApplyLyricsText(res.text, "интернет"))) g_ly.status = 3;
}

// Разбивка строк по ширине окна и подсчёт высоты
static void BuildLyricsLayout(float width, float size) {
    g_ly.layoutW = width;
    float lineH = size * 1.32f, gap = size * 0.55f, y = 0.0f;
    for (size_t i = 0; i < g_ly.lines.size(); i++) {
        LyricLine& L = g_ly.lines[i];
        L.seg.clear();
        if (L.text.empty()) { L.h = lineH * 0.5f; }
        else {
            std::string cur;
            size_t p = 0;
            while (p < L.text.size()) {
                size_t sp = L.text.find(' ', p);
                std::string word = (sp == std::string::npos) ? L.text.substr(p) : L.text.substr(p, sp - p);
                p = (sp == std::string::npos) ? L.text.size() : sp + 1;
                if (word.empty()) continue;
                std::string trial = cur.empty() ? word : cur + " " + word;
                if (!cur.empty() && TxtW(trial.c_str(), size) > width) { L.seg.push_back(cur); cur = word; }
                else cur = trial;
            }
            if (!cur.empty()) L.seg.push_back(cur);
            L.h = lineH * (float)L.seg.size();
        }
        L.y = y;
        y += L.h + gap;
    }
    g_ly.totalH = y;
}

static int LyricsActiveIndex(float pos) {
    if (!g_ly.synced) return -1;
    int idx = -1;
    for (int i = 0; i < (int)g_ly.lines.size(); i++) {
        if (g_ly.lines[i].t <= pos + 0.15f) idx = i; else break;
    }
    return idx;
}

// Рисует прокручиваемый текст: текущая строка светлая, остальные темнее
static void DrawLyricsView(Rectangle view, float alpha, float dt) {
    if (alpha <= 0.003f) return;
    const float fsz = 28.0f;
    const Color panelSolid = { 24, 26, 44, 255 };

    if (g_ly.lines.empty()) {
        const char* msg = "Нет текста";
        if (g_ly.status == 1) msg = "Ищу текст песни…";
        else if (g_ly.status == 3) msg = "Текст песни не найден";
        else if (g_ly.status == 4) msg = "Текст не найден (поиск в интернете выключен в настройках)";
        TxtC(msg, view.x + view.width / 2, view.y + view.height / 2 - 12, 20, Ma(C_DIM, alpha));
        return;
    }
    if (fabsf(g_ly.layoutW - view.width) > 0.5f) BuildLyricsLayout(view.width - 8.0f, fsz);
    if (g_ly.hl.size() != g_ly.lines.size()) g_ly.hl.assign(g_ly.lines.size(), 0.0f);

    int active = LyricsActiveIndex(A.pos);
    double now = GetTime();

    // Прокрутка: сама следует за песней, а колесо мыши даёт 3 секунды свободы
    if (CheckCollisionPointRec(GetMousePosition(), view) && !g_lock) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) { g_ly.scroll -= wheel * 70.0f; g_ly.freeUntil = now + 3.0; }
    }
    if (g_ly.synced && now > g_ly.freeUntil) {
        float target = (active >= 0) ? g_ly.lines[active].y + g_ly.lines[active].h / 2 - view.height * 0.40f : -view.height * 0.15f;
        if (g_ly.snap) { g_ly.scroll = target; g_ly.snap = false; }
        else g_ly.scroll += (target - g_ly.scroll) * Clampf(dt * 6.0f, 0.0f, 1.0f);
    }
    float lo = g_ly.synced ? -view.height * 0.45f : 0.0f;
    float hi = g_ly.synced ? g_ly.totalH - view.height * 0.2f : g_ly.totalH - view.height;
    if (hi < lo) hi = lo;
    g_ly.scroll = Clampf(g_ly.scroll, lo, hi);

    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    float lineH = fsz * 1.32f;
    for (int i = 0; i < (int)g_ly.lines.size(); i++) {
        LyricLine& L = g_ly.lines[i];
        float target = (g_ly.synced && i == active) ? 1.0f : 0.0f;
        g_ly.hl[i] = Approach(g_ly.hl[i], target, dt * 5.0f);
        float top = view.y + L.y - g_ly.scroll;
        if (top + L.h < view.y - 6 || top > view.y + view.height + 6 || L.seg.empty()) continue;

        float e = EaseInOutCubic(g_ly.hl[i]);
        float dimA = 0.78f;
        if (g_ly.synced) {
            float dist = (active >= 0) ? fabsf((float)(i - active)) : (float)(i + 1);
            dimA = Clampf(0.55f - 0.07f * (dist - 1.0f), 0.18f, 0.55f);
        }
        Color col = Ma(ColorLerp(Ma(C_DIM, dimA), C_TEXT, e), alpha);
        float x = view.x + 4.0f, cy = top + L.h / 2;
        float sc = 1.0f + 0.05f * e;
        rlPushMatrix();
        rlTranslatef(x, cy, 0.0f);
        rlScalef(sc, sc, 1.0f);
        rlTranslatef(-x, -cy, 0.0f);
        for (size_t k = 0; k < L.seg.size(); k++) Txt(L.seg[k].c_str(), x, top + k * lineH, fsz, col);
        rlPopMatrix();
    }
    EndScissorMode();

    // Плавное исчезновение у верхнего и нижнего края
    float fh = 56.0f;
    DrawRectangleGradientV((int)view.x, (int)view.y, (int)view.width, (int)fh, Ma(panelSolid, alpha), Ma(panelSolid, 0.0f));
    DrawRectangleGradientV((int)view.x, (int)(view.y + view.height - fh), (int)view.width, (int)fh, Ma(panelSolid, 0.0f), Ma(panelSolid, alpha));
}

// =====================================================================
//  Темы оформления (цвета фона и градиента) и сохранение настроек
// =====================================================================
struct Theme { const char* name; Color bg; Color c[3]; };
static const int THEME_COUNT = 8;
static const Theme THEMES[THEME_COUNT] = {
    { "Космос",{ 10, 12, 22, 255 },{ { 124, 92, 255, 255 },{ 64, 196, 255, 255 },{ 255, 92, 170, 255 } } },
    { "Закат",{ 20, 10, 14, 255 },{ { 255, 110, 64, 255 },{ 255, 60, 120, 255 },{ 255, 200, 80, 255 } } },
    { "Океан",{ 6, 14, 24, 255 },{ { 30, 120, 255, 255 },{ 0, 200, 200, 255 },{ 80, 255, 190, 255 } } },
    { "Лес",{ 8, 18, 12, 255 },{ { 40, 200, 110, 255 },{ 140, 230, 60, 255 },{ 40, 170, 170, 255 } } },
    { "Неон",{ 12, 6, 20, 255 },{ { 255, 0, 200, 255 },{ 0, 240, 255, 255 },{ 160, 80, 255, 255 } } },
    { "Огонь",{ 20, 8, 6, 255 },{ { 255, 60, 30, 255 },{ 255, 150, 20, 255 },{ 255, 220, 80, 255 } } },
    { "Лёд",{ 10, 16, 24, 255 },{ { 120, 190, 255, 255 },{ 200, 235, 255, 255 },{ 150, 150, 255, 255 } } },
    { "Графит",{ 14, 14, 16, 255 },{ { 150, 150, 160, 255 },{ 210, 210, 220, 255 },{ 100, 100, 115, 255 } } }
};

static void ApplyThemePreset(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    A.themeIdx = idx;
    for (int i = 0; i < 3; i++) A.hue[i] = ColorToHSV(THEMES[idx].c[i]).x / 360.0f;
}

static void UpdateThemeTargets() {
    if (g_adaptiveBg && g_coverPalOk) {   // цвета фона берём с обложки играющей песни
        for (int i = 0; i < 3; i++) g_tgtGlow[i] = g_coverPal[i];
        g_tgtBg = ColorFromHSV(ColorToHSV(g_coverPal[0]).x, 0.55f, 0.09f);
        return;
    }
    if (A.themeIdx >= 0 && A.themeIdx < THEME_COUNT) {
        g_tgtBg = THEMES[A.themeIdx].bg;
        for (int i = 0; i < 3; i++) g_tgtGlow[i] = THEMES[A.themeIdx].c[i];
    }
    else {
        for (int i = 0; i < 3; i++) g_tgtGlow[i] = ColorFromHSV(A.hue[i] * 360.0f, 0.75f, 1.0f);
        g_tgtBg = ColorFromHSV(A.hue[0] * 360.0f, 0.55f, 0.09f);
    }
}

static void SnapTheme() {
    UpdateThemeTargets();
    g_bgCol = g_tgtBg;
    for (int i = 0; i < 3; i++) g_glow[i] = g_tgtGlow[i];
}

static void UpdateTheme(float dt) {
    UpdateThemeTargets();
    float k = Clampf(dt * 6.0f, 0.0f, 1.0f);
    g_bgCol = ColorLerp(g_bgCol, g_tgtBg, k);
    for (int i = 0; i < 3; i++) g_glow[i] = ColorLerp(g_glow[i], g_tgtGlow[i], k);
}

static Color WaveColor(float f) {
    return (f < 0.5f) ? ColorLerp(g_glow[0], g_glow[1], f * 2.0f)
        : ColorLerp(g_glow[1], g_glow[2], (f - 0.5f) * 2.0f);
}

static std::string SettingsPath() { return std::string(GetApplicationDirectory()) + "settings.txt"; }

static void SaveSettings() {
    char buf[320];
    snprintf(buf, sizeof buf, "%d\n%.4f\n%.4f\n%.4f\n%d\n%d\n%.3f\n%.3f\n%d\n%d\n%d\n%d\n%.4f\n%.4f\n",
        A.themeIdx, A.hue[0], A.hue[1], A.hue[2], g_onlineLyrics ? 1 : 0, g_adaptiveBg ? 1 : 0,
        g_pan, (float)g_stereoWidth, g_loudness ? 1 : 0, g_crossfade ? 1 : 0, g_normalize ? 1 : 0, g_outMode,
        g_8dX, g_8dY);
    SaveFileText(SettingsPath().c_str(), buf);
}

static void LoadSettings() {
    char* text = LoadFileText(SettingsPath().c_str());
    if (!text) return;

    int idx = 0;
    float h0 = 0.72f, h1 = 0.55f, h2 = 0.93f;

    std::istringstream iss(text);
    if (iss >> idx >> h0 >> h1 >> h2) {
        int ol = 1;
        if (iss >> ol) g_onlineLyrics = (ol != 0);
        int ad = 0;
        if (iss >> ad) g_adaptiveBg = (ad != 0);
        float pan = 0.0f, wd = 1.0f;
        int ld = 0, cf = 0, nm = 0, om = 0;
        if (iss >> pan >> wd >> ld >> cf >> nm >> om) {
            g_pan = Clampf(pan, -1.0f, 1.0f);
            g_stereoWidth = Clampf(wd, 0.0f, 2.0f);
            g_loudness = (ld != 0);
            g_crossfade = (cf != 0);
            g_normalize = (nm != 0);
            g_outMode = (om >= 0 && om <= 2) ? om : 0;
            float dx = 0.0f, dy = 0.0f;
            if (iss >> dx >> dy) {
                g_8dX = Clampf(dx, -1.0f, 1.0f);
                g_8dY = Clampf(dy, -1.0f, 1.0f);
            }
        }
        if (idx >= 0 && idx < THEME_COUNT) {
            ApplyThemePreset(idx);
        }
        else {
            A.themeIdx = -1;
            A.hue[0] = Clampf(h0, 0.0f, 1.0f);
            A.hue[1] = Clampf(h1, 0.0f, 1.0f);
            A.hue[2] = Clampf(h2, 0.0f, 1.0f);
        }
    }
    UnloadFileText(text);
}

// =====================================================================
//  DSP: эквалайзер, темброблок, лимитер, тонокомпенсация, ширина стерео, нормализация
// =====================================================================
// ---- PURE-BEGIN ----
// Ширина стерео: средний сигнал оставляем, разностный умножаем (0 = моно, 1 = как есть, 2 = шире)
static void StereoWidthProcess(float* s, size_t frames, float width) {
    for (size_t i = 0; i < frames; i++) {
        float l = s[2 * i], r = s[2 * i + 1];
        float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
        s[2 * i] = mid + side;
        s[2 * i + 1] = mid - side;
    }
}

// Поправка громкости трека: средняя громкость -> цель -16 дБ, без перегрузки
static float NormGainFromStats(double sumSq, double count, float peak) {
    if (count < 1000.0 || sumSq <= 0.0) return 1.0f;
    double rmsDb = 10.0 * log10(sumSq / count);
    double gainDb = -16.0 - rmsDb;
    if (gainDb > 9.0) gainDb = 9.0;
    if (gainDb < -12.0) gainDb = -12.0;
    double gain = pow(10.0, gainDb / 20.0);
    if (peak > 0.0001f && peak * gain > 0.98) gain = 0.98 / peak;
    return (float)gain;
}
// ---- PURE-END ----

// Вызывается самим BASS для каждого куска звука (только для двухканальных песен)
static void __stdcall StereoWidthDSP(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    float w = g_stereoWidth;
    if (w > 0.99f && w < 1.01f) return;
    StereoWidthProcess((float*)buffer, length / (sizeof(float) * 2), w);
}

struct NormResult { bool ready = false; unsigned int gen = 0; float gain = 1.0f; };
static std::mutex* g_nrMutex = new std::mutex();
static NormResult* g_nrResult = new NormResult();
static unsigned int g_nrGen = 0;

// Фоновый анализ громкости: читаем весь трек «вхолостую» и считаем среднюю громкость
static void NormWorker(std::wstring path, unsigned int gen) {
    NormResult res;
    res.gen = gen;
    HSTREAM h = BASS_StreamCreateFile(0, path.c_str(), 0, 0, BASS_UNICODE | BASS_SAMPLE_FLOAT | BASS_STREAM_DECODE);
    if (h) {
        std::vector<float> buf(16384);
        double sumSq = 0.0, count = 0.0;
        float peak = 0.0f;
        for (;;) {
            DWORD got = BASS_ChannelGetData(h, buf.data(), (DWORD)(buf.size() * sizeof(float)));
            if (got == (DWORD)-1) break;
            got &= 0x7FFFFFFF;
            if (got == 0) break;
            size_t n = got / sizeof(float);
            for (size_t i = 0; i < n; i++) {
                float v = buf[i];
                sumSq += (double)v * (double)v;
                float av = fabsf(v);
                if (av > peak) peak = av;
            }
            count += (double)n;
        }
        BASS_StreamFree(h);
        res.gain = NormGainFromStats(sumSq, count, peak);
    }
    std::lock_guard<std::mutex> lock(*g_nrMutex);
    *g_nrResult = res;
    g_nrResult->ready = true;
}

// Запускает анализ громкости трека (если нормализация включена)
static void StartNormAnalysis(const std::wstring& path) {
    ++g_nrGen;
    g_normGainTarget = 1.0f;
    if (!g_normalize || path.empty()) return;
    std::thread(NormWorker, path, g_nrGen).detach();
}

// Раз в кадр: забираем результат анализа
static void PollNormResult() {
    NormResult r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(*g_nrMutex);
        if (g_nrResult->ready) { r = *g_nrResult; g_nrResult->ready = false; have = true; }
    }
    if (have && r.gen == g_nrGen && g_normalize) g_normGainTarget = r.gain;
}

// ---- PURE-BEGIN ----
// 8D Аудио: положение звука (x: -1 слева ... +1 справа, y: -1 спереди ... +1 сзади) -> параметры звука
struct Spatial { float pan; float backCutDb; float frontBoostDb; float volume; };

static Spatial SpatialFromPos(float x, float y) {
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y > 1.0f) y = 1.0f;
    float back = y > 0.0f ? y : 0.0f;
    float front = y < 0.0f ? -y : 0.0f;
    Spatial s;
    s.pan = x;                          // лево-право: баланс каналов
    s.backCutDb = -12.0f * back;        // сзади звук глуше: срезаем верхние частоты
    s.frontBoostDb = 3.0f * front;      // спереди чуть ярче: добавляем «присутствие»
    s.volume = 1.0f - 0.25f * back;     // сзади чуть тише
    return s;
}
// ---- PURE-END ----

static void SetupStreamFX() {
    if (!g_stream) return;

    for (int i = 0; i < 10; i++) {
        g_fxEq[i] = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 10 + i);
    }
    g_fxToneBass = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 20);
    g_fxToneTreb = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 21);
    g_fxLoudLow = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 22);
    g_fxLoudHigh = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 23);
    g_fx8dBack = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 24);
    g_fx8dFront = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_PARAMEQ, 25);
    g_fxLimiter = BASS_ChannelSetFX(g_stream, BASS_FX_DX8_COMPRESSOR, 30);
}

static void ApplyAllAudioFX() {
    if (!g_stream) return;

    Spatial sp = SpatialFromPos(g_8dX, g_8dY);

    float preampGain = A.eqOn ? powf(10.0f, A.eq[0] / 20.0f) : 1.0f;
    float normK = g_normalize ? g_normGain : 1.0f;
    float finalVol = Clampf(A.volume * preampGain * A.seekGain * g_xfadeGain * normK * sp.volume, 0.0f, 2.5f);
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, finalVol);
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_PAN, Clampf(g_pan + sp.pan, -1.0f, 1.0f));

    for (int i = 0; i < 10; i++) {
        if (!g_fxEq[i]) continue;
        BASS_DX8_PARAMEQ p;
        p.fCenter = EQ_FREQS[i];
        p.fBandwidth = 12.0f;
        p.fGain = A.eqOn ? Clampf(A.eq[i + 1], -15.0f, 15.0f) : 0.0f;
        BASS_FXSetParameters(g_fxEq[i], &p);
    }

    if (g_fxToneBass) {
        BASS_DX8_PARAMEQ pb;
        pb.fCenter = 65.0f;
        pb.fBandwidth = 18.0f;
        pb.fGain = A.toneOn ? (A.bass * 12.0f) : 0.0f;
        BASS_FXSetParameters(g_fxToneBass, &pb);
    }
    if (g_fxToneTreb) {
        BASS_DX8_PARAMEQ pt;
        pt.fCenter = 12000.0f;
        pt.fBandwidth = 18.0f;
        pt.fGain = A.toneOn ? (A.treble * 12.0f) : 0.0f;
        BASS_FXSetParameters(g_fxToneTreb, &pt);
    }

    // Тонокомпенсация: чем тише слушаем, тем больше добавляем низких и высоких частот
    float quiet = 1.0f - Clampf(A.volume, 0.0f, 1.0f);
    if (g_fxLoudLow) {
        BASS_DX8_PARAMEQ pl;
        pl.fCenter = 100.0f;
        pl.fBandwidth = 18.0f;
        pl.fGain = g_loudness ? Clampf(12.0f * quiet, 0.0f, 12.0f) : 0.0f;
        BASS_FXSetParameters(g_fxLoudLow, &pl);
    }
    if (g_fxLoudHigh) {
        BASS_DX8_PARAMEQ ph;
        ph.fCenter = 10000.0f;
        ph.fBandwidth = 18.0f;
        ph.fGain = g_loudness ? Clampf(6.0f * quiet, 0.0f, 6.0f) : 0.0f;
        BASS_FXSetParameters(g_fxLoudHigh, &ph);
    }

    // 8D Аудио: звук сзади глуше (срез верхних частот), звук спереди чуть ярче
    if (g_fx8dBack) {
        BASS_DX8_PARAMEQ pk;
        pk.fCenter = 7000.0f;
        pk.fBandwidth = 36.0f;
        pk.fGain = Clampf(sp.backCutDb, -15.0f, 0.0f);
        BASS_FXSetParameters(g_fx8dBack, &pk);
    }
    if (g_fx8dFront) {
        BASS_DX8_PARAMEQ pf;
        pf.fCenter = 3500.0f;
        pf.fBandwidth = 12.0f;
        pf.fGain = Clampf(sp.frontBoostDb, 0.0f, 15.0f);
        BASS_FXSetParameters(g_fx8dFront, &pf);
    }

    if (g_fxLimiter) {
        BASS_DX8_COMPRESSOR comp;
        comp.fGain = 0.0f;
        comp.fAttack = 1.0f;
        comp.fRelease = 120.0f;
        comp.fThreshold = A.limitOn ? -6.0f : 0.0f;
        comp.fRatio = A.limitOn ? 20.0f : 1.0f;
        comp.fPredelay = 2.0f;
        BASS_FXSetParameters(g_fxLimiter, &comp);
    }
}
// =====================================================================
//  Работа с UTF-8 / UTF-16 и аудиофайлами
// =====================================================================
static fs::path Utf8ToPath(const char* utf8) {
    return fs::path(reinterpret_cast<const char8_t*>(utf8));
}

static bool IsAudioExt(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" ||
        ext == ".m4a" || ext == ".aac" || ext == ".opus" || ext == ".wma";
}

static void ParseTrackName(const std::string& stem, std::string& outArtist, std::string& outTitle) {
    size_t sep = stem.find(" - ");
    if (sep != std::string::npos) {
        outArtist = stem.substr(0, sep);
        outTitle = stem.substr(sep + 3);
    }
    else {
        outArtist = "Неизвестный исполнитель";
        outTitle = stem;
    }
}

static void AddFolderToLibrary(const fs::path& dirPath) {
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return;

    std::wstring wpath = dirPath.wstring();
    for (int i = 0; i < (int)A.folders.size(); i++) {
        if (A.folders[i].path == wpath) {
            A.selectedFolder = i;
            return;
        }
    }

    FolderEntry entry;
    entry.path = wpath;
    entry.name = PathToUtf8(dirPath.filename());
    if (entry.name.empty()) entry.name = PathToUtf8(dirPath);

    std::error_code ec;
    for (const auto& it : fs::recursive_directory_iterator(dirPath, fs::directory_options::skip_permission_denied, ec)) {
        if (it.is_regular_file(ec) && IsAudioExt(it.path())) {
            Track tr;
            tr.fullPath = it.path().wstring();
            std::string stem = PathToUtf8(it.path().stem());
            ParseTrackName(stem, tr.artist, tr.title);
            entry.tracks.push_back(std::move(tr));
        }
    }

    if (!entry.tracks.empty()) {
        std::sort(entry.tracks.begin(), entry.tracks.end(), [](const Track& a, const Track& b) {
            return a.title < b.title;
            });
        A.folders.push_back(std::move(entry));
        A.selectedFolder = (int)A.folders.size() - 1;
        A.trackScroll = 0.0f;
    }
}

// =====================================================================
//  Управление воспроизведением BASS
// =====================================================================
static void PlayTrack(int folderIdx, int trackIdx) {
    if (folderIdx < 0 || folderIdx >= (int)A.folders.size()) return;
    auto& tracks = A.folders[folderIdx].tracks;
    if (trackIdx < 0 || trackIdx >= (int)tracks.size()) return;

    if (g_coverTex.id != 0) {
        UnloadTexture(g_coverTex);
        g_coverTex = { 0 };
    }

    bool fadeIn = false;
    if (g_stream) {
        // Плавный переход: прошлую песню не обрываем, а тихо уводим в ноль
        if (g_crossfade && A.playing && g_xfadeSec > 0.5f && BASS_ChannelIsActive(g_stream) == BASS_ACTIVE_PLAYING) {
            if (g_fadeStream) BASS_StreamFree(g_fadeStream);
            g_fadeStream = g_stream;
            g_fadeGain = 1.0f;
            fadeIn = true;
        }
        else {
            BASS_StreamFree(g_stream);
        }
        g_stream = 0;
        memset(g_fxEq, 0, sizeof(g_fxEq));
        g_fxToneBass = g_fxToneTreb = g_fxLimiter = 0;
        g_fxLoudLow = g_fxLoudHigh = 0;
    }

    const Track& tr = tracks[trackIdx];
    g_stream = BASS_StreamCreateFile(0, tr.fullPath.c_str(), 0, 0, BASS_UNICODE | BASS_SAMPLE_FLOAT | BASS_STREAM_PRESCAN);
    if (g_stream) {
        g_coverTex = GetCover(g_stream, tr.fullPath); // Извлекаем обложку

        // Ширина стерео работает только на двухканальном звуке
        {
            char safeInfo[128] = { 0 };
            if (BASS_ChannelGetInfo(g_stream, (BASS_CHANNELINFO*)safeInfo)) {
                DWORD chans = ((BASS_CHANNELINFO*)safeInfo)->chans;
                if (chans == 2) BASS_ChannelSetDSP(g_stream, StereoWidthDSP, nullptr, 100);
            }
        }

        g_xfadeGain = fadeIn ? 0.0f : 1.0f;
        g_curPath = tr.fullPath;
        g_normGain = 1.0f;
        StartNormAnalysis(tr.fullPath);

        SetupStreamFX();
        ApplyAllAudioFX();
        BASS_ChannelPlay(g_stream, 0);

        QWORD lenBytes = BASS_ChannelGetLength(g_stream, BASS_POS_BYTE);
        A.total = (float)BASS_ChannelBytes2Seconds(g_stream, lenBytes);
        A.pos = 0.0f;
        A.playing = true;
        A.playingFolder = folderIdx;
        A.playingTrack = trackIdx;
        A.currentTitle = tr.title;
        A.currentArtist = tr.artist;
        LoadLyricsForTrack(fs::path(tr.fullPath), tr.artist, tr.title, (double)A.total);
        {
            std::wstring e = fs::path(tr.fullPath).extension().wstring();
            for (size_t i = 0; i < e.size(); i++) if (e[i] < 128) e[i] = (wchar_t)tolower((int)e[i]);
            g_curIsFlac = (e == L".flac");
            g_seekErr = 0;
        }
    }
}

static void TogglePlayPause() {
    if (!g_stream) {
        int f = A.selectedFolder >= 0 ? A.selectedFolder : 0;
        if (!A.folders.empty() && !A.folders[f].tracks.empty()) {
            PlayTrack(f, 0);
        }
        return;
    }
    if (A.playing) {
        BASS_ChannelPause(g_stream);
        A.playing = false;
    }
    else {
        BASS_ChannelPlay(g_stream, 0);
        A.playing = true;
    }
}

static void PlayNext() {
    if (A.playingFolder < 0 || A.playingFolder >= (int)A.folders.size()) return;
    int count = (int)A.folders[A.playingFolder].tracks.size();
    if (count == 0) return;
    PlayTrack(A.playingFolder, (A.playingTrack + 1) % count);
}

static void PlayPrev() {
    if (A.playingFolder < 0 || A.playingFolder >= (int)A.folders.size()) return;
    if (A.pos > 3.0f && g_stream) {
        BASS_ChannelSetPosition(g_stream, 0, BASS_POS_BYTE);
        A.pos = 0.0f;
        return;
    }
    int count = (int)A.folders[A.playingFolder].tracks.size();
    if (count == 0) return;
    PlayTrack(A.playingFolder, (A.playingTrack - 1 + count) % count);
}

// ---------- Иконки ----------
static void Tri(Vector2 a, Vector2 b, Vector2 c, Color col) { DrawTriangle(a, b, c, col); DrawTriangle(a, c, b, col); }

static void IconMenu(float cx, float cy, Color c) {
    for (int i = -1; i <= 1; i++) DrawRectangleRounded(R(cx - 11, cy + i * 8 - 1.5f, 22, 3), 1.0f, 4, c);
}
static void IconSpeaker(float cx, float cy, Color c, float vol) {
    DrawRectangle((int)(cx - 12), (int)(cy - 5), 7, 10, c);
    Tri(V(cx - 5, cy - 5), V(cx + 3, cy - 12), V(cx + 3, cy + 12), c);
    Tri(V(cx - 5, cy - 5), V(cx + 3, cy + 12), V(cx - 5, cy + 5), c);
    if (vol > 0.05f) DrawRing(V(cx + 3, cy), 7, 9, -45, 45, 16, c);
    if (vol > 0.45f) DrawRing(V(cx + 3, cy), 13, 15, -45, 45, 16, c);
}
static void IconSun(float cx, float cy, Color c) {
    DrawCircleV(V(cx, cy), 6, c);
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        DrawLineEx(V(cx + cosf(a) * 10, cy + sinf(a) * 10), V(cx + cosf(a) * 15, cy + sinf(a) * 15), 2.5f, c);
    }
}
static void IconPlay(float cx, float cy, float s, Color c) {
    Tri(V(cx - s * 0.35f, cy - s * 0.5f), V(cx - s * 0.35f, cy + s * 0.5f), V(cx + s * 0.55f, cy), c);
}
static void IconPause(float cx, float cy, float s, Color c) {
    DrawRectangleRounded(R(cx - s * 0.4f, cy - s * 0.5f, s * 0.28f, s), 0.3f, 4, c);
    DrawRectangleRounded(R(cx + s * 0.12f, cy - s * 0.5f, s * 0.28f, s), 0.3f, 4, c);
}
static void IconPrev(float cx, float cy, Color c) {
    DrawRectangle((int)(cx - 10), (int)(cy - 9), 3, 18, c);
    Tri(V(cx + 10, cy - 9), V(cx - 6, cy), V(cx + 10, cy + 9), c);
}
static void IconNext(float cx, float cy, Color c) {
    Tri(V(cx - 10, cy - 9), V(cx - 10, cy + 9), V(cx + 6, cy), c);
    DrawRectangle((int)(cx + 7), (int)(cy - 9), 3, 18, c);
}
static void IconFolder(float x, float y, Color c) {
    DrawRectangleRounded(R(x, y + 3, 22, 15), 0.25f, 4, c);
    DrawRectangleRounded(R(x, y, 10, 6), 0.4f, 4, c);
}

// ---------- Вертикальный ползунок эквалайзера ----------
static bool VSlider(int id, float cx, float top, float bottom, float* val, float lo, float hi, bool enabled) {
    bool changed = false;
    Rectangle hit = R(cx - 22, top - 16, 44, bottom - top + 32);
    if (enabled) {
        if (BeginDrag(id, hit)) {
            float t = Clampf((GetMouseY() - top) / (bottom - top), 0, 1);
            float v = roundf((hi - t * (hi - lo)) * 2.0f) / 2.0f;
            if (v != *val) { *val = v; changed = true; }
        }
        if (Hit(hit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { if (*val != 0) changed = true; *val = 0; }
    }
    float ky = top + (hi - *val) / (hi - lo) * (bottom - top);
    float mid = top + (bottom - top) * 0.5f;
    Color on = enabled ? C_ACCENT : C_DIM;
    DrawRectangleRounded(R(cx - 2, top, 4, bottom - top), 1.0f, 6, C_PANEL2);
    DrawRectangleRounded(R(cx - 2, fminf(ky, mid), 4, fabsf(ky - mid)), 1.0f, 6, on);
    for (int k = 0; k <= 4; k++) {
        float ty = top + (bottom - top) * k / 4.0f;
        DrawRectangle((int)(cx - 16), (int)(ty - 1), 6, 2, Fade(C_DIM, 0.6f));
        DrawRectangle((int)(cx + 10), (int)(ty - 1), 6, 2, Fade(C_DIM, 0.6f));
    }
    DrawRectangleRounded(R(cx - 12, ky - 17, 24, 34), 0.5f, 8, enabled ? C_TEXT : C_DIM);
    DrawRectangle((int)(cx - 6), (int)(ky - 1), 12, 2, C_PANEL);
    return changed;
}

// ---------- Круглая ручка (0..1) ----------
static bool Knob(int id, float cx, float cy, float r, float* val, bool enabled) {
    bool changed = false;
    Rectangle hit = R(cx - r, cy - r, r * 2, r * 2);
    if (enabled) {
        if (BeginDrag(id, hit)) {
            float nv = Clampf(*val - GetMouseDelta().y * 0.006f, 0, 1);
            if (nv != *val) { *val = nv; changed = true; }
        }
        if (Hit(hit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            if (*val != 0) changed = true;
            *val = 0;
        }
    }
    Color on = enabled ? C_ACCENT : C_DIM;
    DrawCircleV(V(cx, cy), r, C_PANEL2);
    DrawRing(V(cx, cy), r - 8, r - 2, 135, 405, 48, Fade(C_DIM, 0.35f));
    if (*val > 0.005f) DrawRing(V(cx, cy), r - 8, r - 2, 135, 135 + 270 * *val, 48, on);
    DrawCircleV(V(cx, cy), r - 14, C_PANEL);
    float a = (135 + 270 * *val) * DEG2RAD;
    DrawLineEx(V(cx + cosf(a) * 10, cy + sinf(a) * 10), V(cx + cosf(a) * 24, cy + sinf(a) * 24), 4, enabled ? C_TEXT : C_DIM);
    return changed;
}

// =====================================================================
//  Фон и частотный спектроанализатор (FFT)
// =====================================================================
static void InitGlowTexture() {
    const int size = 512;
    Image img = GenImageColor(size, size, BLANK);
    Color* pixels = (Color*)img.data;
    float half = size * 0.5f;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (x - half) / half;
            float dy = (y - half) / half;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist < 1.0f) {
                float t = 1.0f - dist;
                float alpha = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                pixels[y * size + x] = { 255, 255, 255, (unsigned char)(alpha * 255.0f) };
            }
        }
    }
    g_glowTex = LoadTextureFromImage(img);
    SetTextureFilter(g_glowTex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
}

static void DrawBackdrop(float t, float level, float glowAlpha) {
    ClearBackground(g_bgCol);
    if (glowAlpha <= 0.001f) return;
    const Color cols[3] = { g_glow[0], g_glow[1], g_glow[2] };
    Rectangle src = { 0, 0, (float)g_glowTex.width, (float)g_glowTex.height };

    BeginBlendMode(BLEND_ADDITIVE);
    for (int b = 0; b < 3; b++) {
        float x = A.W * (0.25f + 0.25f * b) + sinf(t * 0.3f + b * 2.1f) * A.W * 0.14f;
        float y = A.H * (0.35f + 0.15f * b) + cosf(t * 0.25f + b * 1.7f) * A.H * 0.16f;
        float rad = (340.0f + 70.0f * b) * (1.0f + 0.18f * level);

        Rectangle dst = { x - rad, y - rad, rad * 2.0f, rad * 2.0f };
        DrawTexturePro(g_glowTex, src, dst, V(0, 0), 0.0f, Fade(cols[b], 0.32f * glowAlpha));
    }
    EndBlendMode();
}

static void DrawSpectrumWave(float scale, float alpha) {
    if (alpha <= 0.001f) return;
    Vector2 pts[SPEC_BANDS];
    float baseH = A.H * 0.03f;
    float maxH = A.H * 0.38f * scale;

    for (int i = 0; i < SPEC_BANDS; i++) {
        float frac = (float)i / (float)(SPEC_BANDS - 1);
        float x = frac * (float)A.W;

        float v = A.spectrum[i] * 0.5f;
        if (i > 0) v += A.spectrum[i - 1] * 0.25f; else v += A.spectrum[i] * 0.25f;
        if (i < SPEC_BANDS - 1) v += A.spectrum[i + 1] * 0.25f; else v += A.spectrum[i] * 0.25f;

        float h = baseH + v * maxH;
        pts[i] = { x, (float)A.H - h };
    }

    rlDrawRenderBatchActive();
    rlBegin(RL_TRIANGLES);

    for (int i = 0; i < SPEC_BANDS - 1; i++) {
        float f0 = (float)i / (float)(SPEC_BANDS - 1);
        float f1 = (float)(i + 1) / (float)(SPEC_BANDS - 1);

        Color c0 = WaveColor(f0);
        Color c1 = WaveColor(f1);

        Vector2 top0 = pts[i];
        Vector2 top1 = pts[i + 1];
        Vector2 bot0 = { top0.x, (float)A.H };
        Vector2 bot1 = { top1.x, (float)A.H };

        Color c0_top = { c0.r, c0.g, c0.b, (unsigned char)(100 * alpha) };
        Color c1_top = { c1.r, c1.g, c1.b, (unsigned char)(100 * alpha) };
        Color c0_bot = { c0.r, c0.g, c0.b, 0 };
        Color c1_bot = { c1.r, c1.g, c1.b, 0 };

        rlColor4ub(c0_top.r, c0_top.g, c0_top.b, c0_top.a); rlVertex2f(top0.x, top0.y);
        rlColor4ub(c0_bot.r, c0_bot.g, c0_bot.b, c0_bot.a); rlVertex2f(bot0.x, bot0.y);
        rlColor4ub(c1_top.r, c1_top.g, c1_top.b, c1_top.a); rlVertex2f(top1.x, top1.y);

        rlColor4ub(c1_top.r, c1_top.g, c1_top.b, c1_top.a); rlVertex2f(top1.x, top1.y);
        rlColor4ub(c0_bot.r, c0_bot.g, c0_bot.b, c0_bot.a); rlVertex2f(bot0.x, bot0.y);
        rlColor4ub(c1_bot.r, c1_bot.g, c1_bot.b, c1_bot.a); rlVertex2f(bot1.x, bot1.y);
    }
    rlEnd();

    for (int i = 0; i < SPEC_BANDS - 1; i++) {
        float f0 = (float)i / (float)(SPEC_BANDS - 1);
        Color c0 = WaveColor(f0);
        Vector2 top0 = pts[i];
        Vector2 top1 = pts[i + 1];
        DrawLineEx(top0, top1, 3.0f, Fade(c0, 0.92f * alpha));
    }
}

// =====================================================================
//  Левая панель (меню и громкость)
// =====================================================================
static void Sidebar() {
    if (A.viewSidebar <= 0.001f) return;
    float alpha = A.viewSidebar;
    float offsetX = (1.0f - alpha) * -80.0f;
    Rectangle p = R(16 + offsetX, 16, 60, A.H - 32.0f);
    DrawRectangleRounded(p, 0.5f, 16, Fade(C_PANEL, alpha));

    Rectangle mb = R(p.x + 6, p.y + 8, 48, 48);
    if (Hit(mb) || A.menuOpen) DrawRectangleRounded(mb, 0.5f, 12, Fade(C_PANEL2, alpha));
    IconMenu(mb.x + 24, mb.y + 24, Fade(C_TEXT, alpha));
    if (Pressed(mb)) { A.menuOpen = !A.menuOpen; A.volOpen = false; A.presetOpen = false; }

    Rectangle vb = R(p.x + 6, p.y + p.height - 56, 48, 48);
    if (Hit(vb) || A.volOpen) DrawRectangleRounded(vb, 0.5f, 12, Fade(C_PANEL2, alpha));
    IconSpeaker(vb.x + 24, vb.y + 24, Fade(C_TEXT, alpha), A.volume);
    if (Pressed(vb)) { A.volOpen = !A.volOpen; A.menuOpen = false; A.presetOpen = false; }
}

// =====================================================================
//  Экран плеера
// =====================================================================
struct ViewFlags { bool sidebar, cover, bar, wave, glow; };
static const ViewFlags VIEW_MODES[4] = {
    { true,  true,  true,  true,  true },{ false, true,  false, true,  true },{ false, true,  false, false, true },{ false, true,  false, false, false } };

// Кнопка режимов: три полоски и три точки (точки плавно загораются, кнопка «нажимается»)
static void ViewModeButton(Rectangle r) {
    float u = 1.0f - A.viewPulse;                                  // 0 -> 1 после нажатия
    float dip = (A.viewPulse > 0.0f) ? sinf(u * PI) : 0.0f;        // 0 -> 1 -> 0
    float sc = 1.0f - 0.09f * dip;
    Vector2 c = V(r.x + r.width / 2, r.y + r.height / 2);

    rlPushMatrix();
    rlTranslatef(c.x, c.y, 0.0f);
    rlScalef(sc, sc, 1.0f);
    rlTranslatef(-c.x, -c.y, 0.0f);

    DrawRectangleRounded(r, 0.35f, 12, Hit(r) ? ColorBrightness(C_PANEL2, 0.1f) : Fade(C_PANEL, 0.85f));
    float cx = c.x, bot = r.y + 9 + 19, k = 1.0f + 0.30f * dip;   // полоски «подпрыгивают» при нажатии
    DrawRectangleRounded(R(cx - 13, bot - 12 * k, 3, 12 * k), 1.0f, 4, C_TEXT);
    DrawRectangleRounded(R(cx - 1.5f, bot - 19 * k, 3, 19 * k), 1.0f, 4, C_TEXT);
    DrawRectangleRounded(R(cx + 10, bot - 15 * k, 3, 15 * k), 1.0f, 4, C_TEXT);
    for (int i = 0; i < 3; i++) {
        float e = EaseInOutCubic(A.viewDots[i]);
        Vector2 dc = V(cx - 11.5f + i * 11.5f, r.y + r.height - 9);
        if (e > 0.01f) DrawCircleV(dc, 6.5f * e, Fade(WHITE, 0.20f * e));
        DrawCircleV(dc, 3.0f + 0.8f * e, ColorLerp(Fade(C_DIM, 0.55f), WHITE, e));
    }
    rlPopMatrix();
}

// Плавные переходы между режимами: у каждого слоя своя длительность, поэтому всё происходит волной
static void UpdateViewAnim(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    bool onPlayer = (A.screen == 0);
    float tSide = (onPlayer && A.viewMode > 0) ? 0.0f : 1.0f;
    float tWave = (onPlayer && A.viewMode > 1) ? 0.0f : 1.0f;
    float tGlow = (onPlayer && A.viewMode > 2) ? 0.0f : 1.0f;

    A.rawSidebar = Approach(A.rawSidebar, tSide, dt / 0.55f);
    A.rawBar = Approach(A.rawBar, tSide, dt / 0.50f);
    A.rawWave = Approach(A.rawWave, tWave, dt / 0.80f);
    A.rawGlow = Approach(A.rawGlow, tGlow, dt / 0.90f);
    A.viewSidebar = EaseInOutCubic(A.rawSidebar);
    A.viewBar = EaseInOutCubic(A.rawBar);
    A.viewWave = EaseInOutCubic(A.rawWave);
    A.viewGlow = EaseInOutCubic(A.rawGlow);

    for (int i = 0; i < 3; i++) A.viewDots[i] = Approach(A.viewDots[i], A.viewMode > i ? 1.0f : 0.0f, dt / 0.30f);
    A.viewPulse = Approach(A.viewPulse, 0.0f, dt / 0.45f);

    // Окно с текстом песни закрывается, если включён один из режимов кнопки или открыт другой экран
    if (!onPlayer || A.viewMode > 0) A.lyricsOpen = false;
    A.rawLyrics = Approach(A.rawLyrics, A.lyricsOpen ? 1.0f : 0.0f, dt / 0.70f);
}

static void ScreenPlayer() {
    float dt = GetFrameTime();
    float eL = EaseInOutCubic(A.rawLyrics);   // 0 = обычное окно, 1 = окно с текстом песни
    float cc = EaseInOutCubic(Clampf((1.0f - A.rawBar - 0.10f) / 0.90f, 0.0f, 1.0f));   // 0 = обложка над панелью, 1 = по центру экрана
    float barA = A.viewBar;

    DrawSpectrumWave(1.0f + 0.45f * cc, A.viewWave);

    // Кнопка режимов (справа вверху)
    Rectangle vb = R(A.W - 84.0f, 18, 64, 52);
    ViewModeButton(vb);
    if (Pressed(vb)) { A.viewMode = (A.viewMode + 1) % 4; A.viewPulse = 1.0f; A.menuOpen = A.volOpen = A.presetOpen = false; }

    // ----- раскладка 0: обычное окно плеера -----
    float areaX = 96.0f * A.viewSidebar, areaW = A.W - areaX;
    float pw = Clampf(areaW - 80, 360, 600), ph = 180;
    float px = areaX + (areaW - pw) / 2, py = A.H / 2.0f - ph / 2 + 40;
    float offY = (1.0f - barA) * 40.0f;
    float coverTop0 = Lerpf(py - 112, A.H / 2.0f - 62, cc);

    // ----- раскладка 1: окно с текстом песни (обложка и управление слева, текст справа) -----
    Rectangle P1 = R(areaX, 16, A.W - areaX - 16.0f, A.H - 32.0f);
    float lw = Clampf(P1.width * 0.36f, 300, 400), cw = lw - 56, x0 = P1.x + 28;
    float cs = Clampf(fminf(cw, P1.height - 262), 120, 340);
    float coverY1 = P1.y + 28;
    float titleY1 = coverY1 + cs + 20, artistY1 = titleY1 + 30, seekY1 = artistY1 + 52, btnY1 = seekY1 + 70;

    // ----- плавный переход между раскладками -----
    auto mix = [&](float a, float b) { return Lerpf(a, b, eL); };
    Rectangle P = R(mix(px, P1.x), mix(py + offY, P1.y), mix(pw, P1.width), mix(ph, P1.height));
    Rectangle C = R(mix(px + pw * 0.5f - 50, x0 + (cw - cs) / 2), mix(coverTop0 + 12, coverY1), mix(100, cs), mix(100, cs));
    float mx = 15.0f * (1.0f - eL), my = 12.0f * (1.0f - eL);          // поля вкладки вокруг картинки
    Rectangle tab = R(C.x - mx, C.y - my, C.width + 2 * mx, C.height + 2 * my);
    float titleX = mix(px + 28, x0), titleY = mix(py + offY + 16, titleY1), titleW = mix(pw - 56, cw), titleSz = mix(24, 22);
    float artistY = mix(py + offY + 46, artistY1), artistSz = mix(18, 17);
    float sx = mix(px + 28, x0), sw = mix(pw - 56, cw), sy = mix(py + offY + 94, seekY1);
    float bcx = mix(px + pw / 2, x0 + cw / 2), bcy = mix(py + offY + 146, btnY1), gap = mix(96, 84);
    float lbx = mix(px + pw - 46, P1.x + P1.width - 46), lby = mix(py + offY + 146, P1.y + P1.height - 46);

    // ----- увеличение при наведении мышки -----
    float hov = EaseOutCubic(A.playerHover) * (1.0f - eL);
    float sp = 1.0f + 0.045f * hov;                                        // панель
    float sc = (1.0f + 0.6f * cc) * (1.0f + 0.045f * hov);                 // обложка (крупнее, когда панели нет)
    Vector2 pc = V(P.x + P.width / 2, P.y + P.height / 2);
    Vector2 cvc = V(tab.x + tab.width / 2, tab.y + tab.height / 2);
    Vector2 m = GetMousePosition();
    bool overTab = CheckCollisionPointRec(m, R(cvc.x - tab.width * sc / 2, cvc.y - tab.height * sc / 2, tab.width * sc, tab.height * sc));
    bool overPanel = barA > 0.5f && CheckCollisionPointRec(m, R(pc.x - P.width * sp / 2, pc.y - P.height * sp / 2, P.width * sp, P.height * sp));
    bool over = !g_lock && (overTab || overPanel || g_drag == 10);
    A.playerHover = Clampf(A.playerHover + (over ? 1.0f : -1.0f) * dt * 7.0f, 0.0f, 1.0f);

    // 1. Фон вкладки обложки (исчезает, когда обложка уходит внутрь большого окна)
    if (eL < 0.999f) {
        rlPushMatrix();
        rlTranslatef(cvc.x, cvc.y, 0.0f);
        rlScalef(sc, sc, 1.0f);
        rlTranslatef(-cvc.x, -cvc.y, 0.0f);
        DrawRectangleRounded(tab, 0.18f, 10, C_PANEL);
        rlPopMatrix();
    }

    // 2. Окно плеера: название, перемотка, кнопки и кнопка текста песни
    if (barA > 0.001f) {
        rlPushMatrix();
        rlTranslatef(pc.x, pc.y, 0.0f);
        rlScalef(sp, sp, 1.0f);
        rlTranslatef(-pc.x, -pc.y, 0.0f);
        g_xOn = true; g_xS = sp; g_xC = pc;

        float pa = barA;
        float rr = Clampf(28.8f / fminf(P.width, P.height), 0.02f, 1.0f);   // скругление примерно одинаковое по размеру
        DrawRectangleRounded(P, rr, 16, Ma(C_PANEL, pa));
        TxtClipped(A.currentTitle, titleX, titleY, titleW, titleSz, Ma(C_TEXT, pa));
        TxtClipped(A.currentArtist, titleX, artistY, titleW, artistSz, Ma(C_DIM, pa));

        // Перемотка: пока тянем ползунок, звук затихает; отпустили - песня продолжается с этого места
        if (BeginDrag(10, R(sx - 8, sy - 14, sw + 16, 28)) && A.total > 0.0f) {
            A.seeking = true;
            A.pos = Clampf((MousePos().x - sx) / sw, 0, 1) * A.total;
        }
        else if (A.seeking) {
            A.seeking = false;
            if (g_stream) {
                float target = fminf(A.pos, fmaxf(A.total - 0.3f, 0.0f));
                QWORD bytePos = BASS_ChannelSeconds2Bytes(g_stream, target);
                int seekOk = BASS_ChannelSetPosition(g_stream, bytePos, BASS_POS_BYTE | BASS_POS_FLUSH);
                if (!seekOk) seekOk = BASS_ChannelSetPosition(g_stream, bytePos, BASS_POS_BYTE);
                g_seekErr = seekOk ? 0 : BASS_ErrorGetCode();
                if (seekOk) { A.pos = target; g_seekTarget = target; g_seekVerifyAt = GetTime() + 0.45; }
            }
        }
        float frac = (A.total > 0.0f) ? Clampf(A.pos / A.total, 0.0f, 1.0f) : 0.0f;
        DrawRectangleRounded(R(sx, sy - 3, sw, 6), 1.0f, 6, Ma(C_PANEL2, pa));
        DrawRectangleRounded(R(sx, sy - 3, sw * frac, 6), 1.0f, 6, Ma(C_ACCENT, pa));
        DrawCircleV(V(sx + sw * frac, sy), 8, Ma(C_TEXT, pa));
        Txt(FmtTime(A.pos).c_str(), sx, sy + 12, 16, Ma(C_DIM, pa));
        std::string tot = FmtTime(A.total);
        Txt(tot.c_str(), sx + sw - TxtW(tot.c_str(), 16), sy + 12, 16, Ma(C_DIM, pa));

        // Назад / пауза / дальше
        Rectangle pr = R(bcx - gap - 20, bcy - 20, 40, 40), nx = R(bcx + gap - 20, bcy - 20, 40, 40), pb = R(bcx - 24, bcy - 24, 48, 48);
        if (Hit(pr)) DrawCircleV(V(bcx - gap, bcy), 20, Ma(C_PANEL2, pa));
        if (Hit(nx)) DrawCircleV(V(bcx + gap, bcy), 20, Ma(C_PANEL2, pa));
        IconPrev(bcx - gap, bcy, Ma(C_TEXT, pa));
        IconNext(bcx + gap, bcy, Ma(C_TEXT, pa));
        DrawCircleV(V(bcx, bcy), 24, Ma(Hit(pb) ? ColorBrightness(C_ACCENT, 0.15f) : C_ACCENT, pa));
        if (A.playing) IconPause(bcx, bcy, 20, Ma(C_TEXT, pa)); else IconPlay(bcx, bcy, 22, Ma(C_TEXT, pa));
        if (Pressed(pb)) TogglePlayPause();
        if (Pressed(pr)) PlayPrev();
        if (Pressed(nx)) PlayNext();

        // Кнопка «Тт»: открыть/закрыть текст песни
        Rectangle lb = R(lbx - 18, lby - 18, 36, 36);
        Color lbc = A.lyricsOpen ? C_ACCENT : C_PANEL2;
        if (Hit(lb)) lbc = ColorBrightness(lbc, 0.15f);
        DrawRectangleRounded(lb, 0.4f, 10, Ma(lbc, pa));
        TxtC("Тт", lbx, lby - 12, 18, Ma(C_TEXT, pa));
        if (Pressed(lb) && A.viewBar > 0.9f) A.lyricsOpen = !A.lyricsOpen;

        g_xOn = false;
        rlPopMatrix();
    }

    // 3. Картинка обложки (поверх окна: в обычном виде над ним, в режиме текста - слева внутри)
    {
        rlPushMatrix();
        rlTranslatef(cvc.x, cvc.y, 0.0f);
        rlScalef(sc, sc, 1.0f);
        rlTranslatef(-cvc.x, -cvc.y, 0.0f);
        DrawRectangleRounded(C, 0.12f, 8, C_PANEL2);
        if (g_coverTex.id != 0) {
            Rectangle src = { 0, 0, (float)g_coverTex.width, (float)g_coverTex.height };
            DrawTexturePro(g_coverTex, src, C, V(0, 0), 0.0f, WHITE);
        }
        else {
            float k = C.width / 100.0f;
            TxtC("Обложка", C.x + C.width / 2, C.y + C.height / 2 - 10 * k, 16 * k, C_DIM);
            if (!g_coverNote.empty()) TxtC(g_coverNote.c_str(), C.x + C.width / 2, C.y + C.height / 2 + 10 * k, 12 * k, Fade(C_DIM, 0.7f));
        }
        rlPopMatrix();
    }

    // 4. Текст песни справа (появляется, когда окно почти доехало)
    float lyA = Clampf((eL - 0.55f) / 0.45f, 0.0f, 1.0f);
    lyA = lyA * lyA * (3.0f - 2.0f * lyA);
    if (lyA > 0.003f) DrawLyricsView(R(P1.x + lw + 8, P1.y + 70, P1.width - lw - 36, P1.height - 146), lyA, dt);

    {
        std::string st = g_bassOk ? "ТЕСТОВАЯ ВЕРСИЯ!!! НЕ ФИНАЛЬНАЯ!!!" : "BASS: ошибка";
        if (g_curIsFlac && !g_flacPlugin) st += "  |  FLAC без плагина bassflac.dll";
        if (g_seekErr > 0) st += "  |  перемотка не удалась, код " + std::to_string(g_seekErr);
        else if (g_seekErr < 0) st += "  |  перемотка не применилась (формат не поддерживает)";
        Txt(st.c_str(), A.W - 16.0f - TxtW(st.c_str(), 14), A.H - 28.0f, 14, Fade(C_DIM, 0.7f));
    }
}

// =====================================================================
//  Экран папок (Drag & Drop + плейлист)
// =====================================================================
static void ScreenFolders() {
    float L = 96;
    Rectangle area = R(L, 16, A.W - L - 16.0f, A.H - 32.0f);
    DrawRectangleRounded(area, 0.05f, 16, C_PANEL);
    Txt("Папки с музыкой", area.x + 28, area.y + 20, 28, C_TEXT);

    if (A.folders.empty()) {
        Rectangle dropZone = R(area.x + 40, area.y + 80, area.width - 80, area.height - 120);
        DrawRectangleRounded(dropZone, 0.08f, 16, Fade(C_PANEL2, 0.45f));
        DrawRectangleRoundedLinesEx(dropZone, 0.08f, 16, 2.0f, Fade(C_ACCENT, 0.6f));
        TxtC("Перетащите сюда папку с музыкой", dropZone.x + dropZone.width / 2, dropZone.y + dropZone.height / 2 - 24, 24, C_TEXT);
        TxtC("Поддерживаются MP3, FLAC, WAV, OGG, M4A, OPUS", dropZone.x + dropZone.width / 2, dropZone.y + dropZone.height / 2 + 12, 18, C_DIM);
        return;
    }

    float leftW = 280;
    Rectangle fBox = R(area.x + 24, area.y + 72, leftW, area.height - 96);
    DrawRectangleRounded(fBox, 0.06f, 12, Fade(C_BG, 0.45f));

    Vector2 mouse = GetMousePosition();
    float wheel = GetMouseWheelMove();

    BeginScissorMode((int)fBox.x, (int)fBox.y, (int)fBox.width, (int)fBox.height);
    float itemH = 54.0f;
    float maxFScroll = fmaxf(0.0f, A.folders.size() * itemH - fBox.height + 16.0f);
    if (CheckCollisionPointRec(mouse, fBox) && !g_lock) {
        A.folderScroll = Clampf(A.folderScroll - wheel * 35.0f, 0.0f, maxFScroll);
    }

    for (int i = 0; i < (int)A.folders.size(); i++) {
        Rectangle row = R(fBox.x + 8, fBox.y + 8 + i * itemH - A.folderScroll, fBox.width - 16, 46);
        bool sel = (A.selectedFolder == i);
        bool hov = !g_lock && CheckCollisionPointRec(mouse, row) && CheckCollisionPointRec(mouse, fBox);
        if (sel || hov) {
            DrawRectangleRounded(row, 0.3f, 10, sel ? C_ACCENT : C_PANEL2);
        }
        IconFolder(row.x + 12, row.y + 14, sel ? C_TEXT : C_ACC2);
        TxtClipped(A.folders[i].name, row.x + 42, row.y + 7, row.width - 54, 18, C_TEXT);
        char cnt[32];
        snprintf(cnt, sizeof cnt, "Треков: %d", (int)A.folders[i].tracks.size());
        Txt(cnt, row.x + 42, row.y + 26, 14, sel ? Fade(C_TEXT, 0.85f) : C_DIM);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            A.selectedFolder = i;
            A.trackScroll = 0.0f;
        }
    }
    EndScissorMode();

    TxtC("Перетащите ещё папку в окно", fBox.x + fBox.width / 2, fBox.y + fBox.height - 24, 14, Fade(C_DIM, 0.65f));

    Rectangle tBox = R(fBox.x + leftW + 16, fBox.y, area.width - leftW - 64, fBox.height);
    DrawRectangleRounded(tBox, 0.06f, 12, Fade(C_BG, 0.45f));

    if (A.selectedFolder >= 0 && A.selectedFolder < (int)A.folders.size()) {
        const auto& tracks = A.folders[A.selectedFolder].tracks;
        BeginScissorMode((int)tBox.x, (int)tBox.y, (int)tBox.width, (int)tBox.height);

        float trH = 52.0f;
        float maxTScroll = fmaxf(0.0f, tracks.size() * trH - tBox.height + 16.0f);
        if (CheckCollisionPointRec(mouse, tBox) && !g_lock) {
            A.trackScroll = Clampf(A.trackScroll - wheel * 42.0f, 0.0f, maxTScroll);
        }

        for (int i = 0; i < (int)tracks.size(); i++) {
            float ry = tBox.y + 8 + i * trH - A.trackScroll;
            if (ry + trH < tBox.y || ry > tBox.y + tBox.height) continue;

            Rectangle row = R(tBox.x + 8, ry, tBox.width - 16, 46);
            bool isPlayingThis = (A.playingFolder == A.selectedFolder && A.playingTrack == i);
            bool hov = !g_lock && CheckCollisionPointRec(mouse, row) && CheckCollisionPointRec(mouse, tBox);

            if (isPlayingThis) {
                DrawRectangleRounded(row, 0.25f, 10, Fade(C_ACCENT, 0.35f));
                DrawRectangleRoundedLinesEx(row, 0.25f, 10, 1.5f, C_ACCENT);
            }
            else if (hov) {
                DrawRectangleRounded(row, 0.25f, 10, C_PANEL2);
            }

            char num[16];
            snprintf(num, sizeof num, "%d", i + 1);
            TxtC(num, row.x + 22, row.y + 14, 16, isPlayingThis ? C_ACC2 : C_DIM);

            TxtClipped(tracks[i].title, row.x + 48, row.y + 6, row.width - 64, 18, isPlayingThis ? C_ACC2 : C_TEXT);
            TxtClipped(tracks[i].artist, row.x + 48, row.y + 25, row.width - 64, 14, C_DIM);

            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                PlayTrack(A.selectedFolder, i);
            }
        }
        EndScissorMode();
    }
}

// =====================================================================
//  Экран эквалайзера // =====================================================================
static void ScreenEq() {
    float L = 96;
    Rectangle area = R(L, 16, A.W - L - 16.0f, A.H - 32.0f);
    DrawRectangleRounded(area, 0.05f, 16, C_PANEL);
    Txt("Эквалайзер", area.x + 28, area.y + 20, 28, C_TEXT);

    float ctrlW = 310;
    float sx0 = area.x + 36, sx1 = area.x + area.width - ctrlW - 10;
    float slot = (sx1 - sx0) / 11.0f;
    float top = area.y + 100, bottom = area.y + area.height - 100;
    static const char* labels[11] = { "Преамп", "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

    for (int i = 0; i < 11; i++) {
        float cx = sx0 + slot * (i + 0.5f);
        if (VSlider(100 + i, cx, top, bottom, &A.eq[i], -12, 12, A.eqOn)) {
            if (i > 0) A.presetIdx = -1;
            ApplyAllAudioFX();
        }
        TxtC(labels[i], cx, bottom + 18, 16, C_DIM);
        char buf[16];
        float shown = fabsf(A.eq[i]) < 0.05f ? 0.0f : A.eq[i];
        snprintf(buf, sizeof buf, "%.1f", shown);
        TxtC(buf, cx, bottom + 42, 16, C_TEXT);
    }

    float cx0 = area.x + area.width - ctrlW;
    if (Button(R(cx0, area.y + 100, 120, 40), "ЭКВ", A.eqOn)) {
        A.eqOn = !A.eqOn;
        ApplyAllAudioFX();
    }
    if (Button(R(cx0, area.y + 154, 120, 40), "ТОН", A.toneOn)) {
        A.toneOn = !A.toneOn;
        ApplyAllAudioFX();
    }
    if (Button(R(cx0, area.y + 208, 120, 40), "ЛИМИТ", A.limitOn)) {
        A.limitOn = !A.limitOn;
        ApplyAllAudioFX();
    }

    float kx = cx0 + 205, r = 42, ky1 = area.y + 150, ky2 = ky1 + 2 * r + 62;
    char p[16];
    TxtC("Бас", kx, ky1 - r - 30, 18, C_DIM);
    if (Knob(200, kx, ky1, r, &A.bass, A.toneOn)) ApplyAllAudioFX();
    snprintf(p, sizeof p, "%d%%", (int)(A.bass * 100 + 0.5f));
    Txt(p, kx + r + 12, ky1 - 12, 20, C_TEXT);

    if (Knob(201, kx, ky2, r, &A.treble, A.toneOn)) ApplyAllAudioFX();
    snprintf(p, sizeof p, "%d%%", (int)(A.treble * 100 + 0.5f));
    Txt(p, kx + r + 12, ky2 - 12, 20, C_TEXT);
    TxtC("Выс.", kx, ky2 + r + 8, 18, C_DIM);

    g_presetBtn = R(cx0, area.y + area.height - 80, 160, 40);
    const char* pl = A.presetIdx >= 0 ? PRESET_NAMES[A.presetIdx] : "Пресет";
    if (Button(g_presetBtn, pl, A.presetOpen)) { A.presetOpen = !A.presetOpen; A.menuOpen = false; A.volOpen = false; }
}

// =====================================================================
//  Экран настроек: главное меню и подменю с плавными переходами
// =====================================================================
static int   g_setPage = 0;                                // 0 главное меню, 1 фон, 2 текст песни, 3 звук
static float g_setAnim[4] = { 1.0f, 0.0f, 0.0f, 0.0f };    // насколько показана каждая страница (0..1)
static float g_uiAnim[64] = { 0 };                         // плавные состояния кнопок и переключателей

// Плавно двигает значение с номером id к цели и возвращает его со сглаживанием
static float UiAnim(int id, float target, float speed) {
    g_uiAnim[id] = Approach(g_uiAnim[id], target, GetFrameTime() * speed);
    return EaseInOutCubic(g_uiAnim[id]);
}

// Большая кнопка-переход в главном меню настроек
static bool SetCard(int id, Rectangle r, const char* title, const char* sub, float a) {
    float e = UiAnim(id, Hit(r) ? 1.0f : 0.0f, 9.0f);
    Rectangle rr = R(r.x - 3 * e, r.y - 3 * e, r.width + 6 * e, r.height + 6 * e);    // при наведении слегка вырастает
    DrawRectangleRounded(rr, 0.18f, 12, Ma(ColorLerp(C_PANEL2, ColorBrightness(C_PANEL2, 0.18f), e), a));
    Txt(title, rr.x + 24, rr.y + 16, 24, Ma(C_TEXT, a));
    Txt(sub, rr.x + 24, rr.y + 52, 16, Ma(C_DIM, a));
    float cx = rr.x + rr.width - 34 + 5 * e, cy = rr.y + rr.height / 2;                 // стрелка вправо
    DrawLineEx(V(cx - 5, cy - 9), V(cx + 4, cy), 3.0f, Ma(C_TEXT, a));
    DrawLineEx(V(cx + 4, cy), V(cx - 5, cy + 9), 3.0f, Ma(C_TEXT, a));
    return Pressed(r);
}

// Кнопка «Назад» (стрелка влево)
static bool SetBackButton(Rectangle r, float a) {
    float e = UiAnim(3, Hit(r) ? 1.0f : 0.0f, 10.0f);
    DrawRectangleRounded(r, 0.5f, 12, Ma(ColorLerp(C_PANEL2, ColorBrightness(C_PANEL2, 0.22f), e), a));
    float cx = r.x + r.width / 2 - 3 * e, cy = r.y + r.height / 2;
    DrawLineEx(V(cx + 6, cy - 9), V(cx - 5, cy), 3.0f, Ma(C_TEXT, a));
    DrawLineEx(V(cx - 5, cy), V(cx + 6, cy + 9), 3.0f, Ma(C_TEXT, a));
    DrawLineEx(V(cx - 5, cy), V(cx + 9, cy), 3.0f, Ma(C_TEXT, a));
    return Pressed(r);
}

// Переключатель (тумблер) с подписью; ползунок тумблера плавно переезжает
static bool SetToggle(int id, Rectangle r, const char* label, const char* sub, bool* value, float a) {
    float e = UiAnim(id, *value ? 1.0f : 0.0f, 8.0f);
    DrawRectangleRounded(r, 0.3f, 10, Ma(Hit(r) ? ColorBrightness(C_PANEL2, 0.08f) : C_PANEL2, a));
    Txt(label, r.x + 20, r.y + (sub ? 8.0f : (r.height - 22.0f) / 2), 19, Ma(C_TEXT, a));
    if (sub) Txt(sub, r.x + 20, r.y + 31, 14, Ma(C_DIM, a));
    Rectangle sw = R(r.x + r.width - 72, r.y + r.height / 2 - 14, 52, 28);
    Color off = { 70, 74, 110, 255 };
    DrawRectangleRounded(sw, 1.0f, 12, Ma(ColorLerp(off, C_ACCENT, e), a));
    DrawCircleV(V(sw.x + 14 + 24 * e, sw.y + 14), 11, Ma(C_TEXT, a));
    bool clicked = Pressed(r);
    if (clicked) *value = !*value;
    return clicked;
}

// Горизонтальный ползунок: заливка идёт от значения по умолчанию, правая кнопка мыши возвращает его
static bool SetSlider(int id, Rectangle r, const char* label, float* v, float lo, float hi, float def, const char* valueText, float a) {
    bool changed = false;
    DrawRectangleRounded(r, 0.25f, 10, Ma(C_PANEL2, a));
    Txt(label, r.x + 20, r.y + 8, 19, Ma(C_TEXT, a));
    Txt(valueText, r.x + r.width - 20 - TxtW(valueText, 17), r.y + 10, 17, Ma(C_ACC2, a));
    float tx = r.x + 20, tw = r.width - 40, ty = r.y + r.height - 16;
    Rectangle hit = R(tx - 10, ty - 14, tw + 20, 28);
    if (BeginDrag(id, hit)) {
        float nv = lo + Clampf((MousePos().x - tx) / tw, 0.0f, 1.0f) * (hi - lo);
        if (fabsf(nv - def) < (hi - lo) * 0.02f) nv = def;     // «прилипание» к значению по умолчанию
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (Hit(hit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && *v != def) { *v = def; changed = true; }
    float f = (*v - lo) / (hi - lo), d = (def - lo) / (hi - lo);
    DrawRectangleRounded(R(tx, ty - 3, tw, 6), 1.0f, 6, Ma(C_PANEL, a));
    float x0 = tx + tw * fminf(f, d), x1 = tx + tw * fmaxf(f, d);
    DrawRectangleRounded(R(x0, ty - 3, x1 - x0, 6), 1.0f, 6, Ma(C_ACCENT, a));
    DrawCircleV(V(tx + tw * f, ty), 9, Ma(C_TEXT, a));
    return changed;
}

// Переключатель из нескольких вариантов: подсветка плавно скользит к выбранному
static int SetSegment(int id, Rectangle r, const char* const* labels, int count, int current, float a) {
    g_uiAnim[id] = Approach(g_uiAnim[id], (float)current, GetFrameTime() * 9.0f);
    float cw = r.width / count, pos = g_uiAnim[id];
    DrawRectangleRounded(r, 0.4f, 12, Ma(C_PANEL2, a));
    DrawRectangleRounded(R(r.x + 4 + pos * cw, r.y + 4, cw - 8, r.height - 8), 0.4f, 12, Ma(C_ACCENT, a));
    int clicked = -1;
    for (int i = 0; i < count; i++) {
        Rectangle cell = R(r.x + i * cw, r.y, cw, r.height);
        TxtC(labels[i], cell.x + cw / 2, r.y + (r.height - 18) / 2 - 1, 17, Ma(C_TEXT, a));
        if (Pressed(cell)) clicked = i;
    }
    return clicked;
}

// ---------- Страница 0: главное меню настроек ----------
static void DrawSettingsMenu(Rectangle area, float a) {
    Txt("Настройки", area.x + 28, area.y + 20, 28, Ma(C_TEXT, a));
    static const char* titles[3] = { "Изменение фона", "Текст песни", "Настройка звука" };
    static const char* subs[3] = { "Готовые темы, свой градиент, цвет по обложке", "Поиск текста песни", "Баланс, ширина стерео, громкость, вывод" };
    float w = fminf(area.width - 56, 640);
    for (int i = 0; i < 3; i++) {
        if (SetCard(i, R(area.x + 28, area.y + 84 + i * 104, w, 88), titles[i], subs[i], a)) g_setPage = i + 1;
    }
}

// ---------- Страница 1: изменение фона (бывшие настройки фона) ----------
static void DrawSettingsBackground(Rectangle area, float a) {
    if (SetBackButton(R(area.x + 20, area.y + 16, 44, 44), a)) g_setPage = 0;
    Txt("Изменение фона", area.x + 80, area.y + 20, 28, Ma(C_TEXT, a));

    float y0 = area.y + 80;
    SetToggle(5, R(area.x + 28, y0, area.width - 56, 54), "Цвет фона по обложке", "Свечение и волна берут цвета обложки играющей песни", &g_adaptiveBg, a);

    Txt("Готовые темы", area.x + 28, y0 + 66, 18, Ma(C_DIM, a));
    int cols = 4;
    float gap = 12, tileW = (area.width - 56 - gap * (cols - 1)) / cols, tileH = 80;
    for (int i = 0; i < THEME_COUNT; i++) {
        int gx = i % cols, gy = i / cols;
        Rectangle tile = R(area.x + 28 + gx * (tileW + gap), y0 + 90 + gy * (tileH + gap), tileW, tileH);
        float e = UiAnim(16 + i, Hit(tile) ? 1.0f : 0.0f, 10.0f);
        Rectangle t2 = R(tile.x - 2 * e, tile.y - 2 * e, tile.width + 4 * e, tile.height + 4 * e);
        Color base = (A.themeIdx == i) ? C_ACCENT : ColorLerp(C_PANEL2, ColorBrightness(C_PANEL2, 0.18f), e);
        DrawRectangleRounded(t2, 0.14f, 10, Ma(base, a));

        const Theme& th = THEMES[i];
        Rectangle pv = R(t2.x + 8, t2.y + 8, t2.width - 16, 38);
        DrawRectangleRec(pv, Ma(th.bg, a));
        int hw = (int)(pv.width / 2);
        DrawRectangleGradientH((int)pv.x, (int)pv.y, hw, (int)pv.height, Ma(th.c[0], a), Ma(th.c[1], a));
        DrawRectangleGradientH((int)pv.x + hw, (int)pv.y, (int)pv.width - hw, (int)pv.height, Ma(th.c[1], a), Ma(th.c[2], a));
        TxtC(th.name, t2.x + t2.width / 2, t2.y + t2.height - 28, 17, Ma(C_TEXT, a));
        if (Pressed(tile)) { ApplyThemePreset(i); g_adaptiveBg = false; }
    }

    float sy0 = y0 + 290;
    Txt("Свой градиент", area.x + 28, sy0, 18, Ma(A.themeIdx < 0 ? C_ACC2 : C_DIM, a));
    static const char* names[3] = { "Цвет 1", "Цвет 2", "Цвет 3" };
    float sx = area.x + 130, sw = fminf(area.width - 130 - 60, 460);
    for (int i = 0; i < 3; i++) {
        float y = sy0 + 44 + i * 40;
        Txt(names[i], area.x + 28, y - 11, 18, Ma(C_TEXT, a));
        const int N = 48;
        for (int k = 0; k < N; k++) {
            int x0 = (int)(sx + sw * k / N), x1 = (int)(sx + sw * (k + 1) / N);
            DrawRectangleGradientH(x0, (int)(y - 7), x1 - x0 + 1, 14,
                Ma(ColorFromHSV(360.0f * k / N, 0.75f, 1.0f), a), Ma(ColorFromHSV(360.0f * (k + 1) / N, 0.75f, 1.0f), a));
        }
        if (BeginDrag(400 + i, R(sx - 10, y - 16, sw + 20, 32))) {
            A.hue[i] = Clampf((MousePos().x - sx) / sw, 0.0f, 1.0f);
            A.themeIdx = -1;
            g_adaptiveBg = false;
        }
        float hx = sx + sw * A.hue[i];
        DrawCircleV(V(hx, y), 11, Ma(C_TEXT, a));
        DrawCircleV(V(hx, y), 8, Ma(ColorFromHSV(A.hue[i] * 360.0f, 0.75f, 1.0f), a));
    }
}

// ---------- Страница 2: текст песни ----------
static void DrawSettingsLyrics(Rectangle area, float a) {
    if (SetBackButton(R(area.x + 20, area.y + 16, 44, 44), a)) g_setPage = 0;
    Txt("Текст песни", area.x + 80, area.y + 20, 28, Ma(C_TEXT, a));
    SetToggle(6, R(area.x + 28, area.y + 80, area.width - 56, 54), "Поиск текста из интернета", "Сервис lrclib.net, найденное сохраняется в папку lyrics_cache", &g_onlineLyrics, a);
    Txt("Сначала ищется файл .lrc рядом с песней, затем кэш и теги внутри файла.", area.x + 28, area.y + 152, 16, Ma(C_DIM, a));
    Txt("Изменение действует со следующей песни.", area.x + 28, area.y + 176, 16, Ma(C_DIM, a));
}

// Есть ли библиотека нужного режима вывода рядом с программой
static bool OutputLibAvailable(int mode) {
    if (mode == 0) return true;
    const char* dll = (mode == 1) ? "basswasapi.dll" : "bassasio.dll";
    std::error_code ec;
    return fs::exists(fs::path(GetApplicationDirectory()) / dll, ec);
}

// ---- PURE-BEGIN ----
// Точка мыши -> положение звука внутри прямоугольника (точка не выходит за края, у центра «прилипает»)
static void DotFromMouse(float mx, float my, float zx, float zy, float zw, float zh, float r, float* x, float* y) {
    float hw = zw / 2 - r, hh = zh / 2 - r;
    float cx = zx + zw / 2, cy = zy + zh / 2;
    float nx = hw > 0.0f ? (mx - cx) / hw : 0.0f;
    float ny = hh > 0.0f ? (my - cy) / hh : 0.0f;
    if (nx < -1.0f) nx = -1.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < -1.0f) ny = -1.0f;
    if (ny > 1.0f) ny = 1.0f;
    if (nx > -0.04f && nx < 0.04f && ny > -0.04f && ny < 0.04f) { nx = 0.0f; ny = 0.0f; }
    *x = nx;
    *y = ny;
}

// Положение звука -> координаты точки на экране
static void DotToScreen(float x, float y, float zx, float zy, float zw, float zh, float r, float* sx, float* sy) {
    *sx = zx + zw / 2 + x * (zw / 2 - r);
    *sy = zy + zh / 2 + y * (zh / 2 - r);
}
// ---- PURE-END ----

static bool  g_8dOpen = false;          // панель «8D Аудио» раскрыта
static float g_8dAnim = 0.0f;           // раскрытие панели (0..1)
static float g_audioScroll = 0.0f;      // прокрутка страницы звука
static float g_audioWant = 0.0f;        // куда страница хочет прокрутиться
static float g_dotShowX = 0.0f, g_dotShowY = 0.0f;   // показываемое положение точки (плавно догоняет настоящее)

// Стрелка-шеврон: 0 градусов = смотрит вправо, 90 = вниз
static void DrawChevron(float cx, float cy, float deg, Color c) {
    float rad = deg * DEG2RAD, cs = cosf(rad), sn = sinf(rad);
    Vector2 p0 = V(cx + (-4) * cs - (-8) * sn, cy + (-4) * sn + (-8) * cs);
    Vector2 p1 = V(cx + 5 * cs, cy + 5 * sn);
    Vector2 p2 = V(cx + (-4) * cs - 8 * sn, cy + (-4) * sn + 8 * cs);
    DrawLineEx(p0, p1, 3.0f, c);
    DrawLineEx(p1, p2, 3.0f, c);
}

// Внутренность панели «8D Аудио»: прямоугольник с точкой и подписи позиций
static void Draw8DPanel(Rectangle p, float a) {
    DrawRectangleRounded(p, 0.06f, 12, Ma(ColorBrightness(C_PANEL2, 0.06f), a));
    float zw = Clampf(p.width * 0.46f, 240.0f, 440.0f), zh = 150.0f, r = 11.0f;
    Rectangle zone = R(p.x + (p.width - zw) / 2, p.y + 58.0f, zw, zh);
    float cx = zone.x + zone.width / 2, cy = zone.y + zone.height / 2;

    // прямоугольник: рамка, фон и перекрестие
    DrawRectangleRounded(R(zone.x - 2, zone.y - 2, zone.width + 4, zone.height + 4), 0.12f, 10, Ma(C_ACCENT, a * 0.55f));
    DrawRectangleRounded(zone, 0.12f, 10, Ma(C_PANEL, a));
    DrawLineEx(V(zone.x + 10, cy), V(zone.x + zone.width - 10, cy), 1.0f, Ma(C_DIM, a * 0.25f));
    DrawLineEx(V(cx, zone.y + 10), V(cx, zone.y + zone.height - 10), 1.0f, Ma(C_DIM, a * 0.25f));

    // подписи позиций; на них тоже можно нажать, и точка встанет в это место
    struct Lab { const char* t; float lx, ly, px, py; };
    Lab labs[8] = {
        { "Л.П.", zone.x, zone.y - 22, -1, -1 },{ "П.", cx, zone.y - 22, 0, -1 },{ "П.П.", zone.x + zone.width, zone.y - 22, 1, -1 },
        { "Л.", zone.x - 38, cy, -1, 0 },{ "П.", zone.x + zone.width + 38, cy, 1, 0 },
        { "Л.З.", zone.x, zone.y + zone.height + 22, -1, 1 },{ "З.", cx, zone.y + zone.height + 22, 0, 1 },{ "З.П.", zone.x + zone.width, zone.y + zone.height + 22, 1, 1 }
    };
    for (int i = 0; i < 8; i++) {
        Rectangle lr = R(labs[i].lx - 30, labs[i].ly - 14, 60, 28);
        bool hv = Hit(lr);
        TxtC(labs[i].t, labs[i].lx, labs[i].ly - 10, 18, Ma(hv ? C_TEXT : C_DIM, a));
        if (Pressed(lr)) { g_8dX = labs[i].px; g_8dY = labs[i].py; ApplyAllAudioFX(); }
    }

    // зажать левую кнопку и тянуть точку; правая кнопка возвращает в центр
    Rectangle hit = R(zone.x - 6, zone.y - 6, zone.width + 12, zone.height + 12);
    bool dragging = BeginDrag(600, hit);
    if (dragging) {
        Vector2 m = MousePos();
        float nx, ny;
        DotFromMouse(m.x, m.y, zone.x, zone.y, zone.width, zone.height, r, &nx, &ny);
        if (nx != g_8dX || ny != g_8dY) { g_8dX = nx; g_8dY = ny; ApplyAllAudioFX(); }
    }
    if (Hit(hit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && (g_8dX != 0.0f || g_8dY != 0.0f)) {
        g_8dX = 0.0f; g_8dY = 0.0f;
        ApplyAllAudioFX();
    }

    // точка: рисуется чуть плавнее, чем двигается звук
    float k = Clampf(GetFrameTime() * 14.0f, 0.0f, 1.0f);
    g_dotShowX += (g_8dX - g_dotShowX) * k;
    g_dotShowY += (g_8dY - g_dotShowY) * k;
    float sx, sy;
    DotToScreen(g_dotShowX, g_dotShowY, zone.x, zone.y, zone.width, zone.height, r, &sx, &sy);
    float grow = UiAnim(40, dragging ? 1.0f : 0.0f, 10.0f);
    DrawLineEx(V(cx, cy), V(sx, sy), 2.0f, Ma(C_ACCENT, a * 0.45f));
    DrawCircleV(V(cx, cy), 4.0f, Ma(C_DIM, a * 0.6f));
    DrawCircleV(V(sx, sy), r + 9.0f + 5.0f * grow, Ma(C_ACCENT, a * 0.18f));
    DrawCircleV(V(sx, sy), r + 2.0f * grow, Ma(C_ACCENT, a));
    DrawCircleV(V(sx, sy), 4.0f, Ma(C_TEXT, a));

    // подсказка и текущее положение
    char t[80];
    int ax = (int)(fabsf(g_8dX) * 100.0f + 0.5f), ay = (int)(fabsf(g_8dY) * 100.0f + 0.5f);
    if (ax == 0 && ay == 0) snprintf(t, sizeof t, "Сейчас: по центру");
    else snprintf(t, sizeof t, "Сейчас: %s %d%%, %s %d%%", g_8dX < 0 ? "влево" : "вправо", ax, g_8dY < 0 ? "вперёд" : "назад", ay);
    TxtC(t, p.x + p.width / 2, p.y + p.height - 52, 16, Ma(C_ACC2, a));
    TxtC("Тяни точку мышкой. Правая кнопка мыши возвращает в центр.", p.x + p.width / 2, p.y + p.height - 28, 14, Ma(C_DIM, a));
}

static void DrawSettingsAudio(Rectangle area, float a) {
    if (SetBackButton(R(area.x + 20, area.y + 16, 44, 44), a)) g_setPage = 0;
    Txt("Настройка звука", area.x + 80, area.y + 20, 28, Ma(C_TEXT, a));

    float dt = GetFrameTime();
    const float H8 = 300.0f;
    Rectangle clip = R(area.x, area.y + 72, area.width, area.height - 80);   // область со списком настроек
    bool prevLock = g_lock;
    bool mouseIn = CheckCollisionPointRec(GetMousePosition(), clip);
    if (!mouseIn) g_lock = true;                       // то, что ушло под заголовок, не нажимается

    g_8dAnim = Approach(g_8dAnim, g_8dOpen ? 1.0f : 0.0f, dt / 0.40f);
    float e8 = EaseInOutCubic(g_8dAnim);

    // прокрутка страницы колесом мыши
    float totalH = 66 * 2 + 60 * 3 + 60 + (H8 + 8.0f) * e8 + 108.0f;
    float maxScroll = fmaxf(0.0f, totalH + 16.0f - clip.height);
    if (mouseIn && !prevLock) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) g_audioWant = Clampf(g_audioScroll - wheel * 70.0f, 0.0f, maxScroll);
    }
    g_audioScroll += (Clampf(g_audioWant, 0.0f, maxScroll) - g_audioScroll) * Clampf(dt * 10.0f, 0.0f, 1.0f);

    float x = area.x + 28, w = area.width - 56 - 14, y = clip.y + 8 - g_audioScroll;
    char t[40];

    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);

    // Баланс: от -100% (левый канал) до +100% (правый)
    float pan = g_pan;
    if (fabsf(pan) < 0.005f) snprintf(t, sizeof t, "по центру");
    else snprintf(t, sizeof t, "%s %d%%", pan < 0 ? "левый" : "правый", (int)(fabsf(pan) * 100.0f + 0.5f));
    if (SetSlider(500, R(x, y, w, 58), "Баланс (Pan)", &pan, -1.0f, 1.0f, 0.0f, t, a)) { g_pan = pan; ApplyAllAudioFX(); }
    y += 66;

    // Ширина стерео: 0% = моно, 100% = как записано, 200% = шире
    float width = g_stereoWidth;
    snprintf(t, sizeof t, "%d%%", (int)(width * 100.0f + 0.5f));
    if (SetSlider(501, R(x, y, w, 58), "Ширина стерео (Stereo Width)", &width, 0.0f, 2.0f, 1.0f, t, a)) g_stereoWidth = width;
    y += 66;

    if (SetToggle(7, R(x, y, w, 54), "Тонокомпенсация (Loudness)", "На тихой громкости усиливает басы и высокие частоты", &g_loudness, a)) ApplyAllAudioFX();
    y += 60;

    SetToggle(8, R(x, y, w, 54), "Плавный переход между песнями (Crossfade)", "Конец песни затихает, следующая плавно нарастает (4 секунды)", &g_crossfade, a);
    y += 60;

    if (SetToggle(9, R(x, y, w, 54), "Нормализация громкости треков", "Выравнивает громкость разных песен, анализ идёт в фоне", &g_normalize, a)) {
        if (g_normalize) StartNormAnalysis(g_curPath);
        else { ++g_nrGen; g_normGainTarget = 1.0f; }
    }
    y += 60;

    // Пункт-аккордеон «8D Аудио»: стрелка поворачивается, панель плавно выезжает
    Rectangle hd = R(x, y, w, 54);
    float he = UiAnim(10, Hit(hd) ? 1.0f : 0.0f, 10.0f);
    DrawRectangleRounded(hd, 0.3f, 10, Ma(ColorLerp(C_PANEL2, ColorBrightness(C_PANEL2, 0.1f), he), a));
    Txt("8D Аудио", hd.x + 20, hd.y + 8, 19, Ma(C_TEXT, a));
    Txt("Выбери, откуда должен идти звук", hd.x + 20, hd.y + 31, 14, Ma(C_DIM, a));
    DrawChevron(hd.x + hd.width - 34, hd.y + 27, e8 * 90.0f, Ma(C_TEXT, a));
    if (Pressed(hd)) {
        g_8dOpen = !g_8dOpen;
        if (g_8dOpen) g_audioWant = 66 * 2 + 60 * 3 - 8.0f;   // прокрутить так, чтобы панель была видна целиком
    }
    float py = y + 60;
    EndScissorMode();

    // Панель 8D: обрезается по своей растущей высоте и по области списка
    if (e8 > 0.003f) {
        float ph = H8 * e8;
        float cx0 = fmaxf(x, clip.x), cy0 = fmaxf(py, clip.y);
        float cx1 = fminf(x + w, clip.x + clip.width), cy1 = fminf(py + ph, clip.y + clip.height);
        if (cx1 - cx0 > 1.0f && cy1 - cy0 > 1.0f) {
            bool lockBefore = g_lock;
            if (e8 < 0.98f) g_lock = true;               // пока панель выезжает, точку не трогаем
            float pa = a * Clampf((e8 - 0.25f) / 0.75f, 0.0f, 1.0f);
            BeginScissorMode((int)cx0, (int)cy0, (int)(cx1 - cx0), (int)(cy1 - cy0));
            Draw8DPanel(R(x, py, w, H8), pa);
            EndScissorMode();
            g_lock = lockBefore;
        }
    }

    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);
    y = py + (H8 + 8.0f) * e8 + 6;

    // Режим вывода звука
    Txt("Вывод звука", x, y, 18, Ma(C_DIM, a));
    static const char* modes[3] = { "Обычный", "WASAPI Exclusive", "ASIO" };
    int pick = SetSegment(30, R(x, y + 26, fminf(w, 560), 40), modes, 3, g_outMode, a);
    if (pick >= 0) g_outMode = pick;
    if (g_outMode == 0) {
        Txt("Обычный вывод через Windows. Работает сейчас.", x, y + 74, 15, Ma(C_DIM, a));
    }
    else if (!OutputLibAvailable(g_outMode)) {
        Txt(g_outMode == 1 ? "Нужна библиотека basswasapi.dll рядом с программой. Пока звук идёт обычным способом."
            : "Нужна библиотека bassasio.dll рядом с программой. Пока звук идёт обычным способом.", x, y + 74, 15, Ma(C_PINK, a));
    }
    else {
        Txt("Режим сохранён, но переключение вывода на него пока не сделано: звук идёт обычным способом.", x, y + 74, 15, Ma(C_DIM, a));
    }
    EndScissorMode();

    // Полоса прокрутки справа
    if (maxScroll > 1.0f) {
        float trackH = clip.height - 16, thumbH = fmaxf(36.0f, trackH * clip.height / (totalH + 16.0f));
        float ty = clip.y + 8 + (trackH - thumbH) * (g_audioScroll / maxScroll);
        DrawRectangleRounded(R(area.x + area.width - 16, ty, 6, thumbH), 1.0f, 6, Ma(C_DIM, a * 0.45f));
    }
    g_lock = prevLock;
}

// ---------- Показ настроек: страницы плавно уезжают и приезжают ----------
static void ScreenSettings() {
    float dt = GetFrameTime();
    Rectangle area = R(96, 16, A.W - 96 - 16.0f, A.H - 32.0f);
    DrawRectangleRounded(area, 0.05f, 16, C_PANEL);

    bool prevLock = g_lock;
    if (IsKeyPressed(KEY_ESCAPE) && !prevLock && g_setPage != 0) g_setPage = 0;     // Esc = «Назад»

    for (int p = 0; p < 4; p++) g_setAnim[p] = Approach(g_setAnim[p], p == g_setPage ? 1.0f : 0.0f, dt / 0.40f);

    for (int p = 0; p < 4; p++) {
        if (g_setAnim[p] <= 0.003f) continue;
        float e = EaseInOutCubic(g_setAnim[p]);
        float off = (p == 0 ? -1.0f : 1.0f) * (1.0f - e) * 70.0f;                   // меню уезжает влево, подменю приезжает справа
        g_lock = prevLock || p != g_setPage || g_setAnim[p] < 0.97f;                // нажимать можно только на открытой странице
        rlPushMatrix();
        rlTranslatef(off, 0.0f, 0.0f);
        if (p == 0) DrawSettingsMenu(area, e);
        else if (p == 1) DrawSettingsBackground(area, e);
        else if (p == 2) DrawSettingsLyrics(area, e);
        else DrawSettingsAudio(area, e);
        rlPopMatrix();
    }
    g_lock = prevLock;
}


// =====================================================================
//  Всплывающие окна
// =====================================================================
static void Popups(bool wasOpen) {
    g_lock = false;
    Vector2 m = GetMousePosition();
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool inside = false;

    if (A.menuAnim > 0.005f) {
        float e = EaseOutCubic(A.menuAnim);
        float targetW = 240.0f, targetH = 5 * 46.0f + 16.0f;
        float curW = 48.0f + (targetW - 48.0f) * e;
        float curH = 48.0f + (targetH - 48.0f) * e;
        float startX = 22.0f + (88.0f - 22.0f) * e;

        Rectangle r = R(startX, 22, curW, curH);
        DrawRectangleRounded(r, 0.08f, 12, Fade(C_POP, e));

        if (e > 0.25f) {
            float textAlpha = Clampf((e - 0.25f) / 0.75f, 0.0f, 1.0f);
            BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
            for (int i = 0; i < 5; i++) {
                Rectangle it = R(r.x + 8, r.y + 8 + i * 46, targetW - 16, 42);
                bool hover = A.menuOpen && CheckCollisionPointRec(m, it), cur = (A.screen == i);
                if (hover || cur) DrawRectangleRounded(it, 0.3f, 10, Fade(cur ? C_ACCENT : C_PANEL2, textAlpha));
                Txt(MENU_ITEMS[i], it.x + 16, it.y + 9, 22, Fade(C_TEXT, textAlpha));
                if (A.menuOpen && hover && click) { A.screen = i; A.menuOpen = false; }
            }
            EndScissorMode();
        }
        if (A.menuOpen) inside = CheckCollisionPointRec(m, R(88, 22, targetW, targetH)) || CheckCollisionPointRec(m, R(22, 24, 48, 48));
    }

    if (A.volAnim > 0.005f) {
        float e = EaseOutCubic(A.volAnim);
        float targetH = 230.0f;
        float curH = 48.0f + (targetH - 48.0f) * e;
        float startX = 22.0f + (88.0f - 22.0f) * e;
        Rectangle r = R(startX, A.H - 16.0f - curH, 76, curH);
        DrawRectangleRounded(r, 0.2f, 12, Fade(C_POP, e));

        if (e > 0.3f) {
            float alpha = Clampf((e - 0.3f) / 0.7f, 0.0f, 1.0f);
            char buf[16]; snprintf(buf, sizeof buf, "%d%%", (int)(A.volume * 100 + 0.5f));
            TxtC(buf, r.x + r.width / 2, r.y + 12, 18, Fade(C_TEXT, alpha));
            float top = r.y + 52, bottom = r.y + r.height - 22, cx = r.x + r.width / 2;
            if (A.volOpen && BeginDrag(300, R(r.x + 8, top - 14, r.width - 16, bottom - top + 28))) {
                A.volume = Clampf(1.0f - (GetMouseY() - top) / (bottom - top), 0, 1);
                ApplyAllAudioFX();
            }
            float ky = bottom - (bottom - top) * A.volume;
            DrawRectangleRounded(R(cx - 3, top, 6, bottom - top), 1.0f, 6, Fade(C_PANEL2, alpha));
            DrawRectangleRounded(R(cx - 3, ky, 6, bottom - ky), 1.0f, 6, Fade(C_ACCENT, alpha));
            DrawCircleV(V(cx, ky), 9, Fade(C_TEXT, alpha));
        }
        if (A.volOpen) inside = inside || CheckCollisionPointRec(m, r) || CheckCollisionPointRec(m, R(22, A.H - 72.0f, 48, 48));
    }

    if (A.presetAnim > 0.005f) {
        float e = EaseOutCubic(A.presetAnim);
        float ph = (8 * 36 + 16) * e;
        Rectangle r = R(g_presetBtn.x, g_presetBtn.y - 12 - ph, 200, ph);
        DrawRectangleRounded(r, 0.08f, 12, Fade(C_POP, e));
        if (e > 0.3f) {
            float alpha = Clampf((e - 0.3f) / 0.7f, 0.0f, 1.0f);
            BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
            for (int i = 0; i < 8; i++) {
                Rectangle it = R(r.x + 8, r.y + 8 + i * 36, r.width - 16, 32);
                bool hover = A.presetOpen && CheckCollisionPointRec(m, it), cur = (A.presetIdx == i);
                if (hover || cur) DrawRectangleRounded(it, 0.3f, 10, Fade(cur ? C_ACCENT : C_PANEL2, alpha));
                Txt(PRESET_NAMES[i], it.x + 14, it.y + 6, 20, Fade(C_TEXT, alpha));
                if (A.presetOpen && hover && click) {
                    A.presetIdx = i;
                    for (int k = 0; k < 10; k++) A.eq[k + 1] = PRESETS[i][k];
                    ApplyAllAudioFX();
                    A.presetOpen = false;
                }
            }
            EndScissorMode();
        }
        if (A.presetOpen) inside = inside || CheckCollisionPointRec(m, r) || CheckCollisionPointRec(m, g_presetBtn);
    }

    if (wasOpen && click && !inside) A.menuOpen = A.volOpen = A.presetOpen = false;
}

// =====================================================================
//  Обновление частотного спектра из BASS (FFT)
// =====================================================================
static void UpdateSpectrum(float dt) {
    float fft[1024] = { 0 };
    bool hasData = false;

    if (g_stream && A.playing && !A.seeking && BASS_ChannelIsActive(g_stream) == BASS_ACTIVE_PLAYING) {
        if (BASS_ChannelGetData(g_stream, fft, BASS_DATA_FFT2048) != (DWORD)-1) {
            hasData = true;
        }
    }

    float bassEnergy = 0.0f;
    for (int i = 0; i < SPEC_BANDS; i++) {
        float target = 0.0f;
        if (hasData) {
            float f0 = (float)i / (float)SPEC_BANDS;
            float f1 = (float)(i + 1) / (float)SPEC_BANDS;

            int b0 = (int)(1.0f + powf(f0, 2.3f) * 740.0f);
            int b1 = (int)(1.0f + powf(f1, 2.3f) * 740.0f);
            if (b1 <= b0) b1 = b0 + 1;
            if (b1 > 1023) b1 = 1023;

            float peak = 0.0f;
            for (int b = b0; b <= b1; b++) {
                if (fft[b] > peak) peak = fft[b];
            }

            float tilt = 1.0f + f0 * 3.8f;
            float val = sqrtf(peak * tilt) * 2.4f;
            target = Clampf(val, 0.0f, 1.0f) * A.seekGain;
        }

        float speed = (target > A.spectrum[i]) ? 24.0f : (A.seeking ? 16.0f : 8.5f);
        A.spectrum[i] += (target - A.spectrum[i]) * Clampf(dt * speed, 0.0f, 1.0f);

        if (i < SPEC_BANDS / 4) bassEnergy += A.spectrum[i];
    }

    float targetAmp = hasData ? (0.12f + (bassEnergy / (SPEC_BANDS / 4.0f)) * 1.1f) : 0.12f;
    A.amp += (targetAmp - A.amp) * Clampf(dt * 12.0f, 0.0f, 1.0f);
}

// =====================================================================
//  Главная программа
// =====================================================================
static void ScreenPlaceholder(const char* title, const char* hint) {
    float L = 96;
    Rectangle area = R(L, 16, A.W - L - 16.0f, A.H - 32.0f);
    DrawRectangleRounded(area, 0.05f, 16, C_PANEL);
    Txt(title, area.x + 28, area.y + 20, 28, C_TEXT);
    TxtC(hint, area.x + area.width / 2, area.y + area.height / 2 - 12, 22, C_DIM);
}
int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1100, 680, "Music Player");
    SetWindowMinSize(900, 600);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    g_bassOk = BASS_Init(-1, 44100, 0, nullptr, nullptr) != 0;
    LoadBassPlugins();
    InitGlowTexture();
    ApplyThemePreset(0);
    LoadSettings();
    SnapTheme();

    std::vector<int> cp;
    for (int c = 32; c < 127; c++) cp.push_back(c);
    for (int c = 0x410; c <= 0x44F; c++) cp.push_back(c);
    cp.push_back(0x401); cp.push_back(0x451); cp.push_back(0x2014); cp.push_back(0x00B7);
    cp.push_back(0x00AB); cp.push_back(0x00BB);
    // Дополнительные знаки для текстов песен: латиница с акцентами, типографские кавычки и т.п.
    auto addCp = [&](int c) { for (size_t q = 0; q < cp.size(); q++) if (cp[q] == c) return; cp.push_back(c); };
    for (int c = 0xA0; c <= 0x17F; c++) addCp(c);
    for (int c = 0x2010; c <= 0x2027; c++) addCp(c);
    const int extra[] = { 0x0404, 0x0454, 0x0406, 0x0456, 0x0407, 0x0457, 0x0490, 0x0491, 0x040E, 0x045E, 0x2116 };
    for (size_t q = 0; q < sizeof(extra) / sizeof(extra[0]); q++) addCp(extra[q]);
    const char* fontPath = "C:/Windows/Fonts/segoeui.ttf";
    if (!FileExists(fontPath)) fontPath = "C:/Windows/Fonts/arial.ttf";
    g_font = LoadFontEx(fontPath, 36, cp.data(), (int)cp.size());
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);

    while (!WindowShouldClose()) {
        A.W = GetScreenWidth(); A.H = GetScreenHeight();
        float dt = GetFrameTime(), t = (float)GetTime();

        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned int i = 0; i < dropped.count; i++) {
                fs::path p = Utf8ToPath(dropped.paths[i]);
                if (fs::is_directory(p)) {
                    AddFolderToLibrary(p);
                    A.screen = 2;
                }
                else if (fs::is_regular_file(p) && IsAudioExt(p)) {
                    AddFolderToLibrary(p.parent_path());
                    A.screen = 2;
                }
            }
            UnloadDroppedFiles(dropped);
        }

        if (g_stream && A.playing && !A.seeking) {
            DWORD state = BASS_ChannelIsActive(g_stream);
            if (state == BASS_ACTIVE_PLAYING) {
                QWORD bytePos = BASS_ChannelGetPosition(g_stream, BASS_POS_BYTE);
                A.pos = (float)BASS_ChannelBytes2Seconds(g_stream, bytePos);
                if (g_seekVerifyAt > 0.0 && GetTime() >= g_seekVerifyAt) {   // проверяем, что перемотка сработала
                    g_seekVerifyAt = 0.0;
                    if (fabsf(A.pos - g_seekTarget) > 3.0f && g_seekErr == 0) g_seekErr = -1;
                }
            }
            else if (state == BASS_ACTIVE_STOPPED) {
                PlayNext();
            }
        }

        {
            float target = A.seeking ? 0.0f : 1.0f;
            if (A.seekGain != target) {
                float step = dt * (A.seeking ? 8.0f : 5.0f);
                A.seekGain = (A.seekGain < target) ? fminf(A.seekGain + step, target) : fmaxf(A.seekGain - step, target);
                ApplyAllAudioFX();
            }
        }

        UpdateSpectrum(dt);
        UpdateTheme(dt);

        float animSpeed = 9.0f;
        A.menuAnim = Clampf(A.menuAnim + (A.menuOpen ? dt : -dt) * animSpeed, 0.0f, 1.0f);
        A.volAnim = Clampf(A.volAnim + (A.volOpen ? dt : -dt) * animSpeed, 0.0f, 1.0f);
        A.presetAnim = Clampf(A.presetAnim + (A.presetOpen ? dt : -dt) * animSpeed, 0.0f, 1.0f);

        UpdateViewAnim(dt);
        PollLyricsResult();
        // --- Настройки звука: нормализация громкости и плавный переход между песнями ---
        PollNormResult();
        if (fabsf(g_normGain - g_normGainTarget) > 0.001f) {
            g_normGain += (g_normGainTarget - g_normGain) * Clampf(dt * 3.0f, 0.0f, 1.0f);
            ApplyAllAudioFX();
        }
        if (g_crossfade && g_stream && A.playing && !A.seeking && A.total > g_xfadeSec * 2.5f && A.total - A.pos <= g_xfadeSec) {
            PlayNext();   // следующая песня начинается заранее, а эта плавно затихает
        }
        if (g_fadeStream && !A.playing) {   // на паузе уходящую песню сразу убираем
            BASS_StreamFree(g_fadeStream);
            g_fadeStream = 0;
        }
        if (g_fadeStream) {
            g_fadeGain -= dt / g_xfadeSec;
            if (g_fadeGain <= 0.0f || BASS_ChannelIsActive(g_fadeStream) == BASS_ACTIVE_STOPPED) {
                BASS_StreamFree(g_fadeStream);
                g_fadeStream = 0;
            }
            else {
                BASS_ChannelSetAttribute(g_fadeStream, BASS_ATTRIB_VOL, Clampf(A.volume * g_fadeGain, 0.0f, 1.0f));
            }
        }
        if (g_xfadeGain < 1.0f) {
            g_xfadeGain = fminf(1.0f, g_xfadeGain + dt / g_xfadeSec);
            ApplyAllAudioFX();
        }
        if (IsKeyPressed(KEY_ESCAPE)) A.menuOpen = A.volOpen = A.presetOpen = false;
        if (IsKeyPressed(KEY_SPACE)) TogglePlayPause();
        bool wasOpen = A.menuOpen || A.volOpen || A.presetOpen;
        g_lock = wasOpen;

        BeginDrawing();
        DrawBackdrop(t, A.amp, A.viewGlow);
        switch (A.screen) {
        case 0: ScreenPlayer(); break;
        case 1: ScreenEq(); break;
        case 2: ScreenFolders(); break;
        case 3: ScreenPlaceholder("Альбомы", "Здесь будут альбомы"); break;
        case 4: ScreenSettings(); break;
        }
        Sidebar();
        Popups(wasOpen);
        EndDrawing();

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_drag = -1;
    }

    SaveSettings();

    SaveSettings();
    if (g_stream) BASS_StreamFree(g_stream);
    if (g_coverTex.id != 0) UnloadTexture(g_coverTex);
    UnloadTexture(g_glowTex);
    UnloadFont(g_font);
    BASS_Free();
    CloseWindow();
    return 0;
}