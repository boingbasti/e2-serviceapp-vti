#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <algorithm>
#include <cctype>
#include <unistd.h>

// Gleicher sigc++-Kompatibilitaets-Shim wie in serviceapp.cpp: epgcache.h
// nutzt intern den modernen sigc::-Namespace, dieses Projekt hat aber nur die
// alte sigc++-1.2-API (SigC::) zur Verfuegung.
// Jede .cpp-Datei, die epgcache.h einbindet, braucht diesen Shim vorher lokal,
// er lebt nicht in einem gemeinsamen Header.
#ifdef HAVE_EPG
#include <sigc++/sigc++.h>
namespace sigc {
	using namespace SigC;
	typedef Object trackable;
	typedef Connection connection;

	template <typename T> class slot;
	template <typename R> class slot<R()> : public Slot0<R> { public: slot() {} template <typename T> slot(const T& t) : Slot0<R>(t) {} };
	template <typename R, typename P1> class slot<R(P1)> : public Slot1<R, P1> { public: slot() {} template <typename T> slot(const T& t) : Slot1<R, P1>(t) {} };
	template <typename R, typename P1, typename P2> class slot<R(P1, P2)> : public Slot2<R, P1, P2> { public: slot() {} template <typename T> slot(const T& t) : Slot2<R, P1, P2>(t) {} };

	template <typename T> class signal;
	template <typename R> class signal<R()> : public Signal0<R> { public: signal() {} };
	template <typename R, typename P1> class signal<R(P1)> : public Signal1<R, P1> { public: signal() {} };
	template <typename R, typename P1, typename P2> class signal<R(P1, P2)> : public Signal2<R, P1, P2> { public: signal() {} };
}
#include <lib/dvb/epgcache.h>
#endif

#include <map>

#include "serviceapprecord.h"
#include "common.h"
#include "ffprobe/ffprobe_length.h"
#include "exteplayer3.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

