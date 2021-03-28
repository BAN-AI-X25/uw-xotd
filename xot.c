#ident "@(#)xot.c	4.7 - 05/10/03 00:06:34 - XOT driver"

/*
 * Copyright © 2002, 2005 Atlantic Technologies.
 * For conditions of distribution and use, see file LICENSE
 *
 */

#define _DDI 7
#include <sys/ddi.h>

#include <stdarg.h>

#include <sys/xti.h>
#include <sys/tihdr.h>
#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/errno.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/cmn_err.h>
#include <sys/syslog.h>
#include <sys/strlog.h>
#include <sys/ddi.h>
#include <sys/moddefs.h>

#include "xot.h"
#include "private.h"


/* Forward declarations */

static int xot_load (void);
static int xot_unload (void);

int xotopen ();
int xotclose ();

int xotuwput ();
int xotuwsrv ();
int xotursrv ();
int xotlrsrv ();

#ifdef XOT_DEBUG
static void topdebug (struct xot_top *top, char *fmt, ...);
static void botdebug (struct xot_bot *bot, char *fmt, ...);
#endif

static void extract_addr (uchar_t *ablock, int offset, int len, uchar_t *addr);
static void insert_addr (uchar_t *ablock, int offset, int len, uchar_t *addr);
static int validate_addr (uchar_t *addr, int addr_len);

static int decode_fac (struct xot_flow *called,
		       struct xot_flow *calling,
		       uchar_t *fac, int fac_len);
static struct xot_top *find_listen (struct xot_top *master, uchar_t *addr, int len);

static mblk_t *handle_ioctl (queue_t *q, mblk_t *mp);
static mblk_t *send_downstream ();
static mblk_t *send_upstream ();
static struct xot_top *find_listen ();


/*
 * X.25 flow control:
 *
 * Cannot send if reciver has said XOFF,
 *
 * or if senders P(S) is outside receivers window
 *
 * (note, needs "window" variable in scope)
 *
 */

#define FLOW_CONTROL(xlt, snd, rcv, off)			\
(xlt->rcv.xoff ||						\
 (window = (xlt->rcv.pr + xlt->rcv.w - off) & xlt->mask,	\
  (window > xlt->rcv.pr ?					\
   !(xlt->snd.ps >= xlt->rcv.pr && xlt->snd.ps < window) :	\
   !(xlt->snd.ps >= xlt->rcv.pr || xlt->snd.ps < window))))



/*
 * If the top is busy we can't close it.
 *
 * so we can be sure that ...?
 *
 */

#define BUSY_TOP(top) \
do {									\
	if (!++top->busy)						\
		cmn_err (CE_WARN, "xot%d/%d: overbusied",		\
			 top->major, top - xot_top);			\
}									\
while (0)


#define IDLE_TOP(top) \
do {								\
	pl_t pl = LOCK (top->lck, plstr);			\
	if (!top->busy)						\
		cmn_err (CE_WARN, "XOT: IDLE NOT BUSY AT %d", __LINE__); \
	else							\
	if (!--top->busy) {					\
		if (top->flag & XOT_CLOSING) {			\
			SV_BROADCAST (xotsv, 0);		\
		}						\
	}							\
	UNLOCK (top->lck, pl);					\
}								\
while (0)


/*
 * If the bottom is busy we can't unlink it
 *
 * So while it's busy we can be sure it's linked.
 *
 */

#define BUSY_BOT(bot) \
do {									\
	if (!++bot->busy)						\
		cmn_err (CE_WARN, "bottom overbusied");			\
}									\
while (0)


#define IDLE_BOT(bot) \
do {									\
	pl_t pl = LOCK (bot->lck, plstr);				\
	if (!bot->busy)							\
		cmn_err (CE_WARN, "XOT: IDLE NOT BUSY AT %d", __LINE__);\
	else								\
	if (!--bot->busy) {						\
		if (bot->flags & XOT_CLOSING) {				\
			qenable (WR (xot_top[bot->major].qptr));	\
		}							\
	}								\
	UNLOCK (bot->lck, pl);						\
}									\
while (0)



/*
 * DLM initialisation
 */

MOD_DRV_WRAPPER (xot, xot_load, xot_unload, NULL,
		 "xot 4.7 X.25/TCP interface");



/*
 * Streams initialisation tables
 */

static struct module_info info =
	{XOT_MID, "xot", 0, INFPSZ, 512, 128};

static struct qinit urinit = { 				/* upper read */
	NULL, xotursrv, xotopen, xotclose, NULL, &info, NULL};

static struct qinit uwinit = {				/* upper write */
	xotuwput, xotuwsrv, NULL, NULL, NULL, &info, NULL};

static struct qinit lrinit = {				/* lower read */
	putq, xotlrsrv, NULL, NULL, NULL, &info, NULL};

static struct qinit lwinit = {				/* lower write */
	NULL, NULL, NULL, NULL, NULL, &info, NULL};

struct streamtab xotinfo = {&urinit, &uwinit, &lrinit, &lwinit};


/* Device flag */

int xotdevflag = D_MP;


/* Locking stuff */

lkinfo_t xot_lkinfo;

sv_t *xotsv;


/* Stuff from space.c */

extern struct xot_top xot_top [];
extern struct xot_bot xot_bot [];
extern int xot_cnt;
extern int xot_major;
extern int xot_majors;


/*
 * Load XOT driver
 *
 */

static int xot_load (void) {

	register struct xot_top *top;
	register struct xot_top *last_top = xot_top + xot_cnt;

	register struct xot_bot *bot;
	register struct xot_bot *last_bot = xot_bot + xot_cnt;
	
	cmn_err (CE_NOTE, "Load ATI xot version " VERSION);

	/* Could run a xotinit () here */

	mod_drvattach (&xot_attach_info);

	/* Initialise the XOT driver after it has been loaded */

	xotsv = SV_ALLOC (KM_NOSLEEP);
	
	/* Note locking heirarchy -

	    lock top before bot
	    lock top before master

	*/
	
	for (top = xot_top; top < last_top; top++) {
		int hier = XOT_HIER;
		if (top < xot_top + xot_majors) hier = XOT_HIER + 1;
		top->lck =
			LOCK_ALLOC (hier, plstr, &xot_lkinfo, KM_NOSLEEP);
	}

	for (bot = xot_bot; bot < last_bot; ++bot)
		bot->lck =
			LOCK_ALLOC (XOT_HIER + 2, plstr,
				    &xot_lkinfo, KM_NOSLEEP);

	return 0;
}


/*
 * Unload XOT driver
 *
 * I assume all the streams have been closed when we get here.
 *
 */

static int xot_unload (void) {

	register struct xot_top *top;
	register struct xot_top *last_top = xot_top + xot_cnt;
	
	register struct xot_bot *bot;
	register struct xot_bot *last_bot = xot_bot + xot_cnt;
	
	cmn_err (CE_NOTE, "Unload ATI xot version " VERSION);

	/* Release resources allocated by the XOT driver */

	SV_DEALLOC (xotsv);
	
	for (top = xot_top; top < last_top; top++)
		if (top->lck) LOCK_DEALLOC (top->lck);

	for (bot = xot_bot; bot < last_bot; ++bot)
		if (bot->lck) LOCK_DEALLOC (bot->lck);
	
	mod_drvdetach (&xot_attach_info);
	
	return 0;
}


/*
 * Simple, inlineable functions
 *
 */


static mblk_t *allocate (queue_t *q, int len) {
	mblk_t *mp = allocb (len, q ? BPRI_MED : BPRI_HI);
	
	if (!mp && q) bufcall (len, BPRI_MED, qenable, (long) q);

	return mp;
}


static mblk_t* alloc_x25 (queue_t *q, struct xot_bot *bot, int pt,
			  int len, int ulen)
{
	struct xot_head *head;
	mblk_t *mp;

	len += 3;
	
	if (!(mp = allocate (q, len + sizeof *head))) return NULL;

	head = (struct xot_head *) mp->b_wptr;
	mp->b_wptr += sizeof *head;

	head->version = 0;
	head->length = htons (len + ulen);
		
	*mp->b_wptr++ = bot->gfi_lcg;
	*mp->b_wptr++ = bot->lcn;
	*mp->b_wptr++ = pt;

	return mp;
}



static mblk_t *duplicate (queue_t *q, mblk_t *mp) {
	mblk_t *np = dupb (mp);
	
	if (!np) bufcall (0, BPRI_MED, qenable, (long) q);

	return np;
}


/* *NOTE* must have already checked size, or could loop */

static int pullup (queue_t *q, mblk_t **mpp, int len) {
	mblk_t *mp = *mpp;
	mblk_t *np;

	if (mp->b_wptr - mp->b_rptr >= len) return 1;

	if (!(np = msgpullup (mp, len))) {
		bufcall (len, BPRI_MED, qenable, (long) q);
		return 0;
	}

	freemsg (mp);

	*mpp = np;

	return 1;
}


static mblk_t *allocate_clear (queue_t *q) {
	return allocate (q, sizeof (struct xot_cmd));
}


/*
 * bot must be marked busy before calling this
 *
 */

