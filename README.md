# Waveshare ESP32-C6 1.47"

Projekte für das Waveshare **ESP32-C6-LCD-1.47** (172 × 320, ST7789, 4 MB Flash, RGB-LED).

## Flitz hüpft

Ein Jump'n'Run im Querformat. Flitz läuft von allein, die Welt wird zufällig erzeugt.

- **BOOT-Taste kurz tippen:** kleiner Sprung
- **BOOT-Taste halten:** hoher Sprung
- Münzen: 10 Punkte, Stachelkugel von oben: 25 Punkte, jeder Meter: 1 Punkt
- Der Rekord bleibt nach dem Ausschalten erhalten (NVS).
- RGB-LED: gelb = Münze, grün = Treffer, rot = Game Over

## Bauen und Flashen

Voraussetzung: PlatformIO (pioarduino, Arduino-ESP32 3.3).

```
pio run -t upload
```

Falls der Upload nicht startet: BOOT gedrückt halten, kurz RESET drücken, BOOT loslassen.

## Pins

| Funktion | GPIO |
|---|---|
| LCD MOSI / SCLK | 6 / 7 |
| LCD CS / DC / RST | 14 / 15 / 21 |
| Hintergrundlicht (PWM, ~50 %) | 22 |
| RGB-LED (WS2812) | 8 |
| BOOT-Taste | 9 |

## Branch-Namen

Nur zweistufig `präfix/name`, z. B. `feat/flitz-huepft`. Einmal pro Klon: `tools\git_hooks_einrichten.bat`.
