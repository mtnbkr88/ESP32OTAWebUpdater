# ESP32 OTA Web Updater

ESP32 OTA web firmware and EEPROM eraser using esp_http_server.h

03/25/2020 Ed Williams 

This is a shell which includes a password protected Over-The-Air firmware update 
web server which includes the option to erase EEPROM. It also includes the following:

  - Web implementation is based on esp_http_server.h.
  - The use of a static IP address.
  - A major fail flashing led notification followed by sleep and reboot.
  - Time set using NTP if connected to the internet.
  - Mount of SD card.
  - Must compile using Default Partition Scheme which supports OTA firmware updates.
  - Does not require jquery so works over standalone network.

Visit the web site <IP>/updatefirmware and enter the password to upload new firmware 
to the ESP32. 

I use this sketch as a starting point for most of my sketches.

The most important pieces for someone wanting to integrate the web OTA capability in
their sketch is to include:

#include <EEPROM.h>  // for erasing EEPROM ATO, assumes EEPROM is 512 bytes in size  
#include <Update.h>  // for flashing new firmware  

#define uS_TO_S_FACTOR 1000000LL  // Conversion factor for micro seconds to seconds  
#define TIME_TO_SLEEP5S  5        // Time ESP32 will go to sleep (in seconds)  

and the GET and POST uri handler functions for /updatefirmware in the web server config:

updatefirmware_get_handler  
updatefirmware_post_handler  

