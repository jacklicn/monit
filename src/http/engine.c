/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "monit.h"
#include "engine.h"
#include "net.h"
#include "processor.h"
#include "cervlet.h"
#include "socket.h"
#include "SslServer.h"

// libmonit
#include "system/Net.h"
#include "exceptions/AssertException.h"
#include "exceptions/IOException.h"


/**
 *  A naive http 1.0 server. The server delegates handling of a HTTP
 *  request and response to the processor module.
 *
 *  NOTE
 *    This server does not use threads or forks; Requests are
 *    serialized and pending requests will be popped from the
 *    connection queue when the current request finish.
 *
 *    Since this server is written for monit, low traffic is expected.
 *    Connect from not-authenticated clients will be closed down
 *    promptly. The authentication schema or access control is based
 *    on client name/address/pam and only requests from known clients are
 *    accepted. Hosts allowed to connect to this server should be
 *    added to the access control list by calling Engine_addHostAllow().
 *
 *  @file
 */


/* ------------------------------------------------------------- Definitions */


typedef struct HostsAllow_T {
        uint32_t address[4]; // IPv4 or IPv6 address
        uint32_t mask[4];    // mask
        /* For internal use */
        struct HostsAllow_T *next;
} *HostsAllow_T;


#define MAX_SERVER_SOCKETS 3


static struct {
        Socket_Family family;
        union {
                struct sockaddr_storage addr_in;
                struct sockaddr_un addr_un;
        } _addr;
        struct sockaddr *addr;
        socklen_t addrlen;
#ifdef HAVE_OPENSSL
        SslServer_T ssl;
#endif
} data[MAX_SERVER_SOCKETS] = {};


static volatile boolean_t stopped = false;
static int myServerSocketsCount = 0;
static struct pollfd myServerSockets[3] = {};
static HostsAllow_T hostlist = NULL;
static Mutex_T mutex = PTHREAD_MUTEX_INITIALIZER;


/* ----------------------------------------------------------------- Private */


static void _mapIPv4toIPv6(uint32_t *address4, uint32_t *address6) {
        // Map IPv4 address to IPv6 "::ffff:x.x.x.x" notation, so we can compare IPv4 address in IPv6 namespace
        *(address6 + 0) = 0x00000000;
        *(address6 + 1) = 0x00000000;
        *(address6 + 2) = htonl(0x0000ffff);
        *(address6 + 3) = *address4;
}


