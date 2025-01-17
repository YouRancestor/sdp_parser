#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#if defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#elif defined(_WIN32)
#include <Winsock2.h>
#else
#error non supported platform
#endif

#include "util.h"
#include "sdp_parser.h"

#ifndef NOT_IN_USE
#define NOT_IN_USE(a) ((void)(a))
#endif

#define SKIP_WHITESPACES(_ptr_) ({ \
	do { \
		while ((*_ptr_) == ' ') \
			(_ptr_)++; \
	} while (0); \
	*_ptr_; \
})

#define SDPOUT(func_suffix, level) \
	void sdp ## func_suffix(char *fmt, ...) \
	{ \
		va_list va; \
		va_start(va, fmt); \
		sdpout(level, fmt, va); \
		va_end(va); \
	}

#define IS_WHITESPACE_DELIM(_c_) ((_c_) == ' ' || (_c_) == '\t'|| \
	(_c_) == '\r' || (_c_) == '\n')

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

/* returns an SDP line with no trailing whitespaces or line delimiters */
static size_t sdp_getline(char **line, size_t *len, sdp_stream_t sdp)
{
	ssize_t ret;

	ret = sdp_stream_getline(line, len, sdp);
	if (ret <= 0) {
		free(*line);
		*line = NULL;
		*len = 0;
		return 0;
	}

	while (ret && IS_WHITESPACE_DELIM((*line)[ret-1]))
		ret--;
	(*line)[ret] = 0;

	if (!ret) {
		free(*line);
		*line = NULL;
		*len = 0;
		return 0;
	}

	return ret;
}

static char sdp_parse_descriptor_type(char *line)
{
	char descriptor;

	if (strlen(line) < 3) {
		sdperr("'x=<token>' format not found");
		return 0;
	}
	if (line[1] != '=') {
		sdperr("'x=' format not found");
		return 0;
	}

	switch (*line) {
	/* session description */
	case 'v': /* Protocol Version */
	case 'o': /* Origin */
	case 's': /* Session Name */
	case 'i': /* Session Invormation */
	case 'u': /* URI */
	case 'e': /* Email Address */
	case 'p': /* Phone Number */
	case 'c': /* Connection Data */
	case 'b': /* Bandwidth */
	case 't': /* Timing */
	case 'r': /* Repeat Times */
	case 'z': /* Time Zones */
	case 'k': /* Encryption Keys */
	/* media description */
	case 'm': /* Media Descriptions */
	case 'a': /* Attributes */
		descriptor = *line;
		break;
	default:
		descriptor = 0;
		sdperr("unsupported session descriptor: '%c='", *line);
		break;
	}

	return descriptor;
}

static enum sdp_parse_err sdp_parse_non_supported(sdp_stream_t sdp, char **line,
		size_t *len, char *not_supported)
{
	if (!*line)
		return SDP_PARSE_OK;

	do {
		if (!sdp_parse_descriptor_type(*line))
			return SDP_PARSE_ERROR;

		if (!strchr(not_supported, **line))
			return SDP_PARSE_NOT_SUPPORTED;

		sdp_getline(line, len, sdp);
	} while (*line);

	return SDP_PARSE_NOT_SUPPORTED;
}

