// FFmpeg-Metadaten-Probe fuer eStaticServiceAppInfo::getLength(): ermittelt die
// reale Laufzeit einer lokalen Mediendatei, ohne sie abzuspielen.
//
// serviceapp.so ist ein Python-C-Extension-Modul, das ungeschuetzt (kein
// try/except) importiert wird - schlaegt das Laden von serviceapp.so fehl,
// faellt das GESAMTE Plugin aus, nicht nur diese Funktion. Ausserdem ist
// serviceapp laut README als unabhaengig von exteplayer3/ffmpeg installier-
// und aktualisierbares Paket dokumentiert, und MIPS' Dynamic Linker bricht
// dlopen() eines Plugins bei jedem unaufgeloesten Symbol komplett ab (siehe
// die eConnection/eTimer::startLongTimer-Faelle in serviceapp.cpp). Deshalb
// linkt diese Datei NICHT gegen libavformat/libavutil (kein DT_NEEDED in
// serviceapp.so, siehe readelf -d), sondern laedt beide Bibliotheken zur
// Laufzeit per dlopen()/dlsym() nach - schlaegt das fehl, liefert die
// Funktion einfach -1, exakt wie vor Einfuehrung dieses Features.
//
// Die FFmpeg-Header werden nur fuer Typdefinitionen/Konstanten gebraucht
// (kein Link), muessen aber exakt zur vorhandenen .so-ABI passen, da die
// Funktionen ueber typisierte dlsym()-Funktionszeiger aufgerufen werden.

// _LARGEFILE64_SOURCE muss vor jedem Include stehen: die restliche Datei
// baut mit _FILE_OFFSET_BITS=32 (build-serviceapp.sh), wodurch stat() bei
// Dateien >2GiB (off_t 32-Bit) mit EOVERFLOW fehlschlaegt - real aufgetreten
// bei WWE-PPV-Dateien (6-6,5GB) direkt in /media/hdd/movie/, waehrend alle
// funktionierenden Testdateien zufaellig unter 2GB lagen. stat64()/struct
// stat64 bleiben davon unabhaengig und sind ueber dieses Makro immer verfuegbar.
#define _LARGEFILE64_SOURCE

#include "ffprobe_length.h"

#include <dlfcn.h>
#include <cstdio>
#include <sys/time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
}

