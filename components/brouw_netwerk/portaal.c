#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "netwerk_priv.h"
#include "nvs.h"

#define FORMULIER_MAX 1536
#define FORMULIER_TYPE "application/x-www-form-urlencoded"

extern const char formulier_html_start[] asm("_binary_formulier_html_start");

static const char *TAG = "brouw_portaal";
// De HTTP-server handelt deze verzoeken af in één taak.
static bool s_bewaard;

static int hex_cijfer(unsigned char teken)
{
    if (teken >= '0' && teken <= '9') {
        return teken - '0';
    }
    if (teken >= 'A' && teken <= 'F') {
        return teken - 'A' + 10;
    }
    if (teken >= 'a' && teken <= 'f') {
        return teken - 'a' + 10;
    }
    return -1;
}

static bool decodeer_veld(char *uit, size_t capaciteit, const char *in)
{
    size_t lengte = 0;
    while (*in != '\0') {
        unsigned char teken = (unsigned char)*in++;
        if (teken == '+') {
            teken = ' ';
        } else if (teken == '%') {
            if (in[0] == '\0' || in[1] == '\0') {
                return false;
            }
            const int hoog = hex_cijfer(in[0]);
            const int laag = hex_cijfer(in[1]);
            if (hoog < 0 || laag < 0) {
                return false;
            }
            teken = (unsigned char)((hoog << 4) | laag);
            in += 2;
        }
        if (teken < ' ' || teken == 0x7f || lengte + 1 >= capaciteit) {
            return false;
        }
        uit[lengte++] = (char)teken;
    }
    uit[lengte] = '\0';
    return true;
}

static const char *formulier_lezen(char *inhoud, brouw_instellingen_t *instellingen)
{
    char poort[6];
    struct {
        const char *naam;
        char *waarde;
        size_t capaciteit;
        bool ontvangen;
    } velden[] = {
        {"ssid", instellingen->ssid, sizeof(instellingen->ssid), false},
        {"wifi_wachtwoord", instellingen->wifi_wachtwoord, sizeof(instellingen->wifi_wachtwoord), false},
        {"mqtt_host", instellingen->mqtt.host, sizeof(instellingen->mqtt.host), false},
        {"mqtt_poort", poort, sizeof(poort), false},
        {"mqtt_gebruikersnaam", instellingen->mqtt.gebruikersnaam, sizeof(instellingen->mqtt.gebruikersnaam), false},
        {"mqtt_wachtwoord", instellingen->mqtt.wachtwoord, sizeof(instellingen->mqtt.wachtwoord), false},
    };
    const size_t aantal_velden = sizeof(velden) / sizeof(velden[0]);
    char *volgend = NULL;

    for (char *paar = inhoud; paar != NULL; paar = volgend) {
        volgend = strchr(paar, '&');
        if (volgend != NULL) {
            *volgend++ = '\0';
        }
        char *waarde = strchr(paar, '=');
        if (waarde == NULL) {
            return "De gegevens konden niet gelezen worden. Open het formulier opnieuw.";
        }
        *waarde++ = '\0';

        size_t i = 0;
        while (i < aantal_velden && strcmp(paar, velden[i].naam) != 0) {
            ++i;
        }
        if (i == aantal_velden || velden[i].ontvangen) {
            return "Het formulier bevat een onbekend of dubbel veld.";
        }
        if (!decodeer_veld(velden[i].waarde, velden[i].capaciteit, waarde)) {
            return "Een veld is te lang of bevat een ongeldig teken. Ga terug en controleer de invoer.";
        }
        velden[i].ontvangen = true;
    }
    for (size_t i = 0; i < aantal_velden; ++i) {
        if (!velden[i].ontvangen) {
            return "Het formulier is onvolledig. Open het formulier opnieuw.";
        }
    }

    brouw_netwerk_mqtt_t *mqtt = &instellingen->mqtt;
    if (mqtt->host[0] != '\0' || poort[0] != '\0' ||
        mqtt->gebruikersnaam[0] != '\0' || mqtt->wachtwoord[0] != '\0') {
        if (mqtt->host[0] == '\0' || poort[0] == '\0' ||
            mqtt->gebruikersnaam[0] == '\0' || mqtt->wachtwoord[0] == '\0') {
            return "Vul de vier MQTT-velden in of laat ze allemaal leeg.";
        }
        const unsigned long poort_nummer = strtoul(poort, NULL, 10);
        if (strspn(poort, "0123456789") != strlen(poort) ||
            poort_nummer == 0 || poort_nummer > UINT16_MAX) {
            return "Vul een MQTT-poort van 1 tot 65535 in.";
        }
        mqtt->poort = (uint16_t)poort_nummer;
    }
    return brouw_instellingen_fout(instellingen);
}

