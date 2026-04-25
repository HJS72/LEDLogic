# LEDLogic

LEDLogic ist eine webbasierte LED-Steuerung fuer ESP32 mit WS2812-LEDs. 
Der Fokus liegt auf einer einfachen visuellen Programmierung von LED-Ablaufen direkt im Browser.

## Was das Programm macht

- Steuert einen WS2812-LED-Strip ueber den ESP32 (Daten-Pin GPIO 5).
- Bietet eine visuelle Script-Oberflaeche mit Drag-and-Drop.
- Fuehrt LED-Aktionen als Ablauf aus und kann den Ablauf speichern, starten, stoppen und loeschen.
- Zeigt eine LED-Simulation in der Toolbar, damit das Script-Verhalten sofort sichtbar ist.

## LED-Steuerung im Detail

- LED-Anzahl ist einstellbar (1 bis 12).
- Pro Script-Schritt stehen unter anderem folgende Aktionen zur Verfuegung:
	- Farbe setzen
	- Helligkeit setzen
	- Ueberblenden (Fade)
	- Warten
	- Repeat-Block (Start/Ende)
	- Alles aus
- Farben koennen ueber ein Farbrad oder per Hex-Textfeld gesetzt werden.
- Script-Ablauf kann als Loop wiederholt werden.

## Web-UI

- Obere Aktionsleiste mit:
	- Speichern
	- Start
	- Stop
	- Loeschen
- Zustandsanzeige fuer laufendes/gestopptes Script.
- LED-Simulator neben den Buttons zur schnellen visuellen Kontrolle.

## Build und Flash

```bash
pio run
pio run -t upload
pio device monitor
```

## Hinweis zu WLAN, Captive Portal und OTA

WLAN-Konfiguration, Captive Portal und OTA-Update sind weiterhin im Projekt vorhanden, stehen bei LEDLogic aber nicht im Vordergrund. Diese Funktionen liegen gesammelt auf der Konfigurationsseite.
