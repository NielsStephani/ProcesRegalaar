#include "brouw_mqtt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "brouw_netwerk.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#define NETWERK_CHECK_MS 250
#define HERPROBEER_MS 2000
#define OFFLINE_ACK_TIMEOUT_MS 2000
#define RECEPT_WACHTRIJ_LENGTE 3
#define PUBLICATIE_MAX_BYTES 4096
#define RECEPT_QOS 2
#define SERVER_STATUS_TOPIC "brouwer/server/status"   /* retained online/offline van de webserver */
/* Recepten komen op brouwer/<mac>/cmd/recept (met envelop message_id/data)
 * of op brouwer/server/cmd/recept (soms zonder envelop). We luisteren op beide. */
#define RECEPT_TOPIC_SERVER "brouwer/server/cmd/recept"
#define START_TOPIC_SERVER "brouwer/server/cmd/start"
#define OPDRACHT_WACHTRIJ_LENGTE 4

enum {
    MQTT_SESSIE = BIT0,
    MQTT_GEREED = BIT1,
    MQTT_HERSTART = BIT2,
    STOP_GEVRAAGD = BIT3,
    STOP_KLAAR = BIT4,
    OFFLINE_BEVESTIGD = BIT5,
    OFFLINE_MISLUKT = BIT6,
    MQTT_INGESCHAKELD = BIT7,
};

typedef enum {
    TOPIC_INFO,
    TOPIC_STATUS,
    TOPIC_RECEPT,
    TOPIC_START,
    TOPIC_TEMPERATUUR,
    TOPIC_POMP,
    TOPIC_VERWARMING,
    TOPIC_ACK,
    TOPIC_FOUT,
    TOPIC_BROUW_STATUS,
    TOPIC_BROUW_MELDING,
    TOPIC_AANTAL,
} topic_t;

static const struct {
    const char *pad;
    int qos;
    bool retained;
} TOPICS[TOPIC_AANTAL] = {
    [TOPIC_INFO]        = { "info",                        1, true  },
    [TOPIC_STATUS]      = { "status",                      1, true  },
    [TOPIC_RECEPT]      = { "cmd/recept",                  RECEPT_QOS, false },
    [TOPIC_START]       = { "cmd/start",                   1, false },
    [TOPIC_TEMPERATUUR] = { "sensor/temperatuur",          0, false },
    [TOPIC_POMP]        = { "actuator/pomp/state",         1, true  },
    [TOPIC_VERWARMING]  = { "actuator/verwarming/state",   1, true  },
    [TOPIC_ACK]         = { "ack",                         1, false },
    [TOPIC_FOUT]        = { "fout",                        1, false },
    [TOPIC_BROUW_STATUS]  = { "brouw/status",              0, true  },   /* elke seconde van het scherm */
    [TOPIC_BROUW_MELDING] = { "brouw/melding",             1, false },   /* gebeurtenis: start, pauze, ... */
};

typedef struct {
    char *json;
    int lengte;
    int ontvangen;
} recept_buffer_t;

static const char *TAG = "brouw_mqtt";
static EventGroupHandle_t s_events;
static QueueHandle_t s_recepten;
static QueueHandle_t s_opdrachten;
static SemaphoreHandle_t s_client_mutex;
static SemaphoreHandle_t s_start_stop_mutex;
static esp_mqtt_client_handle_t s_client;
static char s_id[13];
static char s_client_id[sizeof("brouwer-") + 12];
static char s_topic[TOPIC_AANTAL][64];
static char *s_info_json;
static atomic_bool s_verbindingspoging_mislukt = false;
static atomic_bool s_server_online = false;

/* Alleen de MQTT-eventtaak gebruikt deze variabelen terwijl de client draait. */
static recept_buffer_t s_rx;
static int s_sub_id;
static int s_info_id;
static int s_offline_id;

bool brouw_mqtt_verbindingspoging_mislukt(void)
{
    return atomic_load(&s_verbindingspoging_mislukt);
}

