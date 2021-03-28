#ident "@(#)xot.h	4.2 - 02/12/17 15:44:05 - XOT to xotlink commands & XOT datastructures"

/*
 * Copyright © 2002 Atlantic Technologies.
 * For conditions of distribution and use, see file LICENSE
 *
 */

struct xot_cmd 
{
	enum { XOT_CMD_OPEN, XOT_CMD_CLOSE } cmd;
	int cookie;
	int index;
};

struct xot_head
{
	uint16_t version;
	uint16_t length;
};


#define XOT_IOCTL 	(('X' << 8)  + 'T')


/*
 * The infamouse DEVNET TLI extensions:
 *
 */

#define T_QDATA_REQ	25	/* Conflicts with T_ADDR_ACK */
#define T_QDATA_IND	26	/* Conflicts with new T_BIND_REQ */
#define T_RST_REQ	27
#define T_RST_IND	28

#if _KERNEL

#define XOT_MID		0x786f		/* xo */
#define XOT_HIER	10		/* FIX */


/* Basic structure, 1 per open of xot device */

#define XOT_OPEN		0x01
#define XOT_CLOSING		0x02
#define XOT_CLEARED		0x04
#ifdef	XOT_DEBUG
#undef	XOT_DEBUG
#define XOT_DEBUG		0x80
#endif

struct xot_top
{
	lock_t *lck;
	queue_t *qptr;		/* The queue */
	struct xot_bot *bot;	/* Associated TCP/IP stream, or list */
	mblk_t *call;		/* Call packet */
	mblk_t *ack;		/* Ack */
	mblk_t *flow;		/* saved flow control packet */
	mblk_t *bound;		/* T_BIND_REQ if stream is bound */
	struct xot_top *binds;	/* For chaining bound streams */
	int queue_len;		/* Number of waiting calls */
	int queue_max;		/* Max waiting calls allowed */
	struct xot_bot **queue;	/* Inbound queue */
	ushort_t p;		/* Default packet len */
	uchar_t w;		/* Default window size */
	uchar_t state;
	uchar_t busy;
	uchar_t flag;
	uchar_t major;
};



/* X.25 flow control parameters */
  
struct xot_flow {
	ushort_t p;		/* data packet length accepted */
	uchar_t w;		/* window size accepted */

	uchar_t ps;		/* next packet seq to send */
	uchar_t pr;		/* next packet seq to accept */
	
	uchar_t xoff;		/* transmission halted */
};


/* Each xot_link represents a TCP/IP connection */

struct xot_bot {
	lock_t *lck;
	queue_t *qptr;		/* tcp/ip queue */
	int id;			/* mux id for this link */
	struct xot_top *top;	/* Pointer to upper */

	mblk_t *partial;	/* Packet assembly */

	struct xot_flow our;
	struct xot_flow his;

	uchar_t gfi_lcg;	/* GFI + LCG */
	uchar_t lcn;		/* Virtual circuit */
	
	uchar_t mask;		/* x7f or x07 for mod12/mod8 */

	uchar_t interrupt;	/* We've sent an interrupt */

	uchar_t busy;		/* In use, don't disconnect */
	uchar_t flags;

	uchar_t major;
	
};


#endif
