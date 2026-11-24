#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "max31865.h"
#include "scherm_uart.h"
#include "brouw_netwerk.h"
#include "brouw_mqtt.h"
#include "esp_log.h"
#include "recept.h"
#include "sdkconfig.h"
#include "freertos/portmacro.h"


#define SPI_HOST    SPI2_HOST
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  12
#define PIN_NUM_CS  10

TaskHandle_t temp_handle;
max31865_t sensor;

const char* webserverPassword = "admin123";

int heating_power = 100; //Placeholder for heating power value, should be set based on your PID logic

static int last_message[8] = {0};
static int next_index = 0;

/* ---- Recept van de website ----
 * Het scherm vraagt een recept met net/start. Daarna wacht de ketel tot iemand op
 * de website op "Recept doorsturen" klikt. Dan komt het recept binnen op
 * brouwer/<mac>/cmd/recept. Een recept dat binnenkomt terwijl het scherm niet
 * wacht, wordt meteen geweigerd (ack ok:false), zodat er nooit een oud recept
 * blijft hangen. */
static volatile bool s_wacht_op_recept = false;   // true tussen net/start (verbonden) en het recept / net/stop
static char s_recept_id[64] = "";                 // message_id van het recept dat nu bij het scherm ligt
static char s_vorig_recept_id[64] = "";           // laatst verwerkte message_id (MQTT kan dubbel sturen)

/* ---- Brouwsessie starten vanaf de website ----
 * cmd/start komt binnen -> brouw/start naar het scherm -> het scherm antwoordt
 * met de melding "start" of "start_fout" -> ack naar de website. */
#define START_ANTWOORD_MS 5000
static char s_start_id[64] = "";                  // message_id van cmd/start dat op het scherm wacht
static TickType_t s_start_tijd = 0;
static portMUX_TYPE s_start_slot = portMUX_INITIALIZER_UNLOCKED;

// Laatste wensen van het scherm (uit brouw/stuur)
static volatile float s_doel = 0.0f;              // doeltemperatuur, 0 = verwarming uit
static int s_pomp_gemeld = -1;                    // wat er laatst op actuator/pomp/state stond (-1 = nog niets)

/* Neemt het wachtende start-id weg (leeg als er geen is). */
static void start_id_nemen(char *id, size_t grootte)
{
    taskENTER_CRITICAL(&s_start_slot);
    strlcpy(id, s_start_id, grootte);
    s_start_id[0] = '\0';
    taskEXIT_CRITICAL(&s_start_slot);
}

static bool net_toestand_versturen(bool wifi, bool mqtt, bool online, bool configuratie)
{
    char ssid[33] = "";
    brouw_netwerk_lees_ssid(ssid);

    brouw_netwerk_mqtt_t broker = {0};
    brouw_netwerk_lees_mqtt(&broker);

    // Zonder wifi geen MQTT, zonder MQTT geen server.
    mqtt = wifi && mqtt;
    online = mqtt && online;

    cJSON *json = cJSON_CreateObject();

    if (json == NULL ||
        cJSON_AddBoolToObject(json, "wifi", wifi) == NULL ||
        cJSON_AddBoolToObject(json, "mqtt", mqtt) == NULL ||
        cJSON_AddBoolToObject(json, "online", online) == NULL ||
        cJSON_AddStringToObject(json, "modus",
                                configuratie ? "configuratie" : "werking") == NULL ||
        cJSON_AddStringToObject(json, "ssid", ssid) == NULL ||
        cJSON_AddStringToObject(json, "server", broker.host) == NULL ||
        cJSON_AddNumberToObject(json, "poort", broker.poort) == NULL) {
        cJSON_Delete(json);
        return false;
    }

    char *inhoud = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (inhoud == NULL) {
        return false;
    }

    scherm_uart_verstuur("net/toestand", inhoud);

    cJSON_free(inhoud);
    return true;
}