static boolean_t _parseNetwork(char *pattern, HostsAllow_T net) {
        ASSERT(pattern);
        ASSERT(net);

        char *longmask = NULL;
        int shortmask = 0;
        int slashcount = 0;
        int dotcount = 0;
        int columncount = 0;
        int count = 0;

        char buf[STRLEN];
        strncpy(buf, pattern, sizeof(buf));
        char *temp = buf;
        Socket_Family family = Socket_Ip4;
        // check if we have IPv4/IPv6 CIDR notation (x.x.x.x/yyy or x::/y) or IPv4 dot-decimal (x.x.x.x/y.y.y.y)
        while (*temp) {
                if (*temp == '/') {
                        if (slashcount > 0 || dotcount != 3 || columncount < 2)
                                return false; // The "/" was found already or its prefix doesn't look like valid address
                        *temp = 0;
                        longmask = *(temp + 1) ? temp + 1 : NULL;
                        slashcount++;
                        dotcount = columncount = count = 0;
                } else if (*temp == '.') {
                        if (family == Socket_Ip6 && slashcount > 0)
                                return false; // No "." allowed past "/" for IPv6 address
                        dotcount++;
                } else if (*temp == ':') {
                        if (slashcount > 0)
                                return false; // ":" not allowed past "/"
                        columncount++;
                        family = Socket_Ip6;
                } else {
                        if (slashcount == 0) {
                                // [0-9a-fA-F] allowed before "/"
                                if (! isxdigit((int)*temp))
                                        return false;
                        } else {
                                // only [0-9] allowed past "/"
                                if (! isdigit((int)*temp))
                                        return false;
                        }
                }
                count++;
                temp++;
        }
        if (slashcount == 0) {
                // Host part only
                shortmask = 128;
        } else if (dotcount == 0 && count > 0 && count < 4) {
                // Mask in CIDR notation
                if (longmask) {
                        shortmask = atoi(longmask);
                        longmask = NULL;
                }
        } else if (dotcount != 3) {
                // The IPv4 dot-decimal mask requires three dots
                return false;
        }
        if (family == Socket_Ip4) {
                struct sockaddr_in addr;
                if (inet_pton(AF_INET, buf, &(addr.sin_addr)) != 1)
                        return false;
                _mapIPv4toIPv6((uint32_t *)&(addr.sin_addr), (uint32_t *)&net->address);
        } else {
#ifdef HAVE_IPV6
                struct sockaddr_in6 addr;
                if (inet_pton(AF_INET6, buf, &(addr.sin6_addr)) != 1)
                        return false;
                memcpy(&net->address, &(addr.sin6_addr), 16);
#else
                THROW(AssertException, "IP-version 6 not supported on this system");
#endif
        }
        if (longmask == NULL) {
                // Convert CIDR notation to integer mask
                if (shortmask < 0)
                        return false;
                if (family == Socket_Ip4) {
                        if (shortmask > 32) {
                                return false;
                        } else if (shortmask == 32) {
                                uint32_t mask = 0xffffffff;
                                _mapIPv4toIPv6(&mask, net->mask);
                        } else {
                                uint32_t mask = htonl(0xffffffff << (32 - shortmask));
                                _mapIPv4toIPv6(&mask, net->mask);
                        }
                } else {
                        if (shortmask > 128) {
                                return false;
                        } else if (shortmask == 128) {
                                memset(net->mask, 0xff, 16);
                        } else {
                                for (int i = 0; i < 4; i++) {
                                        if (shortmask > 32) {
                                                net->mask[i] = 0xffffffff;
                                                shortmask -= 32;
                                        } else if (shortmask > 0) {
                                                net->mask[i] = htonl(0xffffffff << (32 - shortmask));
                                        }
                                }
                        }
                }
        } else {
                // Parse IPv4 dot-decimal mask
                struct sockaddr_in addr;
                if (! inet_aton(longmask, &(addr.sin_addr)))
                        return false;
                _mapIPv4toIPv6((uint32_t *)&(addr.sin_addr), net->mask);
        }
        return true;
}


static void _destroyAllow(HostsAllow_T p) {
        HostsAllow_T a = p;
        if (a->next)
                _destroyAllow(a->next);
        FREE(a);
}


static boolean_t _hasAllow(HostsAllow_T host) {
        for (HostsAllow_T p = hostlist; p; p = p->next)
                if (memcmp(p->address, &(host->address), 16) == 0 && memcmp(p->mask, &(host->mask), 16) == 0)
                        return true;
        return false;
}


static boolean_t _appendAllow(HostsAllow_T h, const char *pattern) {
        boolean_t rv = false;
        LOCK(mutex)
        {
                if (_hasAllow(h))  {
                        LogWarning("Skipping redundant 'allow %s' resolved to [%s]\n", pattern, inet_ntop(AF_INET6, &(h->address), (char[INET6_ADDRSTRLEN]){}, INET6_ADDRSTRLEN));
                } else {
                        DEBUG("Adding 'allow %s' resolved to [%s]\n", pattern, inet_ntop(AF_INET6, &(h->address), (char[INET6_ADDRSTRLEN]){}, INET6_ADDRSTRLEN));
                        h->next = hostlist;
                        hostlist = h;
                        rv = true;
                }
        }
        END_LOCK;
        return rv;
}


static boolean_t _matchAllow(uint32_t address1[4], uint32_t address2[4], uint32_t mask[4]) {
        for (int i = 0; i < 4; i++)
                if ((address1[i] & mask[i]) != (address2[i] & mask[i]))
                        return false;
        return true;
}