static int clear_call (queue_t *q, struct xot_bot *bot, mblk_t *mp) {

	struct xot_cmd *cmd;

	pl_t pl;

	if (!bot->busy) {
		cmn_err (CE_PANIC, "bottom idle in clear_call");
	}

	pl = LOCK (bot->lck, plstr);
	
	if (!bot->flags || bot->flags & XOT_CLEARED) {	/* Unlinked */
		UNLOCK (bot->lck, pl);
		if (mp) freemsg (mp);
		return 1;
	}

	bot->flags |= XOT_CLEARED;

	UNLOCK (bot->lck, pl);
	
	if (!mp && !(mp = allocate_clear (q))) return 0;
	cmd = (struct xot_cmd *) mp->b_rptr;
	mp->b_wptr = mp->b_rptr + sizeof *cmd;

	cmd->cmd = XOT_CMD_CLOSE;
	cmd->index = bot->id;

	BOTDEBUG ((bot, "clear call %x", bot->id));
	
	putnext (xot_top [bot->major].qptr, mp);

	return 1;
}


/* Must have top locked when we get here */

void remove_bind (struct xot_top *top) {

	if (top->queue_max) {
		pl_t pl;

		struct xot_top *master, *t, **pt;

		master = &xot_top [top->major];

		pl = LOCK (master->lck, plstr);
		
		for (pt = &master->binds; (t = *pt); pt = &t->binds) {
			if (t == top) break;
		}
		
		if (t) *pt = t->binds;

		UNLOCK (master->lck, pl);

		/* Free the queue of incoming calls */
		
		kmem_free (top->queue, top->queue_max * sizeof *top->queue);
		
		top->queue = NULL;
		
		top->queue_max = 0;
	}

	freemsg (top->bound);
	top->bound = NULL;
}




/*
 * Open XOT device.
 *
 * Either we're opening minor 0, used for control, or asking for a clone,
 * or opening an already open xot device.
 *
 */

int xotopen (queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp) {

	struct xot_top *top;
	struct xot_top *last_top = xot_top + xot_cnt;
	pl_t pl;
	int major;

	if ((major = getmajor (*devp) - xot_major) < 0) return ENXIO;

	if (sflag == CLONEOPEN) {

		for (top = xot_top + xot_majors; top < last_top; top++) {
			
			if (top->lck == NULL) continue;
			
			pl = LOCK (top->lck, plstr);
			if (!top->flag) break;
			UNLOCK (top->lck, pl);
		}
		
		if (top >= last_top) return ENXIO;

		TOPDEBUG ((top, "open (clone)"));

		*devp = makedevice (xot_major + major, top - xot_top);
	}
	else {
		dev_t minor = getminor (*devp);

		if (minor < 0 || minor  >= xot_cnt) return ENXIO;

		if (minor == 0) {
			/* Open of a controlling device */
			top = xot_top + major;

			TOPDEBUG ((top, "open (major)"));
		}
		else if (minor < xot_majors) {
			return ENXIO;
		}
		else {
			top = &xot_top[minor];

			TOPDEBUG ((top, "open"));
		}
		
		if (top->lck == NULL) return ENXIO;

		if (q->q_ptr) {
			if (q->q_ptr != top) return EBUSY;
			return 0;
		}

		pl = LOCK (top->lck, plstr);
		
		if (top->flag) {
			UNLOCK (top->lck, pl);
			return ENXIO;
		}
	}

	/*
	 * Once we get here, the device is valid and we're holding its lock.
	 */

	top->qptr = q;		/* read side */
#if XOT_DEBUG
	top->flag = XOT_OPEN + XOT_DEBUG;
#else
	top->flag = XOT_OPEN;
#endif
	top->major = major;
	top->bot = NULL;

	top->state = TS_UNBND;

	top->busy = 0;

	/* X.25 defaults - only used for master */
			
	top->p = 128;
	top->w = 3;		/* Hack */
	
	q->q_ptr = (char *) top;
	WR (q)->q_ptr = (char *) top;

	UNLOCK (top->lck, pl);

	qprocson (q);
	
	return 0;
}




/*
 * Close a xot connection
 *
 * Called with top locked
 *
 */

static void clear_x25 (struct xot_top *top, struct xot_bot *bot) {
	
	pl_t pl;
	mblk_t *xp;

	pl = LOCK (bot->lck, plstr);
	
	if (!bot->flags) {	/* Unlinked */
		UNLOCK (bot->lck, pl);
		return;
	}

	bot->top = NULL;	/* 'cos top closed ??? */
	
	BUSY_BOT (bot);

	UNLOCK (bot->lck, pl);

	switch (top->state) {
	    case TS_WACK_DREQ6:	
		/* We got the CLR_REQ from him */
		break;

	    default:
		TOPDEBUG ((top, "send X25_CLR_REQ"));
				
		/* Send an X.25 clear, TCP call will go away
		   when we get the clear conf */

		xp = alloc_x25 (NULL, bot, X25_CLR_REQ, 2, 0);
		
		*xp->b_wptr++ = 0;
		*xp->b_wptr++ = 0;

		/* Dodgy(?), we still have top locked here */
				
		putnext (bot->qptr, xp);
	}
			
	IDLE_BOT (bot);
}


int xotclose (queue_t * q, int flag, cred_t * credp) {
	
	pl_t pl;

	register struct xot_top *top = (struct xot_top *) q->q_ptr;
	struct xot_bot *bot, **pb;

	TOPDEBUG ((top, "xotclose"));
	
	qprocsoff (q);

	pl = LOCK (top->lck, plstr);

	while (top->busy) {
		TOPDEBUG ((top, "xotclose: wait for idle"));
		top->flag |= XOT_CLOSING;
		SV_WAIT (xotsv, prinet, top->lck);
		pl = LOCK (top->lck, plstr);
	}

	/* If a call is active clear it */

	if (top->bot) {
		clear_x25 (top, top->bot);
		top->bot = NULL;
	}

	/* Clear any waiting calls */
	
	for (pb = top->queue; top->queue_len; ++pb) {

		if (!(bot = *pb)) continue;

		--top->queue_len;

		clear_x25 (top, bot);
		*pb = NULL;
	}

	if (top->bound) remove_bind (top);

	top->flag = 0;

	top->state = TS_UNBND;

	UNLOCK (top->lck, pl);

	return 0;
}



/*
 * Upper Write side put.
 *
 * Here to process messages coming from one of the devices above the MUX.
 *
 * We handle the LINK, UNLINK and FLUSH ioctl's here.
 *
 */

int xotuwput (queue_t * q, mblk_t * mp) {

	struct xot_top *top = (struct xot_top *) q->q_ptr;
	struct iocblk *iocp;

	/* Special case for control channels */

	if (top < xot_top + xot_majors) {
		if ((mp = handle_ioctl (q, mp))) putq (q, mp);
		return 0;
	}

	/* Code for all other units */

	switch (mp->b_datap->db_type) {
		
	    case M_IOCTL:
		/* Maybe should just pass down? */

		TOPDEBUG ((top, "uwput: reject ioctl"));
		
		iocp = (struct iocblk *) mp->b_rptr;
		mp->b_datap->db_type = M_IOCNAK;
		iocp->ioc_error = EINVAL;
		
		qreply (q, mp);
		break;

	    case M_FLUSH:

		TOPDEBUG ((top, "uwput: flush"));

		if (*mp->b_rptr & FLUSHW) flushq (q, FLUSHDATA);

		if (*mp->b_rptr & FLUSHR) {
			*mp->b_rptr &= ~FLUSHW;
			qreply (q, mp);
		}
		else {
			freemsg (mp);
		}
		
		break;
		
	    case M_DATA:
	    case M_PROTO:
		/* Non priority data is just left for the service routine */
		putq (q, mp);
		break;

	    case M_PCPROTO:
		if ((mp = send_downstream (q, mp))) {
			TOPDEBUG ((top, "uwput: queue"));
			putq (q, mp);
		}

		break;

	    default:
		TOPDEBUG ((top, "uwput (M_0x%x) - ignored",
			  mp->b_datap->db_type));
		freemsg (mp);
		break;
	}

	return 0;
}


/*
 * Here to deal with queued up stuff for going downstream
 *
 * Returns with any unhandled message, which should be
 * put (back) on queue.
 *
 */

int xotuwsrv (queue_t *q) {

	mblk_t *mp;
	struct xot_top *top = (struct xot_top *) q->q_ptr;

	if (top < xot_top + xot_majors) {
		while ((mp = getq (q))) {
			if ((mp = handle_ioctl (q, mp))) {
				putbq (q, mp);
				return 0;
			}
		}
		return 0;
	}
		
	while ((mp = getq (q))) {

		if (mp->b_datap->db_type == M_DATA) {

			mblk_t *np;

			while ((np = getq (q))) {
				if (np->b_datap->db_type != M_DATA) {
					putbq (q, np);
					break;
				}
				linkb (mp, np);
			}
		}

		if ((mp = send_downstream (q, mp))) {
			putbq (q, mp);
			return 0;
		}
	}
}


/*
 * Handle ioctl on control channels
 *
 */


