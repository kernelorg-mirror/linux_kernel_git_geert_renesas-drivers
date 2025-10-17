/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Constants for for the mailbox part of the Renesas MFIS IP core.
 */

#ifndef _DT_BINDINGS_MAILBOX_RENESAS_MFIS_H
#define _DT_BINDINGS_MAILBOX_RENESAS_MFIS_H

/*
 * MFIS HW design before r8a78001 requires a channel to be marked as either
 * TX or RX.
 */
#define MFIS_CHANNEL_TX	(0 << 0)
#define MFIS_CHANNEL_RX	(1 << 0)

/*
 * Some MFIS variants work with pairs of IICR and EICR registers. Usually, it
 * is specified in the datasheets which of the two a specific core should use.
 * For plain MFIS of r8a78000, this is selectable, though, according to the
 * system design and the firmware in use. These channels need to be marked.
 * This is not needed with other versions of the MFIS, not even with MFIS-SCP
 * of r8a78000.
 */
#define MFIS_CHANNEL_IICR	(0 << 1)
#define MFIS_CHANNEL_EICR	(1 << 1)

#endif
