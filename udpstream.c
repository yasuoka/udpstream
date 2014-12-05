/*
 * Copyright (c) 2014 YASUOKA Masahiko <yasuoka@yasuoka.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

enum MODE {
	SENDER,
	RECEIVER
};

struct stream_packet {
	struct hdr {
		uint32_t	 magic;		/* 0xdeadbeaf */
		uint16_t	 length;
		uint16_t	 __reserved;
	} __packed hdr;
	u_char			 data[0];
} __packed;

struct udp2stream {
	enum MODE	 mode;
	int		 connected;
	int		 sock;
	int		 streamin;
	int		 streamout;
	struct event	 ev_sock;
	struct event	 ev_stream;
};

struct event	 ev_sigint, ev_sigterm;
static void	 udp2stream_start (enum MODE, int, int, int);
static void	 udp_on_io (int, short, void *);
static void	 stream_on_io (int, short, void *);
static void	 on_sigint (int, short, void *);
static void	 on_sigterm (int, short, void *);

static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-sr] [host] port\n", __progname);
}

int
main(int argc, char *argv[])
{
	enum MODE		 mode = SENDER;
	int			 ch, errval, saved_errno, s = 0;
	const char		*host = NULL, *port = NULL, *cause;
	struct addrinfo		 ai_hint, *ai_res, *res;

	memset(&ai_hint, 0, sizeof(ai_hint));
	ai_hint.ai_family = AF_UNSPEC;
	while ((ch = getopt(argc, argv, "46sr")) != -1)
		switch (ch) {
		case '4':
			ai_hint.ai_family = AF_INET;
			break;
		case '6':
			ai_hint.ai_family = AF_INET6;
			break;
		case 'r':
			mode = RECEIVER;
			break;
		case 's':
			mode = SENDER;
			break;
		}
	argc -= optind;
	argv += optind;

	ai_hint.ai_socktype = SOCK_DGRAM;

	if (argc == 0) {
		usage();
		exit(EX_USAGE);
	}

	if (argc > 1) {
		host = argv[0];
		argc--; argv++;
	}
	port = argv[0];

	if (mode == RECEIVER)
		ai_hint.ai_flags |= AI_PASSIVE;

	if ((errval = getaddrinfo(host, port, &ai_hint, &ai_res)) == -1)
		errx(1, "getaddrinfo(): %s", gai_strerror(errval));

	event_init();

	cause = NULL;
	for (res = ai_res; res; res = res->ai_next) {
		s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == -1) {
			cause = "socket";
			continue;
		}
		switch (mode) {
		case SENDER:
			if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
				cause = "connect";
				goto on_error;
			}
			udp2stream_start(mode, s, STDIN_FILENO, STDOUT_FILENO);
			break;
		case RECEIVER:
			if (bind(s, res->ai_addr, res->ai_addrlen) == -1) {
				cause = "bind";
				goto on_error;
			}
			udp2stream_start(mode, s, STDIN_FILENO, STDOUT_FILENO);
			break;
		}
		break;
on_error:
		saved_errno = errno;
		close(s);
		errno = saved_errno;
		s = -1;
		continue;
	}
	if (ai_res != NULL)
		freeaddrinfo(ai_res);
	if (s < 0)
		err(1, "%s", cause);

	signal_set(&ev_sigint, SIGINT, on_sigint, NULL);
	signal_set(&ev_sigterm, SIGTERM, on_sigterm, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);


	event_dispatch();


	exit(EXIT_SUCCESS);
}

static void
udp2stream_start(enum MODE mode, int sock, int streamin, int streamout)
{
	struct udp2stream	*self;

	if ((self = calloc(1, sizeof(struct udp2stream))) == NULL)
		err(1, "calloc");
	self->mode = mode;
	if (mode == SENDER)
		self->connected = 1;
	self->sock = sock;
	self->streamin = streamin;
	self->streamout = streamout;
	event_set(&self->ev_sock, sock, EV_READ | EV_PERSIST, udp_on_io, self);
	event_add(&self->ev_sock, NULL);
	event_set(&self->ev_stream, streamin, EV_READ | EV_PERSIST, stream_on_io,
	    self);
	event_add(&self->ev_stream, NULL);
}

static void
udp2stream_stop(struct udp2stream *self)
{
	if (self->sock >= 0) {
		event_del(&self->ev_sock);
		close(self->sock);
		self->sock = -1;
	}
	if (self->streamin >= 0) {
		event_del(&self->ev_stream);
		self->streamin = -1;
	}
	signal_del(&ev_sigint);
	signal_del(&ev_sigterm);
	free(self);
}

static void
udp_on_io(int fd, short evmask, void *ctx)
{
	size_t				 size;
	struct udp2stream		*self;
	struct sockaddr_storage		 ss;
	static struct {
		struct stream_packet	 packet;
		char			 space[65535];
	} packet;
	socklen_t			 slen = sizeof(ss);

	self = ctx;
	if (evmask & EV_READ) {
		slen = (socklen_t)sizeof(ss);
		if ((size = recvfrom(self->sock, &packet.packet.data, 65535, 0,
		    (struct sockaddr *)&ss, (socklen_t *)&slen)) == -1) {
			warn("recv");
			goto on_error;
		}
		if (!self->connected) {
			/*
			 * connect the first person only
			 */
			if (connect(self->sock, (struct sockaddr *)&ss,
			    ss.ss_len) == -1) {
				warn("connect");
				goto on_error;
			}
			self->connected = 1;
		}
		packet.packet.hdr.magic = htonl(0xdeadbeaf);
		packet.packet.hdr.length = htons((uint16_t)size);
		size = write(self->streamout, &packet.packet,
		    offsetof(struct stream_packet, data[size]));
		if (size == -1) {
			warn("write");
			goto on_error;
		}
	}
	return;
on_error:
	udp2stream_stop(self);
	return;
}

static void
stream_on_io(int fd, short evmask, void *ctx)
{
	size_t				 size;
	struct udp2stream		*self;
	static struct {
		struct stream_packet	 packet;
		char			 space[65535];
	} packet;

	self = ctx;
	if (evmask & EV_READ) {
		if ((size = read(self->streamin, &packet.packet,
		    sizeof(packet.packet.hdr))) == -1) {
			warn("read");
			goto on_error;
		}
		if (size == 0)
			goto on_error;
		if (size != sizeof(packet.packet.hdr)) {
			warnx("received partially");
			goto on_error;
		}
		NTOHL(packet.packet.hdr.magic);
		NTOHS(packet.packet.hdr.length);
		if (packet.packet.hdr.magic != 0xdeadbeaf) {
			warnx("magic check fail");
			goto on_error;
		}

		if ((size = read(self->streamin, packet.packet.data,
		    (size_t)packet.packet.hdr.length)) == -1) {
			warn("read");
			goto on_error;
		}

		if ((size = send(self->sock, packet.packet.data,
		    packet.packet.hdr.length, 0)) == -1 && errno != EAGAIN) {
				warn("%p: send", self);
			warn("send");
			goto on_error;
		}
	}
	return;
on_error:
	udp2stream_stop(self);
	return;
}

static void
on_sigint(int fd, short evmask, void *ctx)
{
	event_loopexit(0);
}

static void
on_sigterm(int fd, short evmask, void *ctx)
{
	event_loopexit(0);
}
