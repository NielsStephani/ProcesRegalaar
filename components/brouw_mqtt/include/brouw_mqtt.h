#ifndef BROUW_MQTT_H
#define BROUW_MQTT_H

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BROUW_MQTT_RECEPT_MAX_BYTES 4096

typedef struct {
    const char *message_id;
    const cJSON *data;
    cJSON *json;  /* Eigenaar van message_id en data. Vrijgeven via de functie hieronder. */
} brouw_mqtt_recept_t;

esp_err_t brouw_mqtt_init(const char *ketelnaam, bool automatisch_starten);
esp_err_t brouw_mqtt_start(void);
bool brouw_mqtt_is_verbonden(void);
/* true als MQTT verbonden is en de webserver retained "online" zette op brouwer/server/status */
bool brouw_mqtt_server_online(void);
esp_err_t brouw_mqtt_publiceer(const char *topic, const char *payload);
esp_err_t brouw_mqtt_ontvang_recept(brouw_mqtt_recept_t *recept, TickType_t wachttijd);
void brouw_mqtt_recept_vrijgeven(brouw_mqtt_recept_t *recept);
esp_err_t brouw_mqtt_ack(const char *message_id, bool ok, const char *fout);
esp_err_t brouw_mqtt_stop(void);
bool brouw_mqtt_verbindingspoging_mislukt(void);

#ifdef __cplusplus
}
#endif

#endif
