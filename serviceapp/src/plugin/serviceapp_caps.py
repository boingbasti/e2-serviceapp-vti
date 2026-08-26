# ServiceApp capabilities for third-party plugins detection
HAS_NATIVE_REFERER = True
HAS_HLS_AUDIO_FILTER = True
HAS_HLS_QUALITY_SELECT = True
HAS_DEBUG_LOGGING_CONTROL = True
HAS_PCM_AUDIO_EXPORT = True
PCM_AUDIO_EXPORT_FIFO_PATH = "/tmp/exteplayer3_pcm_audio.fifo"
HAS_BACKEND_NAME_INFO = True
# Offset auf iServiceInformation.sUser. Liefert "exteplayer3"/"gstplayer",
# wenn serviceapp gerade aktiv wiedergibt. Existiert der Key nicht (native
# eServiceMP3/Wiedergabemodul=Original antwortet nicht darauf, kein
# resIsString), laeuft der Systemplayer statt serviceapp.
INFO_KEY_BACKEND_NAME = 13