static void net_toestand_taak(void *arg)
{
    (void)arg;

    uint32_t afgehandeld = 0;
    bool bewaken = true;
    bool forceer = true;

    bool vorige_geldig = false;
    bool vorige_wifi = false;
    bool vorige_mqtt = false;
    bool vorige_online = false;
    bool vorige_configuratie = false;

    for (;;) {
        const uint32_t nummer =
            brouw_netwerk_configuratienummer();

        // Nieuwe configuratie: wacht eerst op het resultaat.
        if (nummer != afgehandeld) {
            brouw_mqtt_stop();
            const esp_err_t mqtt_start = brouw_mqtt_start();

            TickType_t begin = xTaskGetTickCount();

            while (brouw_netwerk_configuratienummer() == nummer &&
                   !brouw_netwerk_verbindingspoging_klaar() &&
                   xTaskGetTickCount() - begin <
                       pdMS_TO_TICKS(30000)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // Een nieuwe opslag vervangt de lopende poging.
            if (brouw_netwerk_configuratienummer() != nummer) {
                continue;
            }

            if (brouw_netwerk_status() ==
                    BROUW_NETWERK_IP_GEREED &&
                mqtt_start == ESP_OK) {

                begin = xTaskGetTickCount();

                while (brouw_netwerk_configuratienummer() == nummer &&
                       brouw_netwerk_status() ==
                           BROUW_NETWERK_IP_GEREED &&
                       !brouw_mqtt_is_verbonden() &&
                       !brouw_mqtt_verbindingspoging_mislukt() &&
                       xTaskGetTickCount() - begin <
                           pdMS_TO_TICKS(15000)) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }

            if (brouw_netwerk_configuratienummer() != nummer) {
                continue;
            }

            afgehandeld = nummer;
            bewaken = true;

            // Na iedere opslag één resultaatbericht,
            // ook als de uitkomst hetzelfde is als daarvoor.
            forceer = true;
        }

        // Na het eerste resultaat: alleen wijzigingen melden.
        if (bewaken &&
            brouw_netwerk_configuratienummer() == afgehandeld) {

            const brouw_netwerk_status_t toestand =
                brouw_netwerk_status();

            const bool wifi =
                toestand == BROUW_NETWERK_IP_GEREED;

            const bool mqtt =
                wifi && brouw_mqtt_is_verbonden();

            // Server online: retained "online" op brouwer/server/status
            const bool online =
                mqtt && brouw_mqtt_server_online();

            const bool configuratie =
                toestand == BROUW_NETWERK_SETUP;

            if (forceer || !vorige_geldig ||
                wifi != vorige_wifi ||
                mqtt != vorige_mqtt ||
                online != vorige_online ||
                configuratie != vorige_configuratie) {

                if (net_toestand_versturen(
                        wifi, mqtt, online, configuratie)) {

                    vorige_wifi = wifi;
                    vorige_mqtt = mqtt;
                    vorige_online = online;
                    vorige_configuratie = configuratie;
                    vorige_geldig = true;
                    forceer = false;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool message_is_new(int nr)
{
    for(int i = 0; i < 8; i++)
    {
        if(last_message[i] == nr)
        {
            return false;
        }
    }
    last_message[next_index] = nr;
    next_index = (next_index + 1) % 8;
    return true;
}

esp_err_t spi_init(void)
{
    max31865_config_t config = {
        .spi_host = SPI_HOST,
        .pin_sck = PIN_NUM_CLK,
        .pin_mosi = PIN_NUM_MOSI,
        .pin_miso = PIN_NUM_MISO,
        .pin_cs = PIN_NUM_CS,
        .rtd_nominal_ohms = 100.0f,
        .reference_resistor_ohms = 430.0f,
        .temperature_offset_c = 0.0f,
        .filter_50hz = false,
    };

    esp_err_t err = max31865_init(&sensor, &config);
    if (err != ESP_OK) 
    {
        //Wanneer dit voorkomt moet error gegeven worden aan ESP32 scherm
        printf("Failed to initialize MAX31865: %s\n", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}


void temp_task(void *parameters)
{
    esp_err_t err;
#if CONFIG_BROUW_NEP_TEMPERATUUR
    // Thuis testen zonder PT100: geen sensor uitlezen, zie nep_meting hieronder
    float nep_temp = 20.0f;
    printf("LET OP: nep-temperatuur staat aan (menuconfig > Brouwketel)\n");
#else
    err = spi_init();
    if (err != ESP_OK)
    {
        printf("Failed to initialize SPI: %s\n", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
#endif

    while (1)
    {
        float temperature_c = 0.0f;
#if CONFIG_BROUW_NEP_TEMPERATUUR
        // Kouder dan het doel: verwarming vol aan, 1 graad per seconde erbij.
        // Anders verwarming uit en langzaam afkoelen (niet onder 20, niet boven 100).
        const float doel = s_doel;
        if (doel > 0.0f && nep_temp < doel) {
            heating_power = 100;
            nep_temp += 1.0f;
        } else {
            heating_power = 0;
            nep_temp -= 0.1f;
        }
        nep_temp = fminf(fmaxf(nep_temp, 20.0f), 100.0f);
        temperature_c = nep_temp;
        esp_err_t meting = ESP_OK;
#else
        esp_err_t meting = max31865_read_temperature(&sensor, &temperature_c);
#endif
        bool ok = (meting == ESP_OK && isfinite(temperature_c));

        // 1. Naar het scherm (zoals voorheen)
        char json[100];
        if (ok)
        {
            snprintf(json, sizeof(json), "{\"temp\":%.1f,\"ok\":true,\"vermogen\":%d}",
                     temperature_c, heating_power);
            printf("Temperatuur: %.2f °C\n", temperature_c);
        }
        else
        {
            snprintf(json, sizeof(json), "{\"temp\":null,\"ok\":false,\"vermogen\":%d}", heating_power);
            printf("Uitlezen mislukt: %s\n", esp_err_to_name(meting));
        }

        err = scherm_uart_verstuur("ketel/temp", json);
        if (err != ESP_OK)
        {
            printf("Temperatuur versturen mislukt: %s\n", esp_err_to_name(err));
        }

        // 2. Naar MQTT (nieuw), alleen als MQTT verbonden is
        if (brouw_mqtt_is_verbonden())
        {
            char tekst[16];

            if (ok)
            {
                snprintf(tekst, sizeof(tekst), "%.1f", temperature_c);
                brouw_mqtt_publiceer("sensor/temperatuur", tekst);
            }

            snprintf(tekst, sizeof(tekst), "%d", heating_power);
            brouw_mqtt_publiceer("actuator/verwarming/state", tekst);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Haalt recepten uit de wachtrij van brouw_mqtt. Wacht het scherm erop, dan
 * omzetten en doorsturen; anders meteen weigeren. */
static void recept_taak(void *arg)
{
    (void)arg;

    for (;;) {
        brouw_mqtt_recept_t recept = {0};
        const esp_err_t fout = brouw_mqtt_ontvang_recept(&recept, pdMS_TO_TICKS(500));
        if (fout != ESP_OK) {
            // ESP_ERR_TIMEOUT: niets binnen. Een ongeldig recept is al geweigerd.
            if (fout == ESP_ERR_INVALID_STATE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        // Hetzelfde recept nog eens (MQTT QoS kan dubbel leveren): negeren
        if (strcmp(recept.message_id, s_vorig_recept_id) == 0) {
            ESP_LOGI("recept", "Recept %s al verwerkt, genegeerd.", recept.message_id);
            brouw_mqtt_recept_vrijgeven(&recept);
            continue;
        }
        strlcpy(s_vorig_recept_id, recept.message_id, sizeof(s_vorig_recept_id));

        if (!s_wacht_op_recept) {
            ESP_LOGW("recept", "Recept %s geweigerd: het scherm wacht niet.", recept.message_id);
            brouw_mqtt_ack(recept.message_id, false,
                           "ketel wacht niet op een recept: druk eerst op Automatisch");
            brouw_mqtt_recept_vrijgeven(&recept);
            continue;
        }

        char *voor_scherm = NULL;
        const char *reden = recept_omzetten(recept.data, &voor_scherm);
        if (reden != NULL) {
            ESP_LOGW("recept", "Recept %s ongeldig: %s", recept.message_id, reden);
            brouw_mqtt_ack(recept.message_id, false, reden);
            // Het scherm laten weten dat het mislukt is (vinkje 2 rood)
            char json[160];
            snprintf(json, sizeof(json), "{\"status\":\"fout\",\"reden\":\"%s\"}", reden);
            scherm_uart_verstuur("net/status", json);
        } else {
            ESP_LOGI("recept", "Recept %s naar het scherm.", recept.message_id);
            strlcpy(s_recept_id, recept.message_id, sizeof(s_recept_id));
            scherm_uart_verstuur("recept", voor_scherm);   // ack volgt na recept_ok / recept_fout
            free(voor_scherm);
        }
        s_wacht_op_recept = false;
        brouw_mqtt_recept_vrijgeven(&recept);
    }
}

/* Haalt opdrachten (cmd/start) uit de wachtrij van brouw_mqtt en geeft ze door
 * aan het scherm. Antwoordt het scherm niet op tijd, dan een negatieve ack. */
static void opdracht_taak(void *arg)
{
    (void)arg;
    char vorig_id[64] = "";

    for (;;) {
        brouw_mqtt_opdracht_t opdracht;
        if (brouw_mqtt_ontvang_opdracht(&opdracht, pdMS_TO_TICKS(200)) == ESP_OK &&
            strcmp(opdracht.naam, "start") == 0) {

            if (strcmp(opdracht.message_id, vorig_id) == 0) {
                // MQTT QoS 1 kan hetzelfde bericht twee keer leveren
                ESP_LOGI("start", "Startopdracht %s al verwerkt, genegeerd.", opdracht.message_id);
            } else {
                strlcpy(vorig_id, opdracht.message_id, sizeof(vorig_id));

                bool bezig;
                taskENTER_CRITICAL(&s_start_slot);
                bezig = s_start_id[0] != '\0';
                if (!bezig) {
                    strlcpy(s_start_id, opdracht.message_id, sizeof(s_start_id));
                    s_start_tijd = xTaskGetTickCount();
                }
                taskEXIT_CRITICAL(&s_start_slot);

                if (bezig) {
                    brouw_mqtt_ack(opdracht.message_id, false, "er wacht al een startopdracht op het scherm");
                } else {
                    ESP_LOGI("start", "Startopdracht %s naar het scherm.", opdracht.message_id);
                    scherm_uart_verstuur("brouw/start", "{}");
                }
            }
        }

        // Geen antwoord van het scherm binnen 5 s?
        char id[64] = "";
        taskENTER_CRITICAL(&s_start_slot);
        if (s_start_id[0] != '\0' &&
            xTaskGetTickCount() - s_start_tijd > pdMS_TO_TICKS(START_ANTWOORD_MS)) {
            strlcpy(id, s_start_id, sizeof(id));
            s_start_id[0] = '\0';
        }
        taskEXIT_CRITICAL(&s_start_slot);
        if (id[0] != '\0') {
            ESP_LOGW("start", "Scherm antwoordt niet op startopdracht %s.", id);
            brouw_mqtt_ack(id, false, "scherm antwoordt niet");
        }
    }
}

static void bericht_verwerken(const scherm_uart_bericht_t *bericht)
{
    const char* einde = NULL;
    cJSON *data = cJSON_ParseWithOpts(bericht->json, &einde, true);
    if (data == NULL)
    {
        return;
    }

    if (strcmp(bericht->onderwerp, "brouw/meld") == 0)
    {
        cJSON* nummer = cJSON_GetObjectItemCaseSensitive(data, "nr");
        if (cJSON_IsNumber(nummer) && nummer->valuedouble >= 1 &&
            nummer->valuedouble <= INT_MAX &&
            nummer->valuedouble == (int)nummer->valuedouble)
        {
            int nr = (int)nummer->valuedouble;
            char ack[32];
            snprintf(ack, sizeof(ack), "{\"nr\":%d}", nr);

            //ALTIJD bevestigen, ook wanneer dezelfde melding opnieuw komt.
            esp_err_t err = scherm_uart_verstuur("ack", ack);
            if (err != ESP_OK)
            {
                printf("Ack versturen mislukt: %s\n", esp_err_to_name(err));
            }

            if (message_is_new(nr))
            {
                printf("Nieuwe melding: %s\n", bericht->json);
                // Doorgeven aan de website
                if (brouw_mqtt_is_verbonden())
                {
                    brouw_mqtt_publiceer("brouw/melding", bericht->json);
                }

                // Antwoord van het scherm op het recept: doorgeven aan de website
                const char *naam = cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(data, "naam"));
                if (naam != NULL && s_recept_id[0] != '\0' &&
                    (strcmp(naam, "recept_ok") == 0 || strcmp(naam, "recept_fout") == 0))
                {
                    const bool ok = strcmp(naam, "recept_ok") == 0;
                    const char *reden = cJSON_GetStringValue(
                        cJSON_GetObjectItemCaseSensitive(data, "reden"));
                    brouw_mqtt_ack(s_recept_id, ok,
                                   ok ? NULL : (reden != NULL && reden[0] != '\0'
                                                ? reden : "scherm weigert het recept"));
                    s_recept_id[0] = '\0';
                }

                // Antwoord van het scherm op cmd/start van de website
                if (naam != NULL &&
                    (strcmp(naam, "start") == 0 || strcmp(naam, "start_fout") == 0))
                {
                    char id[64];
                    start_id_nemen(id, sizeof(id));
                    if (id[0] != '\0')
                    {
                        const bool ok = strcmp(naam, "start") == 0;
                        const char *reden = cJSON_GetStringValue(
                            cJSON_GetObjectItemCaseSensitive(data, "reden"));
                        brouw_mqtt_ack(id, ok,
                                       ok ? NULL : (reden != NULL && reden[0] != '\0'
                                                    ? reden : "scherm kan niet starten"));
                    }
                }
            }
        }
    }
    else if (strcmp(bericht->onderwerp, "brouw/stuur") == 0)
    {
        cJSON* doel = cJSON_GetObjectItemCaseSensitive(data, "doel");
        cJSON* pomp = cJSON_GetObjectItemCaseSensitive(data, "pomp");

        // Voorbeeld van veld uitlezen met een standaardwaarde als het ontbreekt.
        float doeltemperatuur = cJSON_IsNumber(doel) ? (float)doel->valuedouble : 0.0f;
        bool pomp_aan = cJSON_IsTrue(pomp);
        printf("Doel: %.1f C, pomp gevraagd: %s\n",
               doeltemperatuur, pomp_aan ? "aan" : "uit");
        s_doel = doeltemperatuur;

        // Pomp naar de website, alleen als hij verandert (retained topic)
        if (s_pomp_gemeld != (int)pomp_aan && brouw_mqtt_is_verbonden() &&
            brouw_mqtt_publiceer("actuator/pomp/state", pomp_aan ? "on" : "off") == ESP_OK)
        {
            s_pomp_gemeld = pomp_aan;
        }

        //Hier de doeltemperatuur en pomptoestand doorgeven aan de PID-regelaar en sturing pomp
        //Ontbrekende kp/ki/kd moeten daar hun vorige waarde behouden
    }
    else if (strcmp(bericht->onderwerp, "brouw/status") == 0)
    {
        // Alleen voor de website: doorsturen naar MQTT. Niets terugsturen naar het scherm.
        if (brouw_mqtt_is_verbonden())
        {
            brouw_mqtt_publiceer("brouw/status", bericht->json);
        }
    }
    else if (strcmp(bericht->onderwerp, "net/start") == 0)
    {
        // Echt controleren: MQTT verbonden en de server online (brouwer/server/status).
        if (!brouw_mqtt_is_verbonden())
        {
            scherm_uart_verstuur("net/status", "{\"status\":\"fout\",\"reden\":\"geen MQTT\"}");
        }
        else if (!brouw_mqtt_server_online())
        {
            scherm_uart_verstuur("net/status", "{\"status\":\"fout\",\"reden\":\"server offline\"}");
        }
        else
        {
            scherm_uart_verstuur("net/status", "{\"status\":\"verbonden\"}");
            // Vanaf nu wachten op een recept van de website (recept_taak)
            s_recept_id[0] = '\0';
            s_wacht_op_recept = true;
        }
    }
    else if (strcmp(bericht->onderwerp, "net/stop") == 0)
    {
        printf("Stop commando ontvangen: niet meer wachten op een recept.\n");
        s_wacht_op_recept = false;
    }
    else if (strcmp(bericht->onderwerp, "net/modus") == 0)
    {
        const char *modus = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(data, "modus")
        );

        esp_err_t fout = ESP_ERR_INVALID_ARG;

        if (modus != NULL) {
            if (strcmp(modus, "configuratie") == 0) {
                fout = brouw_netwerk_vraag_setup();
            } else if (strcmp(modus, "werking") == 0) {
                fout = brouw_netwerk_vraag_werking();
            }
        }

        if (fout != ESP_OK) {
            ESP_LOGW("net_modus", "Modusaanvraag mislukt: %s",
                 esp_err_to_name(fout));
        }
    }
    else if(strcmp(bericht->onderwerp, "") == 0)
    {
        //Hier kan code komen om een ander onderwerp te verwerken
    }
    else if(strcmp(bericht->onderwerp, "") == 0)
    {
        //Hier kan code komen om een ander onderwerp te verwerken
    }
    else if(strcmp(bericht->onderwerp, "") == 0)
    {
        //Hier kan code komen om een ander onderwerp te verwerken
    }
    else
    {
        printf("Onbekend onderwerp: %s\n", bericht->onderwerp);
    }

    cJSON_Delete(data);
}

void receive_task(void *parameters)
{
    TickType_t last_send = xTaskGetTickCount();
    bool timeout_reported = false;

    while (1)
    {
        scherm_uart_bericht_t bericht;
        esp_err_t err = scherm_uart_ontvang(&bericht, pdMS_TO_TICKS(50));
        //Tijd tot timeout:
        if(xTaskGetTickCount() - last_send >= pdMS_TO_TICKS(2000) && !timeout_reported)
        {
            printf("Geen berichten ontvangen gedurende 2 seconden.\n");
            memset(last_message, 0, sizeof(last_message));
            next_index = 0;
            timeout_reported = true;
            //Hier komt de code om verwarming, pomp en zoemer uit te zetten bij onderbreking van verbinding
        }

        if(err == ESP_OK)
        {
            if(strcmp(bericht.onderwerp, "brouw/stuur") == 0)
            {
                last_send = xTaskGetTickCount();
                timeout_reported = false;
            }
            bericht_verwerken(&bericht);
        }
        else if (err != ESP_ERR_TIMEOUT)
        {
            printf("UART-ontvangst fout: %s\n", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void app_main(void)
{
    scherm_uart_init();

    xTaskCreate(temp_task, "Temperature task", 1024*8, NULL, 5, &temp_handle);
    xTaskCreate(receive_task, "Receive task", 1024*8, NULL, 5, NULL);

    const esp_err_t fout = brouw_netwerk_start(webserverPassword, false);
    if (fout != ESP_OK) 
    {
        ESP_LOGE("main", "Netwerkstart: %s", esp_err_to_name(fout));
    }

    ESP_ERROR_CHECK(brouw_mqtt_init("Brouwketel 1", true));

    if (xTaskCreate(recept_taak, "recept", 1024 * 8, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE("main", "Kan recepttaak niet starten");
    }

    if (xTaskCreate(opdracht_taak, "opdracht", 1024 * 4, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE("main", "Kan opdrachttaak niet starten");
    }

    if (xTaskCreate(net_toestand_taak, "net_toestand", 1024 * 8, NULL, 5, NULL) != pdPASS) 
    {
        ESP_LOGE("main", "Kan netwerktoestand-taak niet starten");
    }
}
