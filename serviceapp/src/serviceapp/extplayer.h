#ifndef __extplayer_h
#define __extplayer_h

#include <functional>
#include <lib/base/ebase.h>
#include <lib/base/message.h>
#include <lib/base/thread.h>
#include <lib/python/connections.h>


#include "cJSON/cJSON.h"
#include "myconsole.h"
#include "subtitles/subtitles.h"

#ifndef eLog
 #define eLog(lvl,...)
#endif

enum
{
	STD_OUTPUT,
	STD_ERROR,
};

class PlayerApp
{
	ePtr<eConsoleContainer> console;
	std::string jsonstr;
	unsigned int parseOutput;
	unsigned int truncated;
	void stdoutAvail(const char *data);
	void stderrAvail(const char *data);
	void appClosed(int retval);
	void handleOutput(const std::string& data);
	void handleJsonStr(const std::string& data);
	void handleAppClosed(int retval);
	static void s_stdoutAvail(const char *data, PlayerApp *self) { self->stdoutAvail(data); }
	static void s_stderrAvail(const char *data, PlayerApp *self) { self->stderrAvail(data); }
	static void s_appClosed(int retval, PlayerApp *self) { self->appClosed(retval); }
protected:
	virtual std::vector<std::string> buildCommand() = 0;
	virtual void handleJsonOutput(cJSON *json) = 0;
	virtual void handleProcessStopped(int retval) = 0;
	int processStart(eMainloop *context);
	int processSend(const std::string& data);
	void processKill();
	bool processRunning();
public:
	PlayerApp(int parseOutput=STD_ERROR):
		parseOutput(parseOutput),
		truncated(0){}
	~PlayerApp(){}
};


struct PlayerMessage
{
	enum
	{
		start,
		stop,
		pause,
		resume,
		error,
		videoSizeChanged,
		videoProgressiveChanged,
		videoFramerateChanged,
		subtitleAvailable,
	};
};


struct audioStream
{
	int id;
	std::string language_code; /* iso-639, if available. */
	std::string description; /* clear text codec description */
	audioStream(): id(-1){};
};


struct videoStream
{
	int id;
	std::string language_code; /* iso-639, if available. */
	std::string description; /* clear text codec description */
	int width;
	int height;
	int framerate;
	int progressive;
	videoStream(): id(-1), width(-1), height(-1), framerate(-1), progressive(-1){};
};


struct errorMessage
{
	int code;
	std::string message;
	errorMessage():code(-1){}
};


class iPlayerSend
{
public:
	virtual int sendStop() { return -1;}
	virtual int sendForceStop() { return -1;}
	virtual int sendPause(){ return -1;}
	virtual int sendResume(){ return -1;}
	virtual int sendUpdateLength(){ return -1;}
	virtual int sendUpdatePosition(){ return -1;}
	virtual int sendUpdateAudioTracksList(){ return -1;}
	virtual int sendUpdateAudioTrackCurrent(){ return -1;}
	virtual int sendAudioSelectTrack(int trackId){ return -1;}
	virtual int sendUpdateSubtitleTracksList(){ return -1;}
	virtual int sendUpdateSubtitleTrackCurrent(){ return -1;}
	virtual int sendSubtitleSelectTrack(int trackId){ return -1;}
	virtual int sendSeekTo(int seconds){ return -1;}
	virtual int sendSeekRelative(int seconds){ return -1;}
};


class iPlayerCallback
{
public:
	virtual void recvStarted(int status){};
	virtual void recvStopped(int status){};
	virtual void recvPaused(int status){};
	virtual void recvResumed(int status){};
	virtual void recvLength(int status, int mseconds){};
	virtual void recvPosition(int status, int mseconds){};
	virtual void recvSeekTo(int status, int seconds){};
	virtual void recvSeekRelative(int status, int seconds){};
	virtual void recvAudioTracksList(int status, std::vector<audioStream>&){};
	virtual void recvAudioTrackCurrent(int status, audioStream&){}; 
	virtual void recvAudioTrackSelected(int status, int trackId){};
	virtual void recvSubtitleTracksList(int status, std::vector<subtitleStream>&){};
	virtual void recvSubtitleTrackCurrent(int status, subtitleStream&){}; 
	virtual void recvSubtitleTrackSelected(int status, int trackId){};
	virtual void recvSubtitleMessage(subtitleMessage&){};
	virtual void recvVideoTrackCurrent(int status, videoStream&){};
	virtual void recvErrorMessage(errorMessage&){};
};


class BasePlayer: public iPlayerSend, public iPlayerCallback
{
	iPlayerCallback *pCallback;

protected:
	std::string mPath;
	std::map<std::string, std::string> mHeaders;
	