static cJSON *voeg_benoemd_item_toe(cJSON *lijst, const char *naam)
{
    cJSON *item = cJSON_CreateObject();
    if (!cJSON_AddItemToArray(lijst, item)) {
        cJSON_Delete(item);
        return NULL;
    }
    return cJSON_AddStringToObject(item, "name", naam) != NULL ? item : NULL;
}

static bool voeg_waarde_toe(cJSON *lijst, const char *tekst)
{
    cJSON *waarde = cJSON_CreateString(tekst);
    if (cJSON_AddItemToArray(lijst, waarde)) {
        return true;
    }
    cJSON_Delete(waarde);
    return false;
}

static char *maak_info_json(const char *ketelnaam)
{
    char *tekst = NULL;
    cJSON *info = cJSON_CreateObject();
    if (info == NULL ||
        cJSON_AddStringToObject(info, "id", s_id) == NULL ||
        cJSON_AddStringToObject(info, "type", "ketel") == NULL ||
        cJSON_AddStringToObject(info, "name", ketelnaam) == NULL) {
        goto klaar;
    }

    cJSON *sensoren = cJSON_AddArrayToObject(info, "sensors");
    cJSON *actuatoren = cJSON_AddArrayToObject(info, "actuators");
    cJSON *opdrachten = cJSON_AddArrayToObject(info, "commands");
    if (sensoren == NULL || actuatoren == NULL || opdrachten == NULL) {
        goto klaar;
    }

    cJSON *temperatuur = voeg_benoemd_item_toe(sensoren, "temperatuur");
    cJSON *pomp = voeg_benoemd_item_toe(actuatoren, "pomp");
    cJSON *verwarming = voeg_benoemd_item_toe(actuatoren, "verwarming");
    if (temperatuur == NULL || pomp == NULL || verwarming == NULL ||
        cJSON_AddStringToObject(temperatuur, "unit", "°C") == NULL ||
        !voeg_waarde_toe(opdrachten, "recept") || !voeg_waarde_toe(opdrachten, "start")) {
        goto klaar;
    }

    cJSON *pompwaarden = cJSON_AddArrayToObject(pomp, "values");
    cJSON *verwarmingswaarden = cJSON_AddArrayToObject(verwarming, "values");
    if (pompwaarden == NULL || verwarmingswaarden == NULL ||
        !voeg_waarde_toe(pompwaarden, "on") || !voeg_waarde_toe(pompwaarden, "off")) {
        goto klaar;
    }
    for (unsigned procent = 0; procent <= 100; ++procent) {
        char waarde[4];
        snprintf(waarde, sizeof(waarde), "%u", procent);
        if (!voeg_waarde_toe(verwarmingswaarden, waarde)) {
            goto klaar;
        }
    }
    tekst = cJSON_PrintUnformatted(info);

klaar:
    cJSON_Delete(info);
    return tekst;
}

static int voeg_publicatie_toe(esp_mqtt_client_handle_t client, topic_t topic,
                               const char *payload)
{
    return esp_mqtt_client_enqueue(client, s_topic[topic], payload, 0,
                                   TOPICS[topic].qos, TOPICS[topic].retained, true);
}

static void verwijder_onvolledig_recept(void)
{
    free(s_rx.json);
    s_rx = (recept_buffer_t){0};
}

static bool topic_is(esp_mqtt_event_handle_t event, const char *topic)
{
    return event->topic_len == (int)strlen(topic) &&
           memcmp(event->topic, topic, (size_t)event->topic_len) == 0;
}