static mblk_t *handle_ioctl (queue_t *q, mblk_t *mp) {
			
	pl_t pl, pl2;
	
	struct iocblk *iocp;
	struct linkblk *linkp;

	struct xot_top *top = (struct xot_top *) q->q_ptr;
	struct xot_bot *bot, **pb;
	struct xot_bot *last_bot = xot_bot + xot_cnt;

	struct xot_cmd *cmd;
		
	queue_t *lower;

	struct xot_top *t;

	int error;

	mblk_t *partial;

	struct T_ok_ack *ok_ack;

	mblk_t *np;

	int fac_len;
	uchar_t *p;

	int i;

	if (mp->b_datap->db_type != M_IOCTL) {
		/* Just ignore it */
		freemsg (mp);
		return NULL;
	}

	iocp = (struct iocblk *) mp->b_rptr;
		
	switch (iocp->ioc_cmd) {
	    case I_LINK:
		/* Link.  The data contains a linkblk structure.
		   Allocate a xot_bot */
			
		linkp = (struct linkblk *) mp->b_cont->b_rptr;

		TOPDEBUG ((top, "ioctl link %x", linkp->l_index));
		
		pl = LOCK (top->lck, plstr);

		for (bot = xot_bot; bot < last_bot; ++bot) {
			if (!bot->lck) continue;
			pl2 = LOCK (bot->lck, plstr);
			if (!bot->flags) break;
			UNLOCK (bot->lck, pl2);
		}

		if (bot == last_bot) {
			UNLOCK (top->lck, pl);
			error = ENOMEM;
			goto iocnak;
		}
			
		bot->qptr = lower = linkp->l_qbot; /* write side */
		bot->id = linkp->l_index;
		bot->partial = NULL;
		bot->major = top->major;
		bot->top = NULL;
		bot->busy = 0;
		bot->flags = XOT_OPEN;

		top->bot = bot;

		RD (lower)->q_ptr = lower->q_ptr = bot;
		
		UNLOCK (bot->lck, pl2);

		UNLOCK (top->lck, pl);
		
		/* Is this safe?  When is the real "LINK" done? */
		
		qenable (RD (lower));
		
		goto ack;

	    case I_UNLINK:
		/* Unlink.  The data contains a linkblk structure. */

		if (!(np = allocate (q, 1))) return mp;

		linkp = (struct linkblk *) mp->b_cont->b_rptr;

		TOPDEBUG ((top, "ioctl unlink %x, bot=%x",
			   linkp->l_index, linkp->l_qbot));

		bot = linkp->l_qbot->q_ptr;

		/* If the bot is busy we'll have to wait 'till
		   he's ready.  This is hard */

		pl = LOCK (bot->lck, plstr);
		
		if (bot->busy) {
			bot->flags |= XOT_CLOSING;
			UNLOCK (bot->lck, pl);
			freemsg (np);
			return mp;
		}
		
		/* Do the actual unlinking */
		
		bot->flags = 0;

		t = bot->top;
		bot->top = NULL;
		
		partial = bot->partial;
		bot->partial = NULL;

		UNLOCK (bot->lck, pl);

		/* Should only get here if xotlink is going away
		   and we're clearing all the calls before closing
		   the master */
		   
		if (t) {
			/* Someone is talking on this stream */

			pl = LOCK (t->lck, plstr);
			t->bot = NULL;			/* safe??? */

			if (t->flag) {
				BUSY_TOP (t);
				UNLOCK (t->lck, pl);

				np->b_datap->db_type = M_HANGUP;
				putnext (t->qptr, np);
				
				IDLE_TOP (t);
			}
			else {
				UNLOCK (t->lck, pl);
				freemsg (np);
			}
			
		}
		else {
			freemsg (np);
		}

		if (partial) freemsg (partial);

		goto ack;

	    case XOT_IOCTL:
		/* xotlink tells us that a link is for outbound */

		if (iocp->ioc_count != sizeof *cmd) {
			error = EINVAL;
			goto iocnak;
		}
			
		cmd = (struct xot_cmd *) mp->b_cont->b_rptr;

		TOPDEBUG ((top, "ioctl (%x, %x)", cmd->cookie, cmd->index));

		/* use cookie to find appropriate top */

		t = (struct xot_top *) cmd->cookie;
			
		if (t < xot_top || t >= xot_top + xot_cnt) {
			error = ENXIO;
			goto iocnak;
		}

		i = t - xot_top;
			
		if (xot_top + i != t || top->major != t->major) {
			error = ENXIO;
			goto iocnak;
		}
			
		np = t->ack;
		t->ack = NULL;

		np->b_datap->db_type = M_PCPROTO;
				
		if (cmd->index == 0) {
				
			/* A call we asked for has failed */
			
			struct T_error_ack *error_ack;

			freemsg (t->call);
			t->call = NULL;
			t->state = TS_IDLE;
			
			error_ack = (struct T_error_ack *) np->b_rptr;
			np->b_wptr = np->b_rptr + sizeof *error_ack;
			
			error_ack->PRIM_type = T_ERROR_ACK;
			error_ack->ERROR_prim = T_CONN_REQ;
			error_ack->TLI_error = TSYSERR;
			error_ack->UNIX_error = ECONNREFUSED;
			
			putnext (t->qptr, np);

			goto ack;
		}

		/* A call we asked for has been connected. */

		for (bot = xot_bot; bot < last_bot; ++bot) {
			pl = LOCK (bot->lck, plstr);
			if (bot->flags &&
			    bot->major == top->major &&
			    bot->id == cmd->index)
				break;
			UNLOCK (bot->lck, pl);
		}

		if (bot == last_bot) {
			error = ENXIO;
			freemsg (np);
			goto iocnak;
		}
		
		t->bot = bot;
		bot->top = t;

		UNLOCK (bot->lck, pl);
		
		/* Send T_OK_ACK to x, x25_call_req to bot */
		
		ok_ack = (struct T_ok_ack *) np->b_rptr;
		np->b_wptr = np->b_rptr + sizeof *ok_ack;
		
		ok_ack->PRIM_type = T_OK_ACK;
		ok_ack->CORRECT_prim = T_CONN_REQ;
		
		putnext (t->qptr, np);
		
		np = t->call;
		t->call = NULL;
		
		/* extract information from call  packet. */

		p = np->b_rptr + sizeof (struct xot_head);

		bot->gfi_lcg = *p++;
		bot->lcn = *p++;

		if ((bot->gfi_lcg & 0x30) == 0x20) {
			bot->mask = 0x7f;
		}
		else {
			bot->mask = 0x07;
		}

		bot->our.ps = 0;
		bot->his.pr = 0;
		bot->our.pr = 0;
		bot->his.ps = bot->mask;
		bot->our.xoff = 0;
		bot->his.xoff = 0;
		
		p++;			/* Skip pti */

		i = *p++;		/* Skip address length(s) */
		
		i = (i >> 4) + (i & 0x0f);
		p += (i + 1) / 2;	/* Skip address block */
		
		fac_len = *p++;
		
		decode_fac (&bot->our, &bot->his, p, fac_len);
		putnext (bot->qptr, np);
			
	    ack:
		mp->b_datap->db_type = M_IOCACK;
		iocp->ioc_count = 0;
		qreply (q, mp);
		break;

	    default:
			/* Unknown ioctl.  Should just ignore it??? ... */
		error = EINVAL;
			
	    iocnak: /* fail ioctl */

		cmn_err (CE_WARN, "ioctl fail %d", error);

		mp->b_datap->db_type = M_IOCNAK;
		iocp->ioc_error = error;
		
		qreply (q, mp);
	}

	return NULL;
}



/*
 * Convert TLI stuff to X.25
 *
 *
 * Only run from upper write service routine.
 *
 * Only one copy of a queues service routine will run at a time. 
 *
 */