extern bool g_debugLoggingEnabled;
#define SALOG(fmt, ...) do { \
	if (g_debugLoggingEnabled) { \
		FILE *_sf = fopen("/tmp/serviceapp.log", "a"); \
		if (_sf) { fprintf(_sf, "[serviceapp] " fmt "\n", ##__VA_ARGS__); fflush(_sf); fclose(_sf); } \
	} \
} while(0)

DEFINE_REF(eServiceAppRecord);

// Nur rohe (nicht referenzzaehlende) Zeiger: die Registry soll die Objekte
// nicht kuenstlich am Leben halten, sie meldet sich im Destruktor selbst ab.
static std::map<eServiceReference, eServiceAppRecord*> s_activeRecordings;

eServiceAppRecord *eServiceAppRecord::getOrCreate(const eServiceReference &ref)
{
	std::map<eServiceReference, eServiceAppRecord*>::iterator it = s_activeRecordings.find(ref);
	if (it != s_activeRecordings.end())
	{
		SALOG("eServiceAppRecord::getOrCreate: bestehendes Objekt this=%p wiederverwendet", (void*)it->second);
		return it->second;
	}
	eServiceAppRecord *n = new eServiceAppRecord(ref);
	s_activeRecordings[ref] = n;
	return n;
}

eServiceAppRecord::eServiceAppRecord(const eServiceReference &ref):
	m_ref(ref), m_begTime(-1), m_endTime(-1), m_time_create(0),
	m_eit_event_id(-1), m_error(iRecordableService_ENUMS::NoError),
	m_has_event_slot(false), m_userRequestedStop(false)
{
	SALOG("eServiceAppRecord ctor: ref=%s", ref.path.c_str());
}

eServiceAppRecord::~eServiceAppRecord()
{
	SALOG("eServiceAppRecord dtor");
	if (m_console && m_console->running())
		m_console->kill();
	for (std::map<eServiceReference, eServiceAppRecord*>::iterator it = s_activeRecordings.begin(); it != s_activeRecordings.end(); ++it)
	{
		if (it->second == this)
		{
			s_activeRecordings.erase(it);
			break;
		}
	}
}

RESULT eServiceAppRecord::connectEvent(const SigC::Slot2<void,iRecordableService*,int> &event, ePtr<eConnection> &connection)
{
	SALOG("eServiceAppRecord::connectEvent: ENTER this=%p", (void*)this);
	m_event_slot = event;
	m_has_event_slot = true;

	// Gleicher VTi-ABI-Workaround wie eServiceApp::connectEvent() (siehe
	// serviceapp.cpp): eConnection hat keinen nullstellenden Konstruktor,
	// operator new nullt den Speicher nicht - ohne das enthaelt der darin
	// liegende pthread_mutex_t Muell statt PTHREAD_MUTEX_INITIALIZER.
	void *buf = ::operator new(sizeof(eConnection) + 256);
	__builtin_memset(buf, 0, sizeof(eConnection) + 256);
	eConnection *conn = ::new(buf) eConnection((iRecordableService*)this, SigC::Connection());

	connection = conn;
	SALOG("eServiceAppRecord::connectEvent: LEAVE");
	return 0;
}

RESULT eServiceAppRecord::prepare(const char *filename, time_t begTime, time_t endTime, int eit_event_id, const char *name, const char *descr, const char *tags, bool descramble, bool recordecm, int packetsize)
{
	// descramble/recordecm/packetsize sind DVB-Hardware-Entschluesselungs-
	// Parameter, fuer einen reinen ffmpeg-Stream-Copy-Mitschnitt ohne Bedeutung.
	(void)descramble; (void)recordecm; (void)packetsize;

	m_filename = filename;
	// getFilenameExtension() liefert wegen des ABI-Problems (siehe dort)
	// keine Endung, RecordTimer.py haengt also nichts an - hier selbst
	// sicherstellen, dass die Datei immer mit ".ts" endet.
	if (m_filename.size() < 3 || m_filename.compare(m_filename.size() - 3, 3, ".ts") != 0)
		m_filename += ".ts";
	m_begTime = begTime;
	m_endTime = endTime;
	m_eit_event_id = eit_event_id;
	m_name = name ? name : m_ref.getName();
	m_descr = descr ? descr : "";
	m_tags = tags ? tags : "";
	m_time_create = ::time(0);

	SALOG("eServiceAppRecord::prepare: filename=%s name=%s", m_filename.c_str(), m_name.c_str());
	SALOG("eServiceAppRecord::prepare: descr=%s eit_event_id=%d begTime=%ld endTime=%ld tags=%s",
		descr ? descr : "(null)", eit_event_id, (long)begTime, (long)endTime, tags ? tags : "(null)");

	// .meta sofort mit den bekannten Werten anlegen (Laenge/Groesse noch 0,
	// werden in stop() nach Abschluss der Aufnahme nachgetragen) - gleiches
	// Vorgehen wie eDVBServiceRecord::prepare(), das die Datei ebenfalls schon
	// vor dem eigentlichen Aufnahmestart schreibt.
	writeMetaFile(0, 0);
	return 0;
}

void eServiceAppRecord::writeMetaFile(long long length, long long filesize)
{
	// Exaktes Zeilenformat aus eDVBMetaParser::updateMeta() (metaparser.cpp)
	// repliziert, inkl. geleertem path (siehe dort: "ref.path = \"\";") - live
	// gegen eine echte ServiceMP3-Testaufnahme (Typ 4097) verglichen, gleiches
	// Schema.
	std::string metaFilename = m_filename + ".meta";
	FILE *f = fopen(metaFilename.c_str(), "w");
	if (!f)
	{
		SALOG("eServiceAppRecord::writeMetaFile: konnte %s nicht oeffnen", metaFilename.c_str());
		return;
	}
	eServiceReference metaRef = m_ref;
	metaRef.path = "";
	fprintf(f, "%s\n%s\n%s\n%d\n%s\n%lld\n%lld\n%s\n%d\n%d\n",
		metaRef.toString().c_str(), m_name.c_str(), m_descr.c_str(),
		(int)m_time_create, m_tags.c_str(), length, filesize, "", 188, 0);
	fclose(f);
}

namespace {

// Extrem schneller Vorfilter ohne jede I/O: eine URL kann nur dann DASH
// sein, wenn sie auf ein MPD-Manifest verweist. Zattoo/PlutoTV/Mediatheken/
// HLS (.m3u8)/lokale Dateien laufen hier in Mikrosekunden durch, ohne dass
// je ein Netzwerk-Request/FFmpeg-Aufruf stattfindet. Notwendig, weil ein
// bedingungsloser Oeffnungsversuch (siehe Nachtrag unten) bei URLs mit
// angehaengten CLI-Parametern statt einer echten URL (z.B. Zattoo:
// "http://127.0.0.1:8088/https://zattoo.com/live/ard -l debug --zattoo-
// email ...") den GUI-Hauptthread bis zum vollen Timeout blockierte -
// sichtbar als Spinner, gefolgt von einer 0-Byte-Aufnahme, weil ein
// ungeduldiger zweiter Tastendruck die gerade erst gestartete Aufnahme
// sofort wieder stoppte, bevor ffmpeg ueberhaupt Daten schreiben konnte.
bool isPotentialDashUrl(const std::string &url)
{
	std::string lower = url;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return std::tolower(c); });
	return lower.find(".mpd") != std::string::npos ||
	       lower.find("/dash/") != std::string::npos;
}

