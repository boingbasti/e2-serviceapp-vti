#ifndef __serviceapprecord_h
#define __serviceapprecord_h

#include <lib/service/iservice.h>
#include <lib/base/ebase.h>
#include <ctime>
#include <string>

#include "myconsole.h"

// Aufnahme-Unterstuetzung fuer 5001/5002 (idServiceGstPlayer/idServiceExtEplayer3).
// eServiceFactoryApp::record() war bisher ein harter Stub (siehe serviceapp.h),
// da exteplayer3 nie einen rohen Transportstrom liefert, den eine native
// DVB-Aufnahme-Pipeline einfach mitschneiden koennte. Diese Klasse loest das
// komplett unabhaengig vom laufenden Wiedergabepfad: ein eigener, separater
// ffmpeg-Hintergrundprozess (-c copy -f mpegts) schreibt den Stream direkt in
// eine Datei, ueber dieselbe eConsoleContainer-Klasse, die auch PlayerApp
// (siehe extplayer.cpp) fuer exteplayer3 selbst nutzt. Funktioniert dadurch
// auch als reine Timer-/EPG-Hintergrundaufnahme ganz ohne aktive Wiedergabe.
//
// .meta/.eit werden von Enigma2s Python-Seite (RecordTimer.py) NICHT
// automatisch geschrieben - RecordTimer.py reicht name/descr/eit_event_id nur
// als Parameter an prepare() durch, das Schreiben selbst obliegt der
// iRecordableService-Implementierung (siehe eDVBServiceRecord/eDVBMetaParser
// als Referenz). Diese Klasse repliziert exakt deren .meta-Zeilenformat und
// nutzt fuer die .eit-Datei dieselbe eEPGCache::saveEventToFile()-Methode.
class eServiceAppRecord: public iRecordableService
{
	DECLARE_REF(eServiceAppRecord);

	eServiceReference m_ref;
	ePtr<eConsoleContainer> m_console;

	std::string m_filename;
	std::string m_name, m_descr, m_tags;
	time_t m_begTime, m_endTime, m_time_create;
	int m_eit_event_id;
	int m_error;

	SigC::Slot2<void,iRecordableService*,int> m_event_slot;
	bool m_has_event_slot;
	bool m_userRequestedStop;

	void appClosed(int retval);
	void writeMetaFile(long long length, long long filesize);

public:
	eServiceAppRecord(const eServiceReference &ref);
	virtual ~eServiceAppRecord();

	// eNavigation::recordService() ruft record() fuer dieselbe laufende
	// Aufnahme mehrfach auf (u.a. fuer Sanity-Check-Probeaufrufe VOR dem
	// eigentlichen Start), erwartet dabei aber ein stabiles, wiederverwend-
	// bares Objekt pro eServiceReference - so wie es eine native DVB-Auf-
	// nahme (ein Tuner/Recorder pro Kanal) architektonisch von selbst waere.
	// Ohne diese Wiederverwendung erzeugte jeder record()-Aufruf ein eigenes,
	// nie bei eNavigation registriertes Objekt; wurde spaeter versucht, EIN
	// SOLCHES nie registriertes Objekt zu stoppen, fand eNavigation es in
	// keiner seiner internen Aufnahme-Listen und stuerzte mit "try to stop
	// non running recording!!" ab (reproduzierbar in Crashlogs vom
	// 2026-09-04). getOrCreate() haelt daher pro eServiceReference genau ein
	// Objekt vor; der Destruktor meldet sich beim Beenden wieder ab.
	static eServiceAppRecord *getOrCreate(const eServiceReference &ref);

	RESULT connectEvent(const SigC::Slot2<void,iRecordableService*,int> &event, ePtr<eConnection> &connection);
	RESULT getError(int &error) { error = m_error; return 0; }
	RESULT prepare(const char *filename, time_t begTime=-1, time_t endTime=-1, int eit_event_id=-1, const char *name=0, const char *descr=0, const char *tags=0, bool descramble=true, bool recordecm=false, int packetsize=188);
	// prepareStreaming() (Weiterleiten an ein zweites Geraet) ist ein
	// eigenstaendiger Anwendungsfall, bewusst nicht Teil dieses Features.
	RESULT prepareStreaming(bool descramble=true, bool includeecm=false) { return -1; }
	RESULT start(bool simulate=false);
	RESULT stop();
	RESULT frontendInfo(ePtr<iFrontendInformation> &ptr) { ptr = 0; return -1; }
	RESULT stream(ePtr<iStreamableService> &ptr) { ptr = 0; return -1; }
	RESULT subServices(ePtr<iSubserviceList> &ptr) { ptr = 0; return -1; }
	RESULT getFilenameExtension(std::string &ext);
};

#endif
