/* dsr - Digitale Satelliten Radio (DSR) encoder                         */
/*=======================================================================*/
/* Copyright 2021 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */
/*                                                                       */
/* This code has been modified by janosch79.                             */
/* This code is provided "as is" without warranty of any kind, express  */
/* or implied. The user assumes full responsibility for any use of      */
/* this code.                                                            */

// udpsink.c - UDP network sink for the dsr - Digitale Satelliten Radio (DSR) encoder

#include "rf.h"
#include <fcntl.h>

static inline void add_ns(struct timespec *t, uint64_t ns){
    t->tv_nsec += (long)(ns % 1000000000ULL);
    t->tv_sec  += (time_t)(ns / 1000000000ULL);
    if(t->tv_nsec >= 1000000000L){ t->tv_nsec -= 1000000000L; t->tv_sec += 1; }
}

//Open UDP Socket
int rf_udp_open(void **out_private, const char *host, const char *port, size_t payload_bytes)
{
    if (!out_private || !host || !port) return -1;

    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(err));
        return -1;
    }

    int sock = -1;
    struct sockaddr_storage target_addr;
    socklen_t target_addrlen = 0;
    
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        
        /* Speichere Zieladresse für sendto() */
        memcpy(&target_addr, rp->ai_addr, rp->ai_addrlen);
        target_addrlen = rp->ai_addrlen;
        break;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        fprintf(stderr, "UDP socket(%s:%s) fehlgeschlagen\n", host, port);
        return -1;
    }

    /* Broadcast aktivieren für mehrere Clients */
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        fprintf(stderr, "Warnung: SO_BROADCAST konnte nicht aktiviert werden: %s\n", strerror(errno));
    }

    /* sendbuffer defination - größer für WSL Performance */
    int sndbuf = 8<<20; /* 8 MiB - größerer Buffer für WSL */
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    /* Socket auf non-blocking setzen um Buffer-Überlauf zu vermeiden */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    rf_udp_t *u = (rf_udp_t*)calloc(1, sizeof(*u));
    if (!u) { close(sock); return -1; }

    u->sock         = sock;
    u->payload      = (payload_bytes && payload_bytes < 9000) ? payload_bytes : 1400;
    u->preview_done = 0;
    
    /* Zieladresse speichern */
    memcpy(&u->addr, &target_addr, target_addrlen);
    u->addrlen = target_addrlen;

    /* Pacing default: off */
    u->bitrate_bps  = 0;
    u->tokens_bytes = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &u->last);

    *out_private = u;
    return 0;
}

void rf_udp_set_bitrate(void *priv, uint64_t bps)
{
    rf_udp_t *u = (rf_udp_t*)priv;
    if (!u) return;
    u->bitrate_bps = bps;      /* 0 = Pacing off */
    u->tokens_bytes = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &u->last);
}

static inline int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return ((int64_t)a->tv_sec - (int64_t)b->tv_sec) * 1000000000LL
           + ((int64_t)a->tv_nsec - (int64_t)b->tv_nsec);
}

/*Send UDP Packages*/
int rf_udp_send(void *priv, const uint8_t *data, size_t len)
{
    rf_udp_t *u = (rf_udp_t*)priv;
    if (!u || !data || len == 0) return -1;

    size_t off = 0;
    
    const double TOKENS_CAP = (double)u->payload * 6.0;

    while (off < len) {
        size_t chunk = u->payload;
        if (chunk > (len - off)) chunk = (len - off);

        if (u->bitrate_bps > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t dtns = ts_diff_ns(&now, &u->last);
            if (dtns > 0) {
                double add = ((double)u->bitrate_bps * (double)dtns) / 8000000000.0;
                u->tokens_bytes += add;
                if (u->tokens_bytes > TOKENS_CAP) u->tokens_bytes = TOKENS_CAP;
                u->last = now;
            }

            if (u->tokens_bytes < (double)chunk) {
                double deficit = (double)chunk - u->tokens_bytes;
                uint64_t need_ns = (uint64_t)((deficit * 8000000000.0) / (double)u->bitrate_bps);
                if (need_ns > 0) {
                    struct timespec ts;
                    ts.tv_sec  = (time_t)(need_ns / 1000000000ULL);
                    ts.tv_nsec = (long)(need_ns % 1000000000ULL);
                    nanosleep(&ts, NULL);

                    clock_gettime(CLOCK_MONOTONIC, &now);
                    dtns = ts_diff_ns(&now, &u->last);
                    if (dtns > 0) {
                        double add2 = ((double)u->bitrate_bps * (double)dtns) / 8000000000.0;
                        u->tokens_bytes += add2;
                        if (u->tokens_bytes > TOKENS_CAP) u->tokens_bytes = TOKENS_CAP;
                        u->last = now;
                    }
                }
            }
        }

        ssize_t s = sendto(u->sock, data + off, chunk, 0, 
                          (struct sockaddr *)&u->addr, u->addrlen);
        if (s < 0) {
            // fprintf(stderr, "UDP sendto failed: %s\n", strerror(errno));
            return -1;
        }
        off += (size_t)s;

        if (u->bitrate_bps > 0) {
            u->tokens_bytes -= (double)s;
            if (u->tokens_bytes < 0.0) u->tokens_bytes = 0.0;
        }
    }

    return 0;
}

int rf_udp_close(void *priv){
    rf_udp_t *u = (rf_udp_t*)priv;
    if(!u) return 0;
    if(u->sock>=0) close(u->sock);
    free(u);
    return 0;
}