static boolean_t _checkAllow(uint32_t address[4]) {
        boolean_t allow = false;
        LOCK(mutex)
        {
                if (! hostlist) {
                        allow = true;
                } else  {
                        for (HostsAllow_T p = hostlist; p && allow == false; p = p->next)
                                allow = _matchAllow(p->address, address, p->mask);
                }
        }
        END_LOCK;
        return allow;
}


static boolean_t _authenticateHost(struct sockaddr *addr) {
        if (addr->sa_family == AF_INET) {
                boolean_t allow = false;
                struct sockaddr_in *a = (struct sockaddr_in *)addr;
                uint32_t address[4];
                _mapIPv4toIPv6((uint32_t *)&(a->sin_addr), (uint32_t *)&address);
                if (! (allow = _checkAllow(address)))
                        LogError("Denied connection from non-authorized client [%s]\n", inet_ntop(addr->sa_family, &a->sin_addr, (char[INET_ADDRSTRLEN]){}, INET_ADDRSTRLEN));
                return allow;
        }
#ifdef HAVE_IPV6
        else if (addr->sa_family == AF_INET6) {
                boolean_t allow = false;
                struct sockaddr_in6 *a = (struct sockaddr_in6 *)addr;
                if (! (allow = _checkAllow((uint32_t *)&(a->sin6_addr))))
                        LogError("Denied connection from non-authorized client [%s]\n", inet_ntop(addr->sa_family, &(a->sin6_addr), (char[INET6_ADDRSTRLEN]){}, INET6_ADDRSTRLEN));
                return allow;
        }
#endif
        else if (addr->sa_family == AF_UNIX) {
                return true;
        }
        return false;
}


static Socket_T _socketProducer(Httpd_Flags flags) {
        int r = 0;
        do {
                r = poll(myServerSockets, myServerSocketsCount, 1000);
        } while (r == -1 && errno == EINTR);
        if (r > 0) {
                for (int i = 0; i < myServerSocketsCount; i++) {
                        if (myServerSockets[i].revents & POLLIN) {
                                int client = accept(myServerSockets[i].fd, data[i].addr, &(data[i].addrlen));
                                if (client < 0) {
                                        LogError("HTTP server: cannot accept connection -- %s\n", stopped ? "service stopped" : STRERROR);
                                        return NULL;
                                }
                                if (Net_setNonBlocking(client) < 0 || ! Net_canRead(client, 500) || ! Net_canWrite(client, 500) || ! _authenticateHost(data[i].addr)) {
                                        Net_abort(client);
                                        return NULL;
                                }
#ifdef HAVE_OPENSSL
                                return Socket_createAccepted(client, data[i].addr, data[i].addrlen, data[i].ssl);
#else
                                return Socket_createAccepted(client, data[i].addr, data[i].addrlen, NULL);
#endif
                        }
                }
        }
        return NULL;
}


static void _createTcpServer(Socket_Family family, char error[STRLEN]) {
        TRY
        {
                myServerSockets[myServerSocketsCount].fd = create_server_socket_tcp(Run.httpd.socket.net.address, Run.httpd.socket.net.port, family, 1024);
#ifdef HAVE_OPENSSL
                if (Run.httpd.flags & Httpd_Ssl) {
                        if (! (data[myServerSocketsCount].ssl = SslServer_new(Run.httpd.socket.net.ssl.pem, Run.httpd.socket.net.ssl.clientpem, myServerSockets[myServerSocketsCount].fd))) {
                                Net_close(myServerSockets[myServerSocketsCount].fd);
                                THROW(IOException, "HTTP server: could not initialize SSL engine");
                        }
                }
#endif
                data[myServerSocketsCount].family = family;
                data[myServerSocketsCount].addr = (struct sockaddr *)&(data[myServerSocketsCount]._addr.addr_in);
                data[myServerSocketsCount].addrlen = sizeof(struct sockaddr_storage);
                myServerSockets[myServerSocketsCount].events = POLLIN;
                myServerSocketsCount++;
        }
        ELSE
        {
                snprintf(error, STRLEN, "%s", Exception_frame.message);
        }
        END_TRY;
}