static esp_err_t antwoord(httpd_req_t *verzoek, const char *status, const char *tekst)
{
    httpd_resp_set_status(verzoek, status);
    httpd_resp_set_type(verzoek, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(verzoek, "Cache-Control", "no-store");
    return httpd_resp_sendstr(verzoek, tekst);
}

static esp_err_t weiger_en_sluit(httpd_req_t *verzoek, const char *status, const char *tekst)
{
    antwoord(verzoek, status, tekst);
    return ESP_FAIL;
}

static bool lokaal_verzoek(httpd_req_t *verzoek)
{
    char host[sizeof(SETUP_HOST ":80")];
    if (httpd_req_get_hdr_value_str(verzoek, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, SETUP_HOST) != 0 && strcmp(host, SETUP_HOST ":80") != 0)) {
        return false;
    }

    char origin[sizeof("http://" SETUP_HOST ":80")];
    const esp_err_t fout =
        httpd_req_get_hdr_value_str(verzoek, "Origin", origin, sizeof(origin));
    if (fout == ESP_ERR_NOT_FOUND) {
        return true;
    }
    return fout == ESP_OK &&
           (strcasecmp(origin, "http://" SETUP_HOST) == 0 ||
            strcasecmp(origin, "http://" SETUP_HOST ":80") == 0);
}

static esp_err_t formulier_get(httpd_req_t *verzoek)
{
    if (!lokaal_verzoek(verzoek)) {
        return antwoord(verzoek, "403 Forbidden", "Open http://" SETUP_HOST "/");
    }
    httpd_resp_set_type(verzoek, "text/html; charset=utf-8");
    httpd_resp_set_hdr(verzoek, "Cache-Control", "no-store");
    return httpd_resp_sendstr(verzoek, formulier_html_start);
}

static esp_err_t instellingen_get(httpd_req_t *verzoek)
{
    if (!lokaal_verzoek(verzoek)) {
        return antwoord(verzoek, "403 Forbidden", "Open http://" SETUP_HOST "/");
    }

    brouw_instellingen_t instellingen = {0};
    const esp_err_t leesfout = brouw_instellingen_lees(&instellingen);
    if (leesfout == ESP_ERR_NVS_NOT_FOUND || leesfout == ESP_ERR_INVALID_SIZE ||
        leesfout == ESP_ERR_INVALID_VERSION || leesfout == ESP_ERR_INVALID_ARG) {
        memset(&instellingen, 0, sizeof(instellingen));
    } else if (leesfout != ESP_OK) {
        ESP_LOGE(TAG, "Instellingen lezen: %s", esp_err_to_name(leesfout));
        return antwoord(verzoek, "500 Internal Server Error", "De instellingen konden niet worden geladen.");
    }

    char poort[6] = "";
    if (instellingen.mqtt.poort != 0) {
        snprintf(poort, sizeof(poort), "%u", (unsigned)instellingen.mqtt.poort);
    }
    const struct {
        const char *naam;
        const char *waarde;
    } velden[] = {
        {"ssid", instellingen.ssid},
        {"wifi_wachtwoord", instellingen.wifi_wachtwoord},
        {"mqtt_host", instellingen.mqtt.host},
        {"mqtt_poort", poort},
        {"mqtt_gebruikersnaam", instellingen.mqtt.gebruikersnaam},
        {"mqtt_wachtwoord", instellingen.mqtt.wachtwoord},
    };

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return antwoord(verzoek, "503 Service Unavailable", "Te weinig geheugen om de instellingen te laden.");
    }
    for (size_t i = 0; i < sizeof(velden) / sizeof(velden[0]); ++i) {
        if (cJSON_AddStringToObject(json, velden[i].naam, velden[i].waarde) == NULL) {
            cJSON_Delete(json);
            return antwoord(verzoek, "503 Service Unavailable", "Te weinig geheugen om de instellingen te laden.");
        }
    }

    char *inhoud = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (inhoud == NULL) {
        return antwoord(verzoek, "503 Service Unavailable", "Te weinig geheugen om de instellingen te laden.");
    }
    httpd_resp_set_type(verzoek, "application/json; charset=utf-8");
    httpd_resp_set_hdr(verzoek, "Cache-Control", "no-store");
    const esp_err_t verzonden = httpd_resp_sendstr(verzoek, inhoud);
    cJSON_free(inhoud);
    return verzonden;
}

