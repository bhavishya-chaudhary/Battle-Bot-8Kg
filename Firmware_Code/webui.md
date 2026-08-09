# Web UI

## Access

-   **Wi-Fi SSID:** `AndroidAP_6259`
-   **Wi-Fi Password:** `11221122`
-   **Dashboard:** `http://<ESP32-IP>/`
-   **OTA Update:** `http://<ESP32-IP>/update`
-   **OTA Username:** `admin`
-   **OTA Password:** `ota_secret_99`
-   **Telnet:** `<ESP32-IP>:23`

## Dashboard

### Live Telemetry

-   14-channel iBUS monitor
-   Throttle raw / output
-   Steering raw / output
-   Left / right motor speed
-   Left / right forward / reverse PWM
-   Arm status
-   ESC status
-   Weapon lock status
-   Weapon limit
-   Target / current weapon PWM
-   Weapon output percentage
-   Ramping status
-   Failsafe status
-   RC signal age
-   Good / bad iBUS frames
-   Uptime
-   Wi-Fi RSSI
-   Free heap
-   ESP32 IP address
-   System status

### Drive Tuning

-   Left Forward trim
-   Left Reverse trim
-   Right Forward trim
-   Right Reverse trim
-   Maximum throttle
-   Maximum steering
-   Throttle curve
-   Steering curve
-   Throttle deadzone
-   Steering deadzone
-   Save All
-   Reset All
-   EEPROM persistence

### Serial Monitor

-   Live firmware log
-   Clear log

## OTA Update

-   Authenticated firmware upload
-   `.bin` firmware update
-   Automatic reboot after successful update