static void verzamel_recept(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        verwijder_onvolledig_recept();
        if (event->retain || event->topic == NULL ||
            (!topic_is(event, s_topic[TOPIC_RECEPT]) && !topic_is(event, RECEPT_TOPIC_SERVER))) {
            return;
        }
        if (event->total_data_len <= 0 || event->total_data_len > BROUW_MQTT_RECEPT_MAX_BYTES) {
            ESP_LOGW(TAG, "Ongeldige receptlengte (%d bytes).", event->total_data_len);
            return;
        }
        s_rx.json = malloc((size_t)event->total_data_len + 1);
        if (s_rx.json == NULL) {
            ESP_LOGW(TAG, "Onvoldoende geheugen voor het recept.");
            return;
        }
        s_rx.lengte = event->total_data_len;
    }

    if (s_rx.json == NULL) {
        return;
    }
    if (event->current_data_offset != s_rx.ontvangen || event->total_data_len != s_rx.lengte ||
        event->data == NULL || event->data_len <= 0 || event->data_len > s_rx.lengte - s_rx.ontvangen) {
        ESP_LOGW(TAG, "Ongeldig MQTT-fragment voor cmd/recept.");
        verwijder_onvolledig_recept();
        return;
    }

    memcpy(s_rx.json + s_rx.ontvangen, event->data, (size_t)event->data_len);
    s_rx.ontvangen += event->data_len;
    if (s_rx.ontvangen == s_rx.lengte) {
        s_rx.json[s_rx.lengte] = '\0';
        if (xQueueSend(s_recepten, &s_rx, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Receptwachtrij vol.");
            free(s_rx.json);
        }
        s_rx = (recept_buffer_t){0};
    }
}

/* cmd/start: klein bericht {"message_id":"...","data":{}}. Zonder message_id
 * maken we er zelf een, zodat er toch een ack terug kan. */
static void opdracht_ontvangen(esp_mqtt_event_handle_t event, const char *naam)
{
    if (event->retain || event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        return;
    }
    brouw_mqtt_opdracht_t opdracht = {0};
    strlcpy(opdracht.naam, naam, sizeof(opdracht.naam));

    cJSON *json = (event->data != NULL && event->data_len > 0)
                      ? cJSON_ParseWithLength(event->data, (size_t)event->data_len) : NULL;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "message_id"));
    if (id != NULL && id[0] != '\0') {
        strlcpy(opdracht.message_id, id, sizeof(opdracht.message_id));
    } else {
        static uint32_t teller = 0;
        snprintf(opdracht.message_id, sizeof(opdracht.message_id), "%s-%lu", naam,
                 (unsigned long)++teller);
    }
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Opdracht %s ontvangen (%s).", opdracht.naam, opdracht.message_id);
    if (xQueueSend(s_opdrachten, &opdracht, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Opdrachtwachtrij vol.");
    }
}

