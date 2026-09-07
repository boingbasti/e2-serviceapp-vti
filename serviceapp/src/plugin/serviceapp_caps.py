# ServiceApp capabilities for third-party plugins detection
HAS_NATIVE_REFERER = True
HAS_HLS_AUDIO_FILTER = True
HAS_HLS_QUALITY_SELECT = True
HAS_DEBUG_LOGGING_CONTROL = True
HAS_PCM_AUDIO_EXPORT = True
# Getrennt von HAS_NATIVE_REFERER: eServiceFactoryApp::record() war ueber
# alle bisherigen Versionen hinweg ein harter Stub (ptr=0, return -1), auch
# waehrend HAS_NATIVE_REFERER schon True war. Erst ab dieser Version gibt es
# eine echte eServiceAppRecord-Implementierung. Ohne dieses eigene Flag
# wuerde eine Aufnahme auf aelteren Custom-serviceapp-Versionen scheinbar
# starten (Popup erscheint), aber es entsteht keine Datei.
HAS_NATIVE_RECORDING = True
PCM_AUDIO_EXPORT_FIFO_PATH = "/tmp/exteplayer3_pcm_audio.fifo"
HAS_BACKEND_NAME_INFO = True
# Offset auf iServiceInformation.sUser. Liefert "exteplayer3"/"gstplayer",
# wenn serviceapp gerade aktiv wiedergibt. Existiert der Key nicht (native
# eServiceMP3/Wiedergabemodul=Original antwortet nicht darauf, kein
# resIsString), laeuft der Systemplayer statt serviceapp.
INFO_KEY_BACKEND_NAME = 13
