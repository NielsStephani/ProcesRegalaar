# MQTT-component voor de brouwketel

Voor de procescontroller met **ESP-IDF 5.4.3** en de bestaande component `brouw_netwerk`.

## Installeren

1. Pak de map `brouw_mqtt` uit onder `components/` in je ESP-IDF-project, naast `brouw_netwerk`.
2. Voeg `brouw_mqtt` toe aan de bestaande `REQUIRES` of `PRIV_REQUIRES` in `main/CMakeLists.txt`. De component gebruikt de ESP-MQTT-client uit ESP-IDF.
3. Zet `#include "brouw_mqtt.h"` bovenaan `main.c`. Plaats na je bestaande aanroep van `brouw_netwerk_start()` deze regel in `app_main()`.

   ```c
   ESP_ERROR_CHECK(brouw_mqtt_init("Brouwketel 1", true));
   ```

Met `true` wacht de MQTT-taak op een wifi-IP-adres en leest ze de brokergegevens uit NVS via `brouw_netwerk_lees_mqtt()`. De component verbindt opnieuw als de broker of wifi wegvalt. Na opslaan in het configuratieformulier gebruikt de nieuwe MQTT-sessie de opgeslagen instellingen.

Via het bestaande formulier geef je de hostnaam of het IPv4-adres, de poort, de gebruikersnaam en het wachtwoord van de broker in. De component gebruikt MQTT 3.1.1 over TCP (`mqtt://`). Gebruik de poort die informatica opgeeft. `1883` is de gebruikelijke TCP-poort.

### Verbinden via het scherm

Gebruik `brouw_mqtt_init("Brouwketel 1", false)` als de operator via het scherm bepaalt wanneer MQTT verbindt. Laat `net/start` dan `brouw_mqtt_start()` aanvragen. Gebruik `brouw_mqtt_is_verbonden()` in de taak die `net/status` terugstuurt. `ESP_OK` van `brouw_mqtt_start()` betekent dat de startaanvraag is aangenomen.

Roep bij `net/stop` `brouw_mqtt_stop()` aan vanuit de taak die de MQTT-verbinding bedient. Die functie wacht op de MQTT-bevestiging van `offline` met een wachttijd van 2 seconden voor de PUBACK. Daarna wordt de client gesloten. Bij `ESP_ERR_TIMEOUT` kon die melding niet worden bevestigd. Een volgende `brouw_mqtt_start()` opent een nieuwe sessie.

## Topics en berichtvorm

De component bouwt het voorvoegsel `brouwer/<id>/` op met het MAC-adres van de wifi-stationinterface in kleine letters en zonder dubbele punten. Een voorbeeld is `brouwer/a4cf12b3c8e0/`. De client-ID is `brouwer-a4cf12b3c8e0`. De leesbare ketelnaam komt uit `brouw_mqtt_init()` en staat in `info`. De firmwareversie komt uit de ESP-IDF-appbeschrijving.

| Relatief topic | Richting | Voorbeeld van payload | QoS | Retained |
| --- | --- | --- | --- | --- |
| `info` | Ketel → broker | JSON met `id`, `type`, `name`, `firmware`, `sensors`, `actuators`, `commands` | 1 | Ja |
| `status` | Ketel → broker | `online` of `offline` | 1 | Ja |
| `sensor/temperatuur` | Ketel → broker | `66.8` in °C | 0 | Nee |
| `actuator/pomp/state` | Ketel → broker | `on` of `off` | 1 | Ja |
| `actuator/verwarming/state` | Ketel → broker | `42` voor 42% | 1 | Ja |
| `cmd/recept` | Broker → ketel | `{"message_id":"test-001","data":{...}}` | 2 | Nee |
| `ack` | Ketel → broker | `{"message_id":"test-001","ok":true}` | 1 | Nee |
| `fout` | Ketel → broker | `{"code":"sensor","message":"Temperatuursensor geeft geen waarde"}` | 1 | Nee |

Na elke verbinding vraagt de client het receptabonnement aan, publiceert hij `info` en daarna `status=online`. In `info.commands` staat `recept`. De toegelaten verwarmingswaarden staan in `info.actuators[].values` als strings van `"0"` tot `"100"`, passend bij de tekstpayload van het topic. De MQTT-sessie gebruikt clean session, een keepalive van 30 seconden en `offline` als retained Last Will.

## Publiceren vanuit je toepassing

