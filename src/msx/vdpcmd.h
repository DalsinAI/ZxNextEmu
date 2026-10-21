/* The V9938 command engine, and the VDP internals it shares with vdp.c. */
#ifndef MSX_VDPCMD_H
#define MSX_VDPCMD_H

#include "vdp.h"

int vdp_mode(const vdp *v);
int vdp_planar(const vdp *v);
uint32_t vdp_phys(const vdp *v, uint32_t a);
int vdp_in_display(const vdp *v, uint32_t tick, int *hblank);

/* A write to R32..R46 (R44 colour, R46 starts a command). */
void vdp_cmd_write(vdp *v, int reg, uint8_t val);
/* Run the engine up to this tick. */
void vdp_cmd_run(vdp *v, uint32_t tick);
/* The CPU read S#7 (LMCM takes the next pixel). */
void vdp_cmd_read_s7(vdp *v);

#endif