// Eigener, minimaler Ziffernparser statt strtoll(): vermeidet den
// GLIBC_2.38-Symbolversions-Fallstrick strukturell (ein neuer strtoll()-
// Aufruf im serviceapp-Build hatte serviceapp.so unbrauchbar gemacht) - der
// bereits ergaenzte __isoc23_strtoll-Wrapper in glibc_compat.c bleibt als
// weitere Absicherung fuer kuenftige Faelle bestehen, wird hier aber gar
// nicht erst gebraucht. "variant_bitrate" ist immer eine reine, nicht-
// negative Ganzzahl - kein Vorzeichen, keine Fehlerbehandlung noetig.
int64_t parsePositiveInt64(const char *s)
{
	if (!s) return 0;
	int64_t val = 0;
	while (*s >= '0' && *s <= '9')
	{
		val = val * 10 + (*s - '0');
		s++;
	}
	return val;
}

typedef AVFormatContext *(*avformat_alloc_context_t)(void);
typedef int (*avformat_open_input_t)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **);
typedef void (*avformat_close_input_t)(AVFormatContext **);
typedef AVDictionaryEntry *(*av_dict_get_t)(const AVDictionary *, const char *, const AVDictionaryEntry *, int);

struct DashProbeSymbols
{
	avformat_alloc_context_t alloc_context;
	avformat_open_input_t open_input;
	avformat_close_input_t close_input;
	av_dict_get_t dict_get;
};

enum class DashLibAvailability { Unknown, Available, Unavailable };
DashLibAvailability g_dashLibAvailability = DashLibAvailability::Unknown;
DashProbeSymbols g_dashSymbols = {};

template <typename FuncPtr>
bool loadDashSymbol(void *handle, const char *name, FuncPtr &out)
{
	out = reinterpret_cast<FuncPtr>(dlsym(handle, name));
	if (!out)
		SALOG("probeDashBestVideoStream: Symbol '%s' nicht gefunden: %s", name, dlerror());
	return out != NULL;
}

