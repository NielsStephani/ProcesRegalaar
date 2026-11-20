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

/* Eenmaal aanroepen na brouw_netwerk_start(). Kies true om automatisch te verbinden.
 * De MQTT-taak wacht zelf op een IP-adres. */
esp_err_t brouw_mqtt_init(const char *ketelnaam, bool automatisch_starten);

/* Vraagt MQTT aan als automatisch_starten false was of na brouw_mqtt_stop(). */
esp_err_t brouw_mqtt_start(void);

/* True zodra het MQTT-abonnement klaarstaat en de aanmelding is ingepland. */
bool brouw_mqtt_is_verbonden(void);

/* Gebruik het deel na brouwer/<id>/. Ondersteunde topics: sensor/temperatuur,
 * actuator/pomp/state, actuator/verwarming/state, ack en fout.
 * ESP_OK betekent dat ESP-MQTT het bericht heeft aangenomen voor verzending. */
esp_err_t brouw_mqtt_publiceer(const char *topic, const char *payload);

/* Leest een recept uit de wachtrij. Gebruik 0 om te pollen of pdMS_TO_TICKS(ms).
 * ESP_ERR_TIMEOUT betekent dat er geen bericht was. Geef elk ontvangen recept vrij.
 * Bij ESP_OK blijven message_id en data geldig tot brouw_mqtt_recept_vrijgeven(). */
esp_err_t brouw_mqtt_ontvang_recept(brouw_mqtt_recept_t *recept, TickType_t wachttijd);
void brouw_mqtt_recept_vrijgeven(brouw_mqtt_recept_t *recept);

/* Stuur na verwerking een ack met de ontvangen message_id. Geef bij ok=false een fouttekst mee. */
esp_err_t brouw_mqtt_ack(const char *message_id, bool ok, const char *fout);

/* Geplande afsluiting vanuit een taak. Meldt offline met QoS 1 voordat de client stopt.
 * ESP_ERR_TIMEOUT betekent dat de broker die melding niet bevestigde. */
esp_err_t brouw_mqtt_stop(void);

#ifdef __cplusplus
}
#endif

#endif
