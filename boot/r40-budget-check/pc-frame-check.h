/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_PC_FRAME_CHECK_H
#define POKOPIA_PC_FRAME_CHECK_H

#include "live_draw.h"

void pokopia_pc_frame_check(PokopiaVkAdapter *adapter, const PokopiaVkTexture *texture, FILE *log);
void pokopia_pc_frame_draw(const PokopiaLiveDraw *draw, uint64_t serial, FILE *log);

#endif
