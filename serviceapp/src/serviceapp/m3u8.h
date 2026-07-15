#ifndef __m3u8variant__h
#define __m3u8variant__h
#include <map>
#include <vector>
#include <string>
#include <sstream>

#include "wrappers.h"
#include "common.h"

struct M3U8StreamInfo
{
    std::string url;
    HeaderMap headers;
    std::string codecs;
    std::string resolution;
    std::string audioGroupId;
    unsigned long int bitrate;

    bool operator<(const M3U8StreamInfo& m) const
    {
        return bitrate < m.bitrate;
    }
};

struct M3U8AudioTrack
{
    std::string groupId;
    std::string uri;
    bool isDefault;
};

class M3U8VariantsExplorer
{
    std::string url;
    HeaderMap headers;
    std::vector<M3U8StreamInfo> streams;
    std::vector<M3U8AudioTrack> audioTracks;
    const unsigned int redirectLimit;
    int parseStreamInfoAttributes(const char *line, M3U8StreamInfo& info);
    int parseMediaAttributes(const char *line, M3U8AudioTrack& track);
    std::string resolveUrl(const std::string& base, const std::string& ref);
    int getVariantsFromMasterUrl(const std::string& url, HeaderMap& headers, unsigned int redirect);
public:
    M3U8VariantsExplorer(const std::string& url, const HeaderMap& headers):
        url(url),
        headers(headers),
        redirectLimit(3){};
    std::vector<M3U8StreamInfo> getStreams(bool filterDefaultAudio = true);

};

bool isM3U8Url(const std::string& url);
#endif

