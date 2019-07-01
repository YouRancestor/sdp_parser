

#include "../sdp_parser.h"
#include "../third_party/base64/base64.h"
#include <assert.h>

#ifndef NULL
#define NULL 0
#endif // !NULL

#include "h264_aac_specific.c"

int main(int argc, char* argv[])
{
    char *sdp =
        "v=0\n"
        "o=- 0 0 IN IP4 127.0.0.1\n"
        "s=No Name\n"
        "c=IN IP4 2176187384832\n"
        "t=0 0\n"
        "a=tool:libavformat 58.17.101\n"
        "m=video 0 RTP/AVP 96\n"
        "b=AS:1024\n"
        "a=rtpmap:96 H264/90000\n"
        "a=fmtp:96 packetization-mode=1; sprop-parameter-sets=Z2QIFazZQeCP6hAAAAMAEAAAAwMg8WLZYA==,aOvssiw=; profile-level-id=640815\n"
        "a=control:streamid=0\n"
        "m=audio 0 RTP/AVP 97\n"
        "b=AS:128\n"
        "a=rtpmap:97 MPEG4-GENERIC/48000/2\n"
        "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3; config=119056E500\n"
        "a=control:streamid=1\n";
    struct sdp_session *session = sdp_parser_init(SDP_STREAM_TYPE_CHAR, sdp);
    enum sdp_parse_err err;
    err = sdp_session_parse(session, sdp_parse_specific);
    sdp_parser_uninit(session);
    return 0;
}