// Eigene, von ffprobe_length.cpp isolierte dlopen()-Ladung: andere
// Zeitanforderungen (1s statt der dort fuer lokale Dateien passenden 5s)
// und ein anderer fachlicher Zweck (Netzwerk-Stream-Auswahl statt lokale
// Laufzeitermittlung) rechtfertigen eine eigene, unabhaengige Instanz statt
// die bestehende Infrastruktur querzubelasten.
bool ensureDashLibsLoaded()
{
	if (g_dashLibAvailability == DashLibAvailability::Available)
		return true;
	if (g_dashLibAvailability == DashLibAvailability::Unavailable)
		return false;

	// Unversionierter Symlink statt fest kodierter SONAME-Version: eine
	// hartkodierte Versionsnummer waere bruechig gegenueber kuenftigen
	// FFmpeg-Versionswechseln - die Datei "cp -d lib*.so*" in allen
	// build.sh-Skripten liefert den unversionierten Symlink garantiert
	// immer mit aus.
	void *avformatHandle = dlopen("/usr/lib/exteplayer3_deps/libavformat.so", RTLD_NOW);
	if (!avformatHandle)
	{
		SALOG("probeDashBestVideoStream: dlopen(libavformat) fehlgeschlagen: %s", dlerror());
		g_dashLibAvailability = DashLibAvailability::Unavailable;
		return false;
	}
	// av_dict_get() gehoert zu libavutil, nicht zu libavformat - eigenes Handle.
	void *avutilHandle = dlopen("/usr/lib/exteplayer3_deps/libavutil.so", RTLD_NOW);
	if (!avutilHandle)
	{
		SALOG("probeDashBestVideoStream: dlopen(libavutil) fehlgeschlagen: %s", dlerror());
		dlclose(avformatHandle);
		g_dashLibAvailability = DashLibAvailability::Unavailable;
		return false;
	}

	bool ok = true;
	ok = loadDashSymbol(avformatHandle, "avformat_alloc_context", g_dashSymbols.alloc_context) && ok;
	ok = loadDashSymbol(avformatHandle, "avformat_open_input", g_dashSymbols.open_input) && ok;
	ok = loadDashSymbol(avformatHandle, "avformat_close_input", g_dashSymbols.close_input) && ok;
	ok = loadDashSymbol(avutilHandle, "av_dict_get", g_dashSymbols.dict_get) && ok;

	if (!ok)
	{
		dlclose(avutilHandle);
		dlclose(avformatHandle);
		g_dashLibAvailability = DashLibAvailability::Unavailable;
		return false;
	}

	g_dashLibAvailability = DashLibAvailability::Available;
	return true;
}

struct DashProbeDeadline
{
	struct timeval start;
	int timeout_ms;
	explicit DashProbeDeadline(int ms) : timeout_ms(ms) { gettimeofday(&start, NULL); }
	static int check(void *opaque)
	{
		DashProbeDeadline *self = (DashProbeDeadline *)opaque;
		struct timeval now;
		gettimeofday(&now, NULL);
		long elapsed_ms = (now.tv_sec - self->start.tv_sec) * 1000
		                 + (now.tv_usec - self->start.tv_usec) / 1000;
		return elapsed_ms > self->timeout_ms ? 1 : 0;
	}
};

