#include <time.h>
#include <extplayer.h>
#include <cJSON/cJSON.h>
#include <error.h>
#include <common.h>

extern bool g_debugLoggingEnabled;
#define SALOG(fmt, ...) do { \
    if (g_debugLoggingEnabled) { \
        FILE *_sf = fopen("/tmp/serviceapp.log", "a"); \
        if (_sf) { fprintf(_sf, "[serviceapp] " fmt "\n", ##__VA_ARGS__); fflush(_sf); fclose(_sf); } \
    } \
} while(0)

void PlayerApp::handleJsonStr(const std::string& data)
{
	eLog(5, "PlayerApp::handleJsonStr: %s", data.c_str());
	cJSON *json = cJSON_Parse(data.c_str());
	if (!json)
	{
		eDebug("Error before: [%s]", cJSON_GetErrorPtr());
		return;
	}
	handleJsonOutput(json);
	cJSON_Delete(json);
}
void PlayerApp::handleOutput(const std::string& mydata)
{
	//FIXME
	std::size_t pos = 0;
	std::size_t startpos = 0;

	while((pos = mydata.find('\n', pos)) != std::string::npos)
	{
		if (truncated)
		{
			if (mydata[pos-1] != '}')
			{
				jsonstr = "";
				truncated = 0;
				return;
			}
			handleJsonStr(jsonstr + mydata.substr(startpos, pos - startpos));
			jsonstr = "";
			truncated = 0;
		}
		else
		{
			if (mydata[0] != '{')
			{
				jsonstr = "";
				truncated = 0;
				return;
			}
			handleJsonStr(mydata.substr(startpos, pos - startpos));
			jsonstr = "";
			truncated = 0;
		}
		pos+=1;
		startpos = pos;
	}
	if (startpos != std::string::npos && startpos != mydata.length())
	{
		//std::cout << "remains: " << mydata.length()-startpos;
		if (mydata[mydata.length()-1] == '}')
		{
			handleJsonStr(mydata.substr(startpos, mydata.length() - pos));
			truncated = 0;
		}
		else
		{
			truncated = 1;
			jsonstr = mydata.substr(startpos);
		}
	}
}

void PlayerApp::stderrAvail(const char *data)
{
	std::string mydata(data);
	// Der "J":{"ms":...} Positions-Heartbeat feuert waehrend der Wiedergabe
	// etwa alle 100ms und macht damit den grossen Mehrheit des Debug-Logs
	// aus, ohne echten Diagnosewert (reine Positionsangabe, keine Fehler-
	// oder Zustandsaenderung) - wird hier bewusst nicht mitgeloggt, die
	// eigentliche Verarbeitung (recvPosition() via handleOutput() unten)
	// bleibt davon unberuehrt.
	if (mydata.find("\"J\":{\"ms\"") == std::string::npos)
		SALOG("exteplayer3 stderr: %s", mydata.c_str());
	if (parseOutput == STD_ERROR)
	{
		handleOutput(mydata);
	}
}

void PlayerApp::stdoutAvail(const char *data)
{
	std::string mydata(data);
	SALOG("exteplayer3 stdout: %s", mydata.c_str());
	if (parseOutput == STD_OUTPUT)
	{
		handleOutput(mydata);
	}
}

void PlayerApp::appClosed(int retval)
{
	handleProcessStopped(retval);
}

int PlayerApp::processStart(eMainloop *context)
{
	console = new eConsoleContainer();
	console->appClosed   = [this](int retval)          { appClosed(retval); };
	console->stdoutAvail = [this](const char *data)    { stdoutAvail(data); };
	console->stderrAvail = [this](const char *data)    { stderrAvail(data); };
	const std::vector<std::string> args = buildCommand();
	eDebugNoNewLine("PlayerApp::processStart: ");
	char **cargs = (char **) malloc(sizeof(char *) * args.size()+1);
	for (size_t i=0; i <= args.size(); i++)
	{
		// execvp needs args array terminated with NULL
		if (i == args.size())
		{
			cargs[i] = NULL;
			eDebugNoNewLine("\n");
		}
		else
		{
			cargs[i] = strdup(args[i].c_str());
			if (i != 0 && cargs[i][0] != '-')
				eDebugNoNewLine("\"%s\" ", cargs[i]);
			else
				eDebugNoNewLine("%s ", cargs[i]);
		}
	}
	SALOG("PlayerApp::processStart executing %s", cargs[0]); int ret = console->execute(context, cargs[0], cargs); SALOG("console->execute returned %d", ret);
	for (size_t i=0; i < args.size(); i++)
		free(cargs[i]);
	free(cargs);
	return ret;
}

int PlayerApp::processSend(const std::string& data)
{
	if (console && console->running())
	{
		eLog(5, "sending command \"%s\" ", data.c_str());
		console->write(data.c_str(), data.length());
		return 0;
	}
	return -1;
}

#include <string.h>
#include <unistd.h>

void PlayerApp::processKill()
{
	/*
	 * Try to terminate the process gracefully first (SIGINT via sendCtrlC)
	 * so it can flush the hardware decoder and stop playback immediately.
	 * Wait up to 100ms for it to exit, polling its procfs state to break
	 * early once it exits (or becomes a zombie). If it lingers (e.g.
	 * blocked in a network read), forcefully terminate it with SIGKILL (via kill()).
	 */
	if (console)
	{
		int pid = console->getPID();
		SALOG("processKill: enter, pid=%d, running=%d", pid, (int)console->running());
		if (console->running())
		{
			SALOG("processKill: sending Ctrl-C (SIGINT)");
			console->sendCtrlC();
			for (int i = 0; i < 10; ++i)
			{
				if (pid > 0)
				{
					char stat_path[64];
					snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
					FILE *f = fopen(stat_path, "r");
					if (f)
					{
						char buffer[256];
						if (fgets(buffer, sizeof(buffer), f))
						{
							char *close_paren = strrchr(buffer, ')');
							if (close_paren && *(close_paren + 1) == ' ')
							{
								char state = *(close_paren + 2);
								SALOG("processKill: poll %d, state='%c'", i, state);
								if (state == 'Z')
								{
									SALOG("processKill: zombie detected, breaking loop");
									fclose(f);
									break; // Child has terminated and is now a zombie, can exit early!
								}
							}
						}
						fclose(f);
					}
					else
					{
						SALOG("processKill: poll %d, proc entry for %d is gone (reaped/exited)", i, pid);
						break; // Proc entry is gone, child has exited and been reaped
					}
				}
				else if (!console->running())
				{
					SALOG("processKill: poll %d, console->running() became false", i);
					break;
				}
				usleep(10000); // 10ms
			}
		}
		if (console->running())
		{
			SALOG("processKill: console->running() is still true, sending SIGKILL via console->kill()");
			console->kill();
		}
		else
		{
			SALOG("processKill: console->running() is false, no SIGKILL needed");
		}
	}
}

bool PlayerApp::processRunning()
{
	if (console)
	{
		console->pollPipes();
		return console->running();
	}
	return false;
}



void PlayerBackend::_updatePosition()
{
	pPlayer->sendUpdatePosition();
}

int PlayerBackend::start(const std::string& path, const std::map<std::string,std::string>& headers)
{
	pPlayer->setPath(path);
	pPlayer->setHttpHeaders(headers);
	if (pPlayer->start(eApp) < 0)
	{
		if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::stop);
		return -1;
	}
	myTimer = eTimer::create(eApp);
	initTimerMutex(myTimer);
	myTimer->AddRef();
	myTimer->timeout.connect(SigC::bind(SigC::slot(&PlayerBackend::s_updatePosition), this));
	return 0;
}
int PlayerBackend::stop()
{
	SALOG("PlayerBackend::stop: enter");
	if (m_stopping) {
		SALOG("PlayerBackend::stop: already stopping/stopped, skipping");
		return 0;
	}
	m_stopping = true;
	if (myTimer) myTimer->stop();
	pPlayer->sendStop();
	SALOG("PlayerBackend::stop: sent 'q' command, waiting adaptively (max 950ms)...");
	for (int i = 0; i < 95; ++i)
	{
		if (!pPlayer->processRunning()) {
			SALOG("PlayerBackend::stop: exteplayer3 exited cleanly after %d ms", i * 10);
			break;
		}
		usleep(10000); // 10ms
	}
	SALOG("PlayerBackend::stop: calling sendForceStop...");
	pPlayer->sendForceStop();
	playbackStarted = false;
	SALOG("PlayerBackend::stop: done");
	return 0;
}