static esp_err_t formulier_post(httpd_req_t *verzoek)
{
    if (!lokaal_verzoek(verzoek)) {
        return weiger_en_sluit(verzoek, "403 Forbidden", "Open http://" SETUP_HOST "/");
    }
    if (s_bewaard) {
        return weiger_en_sluit(verzoek, "409 Conflict", "Opgeslagen. De ESP32 schakelt naar wifi.");
    }
    char content_type[80];
    if (httpd_req_get_hdr_value_str(verzoek, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, FORMULIER_TYPE) != 0) {
        return weiger_en_sluit(verzoek, "415 Unsupported Media Type", "Verstuur via het instellingenformulier.");
    }
    if (verzoek->content_len == 0 || verzoek->content_len > FORMULIER_MAX) {
        return weiger_en_sluit(verzoek, "400 Bad Request", "Het formulier is leeg of te groot.");
    }

    char *inhoud = malloc(verzoek->content_len + 1);
    if (inhoud == NULL) {
        return weiger_en_sluit(verzoek, "503 Service Unavailable", "Te weinig geheugen om het formulier te ontvangen.");
    }
    size_t ontvangen = 0;
    while (ontvangen < verzoek->content_len) {
        const int gelezen = httpd_req_recv(verzoek, inhoud + ontvangen, verzoek->content_len - ontvangen);
        if (gelezen == HTTPD_SOCK_ERR_TIMEOUT) {
            free(inhoud);
            return weiger_en_sluit(verzoek, "408 Request Timeout", "Het ontvangen van de instellingen duurde te lang.");
        }
        if (gelezen <= 0) {
            free(inhoud);
            return ESP_FAIL;
        }
        ontvangen += (size_t)gelezen;
    }
    if (memchr(inhoud, '\0', ontvangen) != NULL) {
        free(inhoud);
        return antwoord(verzoek, "400 Bad Request", "Het formulier bevat een ongeldig teken.");
    }
    inhoud[ontvangen] = '\0';

    brouw_instellingen_t instellingen = {0};
    const char *invoerfout = formulier_lezen(inhoud, &instellingen);
    free(inhoud);
    if (invoerfout != NULL) {
        return antwoord(verzoek, "400 Bad Request", invoerfout);
    }

    const esp_err_t fout = brouw_instellingen_bewaar(&instellingen);
    if (fout != ESP_OK) {
        ESP_LOGE(TAG, "Opslaan in NVS: %s", esp_err_to_name(fout));
        return antwoord(verzoek, "500 Internal Server Error", "Opslaan mislukt. De ESP32 blijft in setupmodus.");
    }

    s_bewaard = true;
    const esp_err_t verzonden = antwoord(verzoek, "200 OK",
        "Instellingen opgeslagen. Het setupnetwerk sluit en de brouwketel probeert te verbinden met de gekozen WiFi.\r\n" 
        "Wanneer er geen verbinding gemaakt kan worden, schakel de configuratie-modus in op het display.\r\n");
    brouw_netwerk_configuratie_opgeslagen();
    return verzonden;
}

esp_err_t brouw_portaal_start(TaskHandle_t netwerktaak, httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;

    s_bewaard = false;
    esp_err_t fout = httpd_start(server, &config);
    if (fout != ESP_OK) {
        return fout;
    }
    const httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = formulier_get };
    const httpd_uri_t instellingen = {
        .uri = "/instellingen", .method = HTTP_GET, .handler = instellingen_get,
    };
    const httpd_uri_t post = {
        .uri = "/opslaan", .method = HTTP_POST, .handler = formulier_post, .user_ctx = netwerktaak,
    };
    fout = httpd_register_uri_handler(*server, &get);
    if (fout == ESP_OK) {
        fout = httpd_register_uri_handler(*server, &instellingen);
    }
    if (fout == ESP_OK) {
        fout = httpd_register_uri_handler(*server, &post);
    }
    if (fout != ESP_OK) {
        httpd_stop(*server);
    }
    return fout;
}