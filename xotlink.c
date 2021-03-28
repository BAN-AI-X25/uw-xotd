#ident "@(#)xotlink.c	4.1 - 02/12/09 11:11:29 - Make links for XOT device"

/*
 * Copyright © 2002 Atlantic Technologies.
 * For conditions of distribution and use, see file LICENSE
 *
 */

#undef _KERNEL

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>

#include <poll.h>
#include <stropts.h>

#include <sys/byteorder.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "xot.h"


#define rfc1613		1998

int debug = 1;

/* I/O handlers */

struct pollfd *pollfd;
int max_fd;

typedef void (*HANDLER) (int n);

struct handler 
{
	HANDLER call;
	void *data;
} *handler;


/* List of xot devices we handle */
   
struct device 
{
	char name [32];
	int fd;
	int naddr;
	in_addr_t *addr;
	struct device *next;
} *device_list;


/* Map to turn inbound call into appropriate device */

struct map 
{
	in_addr_t addr;
	struct device *device;
} *map;

int map_len;


/* List of active links to XOT driver */

struct link 
{
	int id;
	int fd;
	struct link *next;
} *links;

/* An outbound call */

struct call
{
	int fd;
	int cookie;
	struct device *dev;
	int next;
};



void init (int argc, char **argv);
char *readline (FILE *f);
int map_compare (const void *a, const void *b);
void add_address (struct device *dev, in_addr_t addr);
int add_handler (int fd, HANDLER call, void *data);
void delete_handler (int n);
void handle_listen (int n);
void handle_xot (int n);
void handle_call (int n);
void make_call (struct call *call);
void call_ok (struct call * call);
void tell_xot (int fd, int cookie, int link);
int make_link (int fd, struct device *dev);


int main (int argc, char **argv) {

	/* Initialise */
	
	init (argc, argv);

	/* Now wait for something to happen */

	for (;;) {
		int i;
		int n;
		
		if ((n = poll (pollfd, max_fd, -1)) == -1) {
			if (errno == EAGAIN) {
				sleep (1);
				continue;
			}
			else if (errno == EINTR) {
				continue;
			}
			perror ("poll");
			exit (1);
		}

		for (i = 0; n && i < max_fd; ++i) {
			if (pollfd[i].revents) {
				--n;
				handler[i].call (i);
			}
		}
	}
}



void init (int argc, char **argv) {
	int xot;
	int sock;

	int on;

	struct sockaddr_in addr;

	char *line;

	/* Read list of xot device/remote address(es) from stdin */

	/* Format: device addr,addr,... */
	
	while ((line = readline (stdin))) {
		char *file = strtok (line, " \t\n");
		char *addr;
		struct device *device;
		struct device *dev;
		int i;

		if (!file || *file == '#') continue;
		
		device = calloc (1, sizeof *device);
		
		if (*file != '/') strcpy (device->name, "/dev/");
		strncat (device->name, file, sizeof device->name - 1);

		/* Check if device specified twice */

		for (dev = device_list; dev; dev = dev->next) {
			if (strcmp (device->name, dev->name) == 0) {
				fprintf (stderr, "%s: duplicate\n");
				free (device);
				continue;
			}
		}

		/* See if we can open the device */
		
		if ((device->fd = open (device->name, O_RDWR)) == -1) {
			perror (device->name);
			free (device);
			continue;
		}
		
		/* Now read all the addresses for this one */
		
		while ((addr = strtok (NULL, ","))) {
			unsigned char ch;
			struct hostent *host;
			in_addr_t in_addr;
			
			while ((ch = *addr) && isspace (ch)) ++addr;
			if (!ch) continue;

			if ((in_addr = inet_addr (addr)) != INADDR_NONE) {
				add_address (device, in_addr);
			}
			else if ((host = gethostbyname (addr))) {
				int i;
				for (i = 0; host->h_addr_list[i]; ++i) {
					struct sockaddr_in *s =
						(struct sockaddr_in *)
						host->h_addr_list[i];
					add_address (device,
						     s->sin_addr.s_addr);
				}
			}
		}

		if (!device->naddr) {
			fprintf (stderr, "%s: No addresses\n",
				 device->name);
			close (device->fd);
			free (device);
			continue;
		}

		/* Ok, now make the address->device map */

		for (i = 0; i < device->naddr; ++i) {
			int n = map_len ++;
		        map = realloc (map, map_len * sizeof *map);
			map[n].addr = device->addr[i];
			map[n].device = device;
		}

		add_handler (device->fd, handle_xot, device);

		device->next = device_list;
		device_list = device;
	}

	/* Sort the device map so we can binary chop it */

	qsort (map, map_len, sizeof *map, map_compare);

	/* Open channel for calls from remote */

	if ((sock = socket (AF_INET, SOCK_STREAM, 0)) == -1) {
		perror ("socket");
		exit (1);
	}

	if (setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == -1)
	{
		perror ("setsockopt");
		exit (1);
	}
	
	memset (&addr, 0, sizeof addr);

	addr.sin_family = AF_INET;
	addr.sin_port = htons (rfc1613);
	
	if (bind (sock, (struct sockaddr *) &addr, sizeof addr) == -1) {
		perror ("bind");
		exit (1);
	}

	listen (sock, 5);

	add_handler (sock, handle_listen, NULL);
}