int PlayerBackend::pause()
{
	if (!playbackStarted)
		return -1;
	pPlayer->sendPause();
	return 0;
}

int PlayerBackend::resume()
{
	if (!playbackStarted)
		return -1;
	pPlayer->sendResume();
	return 0;
}

int PlayerBackend::seekTo(int seconds)
{
	if (!playbackStarted)
		return -1;
	pPlayer->sendSeekTo(seconds);
	return 0;
}

int PlayerBackend::seekRelative(int seconds)
{
	if (!playbackStarted)
		return -1;
	pPlayer->sendSeekRelative(seconds);
	return 0;
}

int PlayerBackend::getPlayPosition(int& mseconds)
{
	if (!playbackStarted)
	{
		return -1;
	}
	if (!mPositionInMs) 
	{
		pPlayer->sendUpdatePosition();
		return -2;
	}
	mseconds = mPositionInMs;
	return 0;
}

int PlayerBackend::getLength(int& mseconds)
{
	if (!playbackStarted)
	{
		return -1;
	}
	if (!mLengthInMs) 
	{
		pPlayer->sendUpdateLength();
		return -2;
	}
	mseconds = mLengthInMs;
	return 0;
}

int PlayerBackend::getErrorMessage(errorMessage& error)
{
	if (!playbackStarted || !pErrorMessage)
	{
		return -1;
	}
	error = *pErrorMessage;
	return 0;
}

