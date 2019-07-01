#include <stdio.h>
#include "smpte2110_sdp_parser.h"

int main (int argc, char **argv)
{
	enum sdp_parse_err err;
	struct sdp_session *session;
	char *err2str[] = {
		"SDP_PARSE_OK",
		"SDP_PARSE_NOT_SUPPORTED",
		"SDP_PARSE_ERROR"
	};
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
		
	session = sdp_parser_init(SDP_STREAM_TYPE_CHAR, sdp);
	if (!session) {
		printf("failed to initialize sdp session\n");
		return -1;
	}

	err = sdp_session_parse(session, smpte2110_sdp_parse_specific);
	printf("parsing result: %s\n", err2str[err]);

	sdp_parser_uninit(session);

	return 0;
}

