#ifndef __serviceapp__ffprobe_length_
#define __serviceapp__ffprobe_length_

#include <string>
#include <stdint.h>

// Ermittelt die reale Laufzeit einer lokalen Mediendatei in Sekunden, ohne sie
// abzuspielen (kurze Metadaten-Probe per FFmpeg). Laedt libavformat/libavutil
// dynamisch per dlopen() zur Laufzeit - serviceapp.so selbst hat KEINE feste
// Link-Abhaengigkeit auf FFmpeg (siehe ffprobe_length.cpp fuer die Begruendung).
// Rueckgabe -1 bei jedem Fehler (Datei nicht lesbar/kein Mediencontainer,
// FFmpeg-Libs nicht verfuegbar, Timeout, o.ae.) - niemals ein Absturz.
int64_t ffprobe_get_duration_seconds(const std::string &path);

#endif