/* Comparison for sort & search */

int map_compare (const void *a, const void *b) {
	return ((struct map *)a)->addr - ((struct map *)b)->addr;
}



/* Read line from file; allow \ as line concat */

char *readline (FILE *f) {
	static char * res;
	static int res_len;

	char * p = res;
	int left = res_len;

	if (feof (f)) return NULL;
	
	for (;;) {
		int len;
		
		if (left < 80) {
			res_len += BUFSIZ;
			res = realloc (res, res_len);
			left += BUFSIZ;
			p = res + res_len - left;
		}
		
		if (fgets (p, left, f) == NULL) {
			*p = 0;
			break;
		}
		
		len = strlen (p);
		while (len && isspace (p [len - 1])) --len;
		if (p [len - 1] != '\\') break;
		p [len - 1] = ' ';
		left -= len;
		p += len;
	}

	return res;
}
	



void add_address (struct device *dev, in_addr_t addr) {

	int n;
	
	/* First ignore if already set for this device */

	for (n = 0; n < dev->naddr; ++n) {
		if (dev->addr[n] == addr) return;
	}

	/* Now complain if used by another device */

	for (n = 0; n < map_len; ++n) {
		if (map[n].addr == addr) {
			struct in_addr s;
			s.s_addr = addr;
			fprintf (stderr, "%s: address %s used by %s\n",
				 dev->name,
				 inet_ntoa (s), map[n].device->name);
		}
	}

	n = dev->naddr ++;

	dev->addr = realloc (dev->addr, dev->naddr * sizeof *dev->addr);

	dev->addr[n] = addr;
}




int add_handler (int fd, HANDLER call, void *data) {
	int n;

	for (n = 0; n < max_fd; ++n) {
		if (pollfd[n].fd == -1) break;
	}

	if (n == max_fd) {
		++max_fd;
		pollfd = realloc (pollfd, sizeof *pollfd * max_fd);
		handler = realloc (handler, sizeof *handler * max_fd);
	}

	pollfd[n].fd = fd;
	pollfd[n].events = POLLIN;

	handler[n].call = call;
	handler[n].data = data;

	return n;
}


void delete_handler (int n) {
	pollfd[n].fd = -1;
	while (max_fd && pollfd[max_fd - 1].fd == -1) --max_fd;
}



void handle_listen (int n) {

	/* Incoming virtual circuit establishment, tell xot */

	struct sockaddr_in addr;
	size_t len = sizeof addr;
	int sock;

	struct map in;
	struct map *m;

	struct link *lnk;

	sock = accept (pollfd[n].fd, (struct sockaddr *) &addr, &len);

	if (sock == -1) {
		perror ("accept");
		return;
	}
	
	/* Map calling address into appropriate XOT device */

	in.addr = addr.sin_addr.s_addr;
	
	if (!(m = bsearch (&in, map, map_len, sizeof *map, map_compare))) {
		fprintf (stderr, "Call from unknown addr %s\n",
			 inet_ntoa (addr.sin_addr));
		close (sock);
		return;
	}

	if (debug)
		fprintf (stderr, "%d: %s -> %s\n",
			 sock,
			 inet_ntoa (addr.sin_addr),
			 m->device->name);

	make_link (sock, m->device);
}