static enum sdp_parse_err sdp_parse_version(sdp_stream_t sdp, char **line,
		size_t *len, struct sdp_session_v *v)
{
	int version;
	char *ptr;
	char *endptr;

	if (!sdp_getline(line, len, sdp)||
			sdp_parse_descriptor_type(*line) != 'v') {
		sdperr("missing required sdp version");
		return SDP_PARSE_ERROR;
	}

	ptr = *line + 2;
	version = strtol(ptr, &endptr, 10);
	if (*endptr) {
		sdperr("bad version - %s", *line);
		return SDP_PARSE_ERROR;
	}

	v->version = version;

	if (!sdp_getline(line, len, sdp)) {
		sdperr("no more sdp fields after version");
		return SDP_PARSE_ERROR;
	}

	return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_session_name(sdp_stream_t sdp, char **line,
		size_t *len, char **s)
{
	char *ptr;

	if (strncmp(*line, "s=", 2)) {
		sdperr("missing required sdp session name");
		return SDP_PARSE_ERROR;
	}

	ptr = *line + 2;
	if (!*ptr) {
		sdperr("sdp session name cannot remain empty");
		return SDP_PARSE_ERROR;
	}

	*s = strdup(ptr);
	if (!*s) {
		sdperr("memory acllocation");
		return SDP_PARSE_ERROR;
	}

	if (!sdp_getline(line, len, sdp)) {
		sdperr("no more sdp fields after session name");
		free(*s);
		*s = NULL;

		return SDP_PARSE_ERROR;
	}

	return SDP_PARSE_OK;
}

static int is_multicast_addr(enum sdp_ci_addrtype addrtype, char *addr)
{
	switch (addrtype) {
	case SDP_CI_ADDRTYPE_IPV4:
		return ((unsigned long)inet_addr(addr) & htonl(0xf0000000)) ==
			htonl(0xe0000000); /* 224.0.0.0 - 239.255.255.255 */
	case SDP_CI_ADDRTYPE_IPV6:
		/* not supported */
	default:
		break;
	}

	return 0;
}

static enum sdp_parse_err sdp_parse_connection_information(sdp_stream_t sdp,
		char **line, size_t *len, struct sdp_connection_information *c)
{
	char *nettype;
	char *addrtype;
	char *addr;
	char *ptr;
	char *tmp;
	int ttl = 0;
	int is_ttl_set = 0;

	if (strncmp(*line, "c=", 2))
		return SDP_PARSE_OK;

	ptr = *line + 2;
	nettype = strtok_r(ptr, " ", &tmp);
	if (!nettype) {
		sdperr("bad connection information nettype");
		return SDP_PARSE_ERROR;
	}
	addrtype = strtok_r(NULL, " ", &tmp);
	if (!addrtype) {
		sdperr("bad connection information addrtype");
		return SDP_PARSE_ERROR;
	}

	addr = strtok_r(NULL, "/", &tmp);
	if (!addr) {
		addr = tmp;
	} else {
		char *endptr;

		ttl = strtol(tmp, &endptr, 10);
		if (*endptr) {
			sdperr("bad connection information ttl");
			return SDP_PARSE_ERROR;
		}

		if (ttl)
			is_ttl_set = 1;
	}

	if (!strncmp(nettype, "IN", strlen("IN")))
		c->nettype = SDP_CI_NETTYPE_IN;
	else
		c->nettype = SDP_CI_NETTYPE_NOT_SUPPORTED;

	if (!strncmp(addrtype, "IP4", strlen("IP4"))) {
		if (!is_ttl_set && is_multicast_addr(SDP_CI_ADDRTYPE_IPV4,
				addr)) {
			sdperr("connection information with an IP4 multicast "
				"address requires a TTL value");
			return SDP_PARSE_ERROR;
		}

		c->addrtype = SDP_CI_ADDRTYPE_IPV4;
	} else if (!strncmp(nettype, "IP6", strlen("IP6"))) {
		c->addrtype = SDP_CI_ADDRTYPE_IPV6;
	} else {
		c->addrtype = SDP_CI_ADDRTYPE_NOT_SUPPORTED;
	}

	strncpy(c->sdp_ci_addr, addr, sizeof(c->sdp_ci_addr));
	c->sdp_ci_ttl = ttl;
	c->count = 1;

	sdp_getline(line, len, sdp);
	return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_media_video(struct sdp_media_m *m,
		char **tmp)
{
	char *proto;
	int port;
	int num_ports;
	int fmt;
	char *slash;
	char *endptr;
	struct sdp_media_fmt **smf;

	m->type = SDP_MEDIA_TYPE_VIDEO;

	slash = strchr(*tmp, '/');
	port = strtol(strtok_r(NULL, " /", tmp), &endptr, 10);
	if (*endptr) {
		sdperr("bad media descriptor - port");
		return SDP_PARSE_ERROR;
	}

	if (slash + 1 == *tmp) {
		num_ports = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
		if (*endptr) {
			sdperr("bad media descriptor - num_ports");
			return SDP_PARSE_ERROR;
		}
	} else {
		num_ports = 1;
	}

	proto = strtok_r(NULL, " ", tmp);
	fmt = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
	if (*endptr) {
		sdperr("bad media descriptor - fmt");
		return SDP_PARSE_ERROR;
	}

	if (!strncmp(proto, "RTP/AVP", strlen("RTP/AVP"))) {
		m->proto = SDP_MEDIA_PROTO_RTP_AVP;
	} else {
		sdperr("media protocol not supported: %s", proto);
		m->proto = SDP_MEDIA_PROTO_NOT_SUPPORTED;
		return SDP_PARSE_NOT_SUPPORTED;
	}

	m->port = port;
	m->num_ports = num_ports;
	m->fmt.id = fmt;

	smf = &m->fmt.next;
	while (*tmp && **tmp) {
		if (!(*smf = (struct sdp_media_fmt*)calloc(1,
                		sizeof(struct sdp_media_fmt)))) {
			sdperr("memory acllocation");
			return SDP_PARSE_ERROR;
		}

		fmt = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
		if (*endptr) {
			sdperr("bad media descriptor - fmt");
			return SDP_PARSE_ERROR;
		}
		(*smf)->id = fmt;
		smf = &(*smf)->next;
	}

	return SDP_PARSE_OK;
}
static enum sdp_parse_err sdp_parse_media_audio(struct sdp_media_m *m,
    char **tmp)
{
    m->type = SDP_MEDIA_TYPE_AUDIO;
    char* slash = strchr(*tmp, '/');
    char* endptr;
    int port = strtol(strtok_r(NULL, " /", tmp), &endptr, 10);
    if (*endptr) {
        sdperr("bad media descriptor - port");
        return SDP_PARSE_ERROR;
    }
    int num_ports;
    if (slash + 1 == *tmp) {
        num_ports = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
        if (*endptr) {
            sdperr("bad media descriptor - num_ports");
            return SDP_PARSE_ERROR;
        }
    }
    else {
        num_ports = 1;
    }
    char *proto = strtok_r(NULL, " ", tmp);
    int fmt = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
    if (*endptr) {
        sdperr("bad media descriptor - fmt");
        return SDP_PARSE_ERROR;
    }

    if (!strncmp(proto, "RTP/AVP", strlen("RTP/AVP"))) {
        m->proto = SDP_MEDIA_PROTO_RTP_AVP;
    }
    else {
        sdperr("media protocol not supported: %s", proto);
        m->proto = SDP_MEDIA_PROTO_NOT_SUPPORTED;
        return SDP_PARSE_NOT_SUPPORTED;
    }
    m->port = port;
    m->num_ports = num_ports;
    m->fmt.id = fmt;
    struct sdp_media_fmt **smf = &m->fmt.next;
    while (*tmp && **tmp) {
        if (!(*smf = (struct sdp_media_fmt*)calloc(1,
            sizeof(struct sdp_media_fmt)))) {
            sdperr("memory acllocation");
            return SDP_PARSE_ERROR;
        }

        fmt = strtol(strtok_r(NULL, " ", tmp), &endptr, 10);
        if (*endptr) {
            sdperr("bad media descriptor - fmt");
            return SDP_PARSE_ERROR;
        }
        (*smf)->id = fmt;
        smf = &(*smf)->next;
    }
    return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_media_not_supported(struct sdp_media_m *m,
		char *type)
{
	sdpwarn("media type not supported: %s", type);
	m->type = SDP_MEDIA_TYPE_NOT_SUPPORTED;

	return SDP_PARSE_NOT_SUPPORTED;
}

static enum sdp_parse_err sdp_parse_media(sdp_stream_t sdp, char **line,
		size_t *len, struct sdp_media_m *m)
{
	char *type;
	char *ptr;
	char *tmp;
	enum sdp_parse_err err;

	if (strncmp(*line, "m=", 2)) {
		sdperr("bad media descriptor - m=");
		return SDP_PARSE_ERROR;
	}

	ptr = *line + 2;
	type = strtok_r(ptr, " ", &tmp);
	if (!type) {
		sdperr("bad media descriptor");
		return SDP_PARSE_ERROR;
	}

	if (!strncmp(type, "video", strlen("video"))) {
		err = sdp_parse_media_video(m, &tmp);
	} 
    else if(!strncmp(type, "audio", strlen("audio")))
    {
        err = sdp_parse_media_audio(m, &tmp);
    }
    else {
		err = sdp_parse_media_not_supported(m, type);
	}

	sdp_getline(line, len, sdp);
	return err;
}

static enum sdp_parse_err parse_attr_common(struct sdp_attr *a, char *attr,
		char *value, char *params,
		parse_attr_specific_t parse_attr_specific)
{
	NOT_IN_USE(a);
	NOT_IN_USE(attr);
	NOT_IN_USE(value);
	NOT_IN_USE(params);
	NOT_IN_USE(parse_attr_specific);

	return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_attr(sdp_stream_t sdp, char **line,
		size_t *len, struct sdp_media *media, struct sdp_attr **a,
		char **attr_level,
		enum sdp_parse_err (*parse_level)(struct sdp_media *media,
			struct sdp_attr *a, char *attr, char *value,
			char *params,
			parse_attr_specific_t parse_attr_specific),
		parse_attr_specific_t parse_attr_specific)
{
	char **supported_attr;
	char *attr;
	char *value;
	char *params;
	enum sdp_parse_err err;
	char *ptr = *line;
	char *tmp = NULL;
	struct sdp_attr **iter = a;
	unsigned long long attr_mask;

	char *common_level_attr[] = {
#if 0
		"recvonly",
		"sendrecv",
		"sendoly",
		"inactive",
		"sdplang",
		"lang",
#endif
		NULL
	};

	while (*line && sdp_parse_descriptor_type(*line) == 'a') {
		value = NULL;
		params = NULL;
		ptr = *line + 2;

		attr = strtok_r(ptr, ":", &tmp);
		if (*tmp)
			value = strtok_r(NULL, " ", &tmp);
		if (*tmp)
			params = tmp;

		*a = (struct sdp_attr*)calloc(1, sizeof(struct sdp_attr));
		if (!*a) {
			sdperr("memory acllocation");
			return SDP_PARSE_ERROR;
		}

		/* try to find a supported attribute in the session/media
		 * common list */
		for (supported_attr = common_level_attr; *supported_attr &&
			strcmp(*supported_attr, attr); supported_attr++);
		if (*supported_attr) {
			err = parse_attr_common(*a, *supported_attr, value,
				params, parse_attr_specific);
			if (err == SDP_PARSE_ERROR) {
				free(*a);
				*a = NULL;
				sdperr("parsing attribute: %s", attr);
				return SDP_PARSE_ERROR;
			}

			a = &(*a)->next;

			sdp_getline(line, len, sdp);
			continue;
		}

		/* try to find supported attribute in current level list */
		for (supported_attr = attr_level; *supported_attr &&
			strcmp(*supported_attr, attr); supported_attr++);
		if (*supported_attr) {
			err = parse_level(media, *a, *supported_attr,
					value, params, parse_attr_specific);
			if (err == SDP_PARSE_ERROR) {
				free(*a);
				*a = NULL;
				sdperr("parsing attribute: %s", attr);
				return SDP_PARSE_ERROR;
			}

			a = &(*a)->next;

			sdp_getline(line, len, sdp);
			continue;
		}

		/* attribute is not supported */
		free(*a);
		*a = NULL;

		sdp_getline(line, len, sdp);
	}

	/* assert no multiple instances of supported attributes */
	for (attr_mask = 0; *iter; iter = &(*iter)->next) {
		if ((*iter)->type == SDP_ATTR_NONE ||
				(*iter)->type == SDP_ATTR_SPECIFIC ||
				(*iter)->type == SDP_ATTR_NOT_SUPPORTED) {
			continue;
		}

		if (attr_mask & 1 << (*iter)->type) {
			struct code2str attributes[] = {
				{ SDP_ATTR_GROUP, "group" },
				{ SDP_ATTR_RTPMAP, "rtpmap" },
				{ SDP_ATTR_FMTP, "fmtp" },
				{ SDP_ATTR_SOURCE_FILTER, "source-filter" },
				{ SDP_ATTR_MID, "mid" },
				{ -1 }
			};
			char *type = code2str(attributes, (*iter)->type);

			sdperr("multiple instances of attribute: %s", type ?
				type : "N/A");
			return SDP_PARSE_ERROR;
		}

		attr_mask |= 1 << (*iter)->type;
	}

	return SDP_PARSE_OK;
}

static enum sdp_parse_err parse_attr_session(struct sdp_media *media,
		struct sdp_attr *a, char *attr, char *value, char *params,
		parse_attr_specific_t parse_attr_specific)
{
	if (!strncmp(attr, "group", strlen("group"))) {
		/* currently not supporting the general case */
		if (!parse_attr_specific)
			return SDP_PARSE_NOT_SUPPORTED;

		return parse_attr_specific(media, a, attr, value, params);
	} else {
		a->type = SDP_ATTR_NOT_SUPPORTED;
		return SDP_PARSE_NOT_SUPPORTED;
	}

	return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_session_level_attr(sdp_stream_t sdp,
		char **line, size_t *len, struct sdp_attr **a,
		parse_attr_specific_t parse_attr_specific)
{
	static char *session_level_attr[] = {
		"group",
		NULL
	};

	return sdp_parse_attr(sdp, line, len, NULL, a, session_level_attr,
		parse_attr_session, parse_attr_specific);
}

static enum sdp_parse_err sdp_parse_attr_source_filter(
		struct sdp_attr_value_source_filter *source_filter,
		char *value, char *params)
{
	char *nettype;
	char *addrtype;
	char *dst_addr;
	char *src_addr;
	char *tmp;
	struct source_filter_src_addr src_list;
	int src_list_len;

	/* filter-mode */
	if (!strncmp(value, "incl", strlen("incl"))) {
		source_filter->mode = SDP_ATTR_SRC_FLT_INCL;
	} else if (!strncmp(value, "excl", strlen("excl"))) {
		source_filter->mode = SDP_ATTR_SRC_FLT_EXCL;
	} else {
		sdperr("bad source-filter mode type");
		return SDP_PARSE_ERROR;
	}

	/* filter-spec */
	nettype = strtok_r(params, " ", &tmp);
	if (!nettype) {
		sdperr("bad source-filter nettype");
		return SDP_PARSE_ERROR;
	}

	addrtype = strtok_r(NULL, " ", &tmp);
	if (!addrtype) {
		sdperr("bad source-filter addrtype");
		return SDP_PARSE_ERROR;
	}

	dst_addr = strtok_r(NULL, " ", &tmp);
	if (!dst_addr) {
		sdperr("bad source-filter dst-addr");
		return SDP_PARSE_ERROR;
	}

	src_addr = strtok_r(NULL, " ", &tmp);
	if (!src_addr) {
		sdperr("bad source-filter src-addr");
		return SDP_PARSE_ERROR;
	}
	memset(&src_list, 0, sizeof(struct source_filter_src_addr));
	strncpy(src_list.addr, src_addr, sizeof(src_list.addr));
	src_list.next = NULL;
	src_list_len = 1;

	while (*tmp) {
		/* limitation:
		 * rfc4570 defines a list of source addresses.
		 * The current implementation supports only a single source
		 * address */
		sdpwarn("source filter attribute currently supports a "
			"single source address");
		*tmp = 0;
	}

	if (!strncmp(nettype, "IN", strlen("IN")))
		source_filter->spec.nettype = SDP_CI_NETTYPE_IN;
	else
		source_filter->spec.nettype = SDP_CI_NETTYPE_NOT_SUPPORTED;

	if (!strncmp(addrtype, "IP4", strlen("IP4")))
		source_filter->spec.addrtype = SDP_CI_ADDRTYPE_IPV4;
	else if (!strncmp(nettype, "IP6", strlen("IP6")))
		source_filter->spec.addrtype = SDP_CI_ADDRTYPE_IPV6;
	else
		source_filter->spec.addrtype = SDP_CI_ADDRTYPE_NOT_SUPPORTED;

	strncpy(source_filter->spec.dst_addr, dst_addr,
		sizeof(source_filter->spec.dst_addr));

	memcpy(&source_filter->spec.src_list, &src_list,
		sizeof(struct source_filter_src_addr));

	source_filter->spec.src_list_len = src_list_len;
	return SDP_PARSE_OK;
}

static enum sdp_parse_err parse_attr_media(struct sdp_media *media,
		struct sdp_attr *a, char *attr, char *value, char *params,
		parse_attr_specific_t parse_attr_specific)
{
	char *endptr;

	if (!strncmp(attr, "rtpmap", strlen("rtpmap"))) {
		struct sdp_attr_value_rtpmap *rtpmap = &a->value.rtpmap;
		char *media_subtype, *clock_rate;
		char *tmp;

		a->type = SDP_ATTR_RTPMAP;

		media_subtype = strtok_r(params, "/", &tmp);
		if (!media_subtype || !tmp) {
			sdperr("attribute bad format - %s (media_subtype)",
				attr);
			return SDP_PARSE_ERROR;
		}

		clock_rate = strtok_r(NULL, "/", &tmp);
		if (!clock_rate) {
			sdperr("attribute bad format - %s (clock_rate)", attr);
			return SDP_PARSE_ERROR;
		}
        char* channel_count = strtok_r(NULL, "/", &tmp); // for audio
        if (channel_count)
        {
            rtpmap->num_channel = strtol(channel_count, &endptr, 10);
            if (*endptr) {
                sdperr("attribute bad channel count - %s", attr);
                return SDP_PARSE_ERROR;
            }
        }
        else
        {
            rtpmap->num_channel = 1;
        }
		/* encoding parameters are not supported */

		rtpmap->fmt = strtol(value, &endptr, 10);
		if (*endptr) {
			sdperr("attribute bad format - %s", attr);
			return SDP_PARSE_ERROR;
		}

		strncpy(rtpmap->media_subtype, media_subtype,
			sizeof(rtpmap->media_subtype));

		rtpmap->clock_rate = strtol(clock_rate, &endptr, 10);
		if (*endptr) {
			sdperr("attribute bad format - %s", attr);
			return SDP_PARSE_ERROR;
		}
	} else if (!strncmp(attr, "fmtp", strlen("fmtp"))) {
		struct sdp_attr_value_fmtp *fmtp = &a->value.fmtp;
		char *endptr;

		fmtp->fmt = strtol(value, &endptr, 10);
		if (*endptr) {
			sdperr("attribute bad format - %s", attr);
			return SDP_PARSE_ERROR;
		}

		if (*params && (!parse_attr_specific ||
				parse_attr_specific(media, a, attr, value,
				params) == SDP_PARSE_ERROR)) {
			return SDP_PARSE_ERROR;
		}
	} else if (!strncmp(attr, "source-filter", strlen("source-filter"))) {
		struct sdp_attr_value_source_filter *source_filter;

		source_filter = &a->value.source_filter;
		a->type = SDP_ATTR_SOURCE_FILTER;

		if (sdp_parse_attr_source_filter(source_filter, value,
				params)) {
			sdperr("attribute bad format - %s", attr);
			return SDP_PARSE_ERROR;
		}
	} else if (!strncmp(attr, "mid", strlen("mid"))) {
		char *identification_tag;

		a->type = SDP_ATTR_MID;
		identification_tag = value;
		if (!identification_tag) {
			sdperr("attribute bad format - %s", attr);
			return SDP_PARSE_ERROR;
		}

		a->value.mid.identification_tag = strdup(value);
		if (!a->value.mid.identification_tag) {
			sdperr("failed to allocate memory for "
				"identification_tag: %s", value);
			return SDP_PARSE_ERROR;
		}
	} else {
		a->type = SDP_ATTR_NOT_SUPPORTED;
		return SDP_PARSE_NOT_SUPPORTED;
	}

	return SDP_PARSE_OK;
}

static enum sdp_parse_err sdp_parse_media_level_attr(sdp_stream_t sdp,
		char **line, size_t *len, struct sdp_media *media,
		struct sdp_attr **a, parse_attr_specific_t parse_attr_specific)
{
	static char *media_level_attr[] = {
#if 0
		"ptime",
		"maxptime",
		"orient",
		"framerate",
		"quality",
#endif
		"rtpmap",
		"fmtp",
		"source-filter",
		"mid",
		NULL
	};

	return sdp_parse_attr(sdp, line, len, media, a, media_level_attr,
		parse_attr_media, parse_attr_specific);
}

static void media_fmt_free(struct sdp_media_fmt *fmt)
{
	while (fmt) {
		struct sdp_media_fmt *tmp;

		tmp = fmt;
		fmt = fmt->next;
		free(tmp);
	}
}

static void sdp_attr_free(struct sdp_attr *attr)
{
	while (attr) {
		struct sdp_attr *tmp;

		tmp = attr;
		attr = attr->next;

		switch (tmp->type) {
		case SDP_ATTR_GROUP:
		{
			struct group_identification_tag *tag;

			while ((tag = tmp->value.group.tag)) {
				tmp->value.group.tag =
					tmp->value.group.tag->next;

				free(tag->identification_tag);
				free(tag);
			}

			free(tmp->value.group.semantic);
		}
		break;
		case SDP_ATTR_FMTP:
			if (tmp->value.fmtp.param_dtor) {
				tmp->value.fmtp.param_dtor(
					tmp->value.fmtp.params);
			}
			break;
		case SDP_ATTR_SOURCE_FILTER:
			free(tmp->value.source_filter.spec.src_list.next);
			break;
		case SDP_ATTR_MID:
			free(tmp->value.mid.identification_tag);
			break;
		case SDP_ATTR_SPECIFIC:
			free(tmp->value.specific);
			break;
		case SDP_ATTR_RTPMAP:
		case SDP_ATTR_NOT_SUPPORTED:
		default:
			break;
		}

		free(tmp);
	}
}

static void media_free(struct sdp_media *media)
{
	while (media) {
		struct sdp_media *tmp;

		tmp = media;
		media = media->next;

		media_fmt_free(tmp->m.fmt.next);
		sdp_attr_free(tmp->a);

		free(tmp);
	}
}

struct sdp_session *sdp_parser_init(enum sdp_stream_type type, void *ctx)
{
	struct sdp_session *session;

	session = (struct sdp_session*)calloc(1, sizeof(struct sdp_session));
	if (!session)
		return NULL;

	if (!(session->sdp = sdp_stream_open(type, ctx))) {
		free(session);
		return NULL;
	}

	return session;
}

void sdp_parser_uninit(struct sdp_session *session)
{
	sdp_stream_close(session->sdp);
	free(session->s);
	sdp_attr_free(session->a);
	media_free(session->media);
	free(session);
}

enum sdp_parse_err sdp_session_parse(struct sdp_session *session,
		parse_attr_specific_t parse_attr_specific)
{
	enum sdp_parse_err err = SDP_PARSE_ERROR;
	char *line = NULL;
	size_t len = 0;
	sdp_stream_t sdp = session->sdp;

	/* parse v= */
	if (sdp_parse_version(sdp, &line, &len, &session->v) ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	/* skip parsing of non supported session-level descriptors */
	if (sdp_parse_non_supported(sdp, &line, &len, "o") ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	/* parse s= */
	if (sdp_parse_session_name(sdp, &line, &len, &session->s) ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	/* skip parsing of non supported session-level descriptors */
	if (sdp_parse_non_supported(sdp, &line, &len, "iuep") ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	/* nothing except for (t=[v=]) is compulsory from here on */
	if (!line) {
		err = SDP_PARSE_OK;
		goto exit;
	}

	/* parse c=* */
	if (sdp_parse_connection_information(sdp, &line, &len, &session->c) ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	if (!line) {
		err = SDP_PARSE_OK;
		goto exit;
	}

	/* skip parsing of non supported session-level descriptors */
	if (sdp_parse_non_supported(sdp, &line, &len, "btvuezk") ==
			SDP_PARSE_ERROR) {
		goto exit;
	}

	if (!line) {
		err = SDP_PARSE_OK;
		goto exit;
	}

	if (sdp_parse_session_level_attr(sdp, &line, &len, &session->a,
			parse_attr_specific) == SDP_PARSE_ERROR) {
		goto exit;
	}

	if (!line) {
		err = SDP_PARSE_OK;
		goto exit;
	}

	/* parse media-level description */

	do {
		struct sdp_media *media;
		struct sdp_media **next;
		enum sdp_parse_err err;

		if (sdp_parse_descriptor_type(line) != 'm')
			goto exit;

		/* add media to session */
		for (next = &session->media; *next; next = &(*next)->next);
		if (!(*next= (struct sdp_media*)calloc(1,
                		sizeof(struct sdp_media)))) {
			goto exit;
                }

		media = *next;

		/* parse m= */
		err = sdp_parse_media(sdp, &line, &len, &media->m);
		if (err == SDP_PARSE_ERROR)
			goto exit;

		/* skip non suppored m= media blocks */
		if (err == SDP_PARSE_NOT_SUPPORTED) {
			while (line && sdp_parse_descriptor_type(line) != 'm')
				sdp_getline(&line, &len, sdp);
			continue;
		}

		if (!line)
			return SDP_PARSE_OK;

		/* skip parsing of non supported media-level descriptors */
		if (sdp_parse_non_supported(sdp, &line, &len, "i") ==
				SDP_PARSE_ERROR) {
			goto exit;
		}
		if (!line)
			return SDP_PARSE_OK;

		/* parse c=* */
		if (sdp_parse_connection_information(sdp, &line, &len,
				&media->c) == SDP_PARSE_ERROR) {
			goto exit;
		}
		if (!line)
			return SDP_PARSE_OK;

		/* skip parsing of non supported media-level descriptors */
		if (sdp_parse_non_supported(sdp, &line, &len, "bk") ==
				SDP_PARSE_ERROR) {
			goto exit;
		}
		if (!line)
			return SDP_PARSE_OK;

		/* parse media-level a=* */
		if (sdp_parse_media_level_attr(sdp, &line, &len, media,
				&media->a,
				parse_attr_specific) == SDP_PARSE_ERROR) {
			goto exit;
		}
	} while (line);

	err = SDP_PARSE_OK;
	goto exit;

exit:
	free(line);
	return err;
}

static void sdpout(char *level, char *fmt, va_list va)
{
	fprintf(stderr, "SDP parse %s - ", level);
	vfprintf(stderr, fmt, va);
	fprintf(stderr, "\n");
	fflush(stderr);
}

SDPOUT(warn, "warning")
SDPOUT(err, "error")

static struct sdp_media *sdp_media_locate(struct sdp_media *media,
		enum sdp_media_type type)
{
	if (type == SDP_MEDIA_TYPE_NONE)
		return media;

	for ( ; media && media->m.type != type; media = media->next);
	return media;
}

struct sdp_media *sdp_media_get(struct sdp_session *session,
		enum sdp_media_type type)
{
	return sdp_media_locate(session->media, type);
}

struct sdp_media *sdp_media_get_next(struct sdp_media *media)
{
	return sdp_media_locate(media->next, media->m.type);
}

static struct sdp_attr *sdp_attr_locate(struct sdp_attr *attr,
		enum sdp_attr_type type)
{
	if (attr && attr->type == SDP_ATTR_NONE)
		return attr;

	for ( ; attr && attr->type != type; attr = attr->next);
	return attr;
}

struct sdp_attr *sdp_media_attr_get(struct sdp_media *media,
		enum sdp_attr_type type)
{
	return sdp_attr_locate(media->a, type);
}

struct sdp_attr *sdp_session_attr_get(struct sdp_session *session,
		enum sdp_attr_type type)
{
	return sdp_attr_locate(session->a, type);
}

struct sdp_attr *sdp_attr_get_next(struct sdp_attr *attr)
{
	return sdp_attr_locate(attr->next, attr->type);
}