static void herstart_bij_fout(const char *melding)
{
    atomic_store(&s_verbindingspoging_mislukt, true);
    ESP_LOGW(TAG, "%s", melding);
    xEventGroupClearBits(s_events, MQTT_GEREED);
    xEventGroupSetBits(s_events, MQTT_HERSTART);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    const bool stopt = (xEventGroupGetBits(s_events) & STOP_GEVRAAGD) != 0;

    switch (id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_events, MQTT_SESSIE);
        atomic_store(&s_server_online, false);
        if (!stopt) {
            /* Retained status van de webserver: komt meteen binnen als hij er is. */
            esp_mqtt_client_subscribe_single(event->client, SERVER_STATUS_TOPIC, 1);
            esp_mqtt_client_subscribe_single(event->client, RECEPT_TOPIC_SERVER,
                                             TOPICS[TOPIC_RECEPT].qos);
            esp_mqtt_client_subscribe_single(event->client, s_topic[TOPIC_START],
                                             TOPICS[TOPIC_START].qos);
            esp_mqtt_client_subscribe_single(event->client, START_TOPIC_SERVER,
                                             TOPICS[TOPIC_START].qos);
            s_sub_id = esp_mqtt_client_subscribe_single(event->client, s_topic[TOPIC_RECEPT],
                                                       TOPICS[TOPIC_RECEPT].qos);
            if (s_sub_id <= 0) {
                herstart_bij_fout("Kan het MQTT-receptabonnement niet aanvragen.");
            }
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        if (stopt || s_sub_id <= 0 || event->msg_id != s_sub_id) {
            break;
        }
        s_sub_id = 0;
        if ((event->error_handle != NULL && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) ||
            (event->data != NULL && event->data_len == 1 && (uint8_t)event->data[0] != RECEPT_QOS)) {
            herstart_bij_fout("Broker verleent geen QoS 2 voor cmd/recept.");
            break;
        }
        s_info_id = voeg_publicatie_toe(event->client, TOPIC_INFO, s_info_json);
        if (s_info_id <= 0) {
            herstart_bij_fout("Kan het MQTT-info-bericht niet klaarzetten.");
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        if (!stopt && s_info_id > 0 && event->msg_id == s_info_id) {
            s_info_id = 0;
            if (voeg_publicatie_toe(event->client, TOPIC_STATUS, "online") > 0) {
                xEventGroupSetBits(s_events, MQTT_GEREED);
                ESP_LOGI(TAG, "MQTT klaar voor %s.", s_client_id);
            } else {
                herstart_bij_fout("Kan de MQTT-online-status niet klaarzetten.");
            }
        }
        if (s_offline_id > 0 && event->msg_id == s_offline_id) {
            xEventGroupSetBits(s_events, OFFLINE_BEVESTIGD);
        }
        break;

    case MQTT_EVENT_DATA:
        if (event->topic != NULL &&
            event->topic_len == (int)strlen(SERVER_STATUS_TOPIC) &&
            memcmp(event->topic, SERVER_STATUS_TOPIC, (size_t)event->topic_len) == 0) {
            const bool online = event->data != NULL && event->data_len == 6 &&
                                memcmp(event->data, "online", 6) == 0;
            atomic_store(&s_server_online, online);
            ESP_LOGI(TAG, "Server is %s.", online ? "online" : "offline");
            break;
        }
        if (event->topic != NULL &&
            (topic_is(event, s_topic[TOPIC_START]) || topic_is(event, START_TOPIC_SERVER))) {
            if (!stopt && (xEventGroupGetBits(s_events) & MQTT_GEREED) != 0) {
                opdracht_ontvangen(event, "start");
            }
            break;
        }
        if (!stopt && (xEventGroupGetBits(s_events) & MQTT_GEREED) != 0) {
            verzamel_recept(event);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_verbindingspoging_mislukt, true);
        atomic_store(&s_server_online, false);
        xEventGroupClearBits(s_events, MQTT_SESSIE | MQTT_GEREED);
        verwijder_onvolledig_recept();
        xEventGroupSetBits(s_events, MQTT_HERSTART);
        ESP_LOGI(TAG, "MQTT-verbinding verbroken.");
        break;

    case MQTT_EVENT_ERROR:
        atomic_store(&s_verbindingspoging_mislukt, true);
        if (event->error_handle == NULL) {
            break;
        }
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "Broker weigert verbinding (CONNACK %d).",
                     event->error_handle->connect_return_code);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "MQTT-transportfout (socket errno %d).",
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    case MQTT_USER_EVENT:
        if (stopt) {
            /* Het bericht-ID wordt in de MQTT-taak bewaard, vóór de PUBACK kan binnenkomen. */
            s_offline_id = voeg_publicatie_toe(event->client, TOPIC_STATUS, "offline");
            if (s_offline_id <= 0) {
                xEventGroupSetBits(s_events, OFFLINE_MISLUKT);
            }
        }
        break;

    default:
        break;
    }
}

static esp_err_t start_client(void)
{
    brouw_netwerk_mqtt_t instellingen;
    esp_err_t fout = brouw_netwerk_lees_mqtt(&instellingen);
    if (fout != ESP_OK) {
        return fout;
    }

    const esp_mqtt_client_config_t config = {
        .broker.address = {
            .hostname = instellingen.host,
            .transport = MQTT_TRANSPORT_OVER_TCP,
            .port = instellingen.poort,
        },
        .credentials = {
            .client_id = s_client_id,
            .username = instellingen.gebruikersnaam[0] != '\0' ? instellingen.gebruikersnaam : NULL,
            .authentication.password = instellingen.wachtwoord[0] != '\0' ? instellingen.wachtwoord : NULL,
        },
        .session = {
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            .keepalive = 30,
            .disable_clean_session = false,
            .last_will = {
                .topic = s_topic[TOPIC_STATUS],
                .msg = "offline",
                .msg_len = sizeof("offline") - 1,
                .qos = 1,
                .retain = true,
            },
        },
        .network = {
            .disable_auto_reconnect = true, /* mqtt_taak start een nieuwe sessie met de actuele NVS-instellingen. */
            .timeout_ms = 4000,
        },
        .buffer = {
            .size = 1024,
            .out_size = PUBLICATIE_MAX_BYTES + 128,
        },
        .outbox.limit = 16 * 1024,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    fout = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, NULL);
    if (fout == ESP_OK) {
        ESP_LOGI(TAG, "MQTT verbinden met %s:%u als %s.", instellingen.host,
                 (unsigned)instellingen.poort, s_client_id);
        fout = esp_mqtt_client_start(client);
    }
    if (fout != ESP_OK) {
        esp_mqtt_client_destroy(client);
        return fout;
    }

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_client_mutex);
    return ESP_OK;
}