static mblk_t *send_downstream (queue_t *q, mblk_t *mp) {

	mblk_t *xp, *np, *rp, *sp;
	struct xot_top *top = (struct xot_top *) q->q_ptr;
	struct xot_bot *bot;
	union T_primitives *tpi;
	struct T_ok_ack *ok_ack;
	int mbit = 0;
	int qbit = 0;
	int left;
	int window;

        int ulen;

	uchar_t *fac;
	int fac_len;

	char add[6];
	char add_len;

	struct xot_flow our, his;
	uchar_t gfi;

	int source_len, dest_len;
	uchar_t *source, *dest;

	int len;

	struct xot_bot **pb, **end;
	int reject;

	struct xot_top *master, *t;

	struct xot_cmd *cmd;

	pl_t pl, pl2;

	struct xot_head *head;

	switch (mp->b_datap->db_type) {

	    case M_DATA:
		tpi = NULL;
		goto data;
		
	    case M_PCPROTO:
	    case M_PROTO:
		break;

	    default:
		/* Eurgh, errrorrrr! */
		freemsg (mp);
		return NULL;
	}

	tpi = (union T_primitives *) mp->b_rptr;

	xp = NULL;
	
	switch (tpi->type) {

#ifdef T_QDATA_REQ
	    case T_QDATA_REQ:
		qbit = X25_QBIT;
#endif
	    case T_DATA_REQ:
		goto data;

	    case T_CONN_REQ:	/* connection request     */

		/* Make a nice X.25 call packet, validating the stuff
		   in the connect request */

		TOPDEBUG ((top, "T_CONN_REQ"));

		/* rfc1613: To simplify end-to-end flow control, the packet
		   size and window size are always sent explicitly as
		   facilities in the Call packet.  The Call packet MUST
		   contain both Packet Size and Window Size facilities. */

		our.p = his.p = 0;
		our.w = his.w = 0;

		fac = mp->b_rptr + tpi->conn_req.OPT_offset;
		fac_len = tpi->conn_req.OPT_length;
		
		if (fac_len > 109 || !decode_fac (&our, &his, fac, fac_len)) {
			REPLY_T_ERROR (TBADOPT);
			return NULL;
		}

		if ((dest_len = tpi->conn_req.DEST_length) > 15) {
			REPLY_T_ERROR (TBADADDR);
			return NULL;
		}

		dest = mp->b_rptr + tpi->conn_req.DEST_offset;

		if (!validate_addr (dest, dest_len)) {
			REPLY_T_ERROR (TBADADDR);
			return NULL;
		}

		if ((ulen = msgdsize (mp)) > 128) {
			REPLY_T_ERROR (TBADDATA);
			return NULL;
		}

		master = xot_top + top->major;

		pl = LOCK (master->lck, plstr);

		if (!master->flag) {
			UNLOCK (master->lck, pl);
			REPLY_T_ERROR (TOUTSTATE);
			return NULL;
		}

		BUSY_TOP (master);

		UNLOCK (master->lck, pl);
		
		/* top->bound only changed by send_downstream, so
		   no locking needed */
		
		if ((np = top->bound)) {
			struct T_bind_req *bound =
				(struct T_bind_req *) np->b_rptr;
			
			source_len = bound->ADDR_length;
			source = np->b_rptr + bound->ADDR_offset;
		}
		else {
			source_len = 0;
		}

		add_len = 0;
		
		if (!our.p) {
			/* have to add packet sizes to fac */
			int p;
			our.p = his.p = top->p;
			add[add_len++] = X25_FAC_PACKET;
			for (p = 4; p < 15; ++p)
				if (our.p & (1 << p)) break;
			add[add_len++] = p;
			add[add_len++] = p;
		}

		if (!our.w) {
			/* have to add window sizes to fac */
			our.w = his.w = top->w;
			add[add_len++] = X25_FAC_WINDOW;
			add[add_len++] = our.w;
			add[add_len++] = his.w;
		}
		
		len = 3 +
			1 + (source_len + dest_len + 1) / 2 +
			1 + (fac_len + add_len);
		
		if (!(xp = allocate (q, len + sizeof *head))) goto nomem;

		if (!(np = allocate (q, sizeof *cmd))) {
			freeb (xp);
			goto nomem;
		}

		if (his.w > 7 || our.w > 7) {
			gfi = X25_MOD_128;
		}
		else {
			gfi = X25_MOD_8;
		}

		head = (struct xot_head *) xp->b_wptr;
		xp->b_wptr += sizeof *head;

		head->version = 0;
		head->length = htons (len + ulen);
			
		*xp->b_wptr++ = gfi;
		*xp->b_wptr++ = 1;		/* lcn - use minor? */
		*xp->b_wptr++ = X25_CALL_REQ;

		*xp->b_wptr++ = (source_len << 4) + dest_len;

		insert_addr (xp->b_wptr, 0, dest_len, dest);
		insert_addr (xp->b_wptr, dest_len, source_len, source);

		xp->b_wptr += (source_len + dest_len + 1) / 2;

		*xp->b_wptr++ = fac_len + add_len;

		bcopy (fac, xp->b_wptr, fac_len);
		xp->b_wptr += fac_len;

		bcopy (add, xp->b_wptr, add_len);
		xp->b_wptr += add_len;

		if ((rp = unlinkb (mp))) linkb (xp, rp);

		top->ack = mp;	/* Save for T_OK_ACK */
		top->call = xp;	/* save for later */

		/* We have to ask xotlink to make a connection for us,
		   when it is done we will forward the connect request
		   to the remote X.25 device */

		cmd = (struct xot_cmd *) np->b_rptr;
		np->b_wptr = np->b_rptr + sizeof *cmd;

		cmd->cmd = XOT_CMD_OPEN;
		cmd->cookie = (long) top;
		cmd->index = 0;

		putnext (master->qptr, np);

		IDLE_TOP (master);
		
		top->state = TS_WACK_CREQ;
		
		return NULL;

	    nomem:
		IDLE_TOP (master);
		return mp;
		

	    case T_CONN_RES:	/* connection response,
				   the client program accepts a call. */

		TOPDEBUG ((top, "T_CONN_RES"));

		/* Find the original call on the list of inbound calls,
		   attach it's xlt block to the new stream specified
		   in the T_CONN_RES, then all is hunky-dory. */

		pl = LOCK (top->lck, plstr);

		if (!top->bound) {
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TOUTSTATE);
			return NULL;
		}

		end = top->queue + top->queue_max;
			
		for (pb = top->queue; pb < end; ++pb) {
			if (!(bot = *pb)) continue;
			pl2 = LOCK (bot->lck, plstr);
			if ((long) bot == tpi->conn_res.SEQ_number) break;
			UNLOCK (bot->lck, pl2);
		}

		if (!pb) {
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TBADSEQ);
			return NULL;
		}

		/* If remote didn't force p, w and facilities don't
		   include a p,w then we should add them here.
		   Of course it's a but for the remote not to specify
		   p,w but ... FIX? */
		   
		fac_len = tpi->conn_res.OPT_length;
		fac = mp->b_rptr + tpi->conn_res.OPT_offset;
		
		if (fac_len > 109 ||
		    !decode_fac (&bot->his, &bot->our, fac, fac_len)) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TBADOPT);
			return NULL;
		}

		if ((ulen = msgdsize (mp)) > 128) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TBADDATA);
			return NULL;
		}

		/* addr block + fac block */
		
		len = ((fac_len || ulen) ? 2 : 0) + fac_len;
		
		if (!(xp = alloc_x25 (q, bot, X25_CALL_CON, len, ulen))) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (bot->lck, pl);
			return mp;
		}

		if (fac_len || ulen) {
			*xp->b_wptr++ = 0;	/* address block */
			*xp->b_wptr++ = fac_len;
			bcopy (fac, xp->b_wptr, fac_len);
			xp->b_wptr += fac_len;
			if ((rp = unlinkb (mp))) linkb (xp, rp);
		}

		/* Delete bot from list of incoming calls */

		*pb = NULL;		/* Delete bot from incoming calls */
		--top->queue_len;

		/* Link call and new queue */
		
		t = (struct xot_top *) tpi->conn_res.QUEUE_ptr->q_ptr;

		/* *FIX* ... what if something happens on "t"? */
		
		bot->top = t;
		t->bot = bot;

		BUSY_BOT (bot);
		
		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);

		REPLY_T_OK ();
		mp = NULL;

		t->state = TS_DATA_XFER;

		/* and accept goes downstream */
				
		break;
		
	    case T_DISCON_REQ:	/* disconnect request     */

		TOPDEBUG ((top, "T_DISCON_REQ"));

		/* Either we're clearing an established call, or
		   we're rejecting an inbound call */

		pl = LOCK (top->lck, plstr);

		np = NULL;
		
		if (top->queue) {
			struct xot_bot **end = top->queue + top->queue_max;
			
			for (pb = top->queue; pb < end; ++pb) {
				if (!(bot = *pb)) continue;
				pl2 = LOCK (bot->lck, plstr);
				if ((long) bot == tpi->discon_req.SEQ_number)
					break;
				UNLOCK (bot->lck, pl2);
			}

			if (pb == end) {
				UNLOCK (top->lck, pl);
				REPLY_T_ERROR (TBADSEQ);
				return NULL;
			}

			/* allocate message needed to clear TCP call */
			
			if (!(np = allocate_clear (q))) {
				UNLOCK (bot->lck, pl2);
				UNLOCK (top->lck, pl);
				return mp;
			}
		}
		else {
			if (!(bot = top->bot)) {
				UNLOCK (top->lck, pl);
				REPLY_T_ERROR (TOUTSTATE);
				return NULL;
			}

			pl2 = LOCK (bot->lck, plstr);
		}

		if ((ulen = msgdsize (mp)) > 128) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			if (np) freemsg (np);
			REPLY_T_ERROR (TBADDATA);
			return NULL;
		}

		len = 1 + (ulen ? 3 : 0);

		if (!(xp = alloc_x25 (q, bot, X25_CLR_REQ, len, ulen))) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			if (np) freemsg (np);
			return mp;
		}

		*xp->b_wptr++ = 0;		/* Cause */

		if (ulen) {
			*xp->b_wptr++ = 0;	/* Diag */
			*xp->b_wptr++ = 0;	/* addresses */
			*xp->b_wptr++ = 0;	/* facilities */
			if ((rp = unlinkb (mp))) linkb (xp, rp);
		}

		BUSY_BOT (bot);
		
		if (top->queue) {
			if (--top->queue_len) {
				/* incoming connection pending */
				top->state = TS_WRES_CIND; 
			}
			else {
				top->state = TS_IDLE;
			}
			*pb = NULL;
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			REPLY_T_OK ();
			mp = NULL;

			/* The TCP call will be cleared when we get the
			   CLR_CON from the network */
		}
		else {
			top->state = TS_WACK_DREQ6;
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			
			/* The OK_ACK will be generated when we get the
			   CLR_CON from the network*/
		}
		
		break;
		
	    case T_EXDATA_REQ:	/* expedited data request, 
				   This becomes an X.25 INT_REQ */

		TOPDEBUG ((top, "T_EXDATA_REQ"));

		pl = LOCK (top->lck, plstr);

		if (!(bot = top->bot)) {
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TOUTSTATE);
			return NULL;
		}

		len = msgdsize (mp);

		if (!len || len > 32) {
			UNLOCK (top->lck, pl);
			REPLY_T_ERRNO (EPROTO);
			return NULL;
		}
				
		/* Don't allow 2 interrupts at once */

		pl2 = LOCK (bot->lck, plstr);
		
		if (bot->interrupt) {
			noenable (q);
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			return mp;
		}

		if (!(xp = alloc_x25 (q, bot, X25_INT_REQ, 0, len))) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			return mp;
		}
		
		if (!(np = unlinkb (mp))) {
			UNLOCK (bot->lck, pl2);
			UNLOCK (top->lck, pl);
			freemsg (xp);
			REPLY_T_ERRNO (EPROTO);
			return NULL;
		}

		bot->interrupt = 1;

		BUSY_BOT (bot);

		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);

		linkb (xp, np);

		break;
		
	    case T_INFO_REQ:	/* information request    */

		TOPDEBUG ((top, "T_INFO_REQ"));

		if (!(np = allocate (q, sizeof tpi->info_ack))) return mp;

		np->b_wptr = np->b_rptr + sizeof tpi->info_ack;
		np->b_datap->db_type = M_PCPROTO;

		tpi = (union T_primitives *) np->b_rptr;

		tpi->info_ack.PRIM_type = T_INFO_ACK;
		tpi->info_ack.TSDU_size = -1;
		tpi->info_ack.ETSDU_size = 32;
		tpi->info_ack.CDATA_size = 128;
		tpi->info_ack.DDATA_size = 128;
		tpi->info_ack.ADDR_size = 40;	/* Should be 15? *WHY* */
		tpi->info_ack.OPT_size = 109;

		pl = LOCK (top->lck, plstr);
		if ((bot = top->bot)) {
			tpi->info_ack.TIDU_size = bot->our.p;
		}
		else {
			tpi->info_ack.TIDU_size = top->p;
		}
		UNLOCK (top->lck, pl);
		
		tpi->info_ack.SERV_type = T_COTS;

		tpi->info_ack.CURRENT_state = top->state;
			
		tpi->info_ack.PROVIDER_flag = 0;

		qreply (q, np);
		
		break;
		
	    case O_T_BIND_REQ:	/* old (TLI) bind request */
	    case T_BIND_REQ:	/* new (XTI) bind request */

		TOPDEBUG ((top, "T_BIND_REQ"));

		if (!(np = allocate (q, sizeof tpi->ok_ack))) return mp;

		/* Check address format */

		dest = mp->b_rptr + tpi->bind_req.ADDR_offset;
		dest_len = tpi->bind_req.ADDR_length;

		if (!validate_addr (dest, dest_len)) {
			freemsg (np);
			REPLY_T_ERROR (TBADADDR);
			return NULL;
		}

		pl = LOCK (top->lck, plstr);
		
		if (top->bound) {
			UNLOCK (top->lck, pl);
			freemsg (np);
			REPLY_T_ERROR (TOUTSTATE);
			return NULL;
		}

		master = &xot_top [top->major];

		if ((len = tpi->bind_req.CONIND_number)) {

			struct xot_bot **queue;
			int size = len * sizeof *queue;

			if (!(queue = kmem_zalloc (size, KM_NOSLEEP))) {
				UNLOCK (top->lck, pl);
				REPLY_T_ERRNO (ENOMEM);
				freemsg (np);
				return NULL;
			}
				
			pl2 = LOCK (master->lck, plstr);
		
			if (find_listen (master, dest, dest_len)) {
				UNLOCK (master->lck, pl2);
				UNLOCK (top->lck, pl);
				freemsg (np);
				kmem_free (queue, size);
				REPLY_T_ERROR (TADDRBUSY);
				return NULL;
			}
			
			top->binds = master->binds;
			master->binds = top;
			UNLOCK (master->lck, pl2);

			top->queue_len = 0;
			top->queue_max = len;
			top->queue = queue;
		}

		top->bound = mp;

		top->state = TS_IDLE;
		
		UNLOCK (top->lck, pl);
		
		/* Ack the bind (Can't use REPLY_T_OK 'cos we need mp) */
		
		ok_ack = (struct T_ok_ack *) np->b_rptr;
		np->b_wptr = np->b_rptr + sizeof *ok_ack;

		np->b_datap->db_type = M_PCPROTO;

		ok_ack->PRIM_type = T_OK_ACK;
		ok_ack->CORRECT_prim = tpi->type;

		qreply (q, np);

		return NULL;
		
	    case T_UNBIND_REQ:	/* unbind request	  */

		/* This is only legal if we're idle */
		
		TOPDEBUG ((top, "T_UNBIND_REQ"));

		pl = LOCK (top->lck, plstr);
		
		if (!top->bound || top->queue_len || top->state != TS_IDLE) {
			UNLOCK (top->lck, pl);
			REPLY_T_ERROR (TOUTSTATE);
			return NULL;
		}

		remove_bind (top);

		top->state = TS_UNBND;
		
		UNLOCK (top->lck, pl);

		REPLY_T_OK ();

		return NULL;
		
	    default:
		TOPDEBUG ((top, "Unsupported primitive %d", tpi->type));
		REPLY_T_ERROR (TNOTSUPPORT);
		return NULL;
	}

	if (xp) {
		/* !!! bot must be busy here */
		
		putnext (bot->qptr, xp);

		IDLE_BOT (bot);
	}
	
	if (mp) freemsg (mp);

	return NULL;

    data:

	/* If he told us to shut up then we can't send yet */

	pl = LOCK (top->lck, plstr);
	
	if (!(bot = top->bot)) {
		UNLOCK (top->lck, pl);
		freemsg (mp);
		return NULL;
	}

	pl2 = LOCK (bot->lck, plstr);
	
	if (FLOW_CONTROL (bot, our, his, 0)) {
		noenable (q);
		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);
		TOPDEBUG ((top, "we're flow controlled (pr=%d, ps=%d)",
			   bot->his.pr, bot->our.ps));
			   
		return mp;
	}

	if (!(xp = allocate (q, sizeof *head + 4))) {
		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);
		return mp;
	}

	/* Accumulate one packet's worth of data, putting the rest
	   aside 'till later */

	sp = NULL;
	
	if (tpi) {
		rp = mp;
		mp = unlinkb (rp);
		mbit = tpi->data_req.MORE_flag;
	}
	else {
		rp = NULL;
	}

	left = bot->his.p;	/* Packet length he accepts */
	
	for (np = mp; np; np = np->b_cont) {
		int len = np->b_wptr - np->b_rptr;
		if (len >= left) {
			mblk_t *cp;
			if (len > left) {
				if (!(cp = duplicate (q, np))) {
					UNLOCK (bot->lck, pl2);
					UNLOCK (top->lck, pl);
					if (rp) {
						linkb (rp, mp);
						return rp;
					}
					return mp;
				}
				cp->b_rptr += left;
				sp = cp;
				mbit = tpi != 0;
				np->b_wptr = np->b_rptr + left;
			}
			if ((cp = unlinkb (np))) {
				mbit = tpi != 0;
				if (sp) {
					linkb (sp, cp);
				}
				else {
					sp = cp;
				}
			}
			break;
		}
		left -= len;
	}

	if ((left = (bot->his.ps + 1) & bot->mask) != bot->our.pr) {
		TOPDEBUG ((top, "piggyback ack on data %d", left));
		bot->our.pr = left;
	}

	head = (struct xot_head *) xp->b_wptr;
	xp->b_wptr += sizeof *head;
	
	*xp->b_wptr++ = bot->gfi_lcg + qbit;
	*xp->b_wptr++ = bot->lcn;

	if (bot->mask >= 8) {
		*xp->b_wptr++ = bot->our.ps << 1;
		*xp->b_wptr++ = bot->our.pr << 1 + mbit;
	}
	else {
		*xp->b_wptr++ = X25_DATA (bot->our.pr, mbit, bot->our.ps);
	}

	len = xp->b_wptr - xp->b_rptr - sizeof *head;
	
	linkb (xp, mp);

	len += msgdsize (mp);

	head->version = 0;
	head->length = htons (len);

	TOPDEBUG ((top, "send data (%d bytes, pr=%d, ps=%d)",
		   len, bot->our.pr, bot->our.ps));

	bot->our.ps = (bot->our.ps + 1) & bot->mask;

	BUSY_BOT (bot);

	UNLOCK (bot->lck, pl2);
	UNLOCK (top->lck, pl);

	putnext (bot->qptr, xp);

	IDLE_BOT (bot);

	if (rp) {
		if (sp) {
			linkb (rp, sp);
			sp = rp;
		}
		else {
			freemsg (rp);
		}
	}

	if (sp) {
		mp = sp;
		goto data;
	}

	return NULL;
}
			