int PlayerBackend::audioGetNumberOfTracks(int timeout)
{
	pPlayer->sendUpdateAudioTracksList();
	return mAudioStreams.size();
}

int PlayerBackend::audioGetCurrentTrackNum()
{
	int trackNum = 0, j=0;
	int trackId = pCurrentAudio ? pCurrentAudio->id : 0;
	for (std::vector<audioStream>::const_iterator i(mAudioStreams.begin()); i!=mAudioStreams.end(); i++, j++)
	{
		if (trackId == i->id)
		{
			trackNum = j;
			break;
		}
	}
	return trackNum;
}

int PlayerBackend::audioSelectTrack(int trackNum)
{
	if (trackNum >= 0 && trackNum < (int) mAudioStreams.size())
	{
		pPlayer->sendAudioSelectTrack(mAudioStreams[trackNum].id);
		/* Optimistisch sofort setzen statt nur auf die asynchrone "a_s"-
		 * Bestaetigung zu warten - sonst zeigt die GUI beim direkt danach
		 * folgenden getCurrentTrack() noch die alte Spur an (Bestaetigung
		 * kommt erst in der naechsten eMainloop-Iteration ueber die Pipe
		 * zurueck). recvAudioTrackSelected() setzt denselben Wert spaeter
		 * redundant nochmal - dient als Selbstkorrektur bei Abweichung. */
		if (pCurrentAudio != NULL)
		{
			delete pCurrentAudio;
			pCurrentAudio = NULL;
		}
		pCurrentAudio = new audioStream(mAudioStreams[trackNum]);
		return 0;
	}
	return -1;
}

int PlayerBackend::audioGetTrackInfo(audioStream& trackInfo, int trackNum)
{
	if (trackNum >= 0 && trackNum < (int) mAudioStreams.size())
	{
		trackInfo = mAudioStreams[trackNum];
		return 0;
	}
	return -1;
}