static void sluit_client(bool gepland)
{
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    s_client = NULL;
    xEventGroupClearBits(s_events, MQTT_GEREED);
    xSemaphoreGive(s_client_mutex);

    if (client == NULL) {
        return;
    }

    if (gepland && (xEventGroupGetBits(s_events) & MQTT_SESSIE) != 0) {
        esp_mqtt_event_t verzoek = { .event_id = MQTT_USER_EVENT, .client = client };
        if (esp_mqtt_dispatch_custom_event(client, &verzoek) == ESP_OK) {
            xEventGroupWaitBits(s_events, OFFLINE_BEVESTIGD | OFFLINE_MISLUKT | MQTT_HERSTART,
                                pdFALSE, pdFALSE, pdMS_TO_TICKS(OFFLINE_ACK_TIMEOUT_MS));
        }
        if ((xEventGroupGetBits(s_events) & OFFLINE_BEVESTIGD) == 0) {
            xEventGroupSetBits(s_events, OFFLINE_MISLUKT);
        }
    }

    esp_mqtt_client_destroy(client);
    xEventGroupClearBits(s_events, MQTT_SESSIE | MQTT_GEREED | MQTT_HERSTART);
    verwijder_onvolledig_recept();
    s_sub_id = 0;
    s_info_id = 0;
    s_offline_id = 0;
}

static void mqtt_taak(void *arg)
{
    (void)arg;
    esp_err_t vorige_fout = ESP_OK;
    for (;;) {
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & STOP_GEVRAAGD) != 0) {
            sluit_client(true);
            recept_buffer_t bericht;
            while (xQueueReceive(s_recepten, &bericht, 0) == pdTRUE) {
                free(bericht.json);
            }
            xEventGroupClearBits(s_events, MQTT_INGESCHAKELD | STOP_GEVRAAGD);
            xEventGroupSetBits(s_events, STOP_KLAAR);
            vorige_fout = ESP_OK;
            continue;
        }
        if ((bits & MQTT_INGESCHAKELD) == 0) {
            xEventGroupWaitBits(s_events, MQTT_INGESCHAKELD, pdFALSE, pdTRUE, portMAX_DELAY);
            continue;
        }

        unsigned wacht_ms = NETWERK_CHECK_MS;
        if ((bits & MQTT_HERSTART) != 0) {
            sluit_client(false);
            wacht_ms = HERPROBEER_MS;
        } else if (brouw_netwerk_status() != BROUW_NETWERK_IP_GEREED) {
            if (s_client != NULL) {
                sluit_client(false);
            }
            vorige_fout = ESP_OK;
        } else if (s_client == NULL) {
            const esp_err_t fout = start_client();
            if (fout != ESP_OK) {
                atomic_store(&s_verbindingspoging_mislukt, true);
                if (fout != vorige_fout) {
                    if (fout == ESP_ERR_NOT_FOUND) {
                        ESP_LOGW(TAG, "Geen MQTT-host ingesteld in het wifi-configuratieformulier.");
                    } else {
                        ESP_LOGW(TAG, "MQTT starten: %s", esp_err_to_name(fout));
                    }
                }
                wacht_ms = HERPROBEER_MS;
            }
            vorige_fout = fout;
        }
        xEventGroupWaitBits(s_events, STOP_GEVRAAGD | MQTT_HERSTART, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(wacht_ms));
    }
}

