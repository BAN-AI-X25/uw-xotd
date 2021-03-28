#ident "@(#)Space.c	4.1 - 02/12/09 11:11:32 - Space.c file for xot"

/*
 * Copyright © 2002 Atlantic Technologies.
 * For conditions of distribution and use, see file LICENSE
 *
 */

#include <config.h>

#include <sys/stream.h>
#include <atlantic/xot.h>

/* 'Cos we're lazy we insist that the major #'s are consecutive */

#if XOT_CMAJORS > 8
#error XOT: TOO MANY MAJOR DEVICES
#endif

#if (XOT_CMAJORS > 1 && XOT_CMAJOR_1 != XOT_CMAJOR_0 + 1) || \
    (XOT_CMAJORS > 2 && XOT_CMAJOR_2 != XOT_CMAJOR_1 + 1) || \
    (XOT_CMAJORS > 3 && XOT_CMAJOR_3 != XOT_CMAJOR_2 + 1) || \
    (XOT_CMAJORS > 4 && XOT_CMAJOR_4 != XOT_CMAJOR_3 + 1) || \
    (XOT_CMAJORS > 5 && XOT_CMAJOR_5 != XOT_CMAJOR_4 + 1) || \
    (XOT_CMAJORS > 6 && XOT_CMAJOR_6 != XOT_CMAJOR_5 + 1) || \
    (XOT_CMAJORS > 7 && XOT_CMAJOR_7 != XOT_CMAJOR_6 + 1)
#error XOT: MAJOR DEVICE NUMBERS OUT OF ORDER
#endif

int xot_cnt = XOT_UNITS;

int xot_major = XOT_CMAJOR_0;

int xot_majors = XOT_CMAJORS;

/* Datastructures for upper (x.25) streams */

struct xot_top xot_top [XOT_UNITS];

/* Datastructures for lower (tcp) streams */

struct xot_bot xot_bot [XOT_UNITS];




