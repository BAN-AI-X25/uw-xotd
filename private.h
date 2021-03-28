#ident "@(#)private.h	4.2 - 02/12/17 15:45:56 - Private declarations for XOT"

/*
 * Copyright © 2002 Atlantic Technologies.
 * For conditions of distribution and use, see file LICENSE
 *
 */

#if XOT_DEBUG
#define TOPDEBUG(x) topdebug x
#define BOTDEBUG(x) botdebug x
#else
#define TOPDEBUG(x) do {} while (0)
#define BOTDEBUG(x) do {} while (0)
#endif

#include "version.h"

#define	X25_CALL_IND	0x0b
#define	X25_CALL_REQ	X25_CALL_IND
#define	X25_CALL_CON	0x0f
#define	X25_CLR_IND	0x13
#define	X25_CLR_REQ	X25_CLR_IND
#define	X25_CLR_CON	0x17

#define X25_DATA(r,m,s)	(((r) << 5) + ((m) << 4) + ((s) << 1) + 0)

#define	X25_INT_IND	0x23
#define X25_INT_REQ	X25_INT_IND
#define	X25_INT_CON	0x27

#define	X25_RR(n)	(((n) << 5) + 0x01)
#define	X25_RNR(n)	(((n) << 5) + 0x05)

#define	X25_RST_IND	0x1b
#define	X25_RST_CON	0x1f

/* Bits in GFI */

#define X25_FORMAT	0x30
#define X25_MOD_128	0x20
#define X25_MOD_8	0x10
#define X25_QBIT	0x80
#define X25_TOA		0x80

/* Facilities */

#define X25_FAC_PACKET	0x42
#define X25_FAC_WINDOW	0x43

/* Errors */

#define X25_CAUSE_ERR	0x13
#define X25_CAUSE_NP	0x0d
#define X25_CAUSE_OCC	0x01

#define X25_DIAG_BAD_FAC_LEN	0x45
#define X25_DIAG_BAD_FAC_PARM	0x42



/* TLI protocol stuff */


#define REPLY_T_SYSERR(_err, _sys)					\
do {									\
	tpi->error_ack.ERROR_prim = tpi->type;				\
									\
	tpi->error_ack.TLI_error = _err;				\
	tpi->error_ack.UNIX_error = _sys;				\
									\
	tpi->type = T_ERROR_ACK;					\
									\
	mp->b_wptr = mp->b_rptr + sizeof (tpi->error_ack);		\
									\
	mp->b_datap->db_type = M_PCPROTO;				\
									\
	qreply (q, mp);							\
} while (0)

#define REPLY_T_ERROR(_err)	REPLY_T_SYSERR (_err, 0)
#define REPLY_T_ERRNO(_err)	REPLY_T_SYSERR (TSYSERR, _err)
	
#define REPLY_T_OK()						\
do {								\
	tpi->ok_ack.CORRECT_prim = tpi->type;			\
								\
	tpi->type = T_OK_ACK;					\
								\
	mp->b_wptr = mp->b_rptr + sizeof (tpi->ok_ack);		\
								\
	mp->b_datap->db_type = M_PCPROTO;			\
								\
	qreply (q, mp);						\
} while (0)
