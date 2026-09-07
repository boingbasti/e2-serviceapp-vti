#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

// Gleicher sigc++-Kompatibilitaets-Shim wie in serviceapp.cpp: epgcache.h
// nutzt intern den modernen sigc::-Namespace, dieses Projekt hat aber nur die
// alte sigc++-1.2-API (SigC::) zur Verfuegung (siehe [[project_serviceapp_abi_fixes]]).
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
		m_console->sendCtrlC();
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
		length = probed;

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