/*
 * Upper read service, pass data upstream.
 *
 */

int xotursrv (queue_t *q) {
	mblk_t *mp;
	struct xot_top *top = (struct xot_top *) q->q_ptr;
	struct xot_bot *bot;

	int window;
	int xoff = 0;

	mblk_t *xp;

	struct xot_head *head;

	pl_t pl, pl2;

	if (!top->flow &&
	    !(top->flow = allocate (q, sizeof *head + 4))) return 0;
	
	while ((mp = getq (q))) {
		if (!canputnext (q)) {
			putbq (q, mp);
			xoff = 1;
			break;
		}
		putnext (q, mp);
	}

	pl = LOCK (top->lck, plstr);

	if (!(bot = top->bot)) {
		/* We've been unlinked */
		TOPDEBUG ((top, "ursrv: bottom gone"));
		UNLOCK (top->lck, pl);
		return 0;
	}

	pl2 = LOCK (bot->lck, plstr);

	if (!bot->flags) {
		/* Drat, it's been unlinked */
		TOPDEBUG ((top, "ursrv: bottom unlinked"));
		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);
		return 0;
	}

	if (!xoff && !FLOW_CONTROL (bot, his, our, 1)) {
		/* No need to bother anyway */
		UNLOCK (bot->lck, pl2);
		UNLOCK (top->lck, pl);
		return 0;
	}

	bot->our.xoff = xoff;
	
	xp = top->flow;
	top->flow = NULL;

	head = (struct xot_head *) xp->b_wptr;
	xp->b_wptr += sizeof *head;

	head->version = 0;
	
	*xp->b_wptr++ = bot->gfi_lcg;
	*xp->b_wptr++ = bot->lcn;

	bot->our.pr = (bot->his.ps + 1) & bot->mask; /* Ack the packets */
	
	if (bot->mask >= 8) {
		*xp->b_wptr++ = xoff ? X25_RNR (0) : X25_RR (0);
		*xp->b_wptr++ = bot->our.pr << 1;
		head->length = htons (4);
	}
	else {
		*xp->b_wptr++ = xoff ?
			X25_RNR (bot->our.pr) : X25_RR (bot->our.pr);
		head->length = htons (3);
	}

	TOPDEBUG ((top, xoff ? "send RNR %d" : "send RR %d",
		   bot->our.pr));

	BUSY_BOT (bot);

	UNLOCK (bot->lck, pl2);
	UNLOCK (top->lck, pl);

	putnext (bot->qptr, xp);

	IDLE_BOT (bot);
	
	return 0;
}



