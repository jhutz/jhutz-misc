/*
 *  The TOR TCP DNS Daemon
 *  (c) Collin R. Mulliner <collin(AT)mulliner.org>
 *  http://www.mulliner.org/collin/ttdnsd.php
 *
 *  License: GPLv2
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netdb.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>

#define MAX_PEERS 1
#define MAX_TIME 3
#define MAX_TRY 1

#define MAX_REQUESTS 499
// 199, 1009

#define NOBODY 65534
#define DEF_PORT 5553

#define HELP_STR ""\
	"syntax: torify ttdnsd [bfcd]\n"\
	"\t-d\t\t\tDEBUG don't fork/chroot and print debug\n"\
	"\n"

struct peer_t
{
	struct sockaddr_in tcp;
	int tcp_fd;
	time_t timeout;
	int con; /**< connection state 0=dead, 1=connecting..., 3=connected */
	unsigned char b[1502]; /**< receive buffer */
	int bl; /**< bytes in receive buffer */
};

struct request_t
{
	struct sockaddr_in a;
	socklen_t al;
	unsigned char b[1502]; /**< request buffer */
	int bl; /**< bytes in request buffer */
	int id; /**< dns request id */
	int rid; /**< real dns request id */
	int active; /**< 1=sent, 0=waiting for tcp to become connected */
	time_t timeout; /**< timeout of request */
};

struct peer_t peers[MAX_PEERS]; /**< TCP peers */
struct request_t requests[MAX_REQUESTS]; /**< requests queue */
int udp_fd; /**< port 53 socket */
uint16_t port = DEF_PORT;

int debug = 0;

int request_find(int id)
{
	int pos = id % MAX_REQUESTS;
	
	for (;;) {
		if (requests[pos].id == id) {
			printf("found id=%d at pos=%d\n", id, pos);
			return pos;
		}
		else {
			pos++;
			pos %= MAX_REQUESTS;
			if (pos == (id % MAX_REQUESTS)) {
				printf("can't find id=%d\n", id);
				return -1;
			}
		}
	}
}

