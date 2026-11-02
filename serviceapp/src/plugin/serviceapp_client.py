import os

from enigma import eEnv

import serviceapp


ID_SERVICEMP3 = 4097
ID_SERVICEGSTPLAYER = 5001
ID_SERVICEEXTEPLAYER3 = 5002

OPTIONS_SERVICEMP3_GSTPLAYER = 0
OPTIONS_SERVICEMP3_EXTEPLAYER3 = 0
OPTIONS_SERVICEMP3 = 0
OPTIONS_SERVICEGSTPLAYER = 1
OPTIONS_SERVICEEXTEPLAYER3 = 2
OPTIONS_USER = 3

_SERVICEMP3_REPLACE_PATH = eEnv.resolve("$sysconfdir/enigma2/serviceapp_replaceservicemp3")


def isExtEplayer3Available():
	return os.path.isfile(eEnv.resolve("$bindir/exteplayer3"))


def isGstPlayerAvailable():
	return os.path.isfile(eEnv.resolve("$bindir/gstplayer_gst-1.0"))


def isServiceMP3Replaced():
	return os.path.isfile(_SERVICEMP3_REPLACE_PATH)


def setServiceMP3Replace(replace):
	if replace:
		open(_SERVICEMP3_REPLACE_PATH, "wb").close()
	else:
		if os.path.isfile(_SERVICEMP3_REPLACE_PATH):
			os.remove(_SERVICEMP3_REPLACE_PATH)


def setServiceMP3GstPlayer():
	serviceapp.servicemp3_gstplayer_enable()


def setServiceMP3ExtEplayer3():
	serviceapp.servicemp3_exteplayer3_enable()


def setUseUserSettings():
	serviceapp.use_user_settings()


def setServiceAppSettings(settingId, HLSExplorer, autoSelectStream, connectionSpeedInKb, autoTurnOnSubtitles=2, HLSAudioFilter=True, debugLoggingEnabled=None, probeMovieLengthEnabled=None):
	# debugLoggingEnabled=None statt False: faellt ein Aufrufer (z.B. ein fremdes
	# Plugin) diesen Parameter weg, soll der C-seitige Sentinel greifen und das
	# bestehende Logging-Setting NICHT ungewollt abgeschaltet werden.
	#
	# autoTurnOnSubtitles ist seit dem Forced-Untertitel-Feature kein reines
	# bool mehr, sondern 0=Off/1=Forced only/2=All (SUBTITLE_AUTO_* in
	# serviceapp.h). Default bewusst 2 statt 0, um das alte Verhalten von
	# "autoTurnOnSubtitles=True" fuer Aufrufer ohne diesen Parameter nicht zu
	# aendern. int(...) macht alte True/False-Aufrufe explizit zu 1/0.
	autoTurnOnSubtitles = int(autoTurnOnSubtitles)
	# probeMovieLengthEnabled kann in der C-Varargs-Reihenfolge nur mitgegeben
	# werden, wenn debugLoggingEnabled auch mitgegeben wird - fehlt es, wird es
	# aus dem aktuellen Setting nachgezogen, damit es nicht ungewollt auf False
	# faellt (gleiches Muster wie pcmAudioExportEnabled in setExtEplayer3Settings).
	extra = []
	if debugLoggingEnabled is not None:
		extra.append(debugLoggingEnabled)
		if probeMovieLengthEnabled is not None:
			extra.append(probeMovieLengthEnabled)
	elif probeMovieLengthEnabled is not None:
		from Components.config import config
		try:
			dl = config.plugins.serviceapp.debug_logging.value
		except Exception:
			dl = False
		extra.extend([dl, probeMovieLengthEnabled])
	return serviceapp.serviceapp_set_setting(settingId, HLSExplorer, autoSelectStream, connectionSpeedInKb, autoTurnOnSubtitles, HLSAudioFilter, *extra)

def setGstreamerPlayerSettings(settingId, videoSink, audioSink, subtitleEnabled, bufferSize, bufferDuration):
	return serviceapp.gstplayer_set_setting(settingId, videoSink, audioSink, subtitleEnabled, bufferSize, bufferDuration)

def setExtEplayer3Settings(settingId, aacSwDecoding, dtsSwDecoding, wmaSwDecoding, lpcmInjection, downmix, hlsQualityMode=0, hlsAudioDefaultOnly=False, debugLoggingEnabled=None, pcmAudioExportEnabled=None):
	# Gleicher Sentinel-Gedanke wie oben. pcmAudioExportEnabled kann in der
	# C-Varargs-Reihenfolge nur mitgegeben werden, wenn debugLoggingEnabled auch
	# mitgegeben wird - fehlt es, wird es aus dem aktuellen Setting nachgezogen,
	# damit es nicht ungewollt auf False faellt.
	extra = []
	if debugLoggingEnabled is not None:
		extra.append(debugLoggingEnabled)
		if pcmAudioExportEnabled is not None:
			extra.append(pcmAudioExportEnabled)
	elif pcmAudioExportEnabled is not None:
		from Components.config import config
		try:
			dl = config.plugins.serviceapp.debug_logging.value
		except Exception:
			dl = False
		extra.extend([dl, pcmAudioExportEnabled])

	return serviceapp.exteplayer3_set_setting(settingId, aacSwDecoding, dtsSwDecoding, wmaSwDecoding, lpcmInjection, downmix, hlsQualityMode, hlsAudioDefaultOnly, *extra)

