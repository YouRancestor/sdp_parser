#include "../sdp_parser.h"
#include "../third_party/base64/base64.h"
#include <assert.h>

#define MEDIA_SUBTYPE_H264  "H264"
#define MEDIA_SUBTYPE_AAC   "MPEG4-GENERIC"

typedef struct CodecParH264
{
    unsigned int sps_pps_len;
    unsigned char sps_pps[200];
}CodecParH264;

typedef struct CodecParAAC
{
    unsigned int config_len;
    unsigned char config[40];
}CodecParAAC;

enum sdp_parse_err sdp_parse_sub_h264(struct sdp_attr *a, char *attr, char *value, char *params)
{
    if (!strncmp(attr, "fmtp", strlen("fmtp")))
    {
        a->type = SDP_ATTR_FMTP;
        CodecParH264 *codec_par = (CodecParH264*)malloc(sizeof(CodecParH264));
        memset(codec_par, 0, sizeof(CodecParH264));
        a->value.fmtp.params = codec_par;
        a->value.fmtp.param_dtor = free;
        char *token = strtok(params, ";");
        while (token)
        {
            char* sps = strstr(token, "sprop-parameter-sets=");
            if (sps)
            {
                sps += strlen("sprop-parameter-sets=");
                char* comma = strstr(sps, ",");
                char* pps = comma + 1;
                while (IS_WHITESPACE(*pps))
                    pps++;

                unsigned char *tmp = codec_par->sps_pps;

                *tmp++ = 0x0; *tmp++ = 0x0; *tmp++ = 0x0; *tmp++ = 0x1; // sps nal head 00 00 00 01

                unsigned int len = base64_decode(sps, comma - sps, tmp);// decode sps
                assert(len);
                if (!len)
                {
                    return SDP_PARSE_ERROR;
                }

                codec_par->sps_pps_len = len + 4;
                tmp += len;

                *tmp++ = 0x0; *tmp++ = 0x0; *tmp++ = 0x0; *tmp++ = 0x1; // pps nal head 00 00 00 01

                len = base64_decode(pps, strlen(pps), tmp);
                assert(len);
                if (!len)
                {
                    return SDP_PARSE_ERROR;
                }

                codec_par->sps_pps_len += len + 4;

                a->type = SDP_ATTR_FMTP;
            }
            token = strtok(NULL, ";");
        }
        return SDP_PARSE_OK;
    }
    else
    {
        return SDP_PARSE_NOT_SUPPORTED;
    }
}
static void HexStrToByte(const char* source, unsigned char* dest, unsigned int sourceLen)
{
    short i;
    unsigned char highByte, lowByte;

    for (i = 0; i < sourceLen; i += 2)
    {
        highByte = toupper(source[i]);
        lowByte = toupper(source[i + 1]);

        if (highByte > 0x39)
            highByte -= 0x37;
        else
            highByte -= 0x30;

        if (lowByte > 0x39)
            lowByte -= 0x37;
        else
            lowByte -= 0x30;

        dest[i / 2] = (highByte << 4) | lowByte;
    }
    return;
}
enum sdp_parse_err sdp_parse_sub_aac(struct sdp_attr *a, char *attr, char *value, char *params)
{
    if (!strncmp(attr, "fmtp", strlen("fmtp")))
    {
        a->type = SDP_ATTR_FMTP;
        CodecParAAC *codec_par = (CodecParAAC*)malloc(sizeof(CodecParAAC));
        memset(codec_par, 0, sizeof(CodecParAAC));
        a->value.fmtp.params = codec_par;
        a->value.fmtp.param_dtor = free;

        char *token = strtok(params, ";");
        while (token)
        {
            char* config = strstr(token, "config=");
            if (config)
            {
                config += strlen("config=");
                unsigned int len = strlen(config);
                codec_par->config_len = len / 2;
                HexStrToByte(config, codec_par->config, len);
            }
            token = strtok(NULL, ";");
        }
        return SDP_PARSE_OK;
    }
    else
    {
        return SDP_PARSE_NOT_SUPPORTED;
    }
}
enum sdp_parse_err sdp_parse_specific(struct sdp_media *media,
    struct sdp_attr *a, char *attr, char *value, char *params)
{
    struct sdp_attr *it = media->a;

    if (!strcmp(attr, "control"))
    {
        a->type = SDP_ATTR_SPECIFIC;
        strcpy_s(a->value.specific.name, sizeof(a->value.specific.name), attr);
        a->value.specific.params = strdup(value);
        a->value.specific.param_dtor = free;
        return SDP_PARSE_OK;
    }

    for (it; it != NULL; it = it->next)
    {
        if (it->type == SDP_ATTR_RTPMAP)
        {
            if (!strncmp(it->value.rtpmap.media_subtype, MEDIA_SUBTYPE_H264, strlen(MEDIA_SUBTYPE_H264)))
            {
                return sdp_parse_sub_h264(a, attr, value, params);
            }
            else if (!strncmp(it->value.rtpmap.media_subtype, MEDIA_SUBTYPE_AAC, strlen(MEDIA_SUBTYPE_AAC)))
            {
                return sdp_parse_sub_aac(a, attr, value, params);
            }
            else
            {
                return SDP_PARSE_NOT_SUPPORTED;
            }
        }
    }

    return SDP_PARSE_OK;
}