/*
 * Incoming data from one of our TCP connections.
 *
 * First we have to split into individual X.25 packets.
 * 
 * If we're connected to an upper XOT device then all well and
 * good (modulo flow control &c).
 *
 * Otherwise it better be an incoming call, in which case we
 * look for someone to handle it.
 *
 *
 */


int xotlrsrv (queue_t *q) {

	struct xot_bot *bot = (struct xot_bot *) q->q_ptr;
	mblk_t *mp;
	union T_primitives *tpi;
	struct xot_top *top;
	pl_t pl;
	
	/* How does this happen - we end up in the lower service
	   routine BEFORE THE LOWER STREAM HAS BEEN LINKED! */

	if (bot < xot_bot ||
	    bot >= xot_bot + xot_cnt ||
	    RD (bot->qptr) != q)
	{
		cmn_err (CE_WARN, "lrsrv(%x) %x %d",
			q, bot, bot->major);
		return 0;
	}
	
	pl = LOCK (bot->lck, plstr);

	if (!bot->flags) {
		BOTDEBUG ((bot, "lrsrv: unlinked"));
		UNLOCK (bot->lck, pl);
		return 0;
	}

	BUSY_BOT (bot);

	UNLOCK (bot->lck, plstr);
	
	while ((mp = getq (q))) {
		mblk_t *np;
		
		switch (mp->b_datap->db_type) {
		    case M_DATA:
			if ((mp = send_upstream (q, mp))) putbq (q, mp);
			break;
		
		    case M_PROTO:
		    case M_PCPROTO:
			tpi = (union T_primitives *) mp->b_rptr;

			switch (tpi->type) {
			    case T_DATA_IND:
				if ((np = unlinkb (mp))) 
					if ((np = send_upstream (q, np)))
						putbq (q, np);

				freemsg (mp);
				break;

			    case T_DISCON_IND:
			    case T_ORDREL_IND:
				/* The TCP call has cleared */
				BOTDEBUG ((bot, "lrsrv(%x): T_DISCON_IND", q));
				mp->b_datap->db_type = M_HANGUP;
				goto upstream;

			    default:
				cmn_err (CE_WARN, "top: Unexpected tli msg %d",
				 tpi->type);
				mp->b_datap->db_type = M_ERROR;
				mp->b_wptr = mp->b_rptr;
				*mp->b_wptr++ = EPROTO;
				goto upstream;
			}
			break;

		    case M_HANGUP:
			BOTDEBUG ((bot, "lrsrv: HANGUP"));
			goto upstream;
		
		    case M_ERROR:
			BOTDEBUG ((bot, "lrsrv: ERROR %d", *mp->b_rptr));
			goto upstream;

		    case M_FLUSH:
			BOTDEBUG ((bot, "lrsrv: FLUSH"));
			/* ... do something */
			freemsg (mp);
			break;

		    default:
			BOTDEBUG ((bot, "lsrv; unexpected %x",
				   mp->b_datap->db_type));
			freemsg (mp);
			break;
		}
	}

	IDLE_BOT (bot);
	return 0;

    upstream:

	/* Ask for an unlink */

	clear_call (NULL, bot, NULL);

	IDLE_BOT (bot);

	/* If we have a top side then pass the message on up */

	pl = LOCK (bot->lck, plstr);

	if (!(top = bot->top)) {
		UNLOCK (bot->lck, pl);
		goto free;
	}

	UNLOCK (bot->lck, pl);

	/* Ugly 'cos we're not allowed to lock top after bot */

	pl = LOCK (top->lck, plstr);

	if (top->bot != bot) {
		UNLOCK (top->lck, pl);
		goto free;
	}

	BUSY_TOP (top);
	
	UNLOCK (top->lck, pl);

	putnext (top->qptr, mp);

	IDLE_TOP (top);

	return 0;

    free:

	BOTDEBUG ((bot, "lrsrv: top closed, ignore"));
	freemsg (mp);

	return 0;
}



