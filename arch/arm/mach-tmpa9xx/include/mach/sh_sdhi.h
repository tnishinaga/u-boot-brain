/* SPDX-License-Identifier: GPL-2.0 */
/* TMPA9xx-specific wrapper for the common SH-SDHI definitions. */
#ifndef __TMPA9XX_SH_SDHI_H
#define __TMPA9XX_SH_SDHI_H

#include <sh_sdhi.h>

/* TMPA910 requires bit 1 when releasing the SDHI reset. */
#undef SOFT_RST_OFF
#define SOFT_RST_OFF			(BIT(0) | BIT(1))

#endif /* __TMPA9XX_SH_SDHI_H */
