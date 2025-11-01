#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

bool wifi_connect_blocking(void); // returns true if connected

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H