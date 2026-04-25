# ESP32 WLAN Captive Portal

Dieses Projekt startet auf einem ESP32 ein Captive Portal zur WLAN-Konfiguration und steuert einen WS2812-LED-Streifen.

Verhalten:

- Wenn gespeicherte WLAN-Zugangsdaten vorhanden und erreichbar sind, verbindet sich der ESP32 mit diesem WLAN.
- Wenn keine Zugangsdaten vorhanden sind oder das WLAN nicht erreichbar ist, startet ein Access Point mit Captive Portal.
- Wenn gespeicherte Zugangsdaten vorhanden, aber das WLAN nicht erreichbar ist, bleibt das Captive Portal aktiv und der ESP32 versucht parallel im Hintergrund weiter die Verbindung zum konfigurierten WLAN.
- Sobald die WLAN-Verbindung erfolgreich steht, wird der Access Point beendet.
- Wenn die WLAN-Verbindung abbricht, aktiviert der ESP32 automatisch wieder den Access Point als Fallback.
- Im normalen Betriebsmodus (verbunden mit SSID) liegt die LED-Steuerung auf der Startseite `/`.
- Alle bisherigen WLAN-, Debug- und OTA-Ausgaben/Funktionen liegen auf der separaten Konfigurationsseite `/config`.

## Standardwerte

- AP-SSID: `ESP32-Config`
- AP-Passwort: `configureme`
- Konfigurationsseite im AP-Modus: `http://192.168.4.1/config`

## WS2812 LED-Steuerung

- Daten-Pin: GPIO 5
- Anzahl LEDs: konfigurierbar von 1 bis 12
- Jede LED kann einzeln konfiguriert werden:
	- Aktiv/Inaktiv
	- Farbe
	- Helligkeit (0-255)

## OTA Update

Auf `/config` gibt es drei OTA-Wege:

- Version pruefen gegen GitHub URL (`/ota/check`)
- Direktes Firmware-Update von URL (`/ota/update_url`), z. B. GitHub Release Asset `.bin`
- Datei-Upload (`/ota/upload`) fuer `.bin`, `.zip` oder `.bin.gz`

Hinweis: Auf ESP32 wird der hochgeladene Stream direkt als Firmware verarbeitet. Praktisch wird meist eine `.bin` verwendet.

## Build und Flash

```bash
pio run
pio run -t upload
pio device monitor
```