int PlayerBackend::subtitleGetNumberOfTracks(int timeout)
{
	pPlayer->sendUpdateSubtitleTracksList();
	return mSubtitleStreams.size();
}

int PlayerBackend::subtitleGetCurrentTrackNum()
{
	int trackNum = 0, j=0;
	int trackId = pCurrentSubtitle ? pCurrentSubtitle->id : 0;
	for (std::vector<subtitleStream>::const_iterator i(mSubtitleStreams.begin()); i!=mSubtitleStreams.end(); i++, j++)
	{
		if (trackId == i->id)
		{
			trackNum = j;
			break;
		}
	}
	return trackNum;
}

int PlayerBackend::subtitleSelectTrack(int trackNum)
{
	/* Optimistisch sofort setzen statt nur auf die asynchrone Bestaetigung
	 * zu warten, siehe audioSelectTrack() fuer die ausfuehrliche Begruendung. */
	if (trackNum == -1)
	{
		pPlayer->sendSubtitleSelectTrack(-1);
		if (pCurrentSubtitle != NULL)
		{
			delete pCurrentSubtitle;
			pCurrentSubtitle = NULL;
		}
		return 0;
	}
	if (trackNum >= 0 && trackNum < (int) mSubtitleStreams.size())
	{
		pPlayer->sendSubtitleSelectTrack(mSubtitleStreams[trackNum].id);
		if (pCurrentSubtitle != NULL)
		{
			delete pCurrentSubtitle;
			pCurrentSubtitle = NULL;
		}
		pCurrentSubtitle = new subtitleStream(mSubtitleStreams[trackNum]);
		return 0;
	}
	return -1;
}

int PlayerBackend::subtitleGetTrackInfo(subtitleStream& trackInfo, int trackNum)
{
	if (trackNum >= 0 && trackNum < (int) mSubtitleStreams.size())
	{
		trackInfo = mSubtitleStreams[trackNum];
		return 0;
	}
	return -1;
}

int PlayerBackend::videoGetTrackInfo(videoStream& trackInfo, int trackNum)
{
	if (pCurrentVideo == NULL)
		return -1;
	trackInfo = *pCurrentVideo;
	return 0;
}




void PlayerBackend::recvStarted(int status)
{
	eDebug("PlayerBackend::recvStart - status = %d", status);
	if (playbackStarted || status)
		return;
	playbackStarted = true;
	if (myTimer) myTimer->start(mTimerDelay, false);
	if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::start);
}

void PlayerBackend::recvStopped(int retval)
{
	eDebug("PlayerBackend::recvStopped - retval = %d", retval);
	if (gotPlayerMessage && !m_stopping) gotPlayerMessage(PlayerMessage::stop);
}

void PlayerBackend::recvPaused(int status)
{
	eDebug("PlayerBackend::recvPause - status = %d", status);
	if (!status)
	{
		if (myTimer) myTimer->stop();
		if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::pause);
	}
}

void PlayerBackend::recvResumed(int status)
{
	eDebug("PlayerBackend::recvResume - status = %d", status);
	if (!status)
	{
		if (myTimer) myTimer->start(mTimerDelay, false);
		if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::resume);
	}
}

void PlayerBackend::recvAudioTracksList(int status, std::vector<audioStream>& streams)
{
	if(!status) 
		mAudioStreams = streams;
}

void PlayerBackend::recvAudioTrackCurrent(int status, audioStream& stream)
{ 
	eDebug("PlayerBackend::recvAudioTrackCurrent - status = %d", status);
	if(!status)
	{
		if (pCurrentAudio != NULL)
		{
			delete pCurrentAudio;
			pCurrentAudio = NULL;
		}
		pCurrentAudio = new audioStream(stream);
	}
} 

