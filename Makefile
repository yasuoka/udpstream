PROG=		udpstream
NOMAN=		#
LDADD=		-levent
DPADD=		${LIBEVENT}
CFLAGS=		-Wall

.include <bsd.prog.mk>
