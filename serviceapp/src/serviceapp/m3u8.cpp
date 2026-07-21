#include "m3u8.h"
#include <cstring>

extern bool g_debugLoggingEnabled;
#define M3ULOG(fmt, ...) do { \
    if (g_debugLoggingEnabled) { \
        FILE *_sf = fopen("/tmp/serviceapp.log", "a"); \
        if (_sf) { fprintf(_sf, "[m3u8] " fmt "\n", ##__VA_ARGS__); fflush(_sf); fclose(_sf); } \
    } \
} while(0)

#define M3U8_HEADER "#EXTM3U"
#define M3U8_HEADER_MAX_LINE 5

#define M3U8_STREAM_INFO "#EXT-X-STREAM-INF"
#define M3U8_MEDIA "#EXT-X-MEDIA"
#define M3U8_MEDIA_SEQUENCE "#EXT-X-MEDIA-SEQUENCE"

bool isM3U8Url(const std::string &url)
{
    Url purl(url);
    std::string path = purl.path();
    size_t delim_idx = path.rfind(".");
    if((purl.proto() == "http" || purl.proto() == "https")
            && delim_idx != std::string::npos
            && !path.compare(delim_idx, 5, ".m3u8"))
        return true;
    return false;
}

int parse_attribute(char **ptr, char **key, char **value)
{
    if (ptr == NULL || *ptr == NULL || key == NULL || value == NULL)
        return -1;

    char *end; 
    char *p;
    p = end = strchr(*ptr, ',');
    if (end)
    {
        char *q = strchr(*ptr, '"');
        if (q && q < end)
        {
            q = strchr(++q, '"');
            if (q)
            {
                p = end = strchr(++q, ',');
            }
        }
    }
    if (end)
    {
        do
        {
            end++;
        }
        while(*end && *end == ' ');
        *p = '\0';
    }

    *key = *ptr;
    p = strchr(*ptr, '=');
    if (!p)
        return -1;
    *p++ = '\0';
    *value = p;
    *ptr = end;
    return 0;
}

//https://tools.ietf.org/html/draft-pantos-http-live-streaming-13#section-3.4.10
int M3U8VariantsExplorer::parseStreamInfoAttributes(const char *attributes, M3U8StreamInfo& info)
{
    char *myline = strdup(attributes);
    char *ptr = myline;
    char *key = NULL;
    char *value = NULL;
    while (!parse_attribute(&ptr, &key, &value))
    {
        if (!strcasecmp(key, "bandwidth"))
            info.bitrate = atoi(value);
        if (!strcasecmp(key, "resolution"))
            info.resolution = value;
        if (!strcasecmp(key, "codecs"))
            info.codecs = value;
        if (!strcasecmp(key, "audio"))
        {
            // strip surrounding quotes
            std::string v(value);
            if (v.size() >= 2 && v[0] == '"')
                v = v.substr(1, v.size() - 2);
            info.audioGroupId = v;
        }
    }
    free(myline);
    return 0;
}

int M3U8VariantsExplorer::parseMediaAttributes(const char *attributes, M3U8AudioTrack& track)
{
    char *myline = strdup(attributes);
    char *ptr = myline;
    char *key = NULL;
    char *value = NULL;
    track.isDefault = false;
    while (!parse_attribute(&ptr, &key, &value))
    {
        if (!strcasecmp(key, "type") && strcasecmp(value, "AUDIO"))
        {
            free(myline);
            return -1; // not an audio track
        }
        if (!strcasecmp(key, "group-id"))
        {
            std::string v(value);
            if (v.size() >= 2 && v[0] == '"')
                v = v.substr(1, v.size() - 2);
            track.groupId = v;
        }
        if (!strcasecmp(key, "default") && !strcasecmp(value, "YES"))
            track.isDefault = true;
        if (!strcasecmp(key, "uri"))
        {
            std::string v(value);
            if (v.size() >= 2 && v[0] == '"')
                v = v.substr(1, v.size() - 2);
            track.uri = v;
        }
    }
    free(myline);
    return track.uri.empty() ? -1 : 0;
}

std::string M3U8VariantsExplorer::resolveUrl(const std::string& base, const std::string& ref)
{
    if (ref.substr(0, 4) == "http")
        return ref;
    // Protokoll-relative URL (//host/path, gueltiger RFC-3986-Referenztyp):
    // Schema von base uebernehmen, Rest (Host+Pfad) kommt bereits aus ref.
    if (ref.substr(0, 2) == "//")
    {
        Url purl(base);
        return purl.proto() + ":" + ref;
    }
    // Absoluter Pfad: nur Origin (scheme://host) voranstellen
    if (!ref.empty() && ref[0] == '/')
    {
        Url purl(base);
        std::string origin = purl.proto() + "://" + purl.host();
        int port = purl.port();
        if (port != -1 && port != 80 && port != 443)
            origin += ":" + std::to_string(port);
        return origin + ref;
    }
    // Relativer Pfad: Basisverzeichnis voranstellen
    return base.substr(0, base.rfind('/') + 1) + ref;
}


int M3U8VariantsExplorer::getVariantsFromMasterUrl(const std::string& url, HeaderMap& headers, unsigned int redirect)
{
    if (redirect > redirectLimit)
    {
        fprintf(stderr, "[%s] - reached maximum number of %d - redirects", __func__, redirectLimit);
        return -1;
    }
    Url purl(url);

    int port = purl.port();
    if (port == -1)
    {
        if (purl.proto() == "http")
            port = 80;
        else if (purl.proto() == "https")
            port = 443;
        else
        {
            fprintf(stderr, "[%s] - not defined port!\n", __func__);
            return -1;
        }
    }
    int sd;
    M3ULOG("Connect to %s:%d", purl.host().c_str(), port);
    if((sd = Connect(purl.host().c_str(), port, 5)) < 0)
    {
        M3ULOG("Connect FAILED to %s:%d", purl.host().c_str(), port);
        return -1;
    }
    M3ULOG("Connect OK");

    SSL *ssl = NULL;
    SSL_CTX *ssl_ctx = NULL;

    if (purl.proto() == "https")
    {
        if (SSLConnect(purl.host().c_str(), sd, &ssl, &ssl_ctx) < 0)
        {
            ::close(sd);
            return -1;
        }
        fprintf(stderr, "[%s] - (SSL) Connected with %s encryption\n",
                __func__, SSL_get_cipher(ssl));

        // just inform about verification error but continue to work
        if (SSL_get_verify_result(ssl) != X509_V_OK)
        {
            fprintf(stderr, "[%s] - (SSL) Error in certificate verification: %s\n",
                    __func__, X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
        }
    }
    std::string userAgent = "Enigma2 HbbTV/1.1.1 (+PVR+RTSP+DL;OpenPLi;;;)";
    HeaderMap::const_iterator it;
    if ((it = headers.find("User-Agent")) != headers.end())
    {
        userAgent = it->second;
    }
    headers["User-Agent"] = userAgent;

    std::string path = purl.path();
    std::string query = purl.query();
    if (!query.empty())
        path += "?" + query;
    std::string request = "GET ";
    request.append(path).append(" HTTP/1.1\r\n");
    request.append("Host: ").append(purl.host()).append("\r\n");
    request.append("User-Agent: ").append(userAgent).append("\r\n");
    request.append("Accept: */*\r\n");
    for (HeaderMap::const_iterator it(headers.begin()); it != headers.end(); it++)
    {
        if ((it->first).compare("User-Agent"))
        {
            request.append(it->first + ": ").append(it->second).append("\r\n");
        }
    }
    request.append("Connection: close\r\n");
    request.append("\r\n");

    fprintf(stderr, "[%s] - Request:\n", __func__);
    fprintf(stderr, "%s\n", request.c_str());

    if (writeAll(ssl, sd, request.c_str(), request.length()) < (signed long) request.length())
    {
        fprintf(stderr, "[%s] - writeAll, didn't write everything\n", __func__);
        ::close(sd);
        if (ssl)
        {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
        return -1;
    }
    int lines = 0;
    int contentLines = 0;

    int contentLength = 0;
    int contentSize = 0;
    bool contentStarted = false;
    bool contentTypeParsed = false;
    bool m3u8HeaderParsed = false;
    bool m3u8StreamInfoParsing = false;
    M3U8StreamInfo m3u8StreamInfo;

    size_t bufferSize = 1024;
    char *lineBuffer = (char *) malloc(bufferSize);

    int statusCode;
    char protocol[64], statusMessage[64];

    int result = readLine(ssl, sd, &lineBuffer, &bufferSize);
    fprintf(stderr, "[%s] Response[%d](size=%d): %s\n", __func__, lines++, result, lineBuffer);
    result = sscanf(lineBuffer, "%99s %d %99s", protocol, &statusCode, statusMessage);
    M3ULOG("HTTP status: %d", statusCode);
    if (result != 3 || (statusCode != 200 && statusCode != 302))
    {
            M3ULOG("wrong http response code: %d", statusCode);
            free(lineBuffer);
            if (ssl)
            {
                SSL_free(ssl);
                SSL_CTX_free(ssl_ctx);
            }
            ::close(sd);
            return -1;
    }
    int ret = -1;
    while(1)
    {
        result = readLine(ssl, sd, &lineBuffer, &bufferSize);
        fprintf(stderr, "[%s] Response[%d](size=%d): %s\n", __func__, lines++, result, lineBuffer);
        if (result < 0)
        {
            fprintf(stderr, "[%s] - end of read, nothing was read\n", __func__);
            break;
        }

        if (!contentStarted)
        {
            if (!contentLength)
            {
                sscanf(lineBuffer, "Content-Length: %d", &contentLength);
            }
            if (!contentTypeParsed)
            {
                char contenttype[33];
                if (sscanf(lineBuffer, "Content-Type: %32s", contenttype) == 1)
                {
                    contentTypeParsed = true;
                    if (!(!strncasecmp(contenttype, "application/text", 16)
                            || !strncasecmp(contenttype, "text/plain", 10)
                            || !strncasecmp(contenttype, "audio/x-mpegurl", 15)
                            || !strncasecmp(contenttype, "application/x-mpegurl", 21)
                            || !strncasecmp(contenttype, "application/vnd.apple.mpegurl", 29)
                            || !strncasecmp(contenttype, "audio/mpegurl", 13)
                            || !strncasecmp(contenttype, "application/m3u", 15)))
                    {
                        fprintf(stderr, "[%s] - not supported contenttype detected: %s!\n", __func__, contenttype);
                        break;
                    }
                }
            }
            if (statusCode == 302 && strncasecmp(lineBuffer, "location: ", 10) == 0)
            {
                std::string newurl = &lineBuffer[10];
                fprintf(stderr, "[%s] - redirecting to: %s\n", __func__, newurl.c_str());
                ret = getVariantsFromMasterUrl(newurl, headers, ++redirect);
                break;
            }
            if (!strncmp(lineBuffer, "Set-Cookie: ", 12))
            {
                headers["Cookie"] = &lineBuffer[12];
            }
            if (!result)
            {
                contentStarted = true;
                fprintf(stderr, "[%s] - content part started\n", __func__);
            }
        }
        else
        {
            contentLines++;
            contentSize += result + 1; // newline char
            if (!m3u8HeaderParsed)
            {
                if (contentLines > M3U8_HEADER_MAX_LINE)
                {
                    fprintf(stderr, "[%s] - invalid M3U8 playlist, '%s' header is not in first %d lines\n",
                            __func__, M3U8_HEADER, M3U8_HEADER_MAX_LINE);
                    break;
                }

                // find M3U8 header
                if (result && !strncmp(lineBuffer, M3U8_HEADER, strlen(M3U8_HEADER)))
                {
                    m3u8HeaderParsed = true;
                }
                continue;
            }

            if (!strncmp(lineBuffer, M3U8_MEDIA_SEQUENCE, strlen(M3U8_MEDIA_SEQUENCE)))
            {
                M3ULOG("not a master playlist (EXT-X-MEDIA-SEQUENCE found)");
                break;
            }

            if (m3u8StreamInfoParsing)
            {
                // there shouldn't be any empty line
                if (!result)
                {
                    m3u8StreamInfoParsing = false;
                    continue;
                }

                fprintf(stderr, "[%s] - continue parsing m3u8 stream info\n", __func__);
                m3u8StreamInfo.url = resolveUrl(url, lineBuffer);
                m3u8StreamInfo.headers = headers;
                streams.push_back(m3u8StreamInfo);
                m3u8StreamInfoParsing = false;
            }
            else
            {
                if (!strncmp(lineBuffer, M3U8_STREAM_INFO, strlen(M3U8_STREAM_INFO)))
                {
                    m3u8StreamInfoParsing = true;
                    std::string parsed(lineBuffer);
                    parseStreamInfoAttributes(parsed.substr(strlen(M3U8_STREAM_INFO) + 1).c_str(), m3u8StreamInfo);
                }
                else if (!strncmp(lineBuffer, M3U8_MEDIA, strlen(M3U8_MEDIA)))
                {
                    M3U8AudioTrack track;
                    std::string parsed(lineBuffer);
                    if (parseMediaAttributes(parsed.substr(strlen(M3U8_MEDIA) + 1).c_str(), track) == 0)
                        audioTracks.push_back(track);
                }
                else
                {
                    fprintf(stderr, "[%s] - skipping unrecognised data\n", __func__);
                }
            }

            if (contentLength && contentLength <= contentSize)
            {
                fprintf(stderr, "[%s] - end of read, Content-Length reached\n", __func__);
                break;
            }
        }
    }
    if (!streams.empty())
        ret = 0;
    free(lineBuffer);
    ::close(sd);
    if (ssl)
    {
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
    }
    return ret;
}

std::vector<M3U8StreamInfo> M3U8VariantsExplorer::getStreams(bool filterDefaultAudio)
{
    streams.clear();
    audioTracks.clear();
    std::vector<std::string> masterUrl;
    masterUrl.push_back(url);
    for (std::vector<std::string>::const_iterator it(masterUrl.begin()); it != masterUrl.end(); it++)
    {
        int ret = getVariantsFromMasterUrl(*it, headers, 0);
        if (ret < 0)
            continue;
        break;
    }

    // Für jeden Stream den DEFAULT=YES Audio-Track der Gruppe als suburi anhängen (oder den ersten passenden Track falls kein DEFAULT=YES existiert)
    if (filterDefaultAudio && !audioTracks.empty())
    {
        for (std::vector<M3U8StreamInfo>::iterator s(streams.begin()); s != streams.end(); ++s)
        {
            if (s->audioGroupId.empty())
                continue;

            bool found = false;
            // 1. Suche nach DEFAULT=YES
            for (std::vector<M3U8AudioTrack>::const_iterator a(audioTracks.begin()); a != audioTracks.end(); ++a)
            {
                if (a->isDefault && a->groupId == s->audioGroupId)
                {
                    std::string audioUrl = resolveUrl(url, a->uri);
                    s->url += "&suburi=" + audioUrl;
                    M3ULOG("group '%s': default audio suburi=%s",
                            s->audioGroupId.c_str(), audioUrl.c_str());
                    found = true;
                    break;
                }
            }

            // 2. Fallback: Erster Track der Gruppe
            if (!found)
            {
                for (std::vector<M3U8AudioTrack>::const_iterator a(audioTracks.begin()); a != audioTracks.end(); ++a)
                {
                    if (a->groupId == s->audioGroupId)
                    {
                        std::string audioUrl = resolveUrl(url, a->uri);
                        s->url += "&suburi=" + audioUrl;
                        M3ULOG("group '%s': fallback audio suburi=%s",
                                s->audioGroupId.c_str(), audioUrl.c_str());
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    return streams;
}