int peer_connect(int peer)
{
	struct peer_t *p = &peers[peer];
	int r = 1;
	int cs;
		
	if ((p->tcp_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return 0;
	
	if (setsockopt(p->tcp_fd, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(int))) printf("Setting SO_REUSEADDR failed\n");
	if (fcntl(p->tcp_fd, F_SETFL, O_NONBLOCK)) printf("Setting O_NONBLOCK failed\n");
	
	p->tcp.sin_family = AF_INET;
	p->tcp.sin_port = htons(port);
	p->tcp.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	printf("connecting to: %s\n", inet_ntoa(p->tcp.sin_addr));
	cs = connect(p->tcp_fd, (struct sockaddr*)&p->tcp, sizeof(struct sockaddr_in));

	if (cs != 0) perror("connect status:");
	else printf("connect returned 0\n");

	p->bl = 0;
	p->con = 1;
	
	return 1;
}

int peer_connected(int peer)
{
	struct peer_t *p = &peers[peer];
	int cs;
	
	
	cs = connect(p->tcp_fd, (struct sockaddr*)&p->tcp, sizeof(struct sockaddr_in));
	
	if (cs == 0) {
		p->con = 3;
		return 1;
	}
	else {
		printf("connection failed\n");
		close(p->tcp_fd);
		p->tcp_fd = -1;
		p->con = 0;
		return 0;
	}
}

int peer_keepalive(int peer)
{
	return 1;
}

int peer_sendreq(int peer, int req)
{
	struct peer_t *p = &peers[peer];
	struct request_t *r = &requests[req];
	int ret;

	printf("%s\n", __FUNCTION__);

	while ((ret = write(p->tcp_fd, r->b, (r->bl + 2))) < 0 && errno == EAGAIN);
	
	if (ret == 0) {
		close(p->tcp_fd);
		p->tcp_fd = -1;
		p->con = 0;
		printf("peer %d got disconnected\n", peer);
		return 2;
	}
	
	return 1;
}

int peer_readres(int peer)
{
	struct peer_t *p = &peers[peer];
	struct request_t *r;
	int ret;
	unsigned short int *ul;
	int id;
	int req;
	unsigned short int *l = (unsigned short int*)p->b;
	int len;

	//printf("%s\n", __FUNCTION__);
	
	while ((ret = read(p->tcp_fd, (p->b + p->bl), (1502 - p->bl))) < 0 && errno == EAGAIN);

	if (ret == 0) {
		close(p->tcp_fd);
		p->tcp_fd = -1;
		
		printf("peer %d got disconnected\n", peer);
		p->con = 0;
		return 3;
	}
	
	p->bl += ret;

processanswer:

	if (p->bl < 2) {
		return 2;
	}
	else {
		len = ntohs(*l);
		
		//printf("r l=%d r=%d\n", len, p->bl-2);
		
		if ((len + 2) > p->bl)
			return 2;
	}
		
	printf("received answer %d bytes\n", p->bl);
	
	ul = (unsigned short int*)(p->b + 2);
	id = ntohs(*ul);
	
	if ((req = request_find(id)) == -1) {
		memmove(p->b, (p->b + len + 2), (p->bl - len - 2));
		p->bl -= len + 2;	
		return 0;
	}
	r = &requests[req];
	
	// write back real id
	*ul = htons(r->rid);
	
	r->a.sin_family = AF_INET;
	while (sendto(udp_fd, (p->b + 2), len, 0, (struct sockaddr*)&r->a, sizeof(struct sockaddr_in)) < 0 && errno == EAGAIN);
	
	printf("forwarding answer (%d bytes)\n", len);

	memmove(p->b, p->b + len +2, p->bl - len - 2);
	p->bl -= len + 2;	

	// mark as handled/unused
	r->id = 0;

	if (p->bl > 0) goto processanswer;

	return 1;
}

void peer_handleoutstanding(int peer)
{
	int i;

	printf("%s\n", __FUNCTION__);

	for (i = 0; i < MAX_REQUESTS; i++) {
		if (requests[i].id != 0 && requests[i].active == 0) {
			requests[i].active = 1;
			peer_sendreq(peer, i);
		}
	}
}

int peer_select()
{
	return 0;
}

int request_add(struct request_t *r)
{
	int pos = r->id % MAX_REQUESTS;
	int dst_peer;
	unsigned short int *ul;
	time_t ct = time(NULL);

	printf("adding new request (id=%d)\n", r->id);
	for (;;) {
		if (requests[pos].id == 0) {
			// this one is unused, take it
			break;
		}
		else {
			if (requests[pos].id == r->id) {
				if (memcmp((char*)&r->a, (char*)&requests[pos].a, sizeof(r->a)) == 0) {
					printf("hash position %d already taken by request with same id; dropping it\n", pos);
					return 0;
				}
				else {
					do {
						r->id = ((rand()>>16) % 0xffff);
					} while (r->id < 1);
					pos = r->id % MAX_REQUESTS;
					printf("NATing id (id was %d now is %d)\n", r->rid, r->id);
					continue;
				}
			}
			else if ((requests[pos].timeout + MAX_TIME) > ct) {
				// request timed out, take it
				break;
			}
			else {
				pos++;
				pos %= MAX_REQUESTS;
				if (pos == (r->id % MAX_REQUESTS)) {
					printf("no more free request slots, wow this is a busy node. dropping request!\n");
					return 0;
				}
			}
		}
	}

	r->timeout = time(NULL);
	
	// update id
	ul = (unsigned short int*)(r->b + 2);
	*ul = htons(r->id);
	
	printf("using requests slot %d\n", pos);
	memcpy((char*)&requests[pos], (char*)r, sizeof(struct request_t));

	dst_peer = peer_select();
	if (peers[dst_peer].con == 3) {
		r->active = 1;
		return peer_sendreq(dst_peer, pos);
	}
	else {
		return peer_connect(dst_peer);
	}
}

int server(void)
{
	struct sockaddr_in udp;
	struct pollfd pfd[MAX_PEERS+1];
	int poll2peers[MAX_PEERS];
	int fr;
	int i;
	int pfd_num;
	struct request_t tmp;
	
	for (i = 0; i < MAX_PEERS; i++) {
		peers[i].tcp_fd = -1;
		poll2peers[i] = -1;
		peers[i].con = 0;
	}
	memset((char*)requests, 0, sizeof(requests));

	if ((udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		return(-1);
	}
	
	memset((char*)&udp, 0, sizeof(struct sockaddr_in));
	udp.sin_family = AF_INET;
	udp.sin_port = htons(port);
        udp.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(udp_fd, (struct sockaddr*)&udp, sizeof(struct sockaddr_in)) < 0) {
		close(udp_fd);
		printf("can't bind to %s:%d\n", "127.0.0.1", port);
		return(-1);
	}

	// drop privileges
	if (!debug) {
		setgid(NOBODY);
		setuid(NOBODY);
	}
		
	for (;;) {
	
		// populate poll array
		for (pfd_num = 1, i = 0; i < MAX_PEERS; i++) {	
			if (peers[i].tcp_fd != -1) {
				pfd[pfd_num].fd = peers[i].tcp_fd;
				switch (peers[i].con) {
				case 3:
					pfd[pfd_num].events = POLLIN|POLLPRI;
					break;
				default:
					pfd[pfd_num].events = POLLOUT|POLLERR;
					break;
				}
				poll2peers[pfd_num-1] = i;
				pfd_num++;
			}
		}
	
		pfd[0].fd = udp_fd;
		pfd[0].events = POLLIN|POLLPRI;
		
		printf("watching %d file descriptors\n", pfd_num);
		
		fr = poll(pfd, pfd_num, -1);
		
		printf("%d file descriptors became ready\n", fr);

		// handle tcp connections
		for (i = 1; i < pfd_num; i++) {
			if (pfd[i].fd != -1 && ((pfd[i].revents & POLLIN) == POLLIN || 
					(pfd[i].revents & POLLPRI) == POLLPRI || (pfd[i].revents & POLLOUT) 
					== POLLOUT || (pfd[i].revents & POLLERR) == POLLERR)) {
				
				switch (peers[poll2peers[i-1]].con) {
				case 3:
					peer_readres(poll2peers[i-1]);
					break;
				case 1:
				case 2:
					if (peer_connected(poll2peers[i-1])) {
						peer_handleoutstanding(poll2peers[i-1]);
					}
					break;
				default:
					printf("peer %d in bad state\n", peers[poll2peers[i-1]].con);
					break;
				}
			}
		}
	
		if ((pfd[0].revents & POLLIN) == POLLIN || (pfd[0].revents & POLLPRI) == POLLPRI) {
			unsigned short int *ul;
			
			memset((char*)&tmp, 0, sizeof(struct request_t));
			tmp.al = sizeof(struct sockaddr_in);
			
			while ((tmp.bl = recvfrom(udp_fd, tmp.b+2, 1500, 0, (struct sockaddr*)&tmp.a, &tmp.al)) < 0 && errno == EAGAIN);
			// get request id
			ul = (unsigned short int*) (tmp.b + 2);
			tmp.rid = tmp.id = ntohs(*ul);
			// set request length
			ul = (unsigned short int*)tmp.b;
			*ul = htons(tmp.bl);
			
			printf("received request of %d bytes, id = %d\n", tmp.bl, tmp.id);
			
			request_add(&tmp);
		}
	}
}

int main(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "hd")) != EOF) {
		switch (opt) {
		case 'd':
			debug = 1;
			break;
		case 'h':
		default:
			printf(HELP_STR);
			exit(0);
			break;
		}
	}

	if (optind < argc) {
		opt = atoi(argv[optind]);
		if (opt > 0 && opt < 0xffff)
			port = opt;
	}

	srand(time(NULL));
	
	if (!debug) {
		if (fork()) exit(0);
                int fd;
                fd = open("/dev/null", O_RDWR);
                dup2(fd, 0);
                dup2(fd, 1);
                dup2(fd, 2);
                close(fd);
		setsid();
	}
	
	server();
	exit(0);
}
