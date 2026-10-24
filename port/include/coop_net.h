#ifndef COOP_NET_H
#define COOP_NET_H

/* Co-op transport: one peer, unreliable datagrams. The UDP backend is for
 * local tests (GE_COOP=host:<port> or GE_COOP=join:<ip>:<port>); the Steam
 * backend will sit behind the same calls. */

int  coopNetInit(void);                 /* 1 if co-op is configured and open */
int  coopNetIsHost(void);
int  coopNetHasPeer(void);              /* host: a client has sent something */
void coopNetSend(const void *buf, int len);
int  coopNetRecv(void *buf, int cap);   /* bytes, or 0 when nothing is queued */
void coopNetShutdown(void);

#endif