// Ermittelt den besten per Hardware dekodierbaren Video-Stream-Index einer
// DASH-Quelle (fuer "-map 0:<idx>" an ffmpeg), nur fuer URLs aufgerufen,
// die isPotentialDashUrl() bereits als potenziell DASH eingestuft hat.
// KEIN zusaetzlicher find_stream_info()-Aufruf: FFmpegs eigenes
// dash_read_header() ruft in open_demux_for_component() bereits fuer JEDE
// Repraesentation intern find_stream_info() auf und kopiert width/height/
// codec_id per avcodec_parameters_copy() in die aeusseren Streams (dashdec.c
// verifiziert) - die Werte sind direkt nach avformat_open_input() bereits
// vollstaendig gesetzt, ein zweiter Aufruf waere reine Verschwendung von
// Zeit/Bandbreite.
int32_t probeDashBestVideoStream(const std::string &url, int32_t hls_quality_mode)
{
	if (!ensureDashLibsLoaded())
		return -1;

	AVFormatContext *ctx = g_dashSymbols.alloc_context();
	if (!ctx)
		return -1;

	// Ein knappes Timeout im Sekundenbereich schlaegt bei echten DASH-URLs
	// leicht fehl: dashdec.c oeffnet beim Header-Parsing bereits JEDE
	// Video-Repraesentation einzeln (eigener Netzwerk-Roundtrip pro
	// Variante fuer deren Init-Segment, siehe open_demux_for_component() in
	// dashdec.c) - bei mehreren Bitraten-Stufen und/oder schwaecherer
	// Hardware kann das leicht mehrere Sekunden dauern. Der schnelle
	// isPotentialDashUrl()-Vorfilter oben faengt das eigentliche Problem
	// (Verzoegerung bei Nicht-DASH-URLs) bereits vollstaendig ab, ein
	// grosszuegiges Timeout hier ist also kein Nachteil fuer schnellere
	// Hardware, sondern nur eine Obergrenze.
	DashProbeDeadline deadline(8000);
	ctx->interrupt_callback.callback = &DashProbeDeadline::check;
	ctx->interrupt_callback.opaque = &deadline;

	int32_t best_idx = -1;

	if (g_dashSymbols.open_input(&ctx, url.c_str(), NULL, NULL) == 0)
	{
		if (ctx->iformat && !strcmp(ctx->iformat->name, "dash"))
		{
			int64_t best_bandwidth = -1;
			int64_t best_res = -1;
			unsigned int n;

			for (n = 0; n < ctx->nb_streams; n++)
			{
				AVStream *st = ctx->streams[n];
				AVCodecParameters *par = st->codecpar;

				if (par->codec_type != AVMEDIA_TYPE_VIDEO || par->width <= 0)
					continue;

				// Hardware-Decoder-Schutz: dieselbe Grenze wie in
				// exteplayer3s container_ffmpeg.c - H.264 nur bis 1080p60
				// (Level 4.2), 4K nur ueber HEVC decodierbar.
				if (par->codec_id == AV_CODEC_ID_H264 && (par->width > 1920 || par->height > 1080))
				{
					SALOG("probeDashBestVideoStream: stream %u (%dx%d H.264) exceeds hardware limits, skipping",
					      n, par->width, par->height);
					continue;
				}

				int64_t bandwidth = 0;
				AVDictionaryEntry *bw_entry = g_dashSymbols.dict_get(st->metadata, "variant_bitrate", NULL, 0);
				if (bw_entry) bandwidth = parsePositiveInt64(bw_entry->value);
				int64_t res = (int64_t)par->width * par->height;

				if (best_idx < 0 ||
					(hls_quality_mode != 1 && (bandwidth > best_bandwidth || (bandwidth == best_bandwidth && res > best_res))) ||
					(hls_quality_mode == 1 && (bandwidth < best_bandwidth || (bandwidth == best_bandwidth && res < best_res))))
				{
					best_idx = (int32_t)n;
					best_bandwidth = bandwidth;
					best_res = res;
				}
			}

			SALOG("probeDashBestVideoStream: %s -> Stream %d (hls_quality_mode=%d, bandwidth=%lld, res=%lld)",
			      url.c_str(), best_idx, hls_quality_mode, (long long)best_bandwidth, (long long)best_res);
		}

		g_dashSymbols.close_input(&ctx);
	}
	// Bei Fehlschlag von avformat_open_input() gibt FFmpeg den Kontext selbst
	// frei und setzt ctx=NULL - kein eigener close_input()-Aufruf noetig/erlaubt.

	return best_idx;
}

} // namespace