esp_err_t brouw_mqtt_init(const char *ketelnaam, bool automatisch_starten)
{
    if (ketelnaam == NULL || ketelnaam[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mac[6];
    const esp_err_t fout = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (fout != ESP_OK) {
        return fout;
    }
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x",
             (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    snprintf(s_client_id, sizeof(s_client_id), "brouwer-%s", s_id);
    for (topic_t topic = TOPIC_INFO; topic < TOPIC_AANTAL; ++topic) {
        snprintf(s_topic[topic], sizeof(s_topic[topic]), "brouwer/%s/%s", s_id, TOPICS[topic].pad);
    }

    s_info_json = maak_info_json(ketelnaam);
    s_events = xEventGroupCreate();
    s_recepten = xQueueCreate(RECEPT_WACHTRIJ_LENGTE, sizeof(recept_buffer_t));
    s_opdrachten = xQueueCreate(OPDRACHT_WACHTRIJ_LENGTE, sizeof(brouw_mqtt_opdracht_t));
    s_client_mutex = xSemaphoreCreateMutex();
    s_start_stop_mutex = xSemaphoreCreateMutex();
    if (s_info_json != NULL && s_events != NULL && s_recepten != NULL && s_opdrachten != NULL &&
        s_client_mutex != NULL && s_start_stop_mutex != NULL) {
        if (automatisch_starten) {
            xEventGroupSetBits(s_events, MQTT_INGESCHAKELD);
        }
        if (xTaskCreate(mqtt_taak, "brouw_mqtt", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) == pdPASS) {
            return ESP_OK;
        }
    }

    cJSON_free(s_info_json);
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
    }
    if (s_recepten != NULL) {
        vQueueDelete(s_recepten);
    }
    if (s_opdrachten != NULL) {
        vQueueDelete(s_opdrachten);
    }
    if (s_client_mutex != NULL) {
        vSemaphoreDelete(s_client_mutex);
    }
    if (s_start_stop_mutex != NULL) {
        vSemaphoreDelete(s_start_stop_mutex);
    }
    s_info_json = NULL;
    s_events = NULL;
    s_recepten = NULL;
    s_opdrachten = NULL;
    s_client_mutex = NULL;
    s_start_stop_mutex = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t brouw_mqtt_start(void)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_start_stop_mutex, portMAX_DELAY);
    atomic_store(&s_verbindingspoging_mislukt, false);
    xEventGroupSetBits(s_events, MQTT_INGESCHAKELD);
    xSemaphoreGive(s_start_stop_mutex);
    return ESP_OK;
}

bool brouw_mqtt_is_verbonden(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & (MQTT_GEREED | STOP_GEVRAAGD)) == MQTT_GEREED &&
           brouw_netwerk_status() == BROUW_NETWERK_IP_GEREED;
}

bool brouw_mqtt_server_online(void)
{
    return brouw_mqtt_is_verbonden() && atomic_load(&s_server_online);
}