extern bool g_debugLoggingEnabled;
#define SALOG(fmt, ...) do { \
    if (g_debugLoggingEnabled) { \
        FILE *_f = fopen("/tmp/serviceapp.log", "a"); \
        if (_f) { fprintf(_f, "[serviceapp] " fmt "\n", ##__VA_ARGS__); fclose(_f); } \
    } \
} while(0)

namespace {

// Dieselben, bereits GLIBC-kompatibel gepatchten FFmpeg-6.1.1-Libs, die
// exteplayer3 ohnehin mitbringt (siehe exteplayer3-build/build-ffmpeg.sh).
// Absoluter Pfad statt Rpath auf serviceapp.so: das Verzeichnis liegt bewusst
// ausserhalb des Loader-Caches, ein dlopen() ohne Pfad wuerde nicht greifen.
const char *kAvformatPath = "/usr/lib/exteplayer3_deps/libavformat.so.60";
// 1,5s war zu knapp: sehr grosse Dateien (getestet: 12-21GB MP4s mit dem
// "moov"-Atom am Dateiende statt am Anfang) brauchen fuer den Seek dorthin
// nachweislich bis zu ~2,5s auf lokaler Platte. 5s laesst dafuer ausreichend
// Spielraum, haelt aber die Blockierzeit bei einem haengenden Netzwerk-Mount
// (siehe Kommentar an ProbeDeadline) noch in einem tolerablen Rahmen.
const int kProbeTimeoutMs = 5000;

typedef AVFormatContext *(*avformat_alloc_context_t)(void);
typedef int (*avformat_open_input_t)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **);
typedef int (*avformat_find_stream_info_t)(AVFormatContext *, AVDictionary **);
typedef void (*avformat_close_input_t)(AVFormatContext **);
typedef void (*av_log_set_level_t)(int);
typedef void (*av_log_set_callback_t)(void (*)(void *, int, const char *, va_list));

struct FFmpegSymbols
{
    avformat_alloc_context_t alloc_context;
    avformat_open_input_t open_input;
    avformat_find_stream_info_t find_stream_info;
    avformat_close_input_t close_input;
    av_log_set_level_t log_set_level;
    av_log_set_callback_t log_set_callback;
};

enum class LibAvailability { Unknown, Available, Unavailable };
LibAvailability g_lib_availability = LibAvailability::Unknown;
FFmpegSymbols g_symbols = {};

// Unterdrueckt FFmpegs eigenes Logging (kaputte/exotische Dateien erzeugen
// sonst viele Warnungen) - bewusst leer, schreibt nichts.
void ffmpeg_silent_log_callback(void *, int, const char *, va_list)
{
}

template <typename FuncPtr>
bool loadSymbol(void *handle, const char *name, FuncPtr &out)
{
    out = reinterpret_cast<FuncPtr>(dlsym(handle, name));
    if (!out)
    {
        SALOG("ffprobe_length: Symbol '%s' nicht gefunden: %s", name, dlerror());
        return false;
    }
    return true;
}

// Einmaliger, memoized dlopen/dlsym-Versuch. Kein Symbol wird je erneut
// gesucht, kein Fehlschlag wirft/stuerzt ab - nur Rueckgabewerte.
bool ensure_ffmpeg_loaded()
{
    if (g_lib_availability == LibAvailability::Available)
        return true;
    if (g_lib_availability == LibAvailability::Unavailable)
        return false;

    void *handle = dlopen(kAvformatPath, RTLD_NOW);
    if (!handle)
    {
        SALOG("ffprobe_length: dlopen('%s') fehlgeschlagen: %s", kAvformatPath, dlerror());
        g_lib_availability = LibAvailability::Unavailable;
        return false;
    }

    bool ok = true;
    ok = loadSymbol(handle, "avformat_alloc_context", g_symbols.alloc_context) && ok;
    ok = loadSymbol(handle, "avformat_open_input", g_symbols.open_input) && ok;
    ok = loadSymbol(handle, "avformat_find_stream_info", g_symbols.find_stream_info) && ok;
    ok = loadSymbol(handle, "avformat_close_input", g_symbols.close_input) && ok;
    ok = loadSymbol(handle, "av_log_set_level", g_symbols.log_set_level) && ok;
    ok = loadSymbol(handle, "av_log_set_callback", g_symbols.log_set_callback) && ok;

    if (!ok)
    {
        dlclose(handle);
        g_lib_availability = LibAvailability::Unavailable;
        return false;
    }

    g_symbols.log_set_level(AV_LOG_QUIET);
    g_symbols.log_set_callback(ffmpeg_silent_log_callback);
    g_lib_availability = LibAvailability::Available;
    SALOG("ffprobe_length: FFmpeg-Metadaten-Probe verfuegbar (%s geladen)", kAvformatPath);
    return true;
}

// FFmpegs eigener Interrupt-Mechanismus statt alarm()/SIGALRM (das wuerde mit
// Enigma2s eigener Signalbehandlung im Hauptprozess kollidieren). Schuetzt
// gegen einen eingehaengten, aber gerade nicht erreichbaren Netzwerk-Mount
// unter einem lokal aussehenden Pfad, der sonst den synchron aus dem
// GUI-Thread aufgerufenen Probe unbegrenzt blockieren koennte.
struct ProbeDeadline
{
    struct timeval start;
    int timeout_ms;

    explicit ProbeDeadline(int ms) : timeout_ms(ms)
    {
        gettimeofday(&start, NULL);
    }

    static int check(void *opaque)
    {
        ProbeDeadline *self = (ProbeDeadline *)opaque;
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - self->start.tv_sec) * 1000
                         + (now.tv_usec - self->start.tv_usec) / 1000;
        return elapsed_ms > self->timeout_ms ? 1 : 0;
    }
};

