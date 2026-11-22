#ifndef BROUW_NETWERK_H
#define BROUW_NETWERK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BROUW_NETWERK_UIT,
    BROUW_NETWERK_SETUP,
    BROUW_NETWERK_WACHT_OP_IP,
    BROUW_NETWERK_IP_GEREED,
    BROUW_NETWERK_FOUT,
} brouw_netwerk_status_t;

typedef struct {
    char host[64];
    uint16_t poort;
    char gebruikersnaam[64];
    char wachtwoord[64];
} brouw_netwerk_mqtt_t;

esp_err_t brouw_netwerk_vraag_setup(void);
esp_err_t brouw_netwerk_vraag_werking(void);
esp_err_t brouw_netwerk_start(const char *setup_wachtwoord, bool setup_afdwingen);
brouw_netwerk_status_t brouw_netwerk_status(void);
esp_err_t brouw_netwerk_lees_mqtt(brouw_netwerk_mqtt_t *mqtt);
uint32_t brouw_netwerk_configuratienummer(void);
bool brouw_netwerk_verbindingspoging_klaar(void);
void brouw_netwerk_configuratie_opgeslagen(void);
esp_err_t brouw_netwerk_lees_ssid(char ssid[33]);

#ifdef __cplusplus
}
#endif

#endif
