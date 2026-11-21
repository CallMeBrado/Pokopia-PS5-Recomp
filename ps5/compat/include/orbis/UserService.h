/* PS5 stand-in for OpenOrbis <orbis/UserService.h>. */
#ifndef POKOPIA_PS5_ORBIS_USERSERVICE_H
#define POKOPIA_PS5_ORBIS_USERSERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t sceUserServiceInitialize(void *);
int32_t sceUserServiceGetInitialUser(int32_t *);
int32_t sceUserServiceGetForegroundUser(int32_t *user_id);

#ifdef __cplusplus
}
#endif
#endif
