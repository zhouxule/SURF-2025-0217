In this project, our custom designed boards can only use serial to output data. It is unrealistic to bring the laptop outside under burning hot days, so we use an ESP32 wemos d1-mini development board to fetch data from base station via serial.



##### ESP32 functions are shown below:

* ESP32 can fetch data from base station through serial, and save data in csv file in SPIFFS.

* ESP32 can also connected to Wi-Fi and sync time from NTP server.
* We add a sensor AHT20 to record temperature and humidity every time the data comes in, and save it to csv in the same line.
* When connected to it, the client can see the serial data and download/delete csv file directly on a web page.

(Note that if ESP32 can not connect to Wi-Fi or sync time, it will record time from booting using millis().



##### The data recorded in csv are like below:

Time, Serial data, Temperature, Humidity



##### The serial data includes lora node number (ADDL) and distance to base station (LoRaDistance) like below:

ADDL: 1##LoRaDistance:  46m