Geef `brouw_mqtt_publiceer()` het relatieve topic door. QoS, retain en het MAC-voorvoegsel worden automatisch ingevuld. Onderstaande functie verwacht de werkelijke temperatuur, pompstatus en het werkelijk uitgestuurde verwarmingsvermogen.

```c
#include <stdio.h>
#include "brouw_mqtt.h"

static void publiceer_metingen(float temperatuur_c, bool pomp_aan, unsigned verwarming_pct)
{
    char tekst[24];

    if (!brouw_mqtt_is_verbonden()) {
        return;
    }

    snprintf(tekst, sizeof(tekst), "%.1f", (double)temperatuur_c);
    brouw_mqtt_publiceer("sensor/temperatuur", tekst);

    brouw_mqtt_publiceer("actuator/pomp/state", pomp_aan ? "on" : "off");

    snprintf(tekst, sizeof(tekst), "%u", verwarming_pct);
    brouw_mqtt_publiceer("actuator/verwarming/state", tekst);
}
```

Roep dit bijvoorbeeld periodiek aan vanuit je telemetrietaak. Zo krijgen de actuatoren ook na het opnieuw verbinden actuele retained waarden. De publicatiefunctie neemt maximaal 4096 bytes per payload aan. `ESP_OK` betekent dat het bericht is klaargezet voor verzending. `ESP_ERR_INVALID_STATE` betekent dat de MQTT-sessie op dat moment niet beschikbaar is. `ESP_ERR_NO_MEM` betekent dat de uitgaande wachtrij vol zit.

## Recept ontvangen en bevestigen

Start je recepttaak na `brouw_mqtt_init()`. De taak kan bijvoorbeeld `pdMS_TO_TICKS(1000)` meegeven of met `0` meteen controleren of er iets in de wachtrij staat. `recept.data` is het JSON-object met de inhoud uit `GET /api/recipes/{id}`. De component voegt eventuele berichtfragmenten samen en verwerkt een volledige MQTT-payload tot **4096 bytes**, inclusief de JSON-envelop.

Dit voorbeeld sluit aan op je eigen `laad_recept()`-functie. Die functie moet de structuur valideren, de gegevens naar de procescontroller of receptopslag kopiëren en `ESP_OK` teruggeven zodra het recept geladen is.

```c
static void recepttaak(void *arg)
{
    (void)arg;
    for (;;) {
        brouw_mqtt_recept_t recept = {0};
        const esp_err_t ontvangst = brouw_mqtt_ontvang_recept(&recept, portMAX_DELAY);
        if (ontvangst != ESP_OK) {
            continue;
        }

        const esp_err_t laden = laad_recept(recept.data);
        const bool ok = laden == ESP_OK;
        const esp_err_t ack = brouw_mqtt_ack(recept.message_id, ok,
                                            ok ? NULL : esp_err_to_name(laden));
        if (ack != ESP_OK) {
            printf("Receptbevestiging: %s\n", esp_err_to_name(ack));
        }
        brouw_mqtt_recept_vrijgeven(&recept);
    }
}
```

`recept.data` en `recept.message_id` blijven geldig tot `brouw_mqtt_recept_vrijgeven()`. Kopieer gegevens die je receptlader daarna nog nodig heeft. Stuur de `ack` zodra de laadpoging klaar is. De back-end verwacht die binnen 10 seconden. Bij een geldige `message_id` met een verkeerd `data`-type stuurt de component zelf een negatieve `ack`. Bij onleesbare JSON, een te groot recept of een volle receptwachtrij volgt de back-endtimeout.

## Test met de broker

1. Abonneer je via een MQTT-client op `brouwer/#`. Start de ESP32 en kijk of `info` en `status` met payload `online` verschijnen. Noteer de 12 tekens uit het topic.
2. Publiceer het onderstaande JSON-bericht op `brouwer/<id>/cmd/recept` met QoS 2 en retain uit. Gebruik voor een test van de volledige laadroute een echt recept van de back-end in `data`.

   ```json
   {"message_id":"test-001","data":{"id":1,"name":"Testrecept"}}
   ```

3. Controleer `brouwer/<id>/ack` op `message_id` `test-001` en het resultaat van je receptlader. Publiceer met je telemetrietaak ook de drie waarden uit de tabel.
4. Controleer dat `brouw_mqtt_stop()` `offline` publiceert. Test ook een stroomonderbreking om de Last Will vanaf de broker te zien en controleer na opstarten opnieuw `info` en `online`.