RESULT eServiceAppRecord::start(bool simulate)
{
	if (simulate)
		return 0;

	SALOG("eServiceAppRecord::start: ENTER url=%s -> %s", m_ref.path.c_str(), m_filename.c_str());

	std::vector<std::string> args;
	args.push_back("/usr/bin/ffmpeg");
	args.push_back("-y");
	args.push_back("-nostdin");
	args.push_back("-loglevel");
	args.push_back("warning");
	args.push_back("-reconnect");
	args.push_back("1");
	args.push_back("-reconnect_streamed");
	args.push_back("1");
	args.push_back("-reconnect_delay_max");
	args.push_back("5");

	// Gleiche Header-/User-Agent-Extraktion wie eServiceApp::start() fuer
	// exteplayer3 (siehe serviceapp.cpp), nur auf ffmpegs eigene
	// -user_agent/-headers-Syntax statt exteplayer3s -u/-h uebersetzt.
	HeaderMap headers = getHeaders(m_ref.path);
	HeaderMap::const_iterator uaIt(headers.find("User-Agent"));
	if (uaIt != headers.end())
	{
		args.push_back("-user_agent");
		args.push_back(uaIt->second);
	}
	std::string headersStr;
	for (HeaderMap::const_iterator it(headers.begin()); it != headers.end(); ++it)
	{
		if (it->first.compare("User-Agent") == 0)
			continue;
		headersStr += it->first + ": " + it->second + "\r\n";
	}
	if (!headersStr.empty())
	{
		args.push_back("-headers");
		args.push_back(headersStr);
	}

	args.push_back("-i");
	args.push_back(m_ref.path);

	// DASH liefert mehrere Bitraten-Varianten als getrennte Video-Streams im
	// selben Input (anders als HLS, das exteplayer3 bereits beim Live-
	// Wiedergabepfad ueber AVProgram-Gruppen behandelt, siehe
	// container_ffmpeg.c) - ohne explizite Auswahl nimmt ffmpegs "-c copy"
	// Standardverhalten sonst einfach irgendeine Variante, im schlimmsten
	// Fall eine UHD-H.264-Variante, die diese Box hardwareseitig gar nicht
	// dekodieren kann (Bild bleibt schwarz, Ton laeuft weiter). Der schnelle
	// isPotentialDashUrl()-Vorfilter stellt sicher, dass fuer alle anderen
	// URLs (Zattoo, PlutoTV, HLS, lokale Dateien) ueberhaupt kein Oeffnungs-
	// versuch/Netzwerk-Request stattfindet.
	if (isPotentialDashUrl(m_ref.path))
	{
		int32_t dashBestVideoIdx = probeDashBestVideoStream(m_ref.path, getServiceExt3HlsQualityMode());
		if (dashBestVideoIdx >= 0)
		{
			SALOG("eServiceAppRecord::start: DASH erkannt, waehle Video-Stream %d", dashBestVideoIdx);
			args.push_back("-map");
			args.push_back("0:" + std::to_string(dashBestVideoIdx));
			args.push_back("-map");
			args.push_back("0:a?");
			args.push_back("-map");
			args.push_back("0:s?");
		}
	}

	args.push_back("-c");
	args.push_back("copy");
	args.push_back("-f");
	args.push_back("mpegts");
	args.push_back(m_filename);

	m_console = new eConsoleContainer();
	m_console->appClosed = [this](int retval) { appClosed(retval); };

	char **cargs = (char **)malloc(sizeof(char *) * (args.size() + 1));
	for (size_t i = 0; i < args.size(); i++)
		cargs[i] = strdup(args[i].c_str());
	cargs[args.size()] = NULL;

	int ret = m_console->execute(eApp, cargs[0], cargs);
	for (size_t i = 0; i < args.size(); i++)
		free(cargs[i]);
	free(cargs);

	if (ret != 0)
	{
		SALOG("eServiceAppRecord::start: execute fehlgeschlagen, ret=%d", ret);
		m_error = iRecordableService_ENUMS::errOpenRecordFile;
		if (m_has_event_slot)
			m_event_slot(this, iRecordableService_ENUMS::evRecordFailed);
		return -1;
	}

	SALOG("eServiceAppRecord::start: ffmpeg gestartet, pid=%d", m_console->getPID());
	if (m_has_event_slot)
	{
		m_event_slot(this, iRecordableService_ENUMS::evStart);
		m_event_slot(this, iRecordableService_ENUMS::evRecordRunning);
	}

	// Selbstreferenz: der Aufrufer (RecordTimer/eNavigation) kann seine
	// eigene ePtr<iRecordableService> direkt nach stop() fallen lassen,
	// noch bevor ffmpeg sein asynchrones Prozessende ueber appClosed()
	// meldet. Ohne dieses AddRef() faellt der Refcount dann sofort auf 0
	// und der Destruktor laeuft, bevor appClosed() je aufgerufen wird -
	// beobachtet als fehlende .eit-Datei und fehlende finale Laenge/Groesse
	// im .meta (Crashlogs/Log-Analyse vom 2026-09-06). Release() erfolgt
	// als letzte Aktion in appClosed().
	AddRef();
	return 0;
}

RESULT eServiceAppRecord::getFilenameExtension(std::string &ext)
{
	// RecordTimer.py verkettet "self.Filename + ext" ohne eigenen Punkt
	// dazwischen, daher der fuehrende Punkt hier (sonst entstuende z.B.
	// "...recordts" statt "...record.ts").
	SALOG("eServiceAppRecord::getFilenameExtension: ENTER this=%p &ext=%p", (void*)this, (void*)&ext);
	ext = ".ts";
	SALOG("eServiceAppRecord::getFilenameExtension: nach Zuweisung OK");
	return 0;
}