int64_t probe_uncached(const std::string &path)
{
    if (!ensure_ffmpeg_loaded())
        return -1;

    AVFormatContext *ctx = g_symbols.alloc_context();
    if (!ctx)
        return -1;

    ProbeDeadline deadline(kProbeTimeoutMs);
    ctx->interrupt_callback.callback = &ProbeDeadline::check;
    ctx->interrupt_callback.opaque = &deadline;

    int64_t result = -1;
    if (g_symbols.open_input(&ctx, path.c_str(), NULL, NULL) == 0)
    {
        if (g_symbols.find_stream_info(ctx, NULL) >= 0 && ctx->duration > 0)
        {
            result = ctx->duration / AV_TIME_BASE;
        }
        g_symbols.close_input(&ctx);
    }
    // Bei Fehlschlag von avformat_open_input() gibt FFmpeg den Kontext selbst
    // frei und setzt ctx=NULL - kein eigener close_input()-Aufruf noetig/erlaubt.

    SALOG("ffprobe_length: %s -> %lld Sekunden", path.c_str(), (long long)result);
    return result;
}

// Cache bewusst als freistehender, funktionslokal gekapselter Zustand statt
// als Member von eStaticServiceAppInfo: Ein frueherer Versuch, dafuer neue
// Member (std::unordered_map + pthread_mutex_t) direkt in eStaticServiceAppInfo
// unterzubringen, hat auf MIPS beim Enigma2-Shutdown einen Absturz in
// eServiceFactoryApp::~eServiceFactoryApp() ausgeloest (ueber die
// ePtr<eStaticServiceAppInfo>-Freigabe) - auf ARM unauffaellig. Dieser Cache
// hier hat dieselbe Prozess-Lebensdauer, beeinflusst aber nicht das
// Objekt-Layout/die Destruktions-Kette der eigentlichen Service-Klasse.
struct FFProbeCacheEntry
{
    off64_t size; // nicht off_t - siehe _LARGEFILE64_SOURCE-Kommentar oben
    time_t mtime;
    int64_t length_seconds; // -1 = zuvor fehlgeschlagen (negatives Caching)
};
std::unordered_map<std::string, FFProbeCacheEntry> g_length_cache;
pthread_mutex_t g_length_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

} // namespace

int64_t ffprobe_get_duration_seconds(const std::string &path)
{
    struct stat64 s;
    if (stat64(path.c_str(), &s) != 0)
        return -1;

    pthread_mutex_lock(&g_length_cache_mutex);
    std::unordered_map<std::string, FFProbeCacheEntry>::iterator it = g_length_cache.find(path);
    if (it != g_length_cache.end() && it->second.size == s.st_size && it->second.mtime == s.st_mtime)
    {
        int64_t cached = it->second.length_seconds;
        pthread_mutex_unlock(&g_length_cache_mutex);
        return cached;
    }
    pthread_mutex_unlock(&g_length_cache_mutex);

    // Cache-Miss (neue/geaenderte Datei) - ausserhalb des Locks probieren,
    // die FFmpeg-Metadaten-Probe kann kurzzeitig blockieren (siehe Timeout oben).
    int64_t length = probe_uncached(path);

    FFProbeCacheEntry entry;
    entry.size = s.st_size;
    entry.mtime = s.st_mtime;
    entry.length_seconds = length; // auch -1 wird gecacht (negatives Caching)

    pthread_mutex_lock(&g_length_cache_mutex);
    g_length_cache[path] = entry;
    pthread_mutex_unlock(&g_length_cache_mutex);

    return length;
}
