#ifndef I2S_MANAGER_H
#define I2S_MANAGER_H

#include <driver/i2s.h>

#ifdef __cplusplus
extern "C" {
#endif

void I2SSetup(void);
void I2SSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif // I2S_MANAGER_H