RESULT eServiceAppRecord::stop()
{
	SALOG("eServiceAppRecord::stop: ENTER this=%p", (void*)this);
	// Von hier an ist ein Nicht-Null-Exitcode kein echter Fehler mehr:
	// dieses ffmpeg beendet sich nach einem SIGINT reproduzierbar mit
	// Exitcode 255 (normal beendet, nicht per Signal getoetet - siehe
	// childstatus-Dekodierung in appClosed()), obwohl die Datei danach
	// vollstaendig und abspielbar ist. Ohne dieses Flag meldete jede
	// vom Nutzer gestoppte Aufnahme faelschlich evRecordWriteError.
	m_userRequestedStop = true;
	if (m_console && m_console->running())
	{
		m_console->sendCtrlC();
		// Watchdog analog zu PlayerApp::processKill() in extplayer.cpp: live
		// mit einer echten Aufnahme nachgewiesen, dass ein multithreaded
		// ffmpeg 7 (DASH-Aufnahme) das SIGINT komplett ignorieren und bis
		// zum natuerlichen Ende der Quelle weiterlaufen kann - ohne diesen
		// Fallback lief eine per Fernbedienung nach 40s gestoppte Aufnahme
		// bis zur vollen Laenge (634s) durch. Bis zu 2500ms auf sauberes
		// Beenden warten (bei hohem Download-Durchsatz braucht ffmpeg
		// spuerbar laenger als die 100ms, die fuer exteplayer3 reichen, um
		// av_write_trailer() sauber abzuschliessen und Netzwerk-Sockets zu
		// schliessen), sonst hart per SIGKILL abbrechen - eine Aufnahme darf
		// nie unbegrenzt weiterlaufen.
		//
		// running() prueft NUR, ob die Pipe-Deskriptoren noch offen sind
		// (myconsole.h) - die werden aber erst in readyRead()/closePipes()
		// geschlossen, ausgeloest vom pollTimer der Enigma2-Mainloop. Da
		// dieser Thread hier synchron in usleep() haengt, kann die Mainloop
		// gar nicht laufen, um das Pipe-HUP zu verarbeiten - running() blieb
		// dadurch IMMER true, selbst wenn ffmpeg laengst als Zombie erkannt
		// wurde (isZombie-Fall unten). Ergebnis: der SIGKILL-Zweig feuerte
		// ausnahmslos bei jeder Aufnahme, obwohl SIGINT tatsaechlich
		// zuverlaessig innerhalb von 10-20ms wirkte - und da kill() niemals
		// appClosed() aufruft, blieb dabei jedes Mal die .meta-Aktualisierung,
		// das evRecordStopped-Event UND das abschliessende Release() aus
		// start() aus (Objekt-Leak, sichtbar an dauerhaft wiederverwendeten
		// this-Zeigern im Log). Eigener stoppedCleanly-Merker statt
		// running() als Abbruchkriterium behebt das strukturell.
		bool stoppedCleanly = false;
		int pid = m_console->getPID();
		// 2500ms statt urspruenglich 1500ms: bei sehr schnellen VOD-
		// Downloads (eine DASH-Testquelle schaufelte in wenigen Sekunden
		// >150MB auf die Platte) oder trägerem ARM-I/O braucht ffmpeg
		// gelegentlich 1,8-2,2s, um den TS-Trailer sauber wegzuschreiben
		// und Netzwerkverbindungen zu schliessen - mit 1500ms griff der
		// SIGKILL-Fallback dort noch unnoetig oft.
		for (int i = 0; i < 250; ++i)
		{
			if (pid > 0)
			{
				char stat_path[64];
				snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
				FILE *f = fopen(stat_path, "r");
				if (f)
				{
					char buffer[256];
					bool isZombie = false;
					if (fgets(buffer, sizeof(buffer), f))
					{
						char *close_paren = strrchr(buffer, ')');
						if (close_paren && *(close_paren + 1) == ' ')
							isZombie = (*(close_paren + 2) == 'Z');
					}
					fclose(f);
					if (isZombie)
					{
						stoppedCleanly = true;
						break;
					}
				}
				else
				{
					stoppedCleanly = true;
					break; // /proc-Eintrag weg - Prozess bereits beendet/reaped
				}
			}
			if (!m_console->running())
			{
				stoppedCleanly = true;
				break;
			}
			usleep(10000); // 10ms
		}
		if (!stoppedCleanly)
		{
			SALOG("eServiceAppRecord::stop: ffmpeg reagiert nicht auf SIGINT, sende SIGKILL");
			// kill() schliesst Pipes/pollTimer hart selbst - readyRead()
			// kann danach nie mehr feuern, appClosed() wuerde also sonst
			// NIE aufgerufen: keine .meta-Aktualisierung, kein
			// evRecordStopped, und vor allem kein Release() (Gegenstueck
			// zum AddRef() aus start()) -> Objekt-Leak. appClosed(255)
			// direkt hier nachziehen behebt das; 255 ist sicher, da
			// m_userRequestedStop bereits oben gesetzt wurde und der
			// retval!=0-Fehlerzweig in appClosed() dadurch uebersprungen
			// wird. WICHTIG: appClosed() endet mit Release() und kann
			// "this" damit zerstoeren - direkt danach darf hier auf keine
			// Member mehr zugegriffen werden.
			m_console->kill();
			appClosed(255);
			return 0;
		}
	}
	// Restliche Signalisierung passiert asynchron in appClosed(), sobald
	// ffmpeg den TS-Trailer geschrieben hat und sich beendet.
	return 0;
}

