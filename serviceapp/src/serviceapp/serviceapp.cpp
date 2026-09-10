// stat64()/struct stat64/off64_t explizit verfuegbar machen: der Build setzt
// _FILE_OFFSET_BITS=32, wodurch das normale stat() bei Dateien >2GiB mit
// EOVERFLOW fehlschlaegt (real aufgetreten bei sTimeCreate/sFileSize/
// getFileSize unten fuer 6GB+-Dateien). _LARGEFILE64_SOURCE ist unabhaengig
// von _FILE_OFFSET_BITS und ergaenzt nur die *64-Varianten, ohne bestehendes
// Verhalten zu aendern.
#define _LARGEFILE64_SOURCE
#include "Python.h"
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <endian.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "ffprobe/ffprobe_length.h"

#include <lib/service/service.h>
#include <lib/components/file_eraser.h>
#include <lib/base/init_num.h>
#include <lib/base/init.h>
#include <lib/base/eenv.h>
#include <lib/base/nconfig.h>
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
#include <dvbsi++/event_information_section.h>
#endif
#include <lib/gui/esubtitle.h>
#include <lib/dvb/idvb.h>

#include "serviceapp.h"
#include "gstplayer.h"
#include "exteplayer3.h"
#include "serviceapprecord.h"

/* eerror.h declares "extern int debugLvl;" for the eDebug()/eLog() macros (checked
   inline on every call when built with -DDEBUG), but the real enigma2 binary does not
   export it in its dynamic symbol table (confirmed via nm -D), so it can never resolve
   at plugin load time. Provide my own local definition so the reference is always
   satisfied within serviceapp.so itself, same pattern as glibc_compat.c. */
int debugLvl = 4; // lvlDebug

/* Master switch for the raw SALOG()/M3ULOG() logging macros (fopen/fprintf/fflush/fclose
   per call, one per source file, ungated by design until now). Controlled by the
   "Debug logging" Setup toggle via serviceapp_set_setting(); off by default to avoid
   filling /tmp (a RAM disk on these boxes) during long playback sessions. */
bool g_debugLoggingEnabled = false;

/* Same problem for eDebugImpl(int flags, const char*, ...), the actual logging
   function behind the eDebug()/eLog() macros: not exported by the real enigma2
   binary either, so provide my own implementation writing to /tmp/serviceapp.log
   via the same mechanism as SALOG. */
#include <cstdarg>
void eDebugImpl(int flags, const char *fmt, ...)
{
	(void)flags;
	FILE *_sf = fopen("/tmp/serviceapp.log", "a");
	if (!_sf)
		return;
	fprintf(_sf, "[serviceapp] ");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(_sf, fmt, ap);
	va_end(ap);
	fprintf(_sf, "\n");
	fflush(_sf);
	fclose(_sf);
}

