/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_AUDIO_DEVICE_H
#define POKOPIA_AUDIO_DEVICE_H

#include "../../horizon/cmif.h"
#include "../../horizon/event.h"

bool pokopia_audio_device_request(uint32_t command, const HznCmifRequest *request,
                                  uint8_t *tls, size_t tls_size, HznEvents *events,
                                  void *(*write_guest)(void *, uint64_t, size_t));
#endif