void eServiceAppRecord::appClosed(int retval)
{
	SALOG("eServiceAppRecord::appClosed: retval=%d", retval);

	long long filesize = 0;
	struct stat64 st;
	if (stat64(m_filename.c_str(), &st) == 0)
		filesize = (long long)st.st_size;

	long long length = 0;
	int64_t probed = ffprobe_get_duration_seconds(m_filename);
	if (probed > 0)
		// writeMetaFile() schreibt "length" roh in Zeile 6 der .meta-Datei -
		// Enigma2s eigener Movielist-Parser (Typ 1, servicedvb.cpp) erwartet
		// dort 90kHz-PTS-Ticks, nicht Sekunden (siehe pts_t-Konvention an
		// anderer Stelle in diesem Code, z.B. eServiceApp::getLength() weiter
		// oben: "pts = (pts_t)length * 90" fuer ms->Ticks). Ohne diese
		// Skalierung rechnete Enigma2 laenge/90000 und erhielt bei jeder
		// Aufnahme unter 25 Stunden 0 -> Fortschrittsbalken/Laufzeit fehlten
		// in der Filmliste komplett (echte .meta-Datei enthielt Sekunden roh
		// in Zeile 6, z.B. "175" statt "15750000").
		length = (long long)probed * 90000LL;

	writeMetaFile(length, filesize);

#ifdef HAVE_EPG
	if (m_eit_event_id >= 0 || (m_begTime > 0 && m_endTime > 0))
	{
		std::string eitFilename = m_filename;
		size_t dot = eitFilename.rfind('.');
		if (dot != std::string::npos)
			eitFilename.erase(dot + 1);
		eitFilename += "eit";
		if (eEPGCache::getInstance())
			eEPGCache::getInstance()->saveEventToFile(eitFilename.c_str(), m_ref, m_eit_event_id, m_begTime, m_endTime);
	}
#endif

	if (retval != 0 && !m_userRequestedStop)
	{
		// iRecordableService_ENUMS kennt keinen generischen "Schreibfehler"-
		// Fehlercode - errDiskFull ist die naheliegendste verfuegbare
		// Naeherung fuer einen ffmpeg-Abbruch nach bereits erfolgtem Start
		// (haeufigste reale Ursache: voller Datentraeger).
		SALOG("eServiceAppRecord::appClosed: ffmpeg nicht sauber beendet (retval=%d)", retval);
		m_error = iRecordableService_ENUMS::errDiskFull;
		if (m_has_event_slot)
			m_event_slot(this, iRecordableService_ENUMS::evRecordWriteError);
	}
	else if (m_has_event_slot)
	{
		m_event_slot(this, iRecordableService_ENUMS::evRecordStopped);
	}

	if (m_has_event_slot)
		m_event_slot(this, iRecordableService_ENUMS::evEnd);

	// Gegenstueck zum AddRef() in start(): ab hier darf das Objekt wieder
	// zerstoert werden, sobald auch der Aufrufer seine eigene Referenz
	// freigegeben hat. Muss die letzte Aktion in dieser Methode sein, da
	// "this" danach ungueltig sein kann.
	Release();
}