#define SALOG(fmt, ...) do { \
    if (g_debugLoggingEnabled) { \
        FILE *_sf = fopen("/tmp/serviceapp.log", "a"); \
        if (_sf) { fprintf(_sf, "[serviceapp] " fmt "\n", ##__VA_ARGS__); fflush(_sf); fclose(_sf); } \
    } \
} while(0)

__attribute__((constructor))
static void sa_so_loaded(void) {
    SALOG(".so LOADED");
}

enum
{
	SUBSERVICES_INDEX_START = 1,
	SUBSERVICES_INDEX_END = 0xFF,
	SUBSERVICES_BITRATEKB_START = 0x100
};

enum
{
	EXTEPLAYER3,
	GSTPLAYER,
};

enum
{
	OPTIONS_SERVICEMP3,
	OPTIONS_SERVICEGSTPLAYER,
	OPTIONS_SERVICEEXTEPLAYER3,
	OPTIONS_USER,
};

static int g_playerServiceMP3 = GSTPLAYER;
static bool g_useUserSettings = false;

static GstPlayerOptions g_GstPlayerOptionsServiceMP3;
static GstPlayerOptions g_GstPlayerOptionsServiceGst;
static GstPlayerOptions g_GstPlayerOptionsUser;

static ExtEplayer3Options g_ExtEplayer3OptionsServiceMP3;
static ExtEplayer3Options g_ExtEplayer3OptionsServiceExt3;
static ExtEplayer3Options g_ExtEplayer3OptionsUser;

int getServiceExt3HlsQualityMode()
{
	return g_ExtEplayer3OptionsServiceExt3.hlsQualityMode;
}

static eServiceAppOptions g_ServiceAppOptionsServiceMP3;
static eServiceAppOptions g_ServiceAppOptionsServiceExt3;
static eServiceAppOptions g_ServiceAppOptionsServiceGst;
static eServiceAppOptions g_ServiceAppOptionsUser;

static const std::string gReplaceServiceMP3Path = eEnv::resolve("$sysconfdir/enigma2/serviceapp_replaceservicemp3");
static const bool gReplaceServiceMP3 = ( access( gReplaceServiceMP3Path.c_str(), F_OK ) != -1 );

static BasePlayer *createPlayer(const eServiceReference& ref)
{
	BasePlayer *player = NULL;
	if (ref.type == eServiceFactoryApp::idServiceExtEplayer3 || (ref.type == eServiceFactoryApp::idServiceMP3 && g_playerServiceMP3 == EXTEPLAYER3) )
	{
		ExtEplayer3Options *options = NULL;
		if (g_useUserSettings)
			options = &g_ExtEplayer3OptionsUser;
		else if (ref.type == eServiceFactoryApp::idServiceExtEplayer3)
			options = &g_ExtEplayer3OptionsServiceExt3;
		else
			options = &g_ExtEplayer3OptionsServiceMP3;
		player = new ExtEplayer3(*options);
	}
	else if (ref.type == eServiceFactoryApp::idServiceGstPlayer || (ref.type == eServiceFactoryApp::idServiceMP3 && g_playerServiceMP3 == GSTPLAYER) )
	{
		GstPlayerOptions *options = NULL;
		if (g_useUserSettings)
			options = &g_GstPlayerOptionsUser;
		else if (ref.type == eServiceFactoryApp::idServiceGstPlayer)
			options = &g_GstPlayerOptionsServiceGst;
		else
			options = &g_GstPlayerOptionsServiceMP3;
		player = new GstPlayer(*options);
	}
	return player;
}

static eServiceAppOptions *createOptions(const eServiceReference& ref)
{
	eServiceAppOptions *options = NULL;
	switch(ref.type)
	{
		case eServiceFactoryApp::idServiceMP3:
			options = &g_ServiceAppOptionsServiceMP3;
			break;
                case eServiceFactoryApp::idServiceExtEplayer3:
			options = &g_ServiceAppOptionsServiceExt3;
			break;
                case eServiceFactoryApp::idServiceGstPlayer:
			options = &g_ServiceAppOptionsServiceGst;
			break;
		default:
			break;
	}
	if(g_useUserSettings)
	{
		options = &g_ServiceAppOptionsUser;
	}
	return new eServiceAppOptions(*options);
}


class eServiceOfflineOperations: public iServiceOfflineOperations
{
	DECLARE_REF(eServiceOfflineOperations);
	eServiceReference m_ref;
public:
	eServiceOfflineOperations(const eServiceReference &ref);

	RESULT deleteFromDisk(int simulate);
	RESULT getListOfFilenames(std::list<std::string> &);
	RESULT reindex();
};

DEFINE_REF(eServiceOfflineOperations);

eServiceOfflineOperations::eServiceOfflineOperations(const eServiceReference &ref): m_ref((const eServiceReference&)ref)
{
}

RESULT eServiceOfflineOperations::deleteFromDisk(int simulate)
{
	if (!simulate)
	{
		std::list<std::string> res;
		if (getListOfFilenames(res))
			return -1;

		eBackgroundFileEraser *eraser = eBackgroundFileEraser::getInstance();
		if (!eraser)
			eDebug("[eServiceOfflineOperations] FATAL !! can't get background file eraser");

		for (std::list<std::string>::iterator i(res.begin()); i != res.end(); ++i)
		{
			eDebug("[eServiceOfflineOperations] Removing %s...", i->c_str());
			if (eraser)
				eraser->erase(i->c_str());
			else
				::unlink(i->c_str());
		}
	}
	return 0;
}

RESULT eServiceOfflineOperations::getListOfFilenames(std::list<std::string> &res)
{
	res.clear();
	res.push_back(m_ref.path);
	return 0;
}

RESULT eServiceOfflineOperations::reindex()
{
	return -1;
}


RESULT eServiceFactoryApp::play(const eServiceReference &ref, ePtr<iPlayableService> &ptr)
{
	SALOG("play() called type=%d path=%s", ref.type, ref.path.c_str());
	ptr = new eServiceApp(ref);
	SALOG("play() done ptr=%p", (void*)ptr.operator->());
	return 0;
}

RESULT eServiceFactoryApp::record(const eServiceReference &ref, ePtr<iRecordableService> &ptr)
{
	SALOG("record() called type=%d path=%s", ref.type, ref.path.c_str());
	ptr = eServiceAppRecord::getOrCreate(ref);
	return 0;
}

RESULT eServiceFactoryApp::offlineOperations(const eServiceReference &ref, ePtr<iServiceOfflineOperations> &ptr)
{
	ptr = new eServiceOfflineOperations(ref);
	return 0;
}


void eServiceApp::AddRef()
{
	++ref;
}
void eServiceApp::Release()
{
	int r = --ref;
	if (!r) delete this;
}

eServiceApp::eServiceApp(eServiceReference ref):
	m_ref(ref),
	m_subservices_checked(false),
	player(0),
	extplayer(0),
	m_paused(false),
	m_framerate(-1),
	m_width(-1),
	m_height(-1),
	m_progressive(-1),
	m_subtitle_pages(0),
	m_has_selected_subtitle_track(false),
	m_prev_subtitle_message(0),
	m_prev_subtitle_fps(1),
	m_prev_decoder_time(-1),
	m_decoder_time_valid_state(0),
	my_subtitle_sync_timer(0),
	my_event_updated_info_timer(0)
#ifdef HAVE_EPG
	, my_nownext_timer(0),
	m_event_now_raw(0),
	m_event_next_raw(0)
#endif
	, m_cuesheet_changed(false),
	m_cutlist_enabled(0)
{
	SALOG("eServiceApp ctor: start path=%s", ref.path.c_str());
	SALOG("eServiceApp ctor: sizeof(time_t)=%zu sizeof(timespec)=%zu", sizeof(time_t), sizeof(timespec));
	SALOG("eServiceApp ctor: sizeof(eServiceReference)=%zu offsetof_path=%zu", sizeof(eServiceReference), (size_t)&(((eServiceReference*)0)->path));
	options = createOptions(ref);
	SALOG("eServiceApp ctor: options created");
	extplayer = createPlayer(ref);
	SALOG("eServiceApp ctor: extplayer created");
	player = new PlayerBackend(extplayer);
	SALOG("eServiceApp ctor: PlayerBackend created");

	m_subtitle_widget = 0;
	SALOG("ctor: eApp=%p sizeof_eTimer=%zu", (void*)eApp, sizeof(eTimer));
	my_subtitle_sync_timer = eTimer::create(eApp);
	initTimerMutex(my_subtitle_sync_timer);
	my_subtitle_sync_timer->AddRef();
	SALOG("ctor: my_subtitle_sync_timer OK");
	my_subtitle_sync_timer->timeout.connect(
		SigC::bind(SigC::slot(&eServiceApp::s_pushSubtitles), this));
	SALOG("ctor: my_subtitle_sync_timer connected");

	SALOG("ctor: before my_event_updated_info_timer create");
	my_event_updated_info_timer = eTimer::create(eApp);
	SALOG("ctor: after my_event_updated_info_timer create=%p", (void*)my_event_updated_info_timer);
	initTimerMutex(my_event_updated_info_timer);
	my_event_updated_info_timer->AddRef();
	SALOG("ctor: after my_event_updated_info_timer AddRef");
	my_event_updated_info_timer->timeout.connect(
		SigC::bind(SigC::slot(&eServiceApp::s_signalEventUpdatedInfo), this));
	SALOG("ctor: my_event_updated_info_timer connected");

#ifdef HAVE_EPG
	my_nownext_timer = eTimer::create(eApp);
	initTimerMutex(my_nownext_timer);
	my_nownext_timer->AddRef();
	my_nownext_timer->timeout.connect(
		SigC::bind(SigC::slot(&eServiceApp::s_updateEpgCacheNowNext), this));
#endif
	SALOG("ctor: before gotPlayerMessage connect");
	player->gotPlayerMessage = [this](int msg) { gotExtPlayerMessage(msg); };
	m_has_event_slot = false;
	SALOG("ctor: DONE");
};

eServiceApp::~eServiceApp()
{
	SALOG("~eServiceApp: start");
	delete options;
	SALOG("~eServiceApp: options deleted");
	delete player;
	SALOG("~eServiceApp: player deleted");
	delete extplayer;
	SALOG("~eServiceApp: extplayer deleted");

	if (my_subtitle_sync_timer) {
		my_subtitle_sync_timer->stop();
		my_subtitle_sync_timer->Release();
		my_subtitle_sync_timer = 0;
	}
	if (my_event_updated_info_timer) {
		my_event_updated_info_timer->stop();
		my_event_updated_info_timer->Release();
		my_event_updated_info_timer = 0;
	}
	SALOG("~eServiceApp: timers destroyed");

	if (m_subtitle_widget) m_subtitle_widget->destroy();
	m_subtitle_widget = 0;
#ifdef HAVE_EPG
	if (my_nownext_timer) {
		my_nownext_timer->stop();
		my_nownext_timer->Release();
		my_nownext_timer = 0;
	}
	if (m_event_now_raw) {
		delete m_event_now_raw;
		m_event_now_raw = 0;
	}
	if (m_event_next_raw) {
		delete m_event_next_raw;
		m_event_next_raw = 0;
	}
#endif
	g_useUserSettings = false;
	SALOG("~eServiceApp: done");
};



void eServiceApp::fillSubservices()
{
	m_subservice_vec.clear();
	m_subserviceref_vec.clear();

        if (isM3U8Url(m_ref.path))
	{
		M3U8VariantsExplorer ve(m_ref.path, getHeaders(m_ref.path));
		m_subservice_vec = ve.getStreams(options->HLSAudioFilter);
		if (m_subservice_vec.empty())
		{
			eDebug("eServiceApp::fillSubservices - failed to retrieve subservices");
		}
		else
		{
			// sort subservices from best quality to worst (internally sorted according to bitrate)
			sort(m_subservice_vec.rbegin(), m_subservice_vec.rend());

			std::vector<M3U8StreamInfo>::const_iterator it;
			// find title from parent, if parent has already bitrate
			// string set, we look for this bitrate and separate original
			// name from it.
			std::string original_title(m_ref.name);
			for (it = m_subservice_vec.begin(); it != m_subservice_vec.end(); it++)
			{
				char bitrate_buf[32];
				snprintf(bitrate_buf, sizeof(bitrate_buf), "%lu", it->bitrate);
				std::string bitrate_str(bitrate_buf);
				size_t bitrate_idx = m_ref.name.find(": " + bitrate_str);
				if (bitrate_idx != std::string::npos)
				{
					original_title = m_ref.name.substr(0, bitrate_idx);
					break;
				}
			}

			int i = 0;
			for (it = m_subservice_vec.begin(); it != m_subservice_vec.end(); it++, i++)
			{
				if (SUBSERVICES_INDEX_START + i > SUBSERVICES_INDEX_END)
				{
					eWarning("eServiceApp::fillSubservices - cannot add more then %d subservices!", SUBSERVICES_INDEX_END);
					break;
				}
				// we need to copy all flags from parent service, neccessary for EPG
				eServiceReference ref(m_ref);
				// set index so we know which service to select from master playlist
				ref.setUnsignedData(7, SUBSERVICES_INDEX_START + i);
				// set parentTransportStreamId, since InfoBarSubservicesSupport
				// checks this flag when creating subservices menu. If it's available
				// at least for one subservice then it will allow to add subservices 
				// to bouquet or favorites, see subserviceSelection.
				//
				// If it's not available it will only allow to quickzap subservices and it
				// will also remove name for subservice service, see playSubservice.
				// eServiceReferenceDVB hat ein 'flags2'-Feld das eServiceReference
				// nicht besitzt — der Cast würde 4 Bytes hinter ref schreiben
				// und den Stack korrumpieren (Commit 4d3391f, echter Absturz).
				//
				// Bewusster, noch nicht behobener Funktionsverzicht: Der Fix hat
				// das Setzen von ParentServiceID/ParentTransportStreamID komplett
				// entfernt statt es sicher (z.B. über ein eigenes, richtig
				// dimensioniertes eServiceReferenceDVB-Objekt statt eines Casts
				// auf ref) wiederherzustellen. Laut Kommentar oben heisst das:
				// HLS-Subservices (Bitraten-Varianten) lassen sich seitdem nur
				// noch quickzappen, nicht mehr dauerhaft als eigener Favorit/
				// Bouquet-Eintrag speichern. Quickzap selbst ist unveraendert
				// funktionsfaehig. TODO: bei Gelegenheit sicher wiederherstellen.
				char bitrate_buf2[32];
				snprintf(bitrate_buf2, sizeof(bitrate_buf2), "%lu", it->bitrate);
				ref.name = original_title + ": " + bitrate_buf2 + "b/s";
				if (!it->resolution.empty())
					ref.name += " - " + it->resolution;
				m_subserviceref_vec.push_back(ref);
			}
			eDebug("eServiceApp::fillSubservices - found %zd subservices", m_subservice_vec.size());
		}
	}
	else
	{
		eDebug("eServiceApp::fillSubservices - failed to retrieve subservices, not supported url");
	}
}

#ifdef HAVE_EPG
void eServiceApp::updateEpgCacheNowNext()
{
	ePtr<iPlayableService> guard = this;
	bool update = false;
	Event *ptr_now = 0;
	Event *ptr_next = 0;
	eServiceReference ref;
	ref.flags = m_ref.flags;
	for (int i = 0; i < 8; ++i)
	{
		ref.data[i] = m_ref.getData(i);
	}

	if (eEPGCache::getInstance())
	{
		ref.type = m_ref.getData(0);
		if (ref.type > 0)
		{
			eEPGCache::getInstance()->lookupEventTime(ref, -1, ptr_now, 0);
			eEPGCache::getInstance()->lookupEventTime(ref, -1, ptr_next, 1);
		}
		else
		{
			ref.type = eServiceFactoryApp::idServiceMP3; // 4097
			eEPGCache::getInstance()->lookupEventTime(ref, -1, ptr_now, 0);
			eEPGCache::getInstance()->lookupEventTime(ref, -1, ptr_next, 1);
		}
	}

	if (m_event_now_raw != ptr_now || m_event_next_raw != ptr_next)
	{
		if (m_event_now_raw) delete m_event_now_raw;
		if (m_event_next_raw) delete m_event_next_raw;
		m_event_now_raw = ptr_now;
		m_event_next_raw = ptr_next;
		update = true;
	}
	else
	{
		if (ptr_now) delete ptr_now;
		if (ptr_next) delete ptr_next;
	}

	my_nownext_timer->start(60 * 1000, true); // Refresh every 60 seconds
	if (update)
	{
		if (m_has_event_slot) m_event_slot((iPlayableService*)this, evUpdatedEventInfo);
	}
}
#endif

ssize_t eServiceApp::getTrackPosition(const SubtitleTrack &track)
{
	ssize_t track_pos = -1;
	std::vector<SubtitleTrack>::const_iterator it(m_subtitle_tracks.begin());
	for (size_t i = 0; it != m_subtitle_tracks.end(); it++,i++)
	{
		if (it->pid == track.pid
				&& it->type == track.type
				&& it->page_number == track.page_number
				&& it->magazine_number == track.magazine_number)
		{
			track_pos = i;
			break;
		}
	}
	return track_pos;
}

void eServiceApp::addEmbeddedTrack(std::vector<struct SubtitleTrack> &subtitlelist, subtitleStream &s, int pid)
{
	m_subtitle_streams.push_back(s);
	struct SubtitleTrack track;
	track.type = 2;
	track.page_number = 1;
	track.magazine_number = 0;
	track.pid = pid;
	track.language_code = s.language_code;

	subtitlelist.push_back(track);
	m_subtitle_tracks.push_back(track);
}

void eServiceApp::addExternalTrack(std::vector<struct SubtitleTrack> &subtitlelist, int pid, std::string lang, std::string path)
{
	subtitleStream s;
	s.path = path;
	m_subtitle_streams.push_back(s);

	struct SubtitleTrack track;
	track.type = 2;
	track.page_number = 4;
	track.magazine_number = 0;
	track.pid = pid;
	track.language_code = lang;

	subtitlelist.push_back(track);
	m_subtitle_tracks.push_back(track);
}

bool eServiceApp::isEmbeddedTrack(const SubtitleTrack &track)
{
	return (track.type == 2 && track.page_number == 1);
}

bool eServiceApp::isExternalTrack(const SubtitleTrack &track)
{
	return (track.type == 2 && track.page_number == 4);
}

void eServiceApp::pullSubtitles()
{
	std::queue<subtitleMessage> pulled;
	player->getSubtitles(pulled);
	eDebug("eServiceApp::pullSubtitles - pulling %d subtitles", pulled.size());
	while (!pulled.empty())
	{
		subtitleMessage sub = pulled.front();
		m_embedded_subtitle_pages.insert(subtitle_pages_map_pair(sub.end_ms, sub));
		pulled.pop();
	}
	my_subtitle_sync_timer->start(1, true);
}

void eServiceApp::pushSubtitles()
{
	pts_t running_pts = 0;
	int32_t next_timer = 0, decoder_ms, start_ms, end_ms, diff_start_ms, diff_end_ms;
	subtitle_pages_map::const_iterator current;

	int delay = eConfigManager::getConfigIntValue("config.subtitles.pango_subtitles_delay");
	if (m_has_selected_subtitle_track && isExternalTrack(m_selected_subtitle_track))
	{
		int subtitle_fps = eConfigManager::getConfigIntValue("config.subtitles.pango_subtitles_fps");
		if (subtitle_fps != m_prev_subtitle_fps)
		{
			m_prev_subtitle_fps = subtitle_fps;
			ssize_t track_pos = getTrackPosition(m_selected_subtitle_track);
			const subtitleMap *submap = NULL;
			if (track_pos != -1)
			submap = m_subtitle_manager.load(m_subtitle_streams[track_pos].path, m_framerate, subtitle_fps);
			if (submap)
			{
				m_prev_subtitle_message = NULL;
				m_subtitle_pages = submap;
			}
		}
	}

	if (!m_subtitle_pages)
		return;

	if (getPlayPosition(running_pts) < 0)
	{
		m_decoder_time_valid_state = 0;
		next_timer = 50;
		goto exit;
	}
	if (m_decoder_time_valid_state < 3)
	{
		m_decoder_time_valid_state++;
		// this happens after we start seeking operation
		// decoder pts is not updated, we have to wait
		// for seek to finish.
		if (m_prev_decoder_time == running_pts)
		{
			m_decoder_time_valid_state = 0;
		}
		if (m_decoder_time_valid_state < 3)
		{
			// eDebug("eServiceApp::pushSubtitles - waiting for clock to stabilise: valid=%d, prev=%lld,current=%lld",
			//		m_decoder_time_valid_state, m_prev_decoder_time, decoder_ms);
			m_prev_decoder_time = running_pts;
			// we are updating play position every 100ms in extplayer
			// so to see any progress in decoder_ms we have to wait a little longer
			next_timer = 110;
			goto exit;
		}
		// eDebug("eServiceApp::pushSubtitles - push subtitles, clock stable");
	}
	decoder_ms = (running_pts - delay) / 90;

	for (current = m_subtitle_pages->lower_bound(decoder_ms); current != m_subtitle_pages->end(); current++)
	{
		start_ms = current->second.start_ms;
		end_ms = current->second.end_ms;
		diff_start_ms = start_ms - decoder_ms;
		diff_end_ms = end_ms - decoder_ms;
		
		//eDebug("eServiceApp::pushSubtitles - next subtitle: decoder: %d, start: %d, end: %d, duration_ms: %d, diff_start: %d, diff_end: %d : %s",
		//	decoder_ms, start_ms, end_ms, end_ms - start_ms, diff_start_ms, diff_end_ms, current->second.text.c_str());

		if (diff_end_ms < 0)
		{
			//eDebug("eServiceApp::pushSubtitles - current sub has already ended, skip: %d", diff_end_ms);
			continue;
		}
		if (diff_start_ms > 50)
		{
			//eDebug("eServiceApp::pushSubtitles - current sub in the future, start timer, %d", diff_start_ms);
			next_timer = diff_start_ms;
			goto exit;
		}
		// don't show the same message twice
		if (m_prev_subtitle_message && m_prev_subtitle_message == &(current->second))
		{
			next_timer = 30;
			goto exit;
		}
		if (m_subtitle_widget && !m_paused)
		{
			//eDebug("eServiceApp::pushSubtitles - current sub actual, show!");
			m_prev_subtitle_message = &(current->second);
			ePangoSubtitlePage pango_page;
			gRGB rgbcol(0xD0,0xD0,0xD0);

			pango_page.m_elements.push_back(ePangoSubtitlePageElement(rgbcol, current->second.text.c_str()));
			pango_page.m_show_pts = start_ms * 90; // actually completely unused by widget!
			pango_page.m_timeout = end_ms - decoder_ms; // take late start into account

			m_subtitle_widget->setPage(pango_page);
		}
		//eDebug("eServiceApp::pushSubtitles - no next sub scheduled, check NEXT subtitle");
	}
exit:
	if (next_timer == 0)
	{
		//eDebug("eServiceApp::pushSubtitles - next timer = 0, set default timer!");
		next_timer = 1000;
	}
	my_subtitle_sync_timer->start(next_timer, true);
}

void eServiceApp::signalEventUpdatedInfo()
{
	ePtr<iPlayableService> guard = this;
	eDebug("eServiceApp::signalEventUpdatedInfo");
	if (m_has_event_slot) m_event_slot(this, evUpdatedInfo);
}

void eServiceApp::gotExtPlayerMessage(int message)
{
	ePtr<iPlayableService> guard = this;
	switch (message)
	{
		case PlayerMessage::start:
			eDebug("eServiceApp::gotExtPlayerMessage - start");
			if (m_has_event_slot) m_event_slot(this, evUpdatedEventInfo);
			if (m_has_event_slot) m_event_slot(this, evStart);
			my_event_updated_info_timer->start(1000, true);
#ifdef HAVE_EPG
			updateEpgCacheNowNext();
#endif
			break;
		case PlayerMessage::stop:
			eDebug("eServiceApp::gotExtPlayerMessage - stop");
			// evEOF signals that end of file was reached and we
			// could make operations like seek back or play again, 
			// however when player signals stop, process
			// has already ended, so there is no possibility to do so.
			// This should be fixed on player's side so it doesn't end
			// immediately but waits at the end for our input..
			if (m_has_event_slot) m_event_slot(this, evEOF);
			break;
		case PlayerMessage::pause:
			eDebug("eServiceApp::gotExtPlayerMessage - pause");
			m_paused = true;
			break;
		case PlayerMessage::resume:
			eDebug("eServiceApp::gotExtPlayerMessage - resume");
			m_paused = false;
			break;
		case PlayerMessage::error:
			eDebug("eServiceApp::gotExtPlayerMessage - error");
			if (m_has_event_slot) m_event_slot(this, evUser + 12);
			break;
		case PlayerMessage::videoSizeChanged:
		{
			eDebug("eServiceApp::gotExtPlayerMessage - videoSizeChanged");
			videoStream v;
			if (!player->videoGetTrackInfo(v,0))
			{
				m_width = v.width;
				m_height = v.height;
			}
			if (m_has_event_slot) m_event_slot(this, evVideoSizeChanged);
			break;
		}
		case PlayerMessage::videoFramerateChanged:
		{
			eDebug("eServiceApp::gotExtPlayerMessage - videoFramerateChanged");
			videoStream v;
			if (!player->videoGetTrackInfo(v,0))
			{
				m_framerate = v.framerate;
			}
			if (m_has_event_slot) m_event_slot(this, evVideoFramerateChanged);
			break;
		}
		case PlayerMessage::videoProgressiveChanged:
		{
			eDebug("eServiceApp::gotExtPlayerMessage - videoProgressiveChanged");
			videoStream v;
			if (!player->videoGetTrackInfo(v,0))
			{
				m_progressive = v.progressive;
			}
			if (m_has_event_slot) m_event_slot(this, evVideoProgressiveChanged);
			break;
		}
		case PlayerMessage::subtitleAvailable:
			eDebug("eServiceApp::gotExtPlayerMessage - subtitleAvailable");
			if (m_has_selected_subtitle_track && isEmbeddedTrack(m_selected_subtitle_track))
				pullSubtitles();
			break;
		default:
			eDebug("eServiceApp::gotExtPlayerMessage - unhandled message");
			break;
	}
}


// __iPlayableService
RESULT eServiceApp::connectEvent(const SigC::Slot2< void, iPlayableService*, int >& event, ePtr< eConnection >& connection)
{
	SALOG("eServiceApp::connectEvent: ENTER");
	m_event_slot = event;
	m_has_event_slot = true;

	// VTi ABI workaround: zero-initialize connection memory.
	// Since eConnection inherits DECLARE_REF, it contains a pthread_mutex_t at offset 12.
	// Because eConnection has no constructor that zeroes the mutex, and operator new does not zero memory,
	// the mutex contains garbage. Zeroing the allocated memory provides a clean, zeroed mutex (PTHREAD_MUTEX_INITIALIZER).
	void *buf = ::operator new(sizeof(eConnection) + 256);
	__builtin_memset(buf, 0, sizeof(eConnection) + 256);
	eConnection *conn = ::new(buf) eConnection((iPlayableService*)this, SigC::Connection());

	connection = conn;
	SALOG("eServiceApp::connectEvent: LEAVE");
	return 0;
}

/* connection.h (VTi stub) only declares eConnection::AddRef()/Release() via
   DECLARE_REF without a body, expecting the real enigma2 binary to provide
   them (and thus the vtable) externally. Neither symbol is exported in the
   binary's dynamic symbol table on ARM or MIPS; ARM's dynamic linker
   tolerates the resulting undefined vtable reference (lazy/never actually
   dereferenced in practice), MIPS's does not and refuses to dlopen the
   plugin at all ("undefined symbol: _ZTV11eConnection"). Provide my own
   trivial ref-counting implementation so the vtable is emitted locally
   instead of expected from outside. */
DEFINE_REF(eConnection);

/* ebase.h (VTi stub) declares eTimer::startLongTimer(int) without a body,
   same as start()/stop()/changeInterval() — all four are meant to resolve
   externally against the real enigma2 binary. start()/stop() are proven to
   work (used extensively throughout this file already), but startLongTimer()
   is new (only reachable via the previously-dead HAVE_EPG code) and refuses
   to resolve at all on MIPS ("undefined symbol: _ZN6eTimer14startLongTimerEi"),
   blocking dlopen entirely. Rather than rely on whatever mechanism resolves
   the other three, implement startLongTimer() ourselves purely in terms of
   the already-working public start(long msec, bool singleShot), replicating
   the real implementation's effect (a single-shot timer seconds*1000ms out)
   without touching any private members. */
void eTimer::startLongTimer(int seconds)
{
	start(seconds > 0 ? (long)seconds * 1000L : 0L, true);
}

RESULT eServiceApp::start()
{
	SALOG("eServiceApp::start: ENTER");
	// Muss lange vor dem asynchronen PlayerMessage::start/evStart geladen sein,
	// da InfoBarCueSheetSupport.__serviceStarted() synchron an evStart haengt
	// und sofort getCutList() abfragt. Braucht nur die Datei von der Platte,
	// keine Player-Position - kein Warten auf den Player noetig.
	if (isLocalFile()) loadCuesheet();
	std::string path_str(m_ref.path);
	HeaderMap headers = getHeaders(m_ref.path);
	if (options->HLSExplorer && options->autoSelectStream)
	{
		if (!m_subservices_checked)
		{
			fillSubservices();
			m_subservices_checked = true;
		}
		size_t subservice_num = m_subservice_vec.size();
		if (subservice_num)
		{
			M3U8StreamInfo subservice = *(m_subservice_vec.begin());
			unsigned int subservice_flag = m_ref.getUnsignedData(7);
			bool bitrate_selection = (!subservice_flag || subservice_flag >= SUBSERVICES_BITRATEKB_START);
			if (bitrate_selection)
			{
				unsigned int bitrate = 0;
				if (subservice_flag)
					bitrate = (subservice_flag - SUBSERVICES_BITRATEKB_START);
				else
					bitrate = options->connectionSpeedInKb;
				// vector is sorted from best to lowest quality in fillSubservices
				std::vector<M3U8StreamInfo>::const_reverse_iterator it(m_subservice_vec.rbegin());
				while(it != m_subservice_vec.rend())
				{
					if (it->bitrate > bitrate * 1000L)
					{
						if (it != m_subservice_vec.rbegin())
							subservice = *(--it);
						else
							subservice = *(it);
						break;
					}
					it++;
				}
				SALOG("start: HLS selected bitrate=%lu", subservice.bitrate);
			}
			else
			{
				unsigned int subservice_idx = subservice_flag - SUBSERVICES_INDEX_START;
				if (subservice_idx < subservice_num)
				{
					subservice = m_subservice_vec[subservice_idx];
				}
				else
				{
					eWarning("eServiceApp::start - subservice_idx(%u) >= subservice_num(%zu), assuming lowest quality",
						subservice_idx, subservice_num);
					subservice = *(m_subservice_vec.end() - 1);
				}
			}
			path_str = subservice.url;
			headers = subservice.headers;
		}
	}
	// don't pass fragment part to player
	std::string cleanUrl = Url(path_str).url();
	if (g_debugLoggingEnabled)
	{
		FILE *_f = fopen("/tmp/serviceapp.log", "a");
		if (_f) {
			fprintf(_f, "[serviceapp] start: url='%s' headers=%zu\n", cleanUrl.c_str(), headers.size());
			for (HeaderMap::const_iterator it(headers.begin()); it != headers.end(); it++)
				fprintf(_f, "[serviceapp] start: header '%s'='%s'\n", it->first.c_str(), it->second.c_str());
			fclose(_f);
		}
	}
	player->start(cleanUrl, headers);
	return 0;
}

RESULT eServiceApp::stop()
{
	SALOG("stop: called");
	/* Prevent stale evEOF/evStopped from reaching enigma2 after we return.
	   The pollTimer can still fire while enigma2 is cleaning up; without this
	   guard a second evStopped/evEOF fires and corrupts enigma2 state. */
	m_has_event_slot = false;
	if (my_subtitle_sync_timer) my_subtitle_sync_timer->stop();
	if (my_event_updated_info_timer) my_event_updated_info_timer->stop();
	if (my_nownext_timer) my_nownext_timer->stop();

	// Resume-Bookmark: letzte Wiedergabeposition als Typ-3-Eintrag speichern,
	// bevor player->stop() die IPC-Verbindung beendet (danach schlagen
	// getPlayPosition()/getLength() fehl). Bit 2 von m_cutlist_enabled ist
	// das "nicht merken"-Flag (analog eDVBServicePlay::stop()).
	if (isLocalFile() && ((m_cutlist_enabled & 2) == 0))
	{
		pts_t play_position, length;
		if (getPlayPosition(play_position) == 0)
		{
			for (std::multiset<cueEntry>::iterator i(m_cue_entries.begin()); i != m_cue_entries.end();)
			{
				if (i->what == 3)
				{
					m_cue_entries.erase(i);
					i = m_cue_entries.begin();
					continue;
				}
				++i;
			}

			if (getLength(length) != 0)
				length = 0;

			if (length > 0)
			{
				// Nach echtem Dateiende laeuft der vom Hardware-Decoder gemeldete
				// PLAYBACK_PTS frei weiter (kein neues Material mehr, das die interne
				// Uhr korrigieren koennte), statt am Dateiende einzufrieren - bei einem
				// Test bis zu einem Vielfachen der tatsaechlichen Laenge beobachtet.
				// Ohne diese Kappung wuerde ein nach Fertigschauen gespeicherter
				// Resume-Punkt weit ueber der Laenge liegen; die GUI (Fortschrittsbalken
				// in Filmliste/Moviewall) wertet so einen Wert offenbar als ungueltig
				// und zeigt stattdessen "gerade erst begonnen" statt "fertig gesehen".
				if (play_position > length)
					play_position = length;
				m_cue_entries.insert(cueEntry(play_position, 3));
				m_cuesheet_changed = true;
			}
		}
		if (m_cuesheet_changed) saveCuesheet();
	}

	player->stop();
	SALOG("stop: player->stop() done");
	return 0;
}

// __iPausableService
RESULT eServiceApp::pause()
{
	eDebug("eServiceApp::pause");
	player->pause();
	return 0;
}

RESULT eServiceApp::unpause()
{
	player->resume();
	return 0;
}

RESULT eServiceApp::setSlowMotion(int ratio)
{
	eDebug("eServiceApp::setSlowMotion - ratio = %d", ratio);
	return -1;
}

RESULT eServiceApp::setFastForward(int ratio)
{
	eDebug("eServiceApp::setFastForward - ratio = %d", ratio);
	return -1;
}


// __iSeekableService
RESULT eServiceApp::getLength(pts_t& pts)
{
	//eDebug("eServiceApp::getLength");
	int length;
	if (player->getLength(length) < 0)
	{
		return -1;
	}
	// (pts_t) Cast noetig: length*90 wuerde sonst in 32-Bit int gerechnet und
	// bei Laenge > ca. 6,6h (length > INT_MAX/90) ueberlaufen, siehe Fund beim
	// iCueSheet-Test mit einer 8,6h-Datei (getLength lieferte dadurch <= 0,
	// Resume-Bookmark wurde faelschlich nie geschrieben).
	pts = (pts_t)length * 90;
	return 0;
}

RESULT eServiceApp::seekTo(pts_t to)
{
	eDebug("eServiceApp::seekTo - position = %lld", to);
	pts_t length;
	if (to < 0)
	{
		to = 0;
	}
	else if (getLength(length) < 0)
	{
		eWarning("eServiceApp::seekTo - cannot get length");
	}
	else if (length > 0 && to > length)
	{
		stop();
		return 0;
	}
	player->seekTo(int(to/90000));

	m_prev_decoder_time = -1;
	m_decoder_time_valid_state = 0;
	if (m_has_selected_subtitle_track)
	{
		my_subtitle_sync_timer->start(1, true);
	}
	return 0;
}

RESULT eServiceApp::seekRelative(int direction, pts_t to)
{
	eDebug("eServiceApp::seekRelative - position = %lld", direction*to);
	pts_t position;
	if (getPlayPosition(position) < 0)
	{
		eWarning("eServiceApp::seekRelative - cannot get play position");
		return -1;
	}
	return seekTo(position + (to * direction));
}

RESULT eServiceApp::getPlayPosition(pts_t& pts)
{
	//eDebug("eServiceApp::getPlayPosition");
	int position;
	if (player->getPlayPosition(position) < 0)
	{
		return -1;
	}
	// Gleicher 32-Bit-Overflow-Fix wie in getLength() - waere sonst nach ca.
	// 6,6h Wiedergabeposition betroffen.
	pts = (pts_t)position * 90;
	return 0;
}

RESULT eServiceApp::setTrickmode(int trick)
{
	eDebug("eServiceApp::setTrickmode = %d", trick);
	return -1;
}

RESULT eServiceApp::isCurrentlySeekable()
{
	eDebug("eServiceApp::isCurrentlySeekable");
	return -1;
}


// __iCueSheet
// Nur lokale Dateien bekommen ein Cue-Sheet (.cuts-Datei neben der Datei,
// gleiches Muster wie die externe .srt-Untertitel-Erkennung). Fuer
// Netzwerk-Streams gibt es dafuer in Enigma2 keine Konvention - dort greift
// stattdessen automatisch der vorhandene, service-unabhaengige Python-seitige
// ResumePoints-Fallback (InfoBarGenerics.py), der nur iSeekableService braucht.
bool eServiceApp::isLocalFile() const
{
	return isLocalFilePath(m_ref.path);
}

RESULT eServiceApp::cueSheet(ePtr<iCueSheet> &ptr)
{
	if (isLocalFile())
	{
		ptr = this;
		return 0;
	}
	ptr = 0;
	return -1;
}

PyObject *eServiceApp::getCutList()
{
	PyObject *list = PyList_New(0);

	for (std::multiset<cueEntry>::const_iterator i(m_cue_entries.begin()); i != m_cue_entries.end(); ++i)
	{
		PyObject *tuple = PyTuple_New(2);
		PyTuple_SET_ITEM(tuple, 0, PyLong_FromLongLong(i->where));
		PyTuple_SET_ITEM(tuple, 1, PyLong_FromLong(i->what));
		PyList_Append(list, tuple);
		Py_DECREF(tuple);
	}

	return list;
}

void eServiceApp::setCutList(SWIG_PYOBJECT(ePyObject) list)
{
	if (!PyList_Check(list))
		return;

	Py_ssize_t size = PyList_Size(list);
	m_cue_entries.clear();

	for (Py_ssize_t i = 0; i < size; ++i)
	{
		PyObject *tuple = PyList_GET_ITEM(list, i);
		if (!PyTuple_Check(tuple) || PyTuple_Size(tuple) != 2)
		{
			eDebug("[eServiceApp] setCutList: skipping malformed cutlist entry");
			continue;
		}
		PyObject *ppts = PyTuple_GET_ITEM(tuple, 0);
		PyObject *ptype = PyTuple_GET_ITEM(tuple, 1);
		if (!(PyLong_Check(ppts) && PyLong_Check(ptype)))
		{
			eDebug("[eServiceApp] setCutList: cutlist entries need to be (pts, type)-tuples");
			continue;
		}
		pts_t pts = PyLong_AsLongLong(ppts);
		int type = PyLong_AsLong(ptype);
		m_cue_entries.insert(cueEntry(pts, type));
	}
	m_cuesheet_changed = true;

	if (m_has_event_slot) m_event_slot(this, evCuesheetChanged);
}

void eServiceApp::setCutListEnable(int enable)
{
	// Reiner Flag-Speicher (u.a. Bit 2 = "letzte Position nicht merken",
	// ausgewertet beim Schreiben des Resume-Bookmarks in stop()). Kein
	// automatisches Ueberspringen von Marker-Bereichen beim Abspielen -
	// siehe Kommentar am m_cue_entries-Member in serviceapp.h.
	m_cutlist_enabled = enable;
}

void eServiceApp::loadCuesheet()
{
	std::string filename = m_ref.path + ".cuts";

	m_cue_entries.clear();

	FILE *f = fopen(filename.c_str(), "rb");
	if (f)
	{
		while (1)
		{
			unsigned long long where;
			unsigned int what;

			if (!fread(&where, sizeof(where), 1, f))
				break;
			if (!fread(&what, sizeof(what), 1, f))
				break;

			where = be64toh(where);
			what = ntohl(what);

			if (what > 3)
				break;

			m_cue_entries.insert(cueEntry(where, what));
		}
		fclose(f);
		SALOG("loadCuesheet: %s has %zu entries", filename.c_str(), m_cue_entries.size());
	}
	else
	{
		SALOG("loadCuesheet: no cuts file at %s", filename.c_str());
	}

	m_cuesheet_changed = false;

	if (m_has_event_slot) m_event_slot(this, evCuesheetChanged);
}

void eServiceApp::saveCuesheet()
{
	// nur speichern, wenn die Hauptdatei noch da/lesbar ist (analog eDVBServicePlay)
	if (::access(m_ref.path.c_str(), R_OK) < 0)
	{
		SALOG("saveCuesheet: main file not readable, skipping: %s", m_ref.path.c_str());
		return;
	}

	std::string filename = m_ref.path + ".cuts";

	FILE *f = fopen(filename.c_str(), "wb");
	if (f)
	{
		for (std::multiset<cueEntry>::iterator i(m_cue_entries.begin()); i != m_cue_entries.end(); ++i)
		{
			unsigned long long where = htobe64(i->where);
			unsigned int what = htonl(i->what);
			fwrite(&where, sizeof(where), 1, f);
			fwrite(&what, sizeof(what), 1, f);
		}
		fclose(f);
		SALOG("saveCuesheet: wrote %s (%zu entries)", filename.c_str(), m_cue_entries.size());
	}
	else
	{
		SALOG("saveCuesheet: could not open for writing: %s", filename.c_str());
	}

	m_cuesheet_changed = false;
}


// __iAudioTrackSelection
int eServiceApp::getNumberOfTracks()
{
	eDebug("eServiceApp::getNumberOfTracks");
	return player->audioGetNumberOfTracks(500);
}

RESULT eServiceApp::selectTrack(unsigned int i)
{
	eDebug("eServiceApp::selectTrack = %d", i);
	if (player->audioSelectTrack(i) < 0)
	{
		return -1;
	}
	return 0;
}

RESULT eServiceApp::getTrackInfo(iAudioTrackInfo& trackInfo, unsigned int n)
{
	eDebug("eServiceApp::getTrackInfo = %d", n);
	audioStream track;
	if (player->audioGetTrackInfo(track, n) < 0)
	{
		return -1;
	}
	trackInfo.m_description = track.description;
	trackInfo.m_language = track.language_code;
	trackInfo.m_pid = track.id;
	return 0;
}

int eServiceApp::getCurrentTrack()
{
	eDebug("eServiceApp::getCurrentTrack");
	return player->audioGetCurrentTrackNum();
}


// __iAudioChannelSelection
int eServiceApp::getCurrentChannel()
{
	eDebug("eServiceApp::getCurrentChannel");
	return STEREO;
}

RESULT eServiceApp::selectChannel(int i)
{
	eDebug("eServiceApp::selectChannel %d", i);
	return -1;
}


// __iSubtitleOutput
RESULT eServiceApp::enableSubtitles(iSubtitleUser *user, struct SubtitleTrack &track)
{
	my_subtitle_sync_timer->stop();
	m_prev_subtitle_message = NULL;
	m_subtitle_pages = NULL;
	m_has_selected_subtitle_track = false;

	m_decoder_time_valid_state = 0;
	m_prev_decoder_time = -1;

	ssize_t track_pos = getTrackPosition(track);
	if (track_pos == -1)
	{
		eWarning("eServiceApp::enableSubtitles - track is not in the map!");
		return -1;
	}
	if (isEmbeddedTrack(track))
	{
		eDebug("eServiceApp::enableSubtitles - track = %d (embedded)", track.pid);
		m_embedded_subtitle_pages.clear();
		m_subtitle_pages = &m_embedded_subtitle_pages;
		player->subtitleSelectTrack(track.pid);
		my_subtitle_sync_timer->start(1, true);
	}
	else if (isExternalTrack(track))
	{
		eDebug("eServiceApp::enableSubtitles - track = %d (external)", track.pid);
		subtitleStream s = m_subtitle_streams[track_pos];
		m_subtitle_pages = m_subtitle_manager.load(s.path);
		if (m_subtitle_pages != NULL)
		{
			my_subtitle_sync_timer->start(1, true);
		}
		else
		{
			eWarning("eServiceApp::enableSubtitles - cannot load external subtitles");
			return -1;
		}
	}
	else
	{
		eWarning("eServiceApp::enableSubtitles - not supported track page_number %d", track.page_number);
		return -1;
	}
	m_selected_subtitle_track = m_subtitle_tracks[track_pos];
	m_has_selected_subtitle_track = true;
	m_subtitle_widget = user;

	if (isEmbeddedTrack(track))
	{
		pullSubtitles();
	}

	return 0;
}

RESULT eServiceApp::disableSubtitles()
{
	eDebug("eServiceApp::disableSubtitles");
	my_subtitle_sync_timer->stop();
	m_prev_subtitle_message = NULL;
	m_embedded_subtitle_pages.clear();
	m_subtitle_pages = NULL;
	m_has_selected_subtitle_track = false;
	if (m_subtitle_widget) m_subtitle_widget->destroy();
	m_subtitle_widget = 0;

	player->subtitleSelectTrack(-1);

	m_decoder_time_valid_state = 0;
	m_prev_decoder_time = -1;
	return 0;
}

RESULT eServiceApp::getCachedSubtitle(struct SubtitleTrack &track)
{
	if (!options->autoTurnOnSubtitles)
	{
		eDebug("eServiceApp::getCachedSubtitle - auto-turning disabled in config");
		return -1;
	}
	std::vector<struct SubtitleTrack> tracks;
	if (getSubtitleList(tracks) < 0 || tracks.empty())
	{
		eDebug("eServiceApp::getCachedSubtitle - no subtitles available");
		return -1;
	}
	// TODO consider language setting
	int ret = -1;
	std::vector<struct SubtitleTrack> embedded_tracks;
	std::vector<struct SubtitleTrack> external_tracks;
	std::remove_copy_if(tracks.begin(), tracks.end(), std::back_inserter(embedded_tracks), isExternalTrack);
	std::remove_copy_if(tracks.begin(), tracks.end(), std::back_inserter(external_tracks), isEmbeddedTrack);

	bool select_embedded = (options->preferEmbeddedSubtitles || external_tracks.empty()) && !embedded_tracks.empty();
	if (!select_embedded)
	{
		struct SubtitleTrack tmp_track = *external_tracks.begin();
		subtitleStream tmp_stream = m_subtitle_streams[getTrackPosition(tmp_track)];
		std::string video_base, subtitle_base, extension;
		splitExtension(m_ref.path, video_base, extension);
		splitExtension(tmp_stream.path, subtitle_base, extension);
		if (video_base == subtitle_base)
		{
			track = tmp_track;
			ret = 0;
		}
		else
		{
			select_embedded = !embedded_tracks.empty();
		}
	}
	if (select_embedded)
	{
		track = *embedded_tracks.begin();
		ret = 0;
	}

	if (ret == 0)
	{
		if (options->preferEmbeddedSubtitles && isEmbeddedTrack(track))
			eDebug("eServiceApp::getCachedSubtitle - selected preferred embedded track");
		else if (options->preferEmbeddedSubtitles && !isEmbeddedTrack(track))
			eDebug("eServiceApp::getCachedSubtitle - selected embedded track");
		else if (!options->preferEmbeddedSubtitles && isExternalTrack(track))
			eDebug("eServiceApp::getCachedSubtitle - selected preferred external track");
		else if (!options->preferEmbeddedSubtitles && !isExternalTrack(track))
			eDebug("eServiceApp::getCachedSubtitle - selected external track");
	}
	else
	{
		eDebug("eServiceApp::getCachedSubtitle - no track selected, more than one external track found, name doesn't correspond to video file");
	}
	return ret;
}

RESULT eServiceApp::getSubtitleList(std::vector<struct SubtitleTrack> &subtitlelist)
{
	m_subtitle_tracks.clear();
	m_subtitle_streams.clear();
	int embedded_track_num = player->subtitleGetNumberOfTracks(500);
	eDebug("eServiceApp::getSubtitleList - found embedded tracks (%d)", embedded_track_num);
	int pid = 0;
	for (; pid < embedded_track_num; pid++)
	{
		subtitleStream s;
		if (player->subtitleGetTrackInfo(s, pid) == 0)
		{
			addEmbeddedTrack(subtitlelist, s, pid);
		}
	}
	std::string basename, extension;
	splitExtension(m_ref.path, basename, extension);
	std::string subtitle_path(basename + ".srt");

	std::string dirname, filename;
	splitPath(subtitle_path, dirname, filename);
	// TODO 
	//
	// - try to find out language code from filename if possible
	// - apply some sort of sorting which would add more relevant subtitles to beginning
	// of the list
	//
	// - probably whole thing should be moved to manager
	if (!access(subtitle_path.c_str(), F_OK))
	{
		addExternalTrack(subtitlelist, pid++, filename, subtitle_path);
	}
	std::vector<std::string> directories, files;
	if (listDir(dirname, &files, &directories) == 0)
	{
		std::vector<std::string>::const_iterator it;
		if ((std::find(directories.begin(), directories.end(), "Subs")) != directories.end())
		{
			std::vector<std::string> subsdir_files;
			if (listDir(dirname + "/Subs", &subsdir_files, NULL) == 0)
			{
				for (it = subsdir_files.begin(); it != subsdir_files.end(); it++)
				{
					splitExtension(*it, basename, extension);
					if (extension == ".srt")
					{
						addExternalTrack(subtitlelist, pid++, basename, dirname + "/Subs/" + *it);
					}
				}
			}
		}
		for (it = files.begin(); it != files.end(); it++)
		{
			splitExtension(*it, basename, extension);
			std::string path = dirname + "/" + *it;
			if (extension == ".srt" && subtitle_path != path)
			{
				addExternalTrack(subtitlelist, pid++, basename, path);
			}
		}
	}
	eDebug("eServiceApp::getSubtitleList - found external tracks (%d)", pid - embedded_track_num);
	return 0;
}

// __iSubservices
int eServiceApp::getNumberOfSubservices()
{
	if (options->HLSExplorer && !m_subservices_checked)
	{
		fillSubservices();
		m_subservices_checked = true;
	}
	eDebug("eServiceApp::getNumberOfSubservices - %zu", m_subserviceref_vec.size());
	return m_subserviceref_vec.size();
}

RESULT eServiceApp::getSubservice(eServiceReference &subservice, unsigned int n)
{
	eDebug("eServiceApp::getSubservice - %d", n);
	subservice = m_subserviceref_vec[n];
	return 0;
}

// __iServiceInformation
RESULT eServiceApp::getName(std::string& name)
{
	std::string title = m_ref.getName();
	if (title.empty())
	{
		name = m_ref.path;
		size_t n = name.rfind('/');
		if (n != std::string::npos)
			name = name.substr(n + 1);
	}
	else
		name = title;
	return 0;
}

#ifdef HAVE_EPG
RESULT eServiceApp::getEvent(ePtr<eServiceEvent> &evt, int nownext)
{
	if (eEPGCache::getInstance())
	{
		eServiceReference ref;
		ref.flags = m_ref.flags;
		for (int i = 0; i < 8; ++i)
		{
			ref.data[i] = m_ref.getData(i);
		}
		ref.type = m_ref.getData(0);
		if (ref.type <= 0)
		{
			ref.type = eServiceFactoryApp::idServiceMP3;
		}

		// Directly lookup and write into Enigma2's output ePtr 'evt'.
		// No local ePtr variables are used, avoiding incompatible destructor/release calls in serviceapp.
		if (eEPGCache::getInstance()->lookupEventTime(ref, -1, evt, nownext) >= 0)
		{
			return 0;
		}
	}
	return -1;
}
#endif

int eServiceApp::getInfo(int w)
{
	switch (w)
	{
	case sServiceref: return resIsString;
	case sVideoHeight: return m_height;
	case sVideoWidth: return m_width;
	case sFrameRate: return m_framerate;
	case sProgressive: return m_progressive;
	case sAspect: 
	{
		if (m_height <= 0 || m_width <= 0)
		{
			return -1;
		}
		float aspect = m_width/float(m_height);
		// according to wikipedia, widescreen is when width to height is greater then 1.37:1
		if (aspect > 1.37)
		{
			// WIDESCREEN values from ServiceInfo.py: 3, 4, 7, 8, 0xB, 0xC, 0xF, 0x10
			return 3;
		}
		else
		{
			// 4:3 values from ServiceInfo.py: 1, 2, 5, 6, 9, 0xA, 0xD, 0xE
			return 1;
		}
		return 2;
	}
	case sTagTitle:
	case sTagArtist:
	case sTagAlbum:
	case sTagTitleSortname:
	case sTagArtistSortname:
	case sTagAlbumSortname:
	case sTagDate:
	case sTagComposer:
	case sTagGenre:
	case sTagComment:
	case sTagExtendedComment:
	case sTagLocation:
	case sTagHomepage:
	case sTagDescription:
	case sTagVersion:
	case sTagISRC:
	case sTagOrganization:
	case sTagCopyright:
	case sTagCopyrightURI:
	case sTagContact:
	case sTagLicense:
	case sTagLicenseURI:
	case sTagCodec:
	case sTagAudioCodec:
	case sTagVideoCodec:
	case sTagEncoder:
	case sTagLanguageCode:
	case sTagKeywords:
	case sTagChannelMode:
	case sUser+12:
	case sUser+13:
		return resIsString;
	case sTagTrackGain:
	case sTagTrackPeak:
	case sTagAlbumGain:
	case sTagAlbumPeak:
	case sTagReferenceLevel:
	case sTagBeatsPerMinute:
	case sTagImage:
	case sTagPreviewImage:
	case sTagAttachment:
		return resIsPyObject;
	case sTagTrackNumber:
	case sTagTrackCount:
	case sTagAlbumVolumeNumber:
	case sTagAlbumVolumeCount:
	case sTagBitrate:
	case sTagNominalBitrate:
	case sTagMinimumBitrate:
	case sTagMaximumBitrate:
	case sTagSerial:
	case sTagEncoderVersion:
	case sTagCRC:
	case sBuffer:
	default:
		return resNA;
	}
	return 0;
}

std::string eServiceApp::getInfoString(int w)
{
	if ( Url(m_ref.path).url().find("://") != std::string::npos )
	{
		switch (w)
		{
		case sProvider:
			return "IPTV";
		case sServiceref:
		{
			eServiceReference ref;
			ref.type = m_ref.type;
			ref.flags = m_ref.flags;
			for (int i = 0; i < 8; ++i)
			{
				ref.data[i] = m_ref.getData(i);
			}
			return ref.toString();
		}
		default:
			break;
		}
	}
	if (w < sUser && w > 26 )
		return "";
	switch(w)
	{
	case sUser+12:
	{
		errorMessage e;
		if (!player->getErrorMessage(e))
			return e.message;
		return "";
	}
	case sUser+13:
		// Backend-Name (exteplayer3/gstplayer). Dieser Key existiert nur
		// bei serviceapp - fragt ein Drittanbieter-Plugin das ueber den
		// nativen eServiceMP3 (Wiedergabemodul=Original) ab, bekommt es
		// stattdessen resNA zurueck (kein resIsString), woran sich
		// erkennen laesst, dass serviceapp gar nicht aktiv ist.
		return extplayer ? extplayer->getBackendName() : "";
	default:
		return "";
	}
	return "";
}




DEFINE_REF(eStaticServiceAppInfo);

RESULT eStaticServiceAppInfo::getName(const eServiceReference &ref, std::string &name)
{
	if ( ref.name.length() )
		name = ref.name;
	else
	{
		size_t last = ref.path.rfind('/');
		if (last != std::string::npos)
			name = ref.path.substr(last+1);
		else
			name = ref.path;
	}
	return 0;
}

// Liefert die reale Laufzeit einer Datei OHNE sie abzuspielen (z.B. fuer
// Movie-Wall-Ansichten von Drittanbieter-Plugins wie Advanced Event Library,
// die dafuer eigenstaendig iStaticServiceInformation::getLength() abfragen -
// unabhaengig vom .cuts-Resume-Mechanismus, der nur waehrend/nach aktiver
// Wiedergabe befuellt wird). Netzwerk-Streams bleiben unterstuetzt-NA (-1),
// gleiches Muster wie beim iCueSheet-Feature. Caching passiert komplett in
// ffprobe_get_duration_seconds() selbst (siehe ffprobe/ffprobe_length.cpp) -
// bewusst NICHT als Member dieser Klasse, siehe Kommentar in serviceapp.h.
int eStaticServiceAppInfo::getLength(const eServiceReference &ref)
{
	if (!isLocalFilePath(ref.path))
		return -1;

	return (int)ffprobe_get_duration_seconds(ref.path);
}

int eStaticServiceAppInfo::getInfo(const eServiceReference &ref, int w)
{
	switch (w)
	{
	case iServiceInformation::sTimeCreate:
		{
			struct stat64 s;
			if (stat64(ref.path.c_str(), &s) == 0)
			{
				return s.st_mtime;
			}
		}
		break;
	case iServiceInformation::sFileSize:
		{
			struct stat64 s;
			if (stat64(ref.path.c_str(), &s) == 0)
			{
				return s.st_size;
			}
		}
		break;
	}
	return iServiceInformation::resNA;
}

long long eStaticServiceAppInfo::getFileSize(const eServiceReference &ref)
{
	struct stat64 s;
	if (stat64(ref.path.c_str(), &s) == 0)
	{
		return s.st_size;
	}
	return 0;
}

RESULT eStaticServiceAppInfo::getEvent(const eServiceReference &ref, ePtr<eServiceEvent> &evt, time_t start_time)
{
#ifdef HAVE_EPG
	SALOG("eStaticServiceAppInfo::getEvent - ref=%s data0=%d start_time=%ld", ref.toString().c_str(), ref.getData(0), (long)start_time);
	if (Url(ref.path).url().find("://") != std::string::npos)
	{
		eServiceReference equivalentref;
		equivalentref.type = ref.type;
		equivalentref.flags = ref.flags;
		for (int i = 0; i < 8; ++i)
		{
			equivalentref.data[i] = ref.getData(i);
		}

		if (eEPGCache::getInstance())
		{
			equivalentref.type = ref.getData(0);
			int ret1 = (equivalentref.type > 0) ? eEPGCache::getInstance()->lookupEventTime(equivalentref, start_time, evt) : -2;
			SALOG("eStaticServiceAppInfo::getEvent - getData(0) branch: type=%d ret=%d", equivalentref.type, ret1);
			if (equivalentref.type > 0 && ret1 >= 0)
			{
				SALOG("eStaticServiceAppInfo::getEvent - FOUND via getData(0), evt_valid=%d", (bool)evt);
				return 0;
			}

			equivalentref.type = eServiceFactoryApp::idServiceMP3;
			int ret2 = eEPGCache::getInstance()->lookupEventTime(equivalentref, start_time, evt);
			SALOG("eStaticServiceAppInfo::getEvent - idServiceMP3 fallback branch: ret=%d", ret2);
			if (ret2 >= 0)
			{
				SALOG("eStaticServiceAppInfo::getEvent - FOUND via idServiceMP3 fallback, evt_valid=%d", (bool)evt);
				return 0;
			}
		}
	}
	SALOG("eStaticServiceAppInfo::getEvent - NOT FOUND, returning -1");
	evt = 0;
#endif
	return -1;
}


DEFINE_REF(eServiceFactoryApp)

eServiceFactoryApp::eServiceFactoryApp()
{
	ePtr<eServiceCenter> sc;

	eServiceCenter::getPrivInstance(sc);
	if (sc)
	{
		std::list<std::string> extensions;
		extensions.push_back("dts");
		extensions.push_back("mp2");
		extensions.push_back("mp3");
		extensions.push_back("ogg");
		extensions.push_back("ogm");
		extensions.push_back("ogv");
		extensions.push_back("mpg");
		extensions.push_back("vob");
		extensions.push_back("wav");
		extensions.push_back("wave");
		extensions.push_back("m4v");
		extensions.push_back("mkv");
		extensions.push_back("avi");
		extensions.push_back("divx");
		extensions.push_back("dat");
		extensions.push_back("flac");
		extensions.push_back("flv");
		extensions.push_back("mp4");
		extensions.push_back("mov");
		extensions.push_back("m4a");
		extensions.push_back("3gp");
		extensions.push_back("3g2");
		extensions.push_back("asf");
		extensions.push_back("wmv");
		extensions.push_back("wma");
		extensions.push_back("stream");
		if (gReplaceServiceMP3)
		{
			sc->removeServiceFactory(eServiceFactoryApp::idServiceMP3);
			sc->addServiceFactory(eServiceFactoryApp::idServiceMP3, this, extensions);
		}
		extensions.clear();
		sc->addServiceFactory(eServiceFactoryApp::idServiceGstPlayer, this, extensions);
		sc->addServiceFactory(eServiceFactoryApp::idServiceExtEplayer3, this, extensions);
		
	}
	m_service_info = new eStaticServiceAppInfo();
}

eServiceFactoryApp::~eServiceFactoryApp()
{
	ePtr<eServiceCenter> sc;

	eServiceCenter::getPrivInstance(sc);
	if (sc)
	{
		if (gReplaceServiceMP3)
		{
			sc->removeServiceFactory(eServiceFactoryApp::idServiceMP3);
		}
		sc->removeServiceFactory(eServiceFactoryApp::idServiceGstPlayer);
		sc->removeServiceFactory(eServiceFactoryApp::idServiceExtEplayer3);
	}
	
}


eAutoInitPtr<eServiceFactoryApp> init_eServiceFactoryApp(eAutoInitNumbers::service+1, "eServiceFactoryApp");


static PyObject *
use_user_settings(PyObject *self, PyObject *args)
{
	g_useUserSettings = true;
	Py_RETURN_NONE;
}

static PyObject *
servicemp3_exteplayer3_enable(PyObject *self, PyObject *args)
{
	g_playerServiceMP3 = EXTEPLAYER3;
	Py_RETURN_NONE;
}

static PyObject *
servicemp3_gstplayer_enable(PyObject *self, PyObject *args)
{
	g_playerServiceMP3 = GSTPLAYER;
	Py_RETURN_NONE;
}


static PyObject *
gstplayer_set_setting(PyObject *self, PyObject *args)
{
	bool ret = true;

	int settingId;
	char *audioSink, *videoSink;
	bool subtitlesEnable; 
	long bufferSize, bufferDuration;

	if (!PyArg_ParseTuple(args, "issbll", &settingId, &videoSink, &audioSink, &subtitlesEnable, &bufferSize, &bufferDuration))
		return NULL;
	
	GstPlayerOptions *options = NULL;
	switch (settingId)
	{
		case OPTIONS_SERVICEGSTPLAYER:
			options = &g_GstPlayerOptionsServiceGst;
			eDebug("[gstplayer_set_setting] setting servicegstplayer options");
			break;
		case OPTIONS_SERVICEMP3:
			options = &g_GstPlayerOptionsServiceMP3;
			eDebug("[gstplayer_set_setting] setting servicemp3 options");
			break;
		case OPTIONS_USER:
			options = &g_GstPlayerOptionsUser;
			eDebug("[gstplayer_set_setting] setting user options");
			break;
		default:
			eWarning("[gstplayer_set_setting] option '%d' is not known, cannot be set!", settingId);
			ret = false;
			break;
	}
	if (options != NULL)
	{
		options->videoSink = videoSink;
		options->audioSink = audioSink;
		options->subtitleEnabled = subtitlesEnable;
		options->bufferSize = bufferSize;
		options->bufferDuration = bufferDuration;
	}
	return Py_BuildValue("b", ret);
}

static PyObject *
exteplayer3_set_setting(PyObject *self, PyObject *args)
{
	bool ret = true;

	int settingId;
	bool aacSwDecoding;
	bool dtsSwDecoding;
	bool wmaSwDecoding;
	bool downmix;
	bool lpcmInjection;
	int hlsQualityMode = 0;
	bool hlsAudioDefaultOnly = false;
	bool debugLoggingEnabled = false;
	bool pcmAudioExportEnabled = false;

	if (!PyArg_ParseTuple(args, "ibbbbb|ibbb", &settingId, &aacSwDecoding, &dtsSwDecoding, &wmaSwDecoding, &lpcmInjection, &downmix, &hlsQualityMode, &hlsAudioDefaultOnly, &debugLoggingEnabled, &pcmAudioExportEnabled))
		return NULL;

	ExtEplayer3Options *options = NULL;
	switch (settingId)
	{
		case OPTIONS_SERVICEEXTEPLAYER3:
			options = &g_ExtEplayer3OptionsServiceExt3;
			eDebug("[exteplayer3_set_setting] setting serviceextplayer3 options");
			break;
		case OPTIONS_SERVICEMP3:
			options = &g_ExtEplayer3OptionsServiceMP3;
			eDebug("[exteplayer3_set_setting] setting servicemp3 options");
			break;
		case OPTIONS_USER:
			options = &g_ExtEplayer3OptionsUser;
			eDebug("[exteplayer3_set_setting] setting user options");
			break;
		default:
			eWarning("[exteplayer3_set_setting] option '%d' is not known, cannot be set!", settingId);
			ret = false;
			break;
	}
	eDebug("[exteplayer3_set_setting] options ptr = %p", options);
	if (options != NULL)
	{
		options->aacSwDecoding = aacSwDecoding;
		options->dtsSwDecoding = dtsSwDecoding;
		options->wmaSwDecoding = wmaSwDecoding;
		options->lpcmInjection = lpcmInjection;
		options->downmix = downmix;
		options->hlsQualityMode = hlsQualityMode;
		options->hlsAudioDefaultOnly = hlsAudioDefaultOnly;
		options->debugLoggingEnabled = debugLoggingEnabled;
		options->pcmAudioExportEnabled = pcmAudioExportEnabled;
	}
	return Py_BuildValue("b", ret);
}

static PyObject *
serviceapp_set_setting(PyObject *self, PyObject *args)
{
	bool ret = true;

	bool autoTurnOnSubtitles;
	int settingId;
	bool HLSExplorer;
	bool autoSelectStream;
	int32_t connectionSpeedInKb;
	bool HLSAudioFilter;
	bool debugLoggingEnabled = false;

	if (!PyArg_ParseTuple(args, "ibbIbb|b", &settingId, &HLSExplorer, &autoSelectStream, &connectionSpeedInKb, &autoTurnOnSubtitles, &HLSAudioFilter, &debugLoggingEnabled))
		return NULL;

	/* Process-wide, not per-options-struct: applies regardless of which settingId
	   was pushed, takes effect immediately (no Enigma2 restart) since eDebug()/eLog()
	   and SALOG()/M3ULOG() check these globals fresh on every call. */
	g_debugLoggingEnabled = debugLoggingEnabled;
	debugLvl = debugLoggingEnabled ? 4 /* lvlDebug */ : 2 /* lvlWarning */;
	
	eServiceAppOptions *options = NULL;
	switch (settingId)
	{
		case OPTIONS_SERVICEEXTEPLAYER3:
			options = &g_ServiceAppOptionsServiceExt3;
			eDebug("[serviceapp_set_setting] setting serviceexteplayer3 options");
			break;
		case OPTIONS_SERVICEGSTPLAYER:
			options = &g_ServiceAppOptionsServiceGst;
			eDebug("[serviceapp_set_setting] setting servicegstplayer options");
			break;
		case OPTIONS_SERVICEMP3:
			options = &g_ServiceAppOptionsServiceMP3;
			eDebug("[serviceapp_set_setting] setting servicemp3 options");
			break;
		case OPTIONS_USER:
			options = &g_ServiceAppOptionsUser;
			eDebug("[serviceapp_set_setting] setting user options");
			break;
		default:
			eWarning("[serviceapp_set_setting] option '%d' is not known, cannot be set!", settingId);
			ret = false;
			break;
	}
	eDebug("[serviceapp_set_setting] options ptr = %p", options);
	if (options != NULL)
	{
		options->autoTurnOnSubtitles = autoTurnOnSubtitles;
		options->HLSExplorer = HLSExplorer;
		options->autoSelectStream = autoSelectStream;
		options->connectionSpeedInKb = connectionSpeedInKb;
		options->HLSAudioFilter = HLSAudioFilter;
	}
	return Py_BuildValue("b", ret);
}



static PyMethodDef serviceappMethods[] = {
	{"use_user_settings", use_user_settings, METH_NOARGS,
	 "user settings will be used for creation of player"},
	{"servicemp3_exteplayer3_enable", servicemp3_exteplayer3_enable, METH_NOARGS,
	 "use ffmpeg based extplayer3, when servicemp3 is replaced by serviceapp"},
	{"servicemp3_gstplayer_enable", servicemp3_gstplayer_enable, METH_NOARGS,
	 "use gstreamer based player, when servicemp3 is replaced by serviceapp"},
	{"gstplayer_set_setting", gstplayer_set_setting, METH_VARARGS,
	 "set gstreamer player settings (setting_id, videoSink, audioSink, subtitlesEnabled, bufferSize, bufferDuration\n\n"
	 " setting_id - (0 - servicemp3, 1 - servicegst, 2 - serviceextep3, 3 - user)\n"
	 " videoSink - (dvbvideosink, dvbvideosinkexp, ...)\n"
	 " audioSink - (dvbaudiosink, dvbaudiosinkexp, ...)\n"
	 " subtitleEnable - (True, False)\n"
	 " bufferSize - in kilobytes\n"
	 " bufferDuration - in seconds\n"
	},
	{"exteplayer3_set_setting", exteplayer3_set_setting, METH_VARARGS,
	 "set exteplayer3 settings (setting_id, aacSwDecoding, dtsSwDecoding, wmaSwDecoding, lpcmInjection, downmix, hlsQualityMode, hlsAudioDefaultOnly\n\n"
	 " setting_id - (0 - servicemp3, 1 - servicegst, 2 - serviceextep3, 3 - user)\n"
	 " aacSwDecoding - (True, False)\n"
	 " dtsSwDecoding - (True, False)\n"
	 " wmaSwDecoding - (True, False)\n"
	 " lpcmInjection - (True, False)\n"
	 " downmix - (True, False)\n"
	 " hlsQualityMode - optional, (0 - auto, 1 - lowest, 2 - highest), default 0\n"
	 " hlsAudioDefaultOnly - optional, (True, False), default False\n"
	 " debugLoggingEnabled - optional, (True, False), default False\n"
	 " pcmAudioExportEnabled - optional, (True, False), default False - writes decoded PCM audio to /tmp/exteplayer3_pcm_audio.fifo for third-party plugins, forces software decoding (CPU cost)\n"
	},
	{"serviceapp_set_setting", serviceapp_set_setting, METH_VARARGS,
	 "set serviceapp settings (setting_id, HLSExplorer, autoSelectStream, connectionSpeedInKb, autoTurnOnSubtitles\n\n"
	 " setting_id - (0 - servicemp3, 1 - servicegst, 2 - serviceextep3, 3 - user)\n"
	 " HLSExplorer - defines if HLS explorer will be used to retrieve streams from HLS master playlist (True, False))\n"
	 " autoSelectStream - if there are more streams available, it defines if stream will be auto-selected according to connectionSpeedInKb (True, False)\n"
	 " connectionSpeedInKb - defines bitrate in kilobits/s according to which will be selected stream from playlist <0, max(int32_t)>\n"
	 " autoTurnOnSubtitles - auto turn on subtitles if available (True, False)\n"
	 " debugLoggingEnabled - optional, (True, False), default False (process-wide, not per setting_id)\n"
	},
	 {NULL,NULL,0,NULL}
};

#ifdef __mips__
static void pre_fault_bss(void) {
	volatile char *p;
	p = (volatile char *)&g_ExtEplayer3OptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_ExtEplayer3OptionsServiceExt3; *p = *p;
	p = (volatile char *)&g_ExtEplayer3OptionsUser; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceExt3; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsServiceGst; *p = *p;
	p = (volatile char *)&g_ServiceAppOptionsUser; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsServiceMP3; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsServiceGst; *p = *p;
	p = (volatile char *)&g_GstPlayerOptionsUser; *p = *p;
	/* g_debugLoggingEnabled/debugLvl are new process-wide globals first written by
	   serviceapp_set_setting() at runtime - same lazy-BSS-fault-under-reentrant-
	   signal-handler risk as the options structs above (see mips/MIPS_HEISENBUG.md).
	   Must be pre-faulted here too, before Python can ever call the setter. */
	p = (volatile char *)&g_debugLoggingEnabled; *p = *p;
	p = (volatile char *)&debugLvl; *p = *p;
}
#endif

PyMODINIT_FUNC
initserviceapp(void)
{
#ifdef __mips__
	pre_fault_bss();
#endif
	PyObject *m = Py_InitModule("serviceapp", serviceappMethods);
	if (m)
	{
		PyModule_AddStringConstant(m, "__version__", "vti005-hls1");
	}

	SSL_load_error_strings();
	SSL_library_init();
}