	void recvStarted(int status){pCallback->recvStarted(status);};
	void recvStopped(int status){pCallback->recvStopped(status);};
	void recvPaused(int status){pCallback->recvPaused(status);};
	void recvResumed(int status){pCallback->recvResumed(status);};
	void recvLength(int status, int mseconds){pCallback->recvLength(status, mseconds);};
	void recvPosition(int status, int mseconds){pCallback->recvPosition(status, mseconds);};
	void recvAudioTracksList(int status, std::vector<audioStream>& streams){pCallback->recvAudioTracksList(status, streams);};
	void recvAudioTrackCurrent(int status, audioStream& stream){pCallback->recvAudioTrackCurrent(status, stream);};
	void recvAudioTrackSelected(int status, int trackId){pCallback->recvAudioTrackSelected(status, trackId);};
	void recvSubtitleMessage(subtitleMessage& sub){pCallback->recvSubtitleMessage(sub);};
	void recvSubtitleTracksList(int status, std::vector<subtitleStream>& streams){pCallback->recvSubtitleTracksList(status, streams);};
	void recvSubtitleTrackCurrent(int status, subtitleStream& stream){pCallback->recvSubtitleTrackCurrent(status, stream);};
	void recvSubtitleTrackSelected(int status, int trackId){pCallback->recvSubtitleTrackSelected(status, trackId);};
	void recvVideoTrackCurrent(int status, videoStream& stream){pCallback->recvVideoTrackCurrent(status, stream);};
	void recvSeekTo(int status, int seconds){pCallback->recvSeekTo(status, seconds);};
	void recvSeekRelative(int status, int seconds){pCallback->recvSeekRelative(status, seconds);};
	void recvErrorMessage(errorMessage& message){pCallback->recvErrorMessage(message);};
public:
	virtual ~BasePlayer(){}

	void setCallback(iPlayerCallback *cb){pCallback = cb;}
	void setPath(const std::string& path){mPath = path;}
	void setHttpHeaders(const std::map<std::string, std::string>& headers){mHeaders = headers;}

	virtual int start(eMainloop *context) = 0;
	virtual bool processRunning() { return false; }

};


class PlayerBackend: public iPlayerCallback
{
	int mPositionInMs, mLengthInMs;
	bool playbackStarted;
	bool m_stopping;

	BasePlayer *pPlayer;

	audioStream *pCurrentAudio;
	videoStream *pCurrentVideo;
	subtitleStream *pCurrentSubtitle;
	errorMessage *pErrorMessage;

	std::vector<audioStream> mAudioStreams;
	std::vector<subtitleStream> mSubtitleStreams;
	std::queue<subtitleMessage> mSubtitles;

	eTimer *myTimer;
	unsigned int mTimerDelay;

	void _updatePosition();
	static void s_updatePosition(PlayerBackend *self) { self->_updatePosition(); }

	// iPlayerCallback
	void recvStarted(int status);
	void recvStopped(int status);
	void recvPaused(int status);
	void recvResumed(int status);
	void recvLength(int status, int mseconds){ if (!status) mLengthInMs = mseconds; }
	void recvPosition(int status, int mseconds){ if (!status) mPositionInMs = mseconds; }
	void recvAudioTracksList(int status, std::vector<audioStream>& streams);
	void recvAudioTrackCurrent(int status, audioStream& stream);
	void recvAudioTrackSelected(int status, int trackId);
	void recvSubtitleTracksList(int status, std::vector<subtitleStream>& streams);
	void recvSubtitleTrackCurrent(int status, subtitleStream& stream);
	void recvSubtitleTrackSelected(int status, int trackId);
	void recvVideoTrackCurrent(int status, videoStream& stream);
	void recvSeekTo(int status, int seconds){eDebug("PlayerBackend::recvSeekTo %ds", seconds);}
	void recvSeekRelative(int status, int seconds){eDebug("PlayerBackend::recvSeekRelative %ds", seconds);}
	void recvErrorMessage(errorMessage& message){pErrorMessage = new errorMessage(message);};
	void recvSubtitleMessage(subtitleMessage& sub);

public:
	PlayerBackend(BasePlayer* extplayer):
		mPositionInMs(0),
		mLengthInMs(0),
		playbackStarted(false),
		m_stopping(false),
		pPlayer(extplayer),
		pCurrentAudio(NULL),
		pCurrentVideo(NULL),
		pCurrentSubtitle(NULL),
		pErrorMessage(NULL),
		mTimerDelay(100) // updated play position timer
	{
		pPlayer->setCallback(this);
		myTimer = NULL;
	}
	~PlayerBackend()
	{
		if (pErrorMessage != NULL)
			delete pErrorMessage;
		if (pCurrentVideo != NULL)
			delete pCurrentVideo;
		if (pCurrentAudio != NULL)
			delete pCurrentAudio;
		if (pCurrentSubtitle != NULL)
			delete pCurrentSubtitle;
		stop();
		if (myTimer) {
			myTimer->stop();
			myTimer->Release();
			myTimer = NULL;
		}
	}
	int start(const std::string& path, const std::map<std::string,std::string>& headers);
	int stop();
	int pause();
	int resume();
	int seekTo(int seconds);
	int seekRelative(int seconds);
	int getLength(int& mseconds);
	int getPlayPosition(int& mseconds);
	int getErrorMessage(errorMessage& error);
	int getSubtitles(std::queue<subtitleMessage>&);
	int audioGetNumberOfTracks(int timeout=0);
	int audioSelectTrack(int trackId);
	int audioGetTrackInfo(audioStream& trackInfo, int trackId);
	int audioGetCurrentTrackNum();
	int subtitleGetNumberOfTracks(int timeout=0);
	int subtitleSelectTrack(int trackId);
	int subtitleGetTrackInfo(subtitleStream& trackInfo, int trackId);
	int subtitleGetCurrentTrackNum();
	int videoGetTrackInfo(videoStream& trackInfo, int trackId);

	std::function<void(int)> gotPlayerMessage;
};


#endif