void PlayerBackend::recvAudioTrackSelected(int status, int trackId)
{
	eDebug("PlayerBackend::recvAudioTrackSelected - status = %d, trackId = %d", status, trackId);
	if (!status)
	{
		for (std::vector<audioStream>::const_iterator i(mAudioStreams.begin()); i!=mAudioStreams.end(); i++)
		{
			if (trackId == i->id)
			{
				if (pCurrentAudio != NULL)
				{
					delete pCurrentAudio;
					pCurrentAudio = NULL;
				}
				pCurrentAudio = new audioStream(*i);
				break;
			}
		}
	}
	else
	{
		/* Wechsel im Backend fehlgeschlagen - die optimistische Annahme aus
		 * audioSelectTrack() waere sonst dauerhaft falsch. Echten Zustand
		 * per "ac" nachziehen (kommt ueber recvAudioTrackCurrent() zurueck). */
		pPlayer->sendUpdateAudioTrackCurrent();
	}
}

void PlayerBackend::recvSubtitleTracksList(int status, std::vector<subtitleStream>& streams)
{ 
	if(!status) 
		mSubtitleStreams = streams;
}

void PlayerBackend::recvSubtitleTrackCurrent(int status, subtitleStream& stream)
{ 
	eDebug("PlayerBackend::recvSubtitleTrackCurrent - status = %d", status);
	if(!status)
	{
		if (pCurrentSubtitle != NULL)
		{
			delete pCurrentSubtitle;
			pCurrentSubtitle = NULL;
		}
		pCurrentSubtitle = new subtitleStream(stream);
	}
} 

void PlayerBackend::recvSubtitleTrackSelected(int status, int trackId)
{
	eDebug("PlayerBackend::recvSubtitleTrackSelected - status = %d, trackId = %d", status, trackId);
	if (!status)
	{
		for (std::vector<subtitleStream>::const_iterator i(mSubtitleStreams.begin()); i!=mSubtitleStreams.end(); i++)
		{
			if (trackId == i->id)
			{
				if (pCurrentSubtitle != NULL)
				{
					delete pCurrentSubtitle;
					pCurrentSubtitle = NULL;
				}
				pCurrentSubtitle = new subtitleStream(*i);
				break;
			}
		}
	}
	else
	{
		/* Wechsel im Backend fehlgeschlagen - echten Zustand nachziehen,
		 * siehe recvAudioTrackSelected() fuer die ausfuehrliche Begruendung. */
		pPlayer->sendUpdateSubtitleTrackCurrent();
	}
}

void PlayerBackend::recvVideoTrackCurrent(int status, videoStream& stream)
{
	eDebug("PlayerBackend::recvVideoTrackCurrent - status = %d", status);
	if (!status)
	{
		videoStream prev;
		if (pCurrentVideo != NULL)
		{
			prev = *pCurrentVideo;
			delete pCurrentVideo;
			pCurrentVideo = NULL;
		}
		pCurrentVideo = new videoStream(stream);
		if (stream.progressive >= 0 && prev.progressive != stream.progressive)
			if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::videoProgressiveChanged);
		if (stream.framerate > 0 && prev.framerate != stream.framerate)
			if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::videoFramerateChanged);
		if ((stream.width > 0 && prev.width != stream.width) || (stream.height > 0 && prev.height != stream.height))
			if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::videoSizeChanged);
	}
}

void PlayerBackend::recvSubtitleMessage(subtitleMessage& sub)
{
	mSubtitles.push(sub);
	if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::subtitleAvailable);
}

void PlayerBackend::recvMetadata(const std::string &radiotext, const std::string &title, const std::string &artist)
{
	if (radiotext == mRadioText && title == mMetaTitle && artist == mMetaArtist)
		return;
	mRadioText = radiotext;
	mMetaTitle = title;
	mMetaArtist = artist;
	if (gotPlayerMessage) gotPlayerMessage(PlayerMessage::radioTextAvailable);
}

int PlayerBackend::getSubtitles(std::queue< subtitleMessage >& subtitles)
{
	if (mSubtitles.empty())
	{
		return -1;
	}
	while (!mSubtitles.empty())
	{
		subtitles.push(mSubtitles.front());
		mSubtitles.pop();
	}
	return 0;
}