void handle_xot (int n) {

	struct xot_cmd xot;
	struct link *lnk, **plnk;

	struct device *dev = handler[n].data;

	struct call *call;
	
	if (read (pollfd[n].fd, &xot, sizeof xot) <= 0) {
		perror ("read");
		close (pollfd[n].fd);
		pollfd[n].fd = -1;
		return;
	}

	switch (xot.cmd) {

	    case XOT_CMD_OPEN:
		/* Make outgoing call; when done tell xot */

		call = calloc (1, sizeof *call);
		
		call->dev = dev;
		call->cookie = xot.cookie;

		make_call (call);
		
		break;

	    case XOT_CMD_CLOSE:
		/* Look for link-id to close */
		for (plnk = &links; (lnk = *plnk); plnk = &lnk->next) {
			if (lnk->id == xot.index)
				break;
		}

		if (!lnk) {
			fprintf (stderr, "Can't find link %p for %s\n",
				 xot.index, dev->name);
			return;
		}

		if (ioctl (pollfd[n].fd, I_UNLINK, xot.index) == -1) {
			perror ("I_UNLINK");
			return;
		}

		if (debug)
			fprintf (stderr, "%d: close\n", lnk->fd);

		close (lnk->fd);

		*plnk = lnk->next;

		free (lnk);
	}
}

	

	
	
	
void handle_call (int n) {
	/* Should be ready ? */

	struct call *call = handler[n].data;

	delete_handler (n);
	
	if (pollfd[n].revents != POLLOUT) {
		/* Bleurgh, call failed */
		close (call->fd);
		make_call (call);
		return;
	}

	/* Yipee! */

	call_ok (call);
}


	
void make_call (struct call *call) {

	int flags;
	struct sockaddr_in addr;

    next:
	if (call->next > call->dev->naddr) {
		tell_xot (call->dev->fd, call->cookie, 0);
		return;
	}
	
	if ((call->fd = socket (AF_INET, SOCK_STREAM, 0)) == -1) {
		perror ("socket");
		exit (1);
	}

	if ((flags = fcntl (call->fd, F_GETFL, 0)) == -1) {
		perror ("F_GETFL");
		exit (1);
	}

	flags |= O_NONBLOCK;

	if (fcntl (call->fd, F_SETFL, flags) == -1) {
		perror ("F_SETFL");
		exit (1);
	}

	memset (&addr, 0, sizeof addr);
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons (rfc1613);
	addr.sin_addr.s_addr = call->dev->addr[call->next++];
	
	if (connect (call->fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
		if (errno == EINPROGRESS) {
			/* Wait for the call to connect ... */
			int n = add_handler (call->fd, handle_call, call);
			pollfd[n].events = POLLOUT;
			return;
		}

		close (call->fd);
		goto next;
	}

	/* Call ok */

	call_ok (call);
}



void call_ok (struct call * call) {

	int index = make_link (call->fd, call->dev);

	tell_xot (call->dev->fd, call->cookie, index);
	free (call);
}


void tell_xot (int fd, int cookie, int link) {
		

	struct strioctl ioc;
	struct xot_cmd cmd;
	
	ioc.ic_cmd = XOT_IOCTL;
	ioc.ic_timout = 0;
	ioc.ic_len = sizeof cmd;
	ioc.ic_dp = (char *) &cmd;
		
	cmd.cmd = XOT_CMD_OPEN;
	cmd.cookie = cookie;
	cmd.index = link;
		
	if (ioctl (fd, I_STR, &ioc) == -1) {
		perror ("ioctl");
		exit (1);
	}
}


int make_link (int sock, struct device *dev) {
	struct link *lnk;
	
	if (!(lnk = calloc (1, sizeof *lnk))) {
		perror ("malloc");
		close (sock);
		return 0;
	}
	
	if ((lnk->id = ioctl (dev->fd, I_LINK, sock)) == -1) {
		perror ("I_LINK");
		close (sock);
		free (lnk);
		return 0;
	}

	lnk->fd = sock;
	lnk->next = links;
	links = lnk;

	return lnk->id;
}