static void _createUnixServer() {
        TRY
        {
                myServerSockets[myServerSocketsCount].fd = create_server_socket_unix(Run.httpd.socket.unix.path, 1024);
                data[myServerSocketsCount].family = Socket_Unix;
                data[myServerSocketsCount].addr = (struct sockaddr *)&(data[myServerSocketsCount]._addr.addr_un);
                data[myServerSocketsCount].addrlen = sizeof(struct sockaddr_un);
                myServerSockets[myServerSocketsCount].events = POLLIN;
                myServerSocketsCount++;
        }
        ELSE
        {
                LogError("HTTP server: not available -- could not create a unix socket at %s -- %s\n", Run.httpd.socket.unix.path, STRERROR);
        }
        END_TRY;
}


/* ------------------------------------------------------------------ Public */


void Engine_start() {
        Engine_cleanup();
        stopped = Run.flags & Run_Stopped;
        init_service();
        if (Run.httpd.flags & Httpd_Net) {
                char error[STRLEN];
                // Try to create IPv4 and IPv6 sockets
                _createTcpServer(Socket_Ip4, error);
                _createTcpServer(Socket_Ip6, error);
                // Log error only if no IPv4 nor IPv6 socket was created
                if (myServerSocketsCount == 0)
                        LogError("HTTP server: could not create a server socket at TCP port %d -- %s\n", Run.httpd.socket.net.port, STRERROR);
        }
        if (Run.httpd.flags & Httpd_Unix) {
                _createUnixServer();
        }
        while (! stopped) {
                Socket_T S = _socketProducer(Run.httpd.flags);
                if (S)
                        http_processor(S);
        }
        for (int i = 0; i < myServerSocketsCount; i++) {
#ifdef HAVE_OPENSSL
                if (data[i].ssl)
                        SslServer_free(&(data[i].ssl));
#endif
                Net_close(myServerSockets[i].fd);
        }
        Engine_cleanup();
}


void Engine_stop() {
        stopped = true;
}


void Engine_cleanup() {
        myServerSocketsCount = 0;
        if (Run.httpd.flags & Httpd_Unix)
                unlink(Run.httpd.socket.unix.path);
}


//FIXME: don't store the translated hostname->IPaddress on Monit startup to support DHCP hosts ... resolve the hostname in _authenticateHost()
boolean_t Engine_addHostAllow(char *pattern) {
        ASSERT(pattern);
        struct addrinfo *res, hints = {
                .ai_protocol = IPPROTO_TCP
        };
        int added = 0;
        if (! getaddrinfo(pattern, NULL, &hints, &res)) {
                for (struct addrinfo *_res = res; _res; _res = _res->ai_next) {
                        HostsAllow_T h = NULL;
                        if (_res->ai_family == AF_INET) {
                                NEW(h);
                                struct sockaddr_in *sin = (struct sockaddr_in *)_res->ai_addr;
                                _mapIPv4toIPv6((uint32_t *)&(sin->sin_addr), h->address);
                        }
#ifdef HAVE_IPV6
                        else if (_res->ai_family == AF_INET6) {
                                NEW(h);
                                struct sockaddr_in6 *sin = (struct sockaddr_in6 *)_res->ai_addr;
                                memcpy(&h->address, &(sin->sin6_addr), 16);
                        }
#endif
                        if (h) {
                                memset(h->mask, 0xff, 16); // compare all 128 bits
                                if (_appendAllow(h, pattern))
                                        added++;
                        }
                }
                freeaddrinfo(res);
        }
        return added ? true : false;
}


boolean_t Engine_addNetAllow(char *pattern) {
        ASSERT(pattern);

        HostsAllow_T h;
        NEW(h);
        if (_parseNetwork(pattern, h) && _appendAllow(h, pattern))
                return true;
        FREE(h);
        return false;
}


boolean_t Engine_hasHostsAllow() {
        int rv;
        LOCK(mutex)
        {
                rv = hostlist ? true : false;
        }
        END_LOCK;
        return rv;
}


void Engine_destroyHostsAllow() {
        if (Engine_hasHostsAllow()) {
                LOCK(mutex)
                {
                        _destroyAllow(hostlist);
                        hostlist = NULL;
                }
                END_LOCK;
        }
}