mblk_t *send_upstream (queue_t *q, mblk_t *mp) {

	struct xot_bot *bot = q->q_ptr;
	pl_t pl;
	
	mblk_t *np;
	
	for (; mp; mp = np) {
		
		struct xot_head *xot_head;
		int len;
		int need;

		uchar_t *p, *e;
		int gfi;
		int lcn;
		int type;

		int pr;
		int ps;

		union T_primitives *tpi;
		mblk_t *tp = NULL;

		uchar_t source_len, dest_len;
		uchar_t fac_len;
		uchar_t *fac;

		int window;

		mblk_t *ap;

		struct xot_top *top;
		struct xot_top *master;

		struct xot_bot **pb;

		int busy = 0;

		if (bot->partial) {
			linkb (bot->partial, mp);
			mp = bot->partial;
			bot->partial = NULL;
		}

		if ((len = msgdsize (mp)) < sizeof *xot_head) {
			bot->partial = mp;
			return NULL;
		}

		if (!pullup (q, &mp, len)) return mp;

		xot_head = (struct xot_head *) mp->b_rptr;

		if (xot_head->version) {
			if (!clear_call (q, bot, NULL)) return mp;
			cmn_err (CE_NOTE, "Bad xot version %d",
				 ntohs (xot_head->version));
			freemsg (mp);
			return NULL;
		}
		
		need = ntohs (xot_head->length) + sizeof *xot_head;

		if (msgdsize (mp) < need) {
			bot->partial = mp;
			return NULL;
		}

		if (!pullup (q, &mp, need)) return mp;
		
		len = mp->b_wptr - mp->b_rptr;
	
		if (len == need) {
			/* save any remainder blocks */
			np = unlinkb (mp);
		}
		else {
			/* Got too much, split */
			mblk_t *rp;
			if (!(np = duplicate (q, mp))) return mp;
			if ((rp = unlinkb (mp))) linkb (np, rp);
			np->b_rptr += need;
			mp->b_wptr = mp->b_rptr + need;
		}
		
		/* Ok, mp is a nice X.25 message, handle it */

		p = mp->b_rptr + sizeof *xot_head;
		e = mp->b_rptr + need;

		gfi =  *p++;
		lcn =  *p++;
		type = *p++;

		/* Handle incoming calls */

		if (type == X25_CALL_IND) {

			struct T_conn_ind *conn_ind;
			struct T_bind_req *bind_req;
			
			uchar_t dest [16];

			uchar_t cause;
			uchar_t diag;

			BOTDEBUG ((bot, "X25_CALL_IND"));

			if (bot->top) {
				/* Incoming call, but bot already in use */
				goto teardown;
			}

			/* Get length of source & dest addrs */
			
			source_len = *p++;
			dest_len = source_len & 0x0f;
			source_len >>= 4;

			/* Extract called address */

			extract_addr (p, 0, dest_len, dest);

			/* get length of facilities */

			fac = p + (source_len + dest_len + 1) / 2;

			if (fac >= e) {
				fac_len = 0;
			}
			else {
				fac_len = *fac++;
			}

			master = xot_top + bot->major;
			
			bot->our.p = bot->his.p = master->p;
			bot->our.w = bot->his.w = master->w;

			if (!decode_fac (&bot->his, &bot->our, fac, fac_len))
			{
				/* Should send back CLR_REQ and clear the
				   TCP/IP call. */
				BOTDEBUG ((bot, "bad facilities"));
				cause = X25_CAUSE_ERR;
				diag = X25_DIAG_BAD_FAC_LEN;
				goto reject;
			}

			if ((gfi & 0x30) == 0x20) {
				bot->mask = 0x7f;
			}
			else {
				bot->mask = 0x07;
			}

			bot->gfi_lcg = gfi & 0x3f;
			bot->lcn = lcn;

			if (bot->his.w > 7 || bot->our.w > 7) {
				if (bot->mask < 8) {
					/* Should send back CLR_REQ and
					   clear the TCP/IP call */
					BOTDEBUG ((bot, "bad window %d,%d",
						   bot->his.w, bot->our.w));
					cause = X25_CAUSE_ERR;
					diag = X25_DIAG_BAD_FAC_PARM;
					goto reject;
				}
			}

			/* Allocate a conn_ind */

			len = sizeof *conn_ind + source_len + fac_len;
			
			if (!(tp = allocate (q, len))) {
				if (np) linkb (mp, np);
				return mp;
			}

			/* find listen */

			master = xot_top + bot->major;	/* safe as bot busy */
			
			pl = LOCK (master->lck, plstr);
			
			if (!(top = find_listen (master, dest, dest_len)))
			{
				UNLOCK (master->lck, pl);
				BOTDEBUG ((bot, "can't find listen"));
				cause = X25_CAUSE_NP;
				diag = 0;
				goto reject;
			}

			UNLOCK (master->lck, pl);

			pl = LOCK (top->lck, plstr);

			/* ... sync with close; *FIX* */

			if (top->queue_len >= top->queue_max) {
				/* too many unaccepted calls */
				/* Send back CLR_REQ and clear the TCP/IP
				   call */
				UNLOCK (top->lck, pl);
				TOPDEBUG ((top, "too many calls"));
				cause = X25_CAUSE_OCC;
				diag = 0;
				goto reject;
			}

			for (pb = top->queue; *pb; ++pb);

			*pb = bot;
			++top->queue_len;

			/* incoming connection pending */
			top->state = TS_WRES_CIND;

			BUSY_TOP (top);
			++busy;
			
			UNLOCK (top->lck, pl);

			tp->b_datap->db_type = M_PROTO;
			
			conn_ind = (struct T_conn_ind *) tp->b_rptr;

			tp->b_wptr += sizeof *conn_ind;
			
			conn_ind->PRIM_type = T_CONN_IND;

			conn_ind->SEQ_number = (long) bot;
			
			conn_ind->SRC_length = source_len;
			conn_ind->SRC_offset = tp->b_wptr - tp->b_rptr;

			extract_addr (p, dest_len, source_len, tp->b_wptr);
			tp->b_wptr += source_len;

			conn_ind->OPT_length = fac_len;
			conn_ind->OPT_offset = tp->b_wptr - tp->b_rptr;
			
			bcopy (fac, tp->b_wptr, fac_len);
			tp->b_wptr += fac_len;

			if ((p = fac + fac_len) < e) {
				mp->b_rptr = p;
				linkb (tp, mp);
				mp = NULL;
			}

			bot->top = top;
			
			bot->our.ps = 0;
			bot->his.pr = 0;
			bot->our.pr = 0;
			bot->his.ps = bot->mask;
			bot->our.xoff = 0;
			bot->his.xoff = 0;

			goto upstream;

			/* Get here to reject X.25 call.  reuse inbound
			   call packet as the reject. */
		
		    reject:

			mp->b_wptr = mp->b_rptr + sizeof *xot_head + 2;

			*mp->b_wptr++ = X25_CLR_REQ;
			*mp->b_wptr++ = cause;
			*mp->b_wptr++ = diag;

			xot_head->length = htons (5);

			qreply (q, mp);

			mp = NULL;

			/* Now we wait for the incoming CLR CNF and
			   hang up the call when done */

			continue;
		}

		/* Check lcn, gfi - ok, don't check lcn 'cos cisco are
		   slobs, they don't keep a good lcn. */

#if 0
		/* ... should check gfi, fixme */
		
		if ((gfi & 0x3f) != bot->gfi_lcg || lcn != bot->lcn) {
			/* Eurgh! */
			cmn_err (CE_WARN,
				 "got bad gfi %x lcn %d want gfi %x lcn %d",
				 gfi & 0x3f, lcn,
				 bot->gfi_lcg, bot->lcn);
			goto teardown;
		}
#endif

		/* Ok, now we must have a top, check */

		pl = LOCK (bot->lck, plstr);
		
		if (!(top = bot->top)) {
			UNLOCK (bot->lck, pl);
			goto topless;
		}
			
		/* Ugerly, we're not allowed to lock top if we have bot
		   locked, so this complexity */

		UNLOCK (bot->lck, pl);

		LOCK (top->lck, plstr);

		if (top->bot != bot) {
			UNLOCK (top->lck, pl);

		    topless:
			if (type == X25_CLR_CON) {
				BOTDEBUG ((bot, "got X25_CLR_CON"));
			}
			else {
				cmn_err (CE_NOTE, "Unexpected X25 %x", type);
			}
			
			goto teardown;
		}
			
		BUSY_TOP (top);
		++busy;

		UNLOCK (top->lck, pl);

		/* Allocate a TLI thingy */

		if (!(tp = allocate (q, sizeof *tpi + 128))) goto nomem;

		tp->b_datap->db_type = M_PROTO;
		
		tpi = (union T_primitives *) tp->b_rptr;
		
		if (!(type & 1)) {	/* Data */
			int m;
			int q;

			if (bot->mask >= 8) {
				pr = *p++ >> 1;
				m = *p++;
				ps = m >> 1;
				m &= 1;
			}
			else {
				m = type & 0x10;
				pr = type >> 5;
				ps = (type >> 1) & 0x07;
			}

			BOTDEBUG ((bot, "got DATA (pr=%d, ps=%d)", pr, ps));

			bot->his.ps = (bot->his.ps + 1) & bot->mask;

			if (bot->his.ps != ps) {
				BOTDEBUG ((bot, "his ps = %d not %d",
					   ps, bot->his.ps));
				/* ... should send reset here */
				bot->his.ps = ps;
			}
			
			bot->his.pr = pr;

			/* If he's relaxed our flow control then we'd
			   better send him more data, enable write
			   side of upper queue */
			
			if (!FLOW_CONTROL (bot, our, his, 0)) {
				queue_t *upper = WR (top->qptr);
				if (!canenable (upper)) {
					BOTDEBUG ((bot, "relax flow control"));
					enableok (upper);
				}
				qenable (upper);
			}

			tpi->type = T_DATA_IND;
			
#if defined T_QDATA_IND
			if (gfi & X25_QBIT) tpi->type = T_QDATA_IND;

#endif
			tpi->data_ind.MORE_flag = m != 0;

			mp->b_rptr = p;

			tp->b_wptr = tp->b_rptr + sizeof tpi->data_ind;

			linkb (tp, mp);

			mp = NULL;

			/* Send data upstream, bung it on upper read
			   queue where service routine will handle
		           flow controlling the remote end.
			*/

			putq (top->qptr, tp);

			tp = NULL;
		}
		else switch (type) {
			
		    case X25_CALL_CON:
			/* Network tells us our call has worked,
			   Make a T_CONN_CON, send upstream */

			BOTDEBUG ((bot, "got X25_CALL_CON"));

			tpi->type = T_CONN_CON;

			tp->b_wptr = tp->b_rptr + sizeof tpi->conn_con;
			
			/* Get length of source & dest addrs */
			
			source_len = *p++;
			dest_len = source_len & 0x0f;
			source_len >>= 4;

			/* Extract called address */

			tpi->conn_con.RES_offset = tp->b_wptr - tp->b_rptr;
			tpi->conn_con.RES_length = dest_len;
			
			extract_addr (p, 0, dest_len, tp->b_wptr);

			tp->b_wptr += dest_len;

			/* Extract facilities */

			fac = p + (source_len + dest_len + 1) / 2;

			if (fac >= e) {
				fac_len = 0;
			}
			else {
				fac_len = *fac++;
			}

			/* See if remote has changed the flow control params.
			   we should check the changes they want, but we're
			   too lazy */
			
			decode_fac (&bot->his, &bot->our, fac, fac_len);

			tpi->conn_con.OPT_offset = tp->b_wptr - tp->b_rptr;
			tpi->conn_con.OPT_length = fac_len;

			bcopy (fac, tp->b_wptr, fac_len);

			tp->b_wptr += fac_len;
			
			/* Copy user data if any */
			
			if ((p = fac + fac_len) < e) {
				mp->b_rptr = p;
				linkb (tp, mp);
				mp = NULL;
			}

			top->state = TS_DATA_XFER;

			break;

		    case X25_CLR_IND:
			/* Network informs us that call is clearing,
			   Make a T_DISCON_IND, send upstream */

			BOTDEBUG ((bot, "got X25_CLR_IND"));

			/* allocate block for reply */
			
			if (!(ap = alloc_x25 (q, bot, X25_CLR_CON, 0, 0)))
				goto nomem;

			tpi->type = T_DISCON_IND;

			tp->b_wptr = tp->b_rptr + sizeof tpi->discon_ind;
			
			tpi->discon_ind.DISCON_reason = *p++ << 8;

			if (p < e)
				tpi->discon_ind.DISCON_reason += *p++;

			if (p < e) {

				/* Skip address block */
				
				source_len = *p++;
				dest_len = source_len & 0x0f;
				source_len >>= 4;

				p += (source_len + dest_len + 1) / 2;

				/* Skip facilities */
				
				if (fac >= e) {
					fac_len = 0;
				}
				else {
					fac_len = *p++;
				}

				p += fac_len;

				/* Copy user data */

				if (p < e) {
					mp->b_rptr = p;
					linkb (tp, mp);
					mp = NULL;
				}
			}

			/* If this is an as-yet-unaccepted call failing
			   we need to set the sequence number */
			
			if (top->bot != bot) {
				tpi->discon_ind.SEQ_number = (long) bot;
			}
			else {
				/* disconnect bot from top? */
				tpi->discon_ind.SEQ_number = -1;
			}
			
			/* send a CLR CONF back to the network */

			BOTDEBUG ((bot, "send X25_CLR_CON"));
			
			qreply (q, ap);

			/* Wait for other end to clear the TCP call */

			top->state = TS_WACK_DREQ6;
				
			break;
			
		    case X25_CLR_CON:
			/* Network confirms our clear, 
			   Send T_OK_ACK (T_DISCON_REQ) upstream */

			BOTDEBUG ((bot, "got X25_CLR_CON"));

			tpi->type = T_OK_ACK;
			tpi->ok_ack.CORRECT_prim = T_DISCON_REQ;

			tp->b_datap->db_type = M_PCPROTO;
			tp->b_wptr = tp->b_rptr + sizeof tpi->ok_ack;

			if (!clear_call (q, bot, NULL)) goto nomem;

			top->state = TS_IDLE;
			
			break;
			
		    case X25_INT_IND:
			/* Network tells us an interrupt has happened,
			   Send upstream as a T_EXPDATA_IND */

			BOTDEBUG ((bot, "got X25_INT_IND"));

			/* allocate block for reply */
			
			if (!(ap = alloc_x25 (q, bot, X25_INT_CON, 0, 0)))
				goto nomem;
			
			tpi->type = T_EXDATA_IND;
			tpi->exdata_ind.MORE_flag = 0;

			mp->b_rptr = p;

			tp->b_wptr = tp->b_rptr + sizeof tpi->exdata_ind;

			linkb (tp, mp);

			mp = NULL;

			qreply (q, ap);
			
			break;
			
		    case X25_INT_CON:
			/* Network confirms our INT, we can send more
			   if we want */

			BOTDEBUG ((bot, "got X25_INT_CON"));

			bot->interrupt = 0;

			enableok (WR (top->qptr));
			qenable (WR (top->qptr));
		
			freemsg (tp);
			tp = NULL;
			break;
			
		    case X25_RNR(0): case X25_RNR(1):
		    case X25_RNR(2): case X25_RNR(3):
		    case X25_RNR(4): case X25_RNR(5):
		    case X25_RNR(6): case X25_RNR(7):
			/* Network wants us to shut up */
			bot->his.xoff = 1;
			goto rr;
			
		    case X25_RR(0): case X25_RR(1):
		    case X25_RR(2): case X25_RR(3):
		    case X25_RR(4): case X25_RR(5):
		    case X25_RR(6): case X25_RR(7):
			/* Network ACK's our data */
			bot->his.xoff = 0;

		    rr:
			if (bot->mask >= 8) {
				pr = *p++ >> 1;
			}
			else {
				pr = type >> 5;
			}

			BOTDEBUG ((bot,
				   bot->his.xoff ? "got RNR %d" : "got RR %d",
				   pr));

			bot->his.pr = pr;

			freemsg (tp);
			tp = NULL;

			/* We should retry any send if we are not now flow
			   controlled */

			if (!FLOW_CONTROL (bot, our, his, 0)) {
				queue_t *upper = WR (top->qptr);
				if (!canenable (upper)) {
					BOTDEBUG ((bot, "flow relaxed"));
					enableok (upper);
				}
				qenable (upper);
			}

			break;

		    case X25_RST_IND:
			/* Network tells us the VC has been reset */

			BOTDEBUG ((bot, "got X25_RST_IND"));

			/* allocate block for confirm */
			
			if (!(ap = alloc_x25 (q, bot, X25_RST_CON, 0, 0)))
				goto nomem;

			qreply (q, ap);

			/* Need devnet extension to signal app... *FIX* */

			freemsg (tp);
			tp = NULL;
			
			break;

		    default:
			/* Unhandled packet type,
			   clear the TCP call, complain.
			   Should do a reset?
			*/

			BOTDEBUG ((bot, "got x25 %x", type)); 
			
			cmn_err (CE_NOTE, "Unknown X25 packet %x", type);
			
			clear_call (q, bot, tp);
			tp = NULL;
			
			break;
		}

	    upstream:
		
		/* Send tp upstream */

		if (tp) putnext (top->qptr, tp);

		/* Clean up */
		
		if (busy) IDLE_TOP (top);

		if (mp) freemsg (mp);

		continue;

		/* Get here to clear TCP/IP call */

	    teardown:

		if (!(clear_call (q, bot, NULL))) goto nomem;

		if (tp) freemsg (tp);
		if (mp) freemsg (mp);
		if (np) freemsg (np);

		if (busy) IDLE_TOP (top);
		
		return NULL;

		/* Get here on allocation failure. */
		
	    nomem:
		if (tp) freemsg (tp);

		if (busy) IDLE_TOP (top);
		
		if (np) linkb (mp, np);
		
		return mp;
	}

	return NULL;
}