esp_err_t brouw_mqtt_publiceer(const char *topic, const char *payload)
{
    if (topic == NULL || payload == NULL || payload[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strnlen(payload, PUBLICATIE_MAX_BYTES + 1) > PUBLICATIE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    topic_t index = TOPIC_TEMPERATUUR;
    while (index < TOPIC_AANTAL && strcmp(topic, TOPICS[index].pad) != 0) {
        ++index;
    }
    if (index == TOPIC_AANTAL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!brouw_mqtt_is_verbonden()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    esp_err_t fout = ESP_ERR_INVALID_STATE;
    if (s_client != NULL && brouw_mqtt_is_verbonden()) {
        /* Meteen versturen. Met enqueue gaat er maar ongeveer een bericht per
         * seconde de deur uit (de MQTT-taak verstuurt er een per ronde en wacht
         * tot 1 s per ronde). Met status, temperatuur en verwarming elke seconde
         * liep de wachtrij dan steeds verder achter. */
        const int bericht_id = esp_mqtt_client_publish(s_client, s_topic[index], payload, 0,
                                                       TOPICS[index].qos, TOPICS[index].retained);
        fout = bericht_id >= 0 ? ESP_OK : ESP_FAIL;
        if (bericht_id == -2) {
            fout = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_client_mutex);
    return fout;
}

esp_err_t brouw_mqtt_ontvang_recept(brouw_mqtt_recept_t *recept, TickType_t wachttijd)
{
    if (recept == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    recept_buffer_t bericht;
    if (xQueueReceive(s_recepten, &bericht, wachttijd) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    cJSON *json = NULL;
    if (memchr(bericht.json, '\0', (size_t)bericht.lengte) == NULL) {
        json = cJSON_ParseWithLengthOpts(bericht.json, (size_t)bericht.lengte + 1, NULL, true);
    }
    free(bericht.json);
    if (!cJSON_IsObject(json)) {
        ESP_LOGW(TAG, "Recept is geen geldige JSON.");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "message_id");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
        /* Recept zoals de website het stuurt: het hele bericht is het recept.
         * We maken zelf een uniek nummer, zodat hetzelfde recept twee keer
         * na elkaar sturen ook werkt. */
        static uint32_t teller = 0;
        char eigen_id[32];
        const cJSON *recept_id = cJSON_GetObjectItemCaseSensitive(json, "id");
        snprintf(eigen_id, sizeof(eigen_id), "recept-%d-%lu",
                 cJSON_IsNumber(recept_id) ? recept_id->valueint : 0, (unsigned long)++teller);
        cJSON_DeleteItemFromObjectCaseSensitive(json, "message_id");
        id = cJSON_AddStringToObject(json, "message_id", eigen_id);
        if (id == NULL) {
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        data = json;
        ESP_LOGI(TAG, "Recept van de website ontvangen (%s).", eigen_id);
    }
    if (!cJSON_IsObject(data)) {
        const esp_err_t fout = brouw_mqtt_ack(id->valuestring, false, "data moet een JSON-object zijn");
        if (fout != ESP_OK) {
            ESP_LOGW(TAG, "Negatieve recept-ack: %s", esp_err_to_name(fout));
        }
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *recept = (brouw_mqtt_recept_t){ .message_id = id->valuestring, .data = data, .json = json };
    return ESP_OK;
}

esp_err_t brouw_mqtt_ontvang_opdracht(brouw_mqtt_opdracht_t *opdracht, TickType_t wachttijd)
{
    if (opdracht == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_opdrachten == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueReceive(s_opdrachten, opdracht, wachttijd) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void brouw_mqtt_recept_vrijgeven(brouw_mqtt_recept_t *recept)
{
    if (recept != NULL) {
        cJSON_Delete(recept->json);
        *recept = (brouw_mqtt_recept_t){0};
    }
}

esp_err_t brouw_mqtt_ack(const char *message_id, bool ok, const char *fout)
{
    if (message_id == NULL || message_id[0] == '\0' ||
        (!ok && (fout == NULL || fout[0] == '\0'))) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL || cJSON_AddStringToObject(json, "message_id", message_id) == NULL ||
        cJSON_AddBoolToObject(json, "ok", ok) == NULL ||
        (!ok && cJSON_AddStringToObject(json, "fout", fout) == NULL)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t resultaat = brouw_mqtt_publiceer("ack", payload);
    cJSON_free(payload);
    return resultaat;
}

esp_err_t brouw_mqtt_stop(void)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_start_stop_mutex, portMAX_DELAY);
    esp_err_t resultaat = ESP_OK;
    if ((xEventGroupGetBits(s_events) & MQTT_INGESCHAKELD) != 0) {
        xEventGroupClearBits(s_events, STOP_KLAAR | OFFLINE_BEVESTIGD | OFFLINE_MISLUKT);
        xEventGroupSetBits(s_events, STOP_GEVRAAGD);
        const EventBits_t bits = xEventGroupWaitBits(s_events, STOP_KLAAR, pdFALSE, pdTRUE, portMAX_DELAY);
        if ((bits & OFFLINE_MISLUKT) != 0) {
            resultaat = ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreGive(s_start_stop_mutex);
    return resultaat;
}
