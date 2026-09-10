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
#include <cstdint>
#include <cerrno>
#include <cstring>
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

// Dieselben FFmpeg-Libs, die exteplayer3 ohnehin mitbringt (siehe
// exteplayer3-build/build-ffmpeg.sh). Absoluter Pfad statt Rpath auf
// serviceapp.so: das Verzeichnis liegt bewusst ausserhalb des Loader-Caches,
// ein dlopen() ohne Pfad wuerde nicht greifen. Unversionierter Symlink statt
// fest kodierter SONAME-Version: eine hartkodierte Versionsnummer waere
// bruechig gegenueber kuenftigen FFmpeg-Versionswechseln - der unversionierte
// Symlink liegt dank der "cp -d lib*.so*"-Packaging-Zeile in build.sh
// garantiert immer mit vor.
const char *kAvformatPath = "/usr/lib/exteplayer3_deps/libavformat.so";
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

static uint32_t read32_be(FILE *f)
{
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
}

static uint64_t read64_be(FILE *f)
{
    uint8_t buf[8];
    if (fread(buf, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | buf[i];
    return v;
}

// Direkter Parser fuer das mvhd-Atom in MP4/MOV-Dateien, ohne jede FFmpeg-
// Abhaengigkeit (reines libc: fopen/fseeko64/fread). Motivation: bei manchen
// grossen MP4s (moov-Atom weit hinten in der Datei, z.B. WWE/AEW-Events als
// mehrstuendige Web-DL-Releases) liefert avformat_open_input() kein sofort
// gueltiges ctx->duration, und der Fallback auf avformat_find_stream_info()
// baut dafuer die kompletten Frame-Index-Tabellen im RAM auf (bis zu 1,4s
// Blockierzeit im Hauptthread pro Datei bei mehreren hunderttausend Frames).
// Die mvhd-eigene Dauer im Header wird nie ueberschrieben oder validiert
// (auch FFmpegs eigener Header-Fast-Path in probe_uncached() tut das nicht)
// - stimmt sie nicht mit dem echten Sample-basierten Ergebnis ueberein, gilt
// dasselbe Restrisiko wie beim bereits bestehenden ctx->duration-Fast-Path.
// Schlaegt das Erkennen fehl (kein MP4, keine gueltige mvhd-Dauer, defekte
// Atom-Struktur), liefert die Funktion -1 und probe_uncached() faellt auf den
// bestehenden FFmpeg-Pfad zurueck - keine Verhaltensaenderung fuer den Fehlerfall.
static int64_t try_fast_mp4_probe(const std::string &path)
{
    // Obergrenze gegen ein pathologisch/absichtlich kaputtes Atom-Layout
    // (z.B. eine lange Kette winziger gefaelschter Boxen) - dieselbe
    // Vorsicht wie beim ProbeDeadline-Timeout im FFmpeg-Pfad: eine defekte
    // Datei darf den Hauptthread nie unbegrenzt blockieren.
    const int kMaxBoxesScanned = 10000;

    // fopen() statt fopen64() schlaegt bei _FILE_OFFSET_BITS=32 (siehe
    // Kommentar oben zu stat64()) fuer Dateien >2GiB mit EOVERFLOW fehl -
    // real aufgetreten bei den grossen WWE/AEW-Events (12-21GB), die genau
    // die Zielgruppe dieser Optimierung sind.
    FILE *f = fopen64(path.c_str(), "rb");
    if (!f)
    {
        SALOG("fast_mp4_probe: %s -> Abbruch: fopen fehlgeschlagen (errno=%d %s)",
              path.c_str(), errno, strerror(errno));
        return -1;
    }

    uint32_t first_size = read32_be(f);
    (void)first_size;
    uint32_t first_type = read32_be(f);
    if (first_type != 0x66747970) // "ftyp"
    {
        fclose(f);
        SALOG("fast_mp4_probe: %s -> Abbruch: erste Box ist nicht ftyp (0x%08x)", path.c_str(), first_type);
        return -1;
    }

    if (fseeko64(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        SALOG("fast_mp4_probe: %s -> Abbruch: seek zum Dateiende fehlgeschlagen", path.c_str());
        return -1;
    }
    int64_t file_size = ftello64(f);
    if (file_size <= 0)
    {
        fclose(f);
        SALOG("fast_mp4_probe: %s -> Abbruch: ftello64 <= 0", path.c_str());
        return -1;
    }

    int64_t pos = 0;
    int boxes_scanned = 0;
    while (pos < file_size && boxes_scanned++ < kMaxBoxesScanned)
    {
        if (fseeko64(f, pos, SEEK_SET) != 0) break;
        uint64_t size = read32_be(f);
        uint32_t type = read32_be(f);
        if (size == 0) break; // laut Spezifikation nur fuer die letzte Box gueltig

        int64_t header_size = 8;
        if (size == 1)
        {
            size = read64_be(f);
            header_size = 16;
        }
        if (size < (uint64_t)header_size) break; // defekte/absichtlich falsche Groesse

        if (type == 0x6d6f6f76) // "moov"
        {
            int64_t moov_end = pos + (int64_t)size;
            int64_t sub_pos = pos + header_size;
            int sub_boxes_scanned = 0;
            while (sub_pos < moov_end && sub_boxes_scanned++ < kMaxBoxesScanned)
            {
                if (fseeko64(f, sub_pos, SEEK_SET) != 0) break;
                uint64_t sub_size = read32_be(f);
                uint32_t sub_type = read32_be(f);
                if (sub_size == 0) break;

                int64_t sub_header_size = 8;
                if (sub_size == 1)
                {
                    sub_size = read64_be(f);
                    sub_header_size = 16;
                }
                if (sub_size < (uint64_t)sub_header_size) break;

                if (sub_type == 0x6d766864) // "mvhd"
                {
                    if (fseeko64(f, sub_pos + sub_header_size, SEEK_SET) != 0) break;
                    uint8_t version = 0;
                    if (fread(&version, 1, 1, f) != 1) break;
                    if (fseeko64(f, 3, SEEK_CUR) != 0) break; // Flags ueberspringen

                    uint64_t timescale = 0;
                    uint64_t duration = 0;
                    if (version == 1)
                    {
                        if (fseeko64(f, 16, SEEK_CUR) != 0) break; // Creation/Modification Time
                        timescale = read32_be(f);
                        duration = read64_be(f);
                    }
                    else
                    {
                        if (fseeko64(f, 8, SEEK_CUR) != 0) break;
                        timescale = read32_be(f);
                        duration = read32_be(f);
                    }

                    fclose(f);
                    if (timescale > 0 && duration > 0 &&
                        duration != 0xFFFFFFFFULL && duration != 0xFFFFFFFFFFFFFFFFULL)
                    {
                        return (int64_t)(duration / timescale);
                    }
                    SALOG("fast_mp4_probe: %s -> Abbruch: mvhd gefunden, aber timescale=%llu duration=%llu ungueltig",
                          path.c_str(), (unsigned long long)timescale, (unsigned long long)duration);
                    return -1;
                }
                sub_pos += (int64_t)sub_size;
            }
            // moov gefunden, aber keine (gueltige) mvhd darin - kein Sinn,
            // nach einer zweiten moov-Box weiterzusuchen.
            fclose(f);
            SALOG("fast_mp4_probe: %s -> Abbruch: moov gefunden (pos=%lld size=%llu), aber keine mvhd darin (sub_boxes_scanned=%d)",
                  path.c_str(), (long long)pos, (unsigned long long)size, sub_boxes_scanned);
            return -1;
        }
        pos += (int64_t)size;
    }
    fclose(f);
    SALOG("fast_mp4_probe: %s -> Abbruch: moov nicht gefunden (boxes_scanned=%d, file_size=%lld)",
          path.c_str(), boxes_scanned, (long long)file_size);
    return -1;
}

int64_t probe_uncached(const std::string &path)
{
    int64_t fast_dur = try_fast_mp4_probe(path);
    if (fast_dur > 0)
    {
        SALOG("ffprobe_length: %s -> %lld Sekunden [fast_mp4_mvhd]", path.c_str(), (long long)fast_dur);
        return fast_dur;
    }

    if (!ensure_ffmpeg_loaded())
        return -1;

    AVFormatContext *ctx = g_symbols.alloc_context();
    if (!ctx)
        return -1;

    ProbeDeadline deadline(kProbeTimeoutMs);
    ctx->interrupt_callback.callback = &ProbeDeadline::check;
    ctx->interrupt_callback.opaque = &deadline;

    struct timeval t_start, t_open_done = {0, 0}, t_done;
    gettimeofday(&t_start, NULL);

    int64_t result = -1;
    const char *path_taken = "open_failed";
    if (g_symbols.open_input(&ctx, path.c_str(), NULL, NULL) == 0)
    {
        gettimeofday(&t_open_done, NULL);
        // Bei vielen Containern (MP4/MOV, MKV) steht die Laufzeit bereits im
        // Header und ist direkt nach avformat_open_input() gueltig - das
        // deutlich teurere avformat_find_stream_info() (liest/dekodiert
        // Pakete zur Codec-Erkennung) wird dann uebersprungen. Wichtig fuer
        // Massenabfragen (z.B. AEL-Moviewall-Rescan ueber eine ganze
        // Bibliothek), die sonst den Hauptthread lange genug blockieren
        // koennen, um einen Hardware-Watchdog-Reset auszuloesen.
        if (ctx->duration > 0)
        {
            result = ctx->duration / AV_TIME_BASE;
            path_taken = "header";
        }
        else if (g_symbols.find_stream_info(ctx, NULL) >= 0 && ctx->duration > 0)
        {
            result = ctx->duration / AV_TIME_BASE;
            path_taken = "find_stream_info";
        }
        else
        {
            path_taken = "find_stream_info_failed";
        }
        g_symbols.close_input(&ctx);
    }
    // Bei Fehlschlag von avformat_open_input() gibt FFmpeg den Kontext selbst
    // frei und setzt ctx=NULL - kein eigener close_input()-Aufruf noetig/erlaubt.

    gettimeofday(&t_done, NULL);
    long open_ms = (t_open_done.tv_sec == 0) ? -1 :
                   (t_open_done.tv_sec - t_start.tv_sec) * 1000
                  + (t_open_done.tv_usec - t_start.tv_usec) / 1000;
    long total_ms = (t_done.tv_sec - t_start.tv_sec) * 1000
                   + (t_done.tv_usec - t_start.tv_usec) / 1000;
    SALOG("ffprobe_length: %s -> %lld Sekunden [%s, open=%ldms, total=%ldms]",
          path.c_str(), (long long)result, path_taken, open_ms, total_ms);
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