static void extract_addr (uchar_t *ablock, int offset, int len, uchar_t *addr)
{
	ablock += (offset / 2);
	while (len--) {
		uchar_t ch = *ablock;
		if (offset & 1) {
			ch &= 0x0f;
			++ablock;
			offset = 0;
		}
		else {
			ch >>= 4;
			offset = 1;
		}
		*addr++ = '0' + ch;
	}
}



static void insert_addr (uchar_t *ablock, int offset, int len, uchar_t *addr)
{
	uchar_t out = 0;

	if (!len) return;
	
	ablock += (offset / 2);

	if (offset & 1) out = *ablock & 0xf0;

	do {
		uchar_t ch = *addr++ - '0';
		
		if (offset & 1) {
			*ablock++ = ch | out;
			offset = 0;
		}
		else {
			out = (ch << 4);
			offset = 1;
		}
	}
	while (--len);

	if (offset & 1) *ablock = out;
}


static int validate_addr (uchar_t *addr, int addr_len) {
	if (addr_len > 15) return 0;

	while (addr_len--) {
		uchar_t ch = *addr++;
		if (ch < '0' || ch > '9') return 0;
	}

	return 1;
}




static struct xot_top *find_listen (struct xot_top *master,
				    uchar_t *addr, int len)
{
	struct xot_top *top;
	for (top = master->binds; top; top = top->binds) {
		struct T_bind_req *bind;
		if (!top->bound) continue;
		bind = (struct T_bind_req *) top->bound->b_rptr;
		if (bind->ADDR_length != len) continue;
		if (!bcmp (top->bound->b_rptr + bind->ADDR_offset, addr, len))
			break;
	}

	return top;
}




static int decode_fac (struct xot_flow *called,
		       struct xot_flow *calling,
		       uchar_t *fac,
		       int fac_len)
{
	uchar_t *e = fac + fac_len;

	while (fac < e) {
		int code = *fac++;
		int len = 1 + (code >> 6);

		if (len == 4) {
			if (fac >= e) return 0;
			len = *fac++;
		}
		
		if (fac + len > e) return 0;

		switch (code) {
			
		    case X25_FAC_PACKET:

			/* The packet size for the direction of transmission
			   FROM the CALLED DTE is indicated in the first
			   octet of the parameter field */

			if ((called->p = 1 << fac[0]) < 16) return 0;
			if ((calling->p = 1 << fac[1]) < 16) return 0;
			
			break;
			
		    case X25_FAC_WINDOW:

			/* The window size for the direction of transmission
			   FROM the CALLED DTE is indicated in the first
			   octet of the parameter field */

			if (!(called->w = fac[0])) return 0;
			if (!(calling->w = fac[1])) return 0;
			
			break;
		}

		fac += len;
	}

	return 1; 	/* ok */
}


#if XOT_DEBUG

#define COPY_INT(_val)				\
do {						\
	unsigned _v = _val;			\
						\
	q = tmp; 				\
	do {					\
		*q++ = '0' + _v % 10;		\
		_v /= 10;		       	\
	}					\
	while (_v);				\
						\
	while (q > tmp) *p++ = *--q;		\
 						\
} while (0)


void topdebug (struct xot_top *top, char *fmt, ...) {
	va_list ap;

	int sid = 0;
	char format [128];

	int a1, a2, a3;

	if (top) {
		char tmp [10];	/* big enough for 4 byte unsigned */
		char *q;
		char *p = format;
		char *e = format + sizeof format - 2;
		char c;

#if 0
		if (!(top->flag & XOT_DEBUG)) return;
#endif
		
		sid = top - xot_top;
		
		*p++ = 'x';
		*p++ = 'o';
		*p++ = 't';
		COPY_INT (top->major);
		*p++ = '/';
		COPY_INT (sid);
		*p++ = ':';
		*p++ = ' ';

		while (p < e && (c = *fmt++)) *p++ = c;

#if DEBUG_TO_CONSOLE
		*p++ = '\n';
#endif
		*p++ = 0;

		fmt = format;
	}

	va_start (ap, fmt);
	
	a1 = va_arg (ap, int);
	a2 = va_arg (ap, int);
	a3 = va_arg (ap, int);

	va_end (ap);

#if DEBUG_TO_CONSOLE
	cmn_err (CE_CONT, fmt, a1, a2, a3);
#else
	strlog (XOT_MID, sid, LOG_DEBUG, SL_TRACE, fmt, a1, a2, a3);
#endif
}


void botdebug (struct xot_bot *bot, char *fmt, ...) {
	va_list ap;
	int a1, a2, a3;

	struct xot_top *top;

	if (!bot) {
		top = NULL;
	}
	else if (!(top = bot->top)) {
		top = xot_top + bot->major;
	}
	
	va_start (ap, fmt);
	
	a1 = va_arg (ap, int);
	a2 = va_arg (ap, int);
	a3 = va_arg (ap, int);

	va_end (ap);
	
	topdebug (top, fmt, a1, a2, a3);
}

#endif
