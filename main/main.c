#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "max31865.h"
#include "scherm_uart.h"
#include "brouw_netwerk.h"
#include "esp_log.h"

#define SPI_HOST    SPI2_HOST
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  12
#define PIN_NUM_CS  10

TaskHandle_t temp_handle;
max31865_t sensor;

int heating_power = 100; //Placeholder for heating power value, should be set based on your PID logic

static int last_message[8] = {0};
static int next_index = 0;

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
    esp_err_t err = spi_init();
    if (err != ESP_OK)
    {
        printf("Failed to initialize SPI: %s\n", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    while (1)
    {
        float temperature_c = 0.0f;
        esp_err_t err = max31865_read_temperature(&sensor, &temperature_c);

        char json[100];
        if (err == ESP_OK && isfinite(temperature_c))
        {
            int vermogen = heating_power; //heating_power wordt later bepaald door PID-regelaar
            snprintf(json, sizeof(json), "{\"temp\":%.1f,\"ok\":true,\"vermogen\":%d}", 
            temperature_c, heating_power);
            printf("Temperatuur: %.2f °C\n", temperature_c);
        }
        else
        {
            snprintf(json, sizeof(json),"{\"temp\":null,\"ok\":false,\"vermogen\":%d}", heating_power);
            printf("Uitlezen mislukt: %s\n", esp_err_to_name(err));
        }

        err = scherm_uart_verstuur("ketel/temp", json);
        if (err != ESP_OK)
        {
            printf("Temperatuur versturen mislukt: %s\n", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
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
                //Dit JSON bericht doorgeven naar MQTT broker voor de server
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

        //Hier de doeltemperatuur en pomptoestand doorgeven aan de PID-regelaa en sturing pomp
        //Ontbrekende kp/ki/kd moeten daar hun vorige waarde behouden
    }
    else if (strcmp(bericht->onderwerp, "net/start") == 0)
    {
        cJSON* server = cJSON_GetObjectItemCaseSensitive(data, "server");
        cJSON* poort = cJSON_GetObjectItemCaseSensitive(data, "poort");

        char* server_IP = cJSON_IsString(server) ? server->valuestring : "onbekend";
        int poort_nummer = cJSON_IsNumber(poort) ? (int)poort->valuedouble : 0;

        printf("Server: %s, Poort: %d\n", server_IP, poort_nummer);

        vTaskDelay(pdMS_TO_TICKS(1000)); // Simulate some processing delay

        char json[100];
        snprintf(json, sizeof(json), "{\"status\":\"verbonden\"}");
        scherm_uart_verstuur("net/status", json);
    }
    else if (strcmp(bericht->onderwerp, "net/stop") == 0)
    {
        printf("Stop commando ontvangen.\n");
        //Hier de stop van het brouwproces starten
    }
    else if (strcmp(bericht->onderwerp, "brouw/status") == 0)
    {
        cJSON* server = cJSON_GetObjectItemCaseSensitive(data, "server");
        cJSON* poort = cJSON_GetObjectItemCaseSensitive(data, "poort");

        char* server_IP = cJSON_IsString(server) ? server->valuestring : "onbekend";
        int poort_nummer = cJSON_IsNumber(poort) ? (int)poort->valuedouble : 0;

        printf("Server: %s, Poort: %d\n", server_IP, poort_nummer);

        char json[100];
        snprintf(json, sizeof(json), "{\"status\":\"verbonden\"}");
        scherm_uart_verstuur("net/status", json);
    }
    else
    {
        printf("Ontvangen: %s %s\n", bericht->onderwerp, bericht->json);
        //TODO: brouw/status, net/start en net/stop aan je procescode doorgeven.
        //net/start en net/stop krijgen GEEN ack; resultaat gaat via net/status.
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
    //spi_init();
    scherm_uart_init();

    xTaskCreate(temp_task, "Temperature task", 1024*8, NULL, 5, &temp_handle);
    xTaskCreate(receive_task, "Receive task", 1024*8, NULL, 5, NULL);

    const esp_err_t fout = brouw_netwerk_start("admin", false);
    if (fout != ESP_OK) 
    {
        ESP_LOGE("main", "Netwerkstart: %s", esp_err_to_name(fout));
    }

    /*
    while(1)
    {   
        //Geeft aan hoeveel geheugen er door taak wordt gebruikt
        printf("stack gebruikt: %ld\n", uxTaskGetStackHighWaterMark2(temp_handle));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    */
}
