/*
 * board.h - per-revision board configuration selector.
 *
 * The build picks a hardware revision with a compile-time define (see Makefile
 * BOARD=r1|r2). All other modules include THIS header, never a board_rN.h
 * directly, so a single codebase builds both MIDEX8 revisions:
 *
 *   - r1 (default): EZ-USB AN2131 + 2x ST16C454, 8 external 16550 channels.
 *   - r2 (-DBOARD_R2): EZ-USB FX CY7C646, hybrid backend = 6 external 16550
 *     channels (ST16C454 + ST16C452) for ports 0-5 + 2 on-chip UARTs for
 *     ports 6-7. See board_r2.h and ../doc/midex8_r1_vs_r2.md.
 */
#ifndef BOARD_H
#define BOARD_H

#if defined(BOARD_R2)
#include "board_r2.h"
#else
#include "board_r1.h"
#endif

#endif /* BOARD_H */
