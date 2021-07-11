/**********************************************************************************
 * 07/11/2021 Edward Williams
 * This is a shell which includes an Over-The-Air firmware update web server which
 * includes the option to erase EEPROM and preferences, fixed IP address, a major 
 * fail flashing led notice with sleep reboot, time set and mount of SD card. I use 
 * this as a starting point for all my sketches. Visit the web site <IP>/updatefirmware 
 * and enter the password to upload new firmware to the ESP32. Compile using the 
 * "Default" Partition Scheme.
 **********************************************************************************/
 
// edit the below for local settings 

// wifi info
const char* ssid = "YourSSID";
const char* password = "YourSSIDPwd";
// fixed IP info
const uint8_t IP_Address[4] = {192, 168, 2, 30};
const uint8_t IP_Gateway[4] = {192, 168, 2, 1};
const uint8_t IP_Subnet[4] = {255, 255, 255, 0};
const uint8_t IP_PrimaryDNS[4] = {8, 8, 8, 8};
const uint8_t IP_SecondaryDNS[4] = {8, 8, 4, 4};

const char* TZ_INFO = "PST8PDT,M3.2.0/2:00:00,M11.1.0/2:00:00";

const int SERVER_PORT = 80;  // port the main web server will listen on

const char* appName = "ESP32OTAWebUpdater";
const char* appVersion = "1.0.7";
const char* firmwareUpdatePassword = "87654321";

// should not need to edit the below

#include "esp_http_server.h"
httpd_handle_t webserver_httpd = NULL;   

#include <WiFi.h>

WiFiEventId_t eventID;

#include "soc/soc.h"  //disable brownout problems
#include "soc/rtc_cntl_reg.h"  //disable brownout problems

#include "time.h"
#include "lwip/err.h"
#include "lwip/apps/sntp.h"
struct tm timeinfo;
time_t now;

#include <EEPROM.h>  // for erasing EEPROM ATO, assumes EEPROM is 512 bytes in size
#include <Update.h>  // for flashing new firmware

#include <Preferences.h>  // for erasing preferences in the my_app namespace
Preferences preferences;

#define uS_TO_S_FACTOR 1000000LL  // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP5S  5        // Time ESP32 will go to sleep (in seconds)


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

void major_fail() {  //flash the led on the ESP32 in case of error, then sleep and reboot

  for  (int i = 0;  i < 5; i++) {
    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);
    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);
    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);

    delay(150);

    digitalWrite(33, LOW);  delay(500);
    digitalWrite(33, HIGH); delay(150);
    digitalWrite(33, LOW);  delay(500);
    digitalWrite(33, HIGH); delay(150);
    digitalWrite(33, LOW);  delay(500);
    digitalWrite(33, HIGH); delay(150);

    delay(150);

    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);
    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);
    digitalWrite(33, LOW);   delay(150);
    digitalWrite(33, HIGH);  delay(150);

    delay(450);
  }

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP5S * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

bool init_wifi()
{
  int connAttempts = 0;
  
  // this is the fixed ip stuff 
  IPAddress local_IP(IP_Address);

  // Set your Gateway IP address
  IPAddress gateway(IP_Gateway);

  IPAddress subnet(IP_Subnet);
  IPAddress primaryDNS(IP_PrimaryDNS); // optional
  IPAddress secondaryDNS(IP_SecondaryDNS); // optional

  //WiFi.persistent(false);
  
  WiFi.mode(WIFI_STA);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
    major_fail();
  }

  //WiFi.setSleep(false);  // turn off wifi power saving, makes response MUCH faster
  
  WiFi.printDiag(Serial);

  // uncomment the below to use DHCP
  //WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // workaround for bug in WiFi class

  WiFi.begin(ssid, password);
  
  char hostname[12];
  sprintf( hostname, "ESP32CAM%d", IP_Address[3] );
  WiFi.setHostname(hostname);  // only effective when using DHCP

  while (WiFi.status() != WL_CONNECTED ) {
    delay(500);
    Serial.print(".");
    if (connAttempts > 10) {
      Serial.println("Cannot connect");
      WiFi.printDiag(Serial);
      major_fail();
      return false;
    }
    connAttempts++;
  }
  return true;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

void init_time() {

  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_setservername(1, "time.windows.com");
  sntp_setservername(2, "time.nist.gov");

  sntp_init();

  // wait for time to be set
  time_t now = 0;
  timeinfo = { 0 };
  int retry = 0;
  const int retry_count = 10;
  while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
    Serial.printf("Waiting for system time to be set... (%d/%d) -- %d\n", retry, retry_count, timeinfo.tm_year);
    delay(2000);
    time(&now);
    localtime_r(&now, &timeinfo);
    Serial.println(ctime(&now));
  }

  if (timeinfo.tm_year < (2016 - 1900)) {
    major_fail();
  }
}


char * the_page = NULL;  // used to hold complete response page before its sent to browser

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

static esp_err_t updatefirmware_get_handler(httpd_req_t *req) {

  Serial.println("In updatefirmware_get_handler");

  const char msg[] PROGMEM = R"rawliteral(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Firmware Updater</title>
<script>

function clear_status() {
  document.getElementById('status').innerHTML = " &nbsp; ";
}

function uploadfile(file) {
  if ( !document.getElementById("updatefile").value ) {
    alert( "Choose a valid firmware file" );
  } else {
    let xhr = new XMLHttpRequest();
    document.getElementById('updatebutton').disabled = true;
    document.getElementById('status').innerText = "Progress 0%%";
    let eraseEEPROMvalue = document.getElementById('EraseEEPROM').checked;
    let upwd = document.getElementById('upwd').value;
    // track upload progress
    xhr.upload.onprogress = function(event) {
      if (event.lengthComputable) {
        var per = event.loaded / event.total;
        document.getElementById('status').innerText = "Progress " + Math.round(per*100) + "%%";
      }
    };
    // track completion: both successful or not
    xhr.onloadend = function() {
      if (xhr.status == 200) {
        document.getElementById('status').innerText = xhr.response;
      } else {
        document.getElementById('status').innerText = "Firmware update failed";
      }
      document.getElementById('updatebutton').disabled = false;
      document.getElementById('upwd').value = "";
    };
    xhr.open("POST", "/updatefirmware");
    xhr.setRequestHeader('EraseEEPROM', eraseEEPROMvalue);
    xhr.setRequestHeader('UPwd', upwd);
    xhr.send(file);
  }
}

</script>
</head>
<body><center>
<h1>ESP32 Firmware Updater</h1>

Select an ESP32 firmware file (.bin) to update the ESP32 firmware<br><br>

<table>
<tr><td align="center"><input type="file" id="updatefile" accept=".bin" onclick="clear_status();"><br><br></td></tr>
<tr><td align="center"><input type="checkbox" id="EraseEEPROM" onclick="clear_status();"> Erase EEPROM/Preferences<br><br></td></tr>
<tr><td align="center">Update Password <input type="password" id="upwd" maxlength="20"><br><br></td></tr>
<tr><td align="center"><input type="button" id="updatebutton" onclick="uploadfile(updatefile.files[0]);" value="Update"><br><br></td></tr>
<tr><td align="center"><div id="status"> &nbsp; </div><br><br></td></tr>
<tr><td align="center">%s Version %s</td></tr>
</table>
</center></body>
</html>)rawliteral";

  //strcpy(the_page, msg);
  sprintf(the_page, msg, appName, appVersion);

  httpd_resp_send(req, the_page, strlen(the_page));

  return ESP_OK;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 
#define UPLOAD_CACHE_SIZE 1600

static esp_err_t updatefirmware_post_handler(httpd_req_t *req) {

  Serial.println("In updatefirmware_post_handler");

  char contentBuffer[UPLOAD_CACHE_SIZE];
  size_t recv_size = sizeof(contentBuffer);
  size_t contentLen = req->content_len;

  char eraseEeprom[10];
  httpd_req_get_hdr_value_str(req, "EraseEEPROM", eraseEeprom, sizeof(eraseEeprom));
  Serial.println((String) "EraseEEPROM/Preferences " + eraseEeprom );
  char upwd[20];
  httpd_req_get_hdr_value_str(req, "UPwd", upwd, sizeof(upwd));
  Serial.println((String) "Update password " + upwd );

  Serial.println((String) "Content length is " + contentLen);

  if ( !strcmp( firmwareUpdatePassword, upwd ) ) {
    // update password is good, do the firmware update

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start flash with max available size
      Update.printError(Serial);
      strcpy(the_page, "Firmware update failed - Update failed begin step");
      Serial.println( the_page );
      httpd_resp_send(req, the_page, strlen(the_page));
      return ESP_OK;
    }
      
    size_t bytes_recvd = 0;
    while (bytes_recvd < contentLen) {
      //if ((contentLen - bytes_recvd) < recv_size) recv_size = contentLen - bytes_recvd;
      int ret = httpd_req_recv(req, contentBuffer, recv_size);
      if (ret <= ESP_OK) {  /* ESP_OK return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
          httpd_resp_send_408(req);
        }
        return ESP_FAIL;
      }
      if (Update.write((uint8_t *) contentBuffer, ret) != ret) {
        Update.printError(Serial);
        strcpy(the_page, "Firmware update failed - Update failed write step");
        Serial.println( the_page );
        httpd_resp_send(req, the_page, strlen(the_page));
        return ESP_OK;
      }
      bytes_recvd += ret;
    }

    if (!Update.end(true)) { //true to set the size to the current progress
      Update.printError(Serial);
      strcpy(the_page, "Firmware update failed - Update failed end step");
      Serial.println( the_page );
      httpd_resp_send(req, the_page, strlen(the_page));
      return ESP_OK;
    }

    if ( !strcmp( "true", eraseEeprom ) ) { // erase EEPROM
      EEPROM.end();
      EEPROM.begin(512);
      for (int i = 0 ; i < 512 ; i++) {
        EEPROM.write(i, 0xFF);  // set all EEPROM memory to FF
      }
      EEPROM.end();
      strcpy(the_page, "Firmware update and EEPROM/Preferences erase successful - Rebooting");
    } else {
      strcpy(the_page, "Firmware update successful - Rebooting");
    }
    Serial.println( the_page );
    httpd_resp_send(req, the_page, strlen(the_page));

    delay(5000);
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP5S * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  } else {
    strcpy(the_page, "Firmware update failed - Invalid password");
  }

  Serial.println( the_page );
  httpd_resp_send(req, the_page, strlen(the_page));
  return ESP_OK;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

void startWebServer() {

  // start a web server for OTA updates
  // this same web server can be used for all other normal web server stuff
  // just add the appropriate uri handlers
  
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = SERVER_PORT;
  config.stack_size = 8192;

  httpd_uri_t updatefirmware_get_uri = {
    .uri       = "/updatefirmware",
    .method    = HTTP_GET,
    .handler   = updatefirmware_get_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t updatefirmware_post_uri = {
    .uri       = "/updatefirmware",
    .method    = HTTP_POST,
    .handler   = updatefirmware_post_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&webserver_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webserver_httpd, &updatefirmware_get_uri);
    httpd_register_uri_handler(webserver_httpd, &updatefirmware_post_uri);
  }
}

/*
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

// MicroSD
#include "SD_MMC.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static esp_err_t init_sdcard()
{
  esp_err_t ret = ESP_FAIL;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    //.max_files = 10,
    .max_files = 6,
  };
  sdmmc_card_t *card;

  Serial.println("Mounting SD card...");
  ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);

  if (ret == ESP_OK) {
    Serial.println("SD card mount successfully!");
  }  else  {
    Serial.printf("Failed to mount SD card VFAT filesystem. Error: %s", esp_err_to_name(ret));
    major_fail();
  }

  Serial.print("SD_MMC Begin: "); Serial.println(SD_MMC.begin());
}
*/


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// idle_task0() - used to calculate system load on CPU0 (run at priority 0)
//
static unsigned long idle_cnt0 = 0LL;

static void idle_task0(void *parm) {
  while(1==1) {
    int64_t now = esp_timer_get_time();     // time anchor
    vTaskDelay(0 / portTICK_RATE_MS);
    int64_t now2 = esp_timer_get_time();
    idle_cnt0 += (now2 - now) / 1000;        // diff
  }
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// idle_task1() - used to calculate system load on CPU1 (run at priority 0)
//
static unsigned long idle_cnt1 = 0LL;

static void idle_task1(void *parm) {
  while(1==1) {
    int64_t now = esp_timer_get_time();     // time anchor
    vTaskDelay(0 / portTICK_RATE_MS);
    int64_t now2 = esp_timer_get_time();
    idle_cnt1 += (now2 - now) / 1000;        // diff
  }
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// mon_task() - used to display system load (run at priority 10)
//
static int mon_task_freq = 60;  // how often mon_task will wake in seconds

static void mon_task(void *parm) {
  while(1==1) {
    float new_cnt0 =  (float)idle_cnt0;    // Save the count for printing it ...
    float new_cnt1 =  (float)idle_cnt1;    // Save the count for printing it ...
        
    // Compensate for the 100 ms delay artifact: 900 ms = 100%
    float cpu_percent0 = ((99.9 / 90.) * new_cnt0) / (mon_task_freq * 10);
    float cpu_percent1 = ((99.9 / 90.) * new_cnt1) / (mon_task_freq * 10);
    printf("Load (CPU0, CPU1): %.0f%%, %.0f%%\n", 100 - cpu_percent0, 
      100 - cpu_percent1); fflush(stdout);
    idle_cnt0 = 0;                        // Reset variable
    idle_cnt1 = 0;                        // Reset variable
    vTaskDelay((mon_task_freq * 1000) / portTICK_RATE_MS);
  }
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector

  Serial.begin(115200);

  pinMode(33, OUTPUT);    // little red led on back of chip
  digitalWrite(33, LOW);  // turn on the red LED on the back of chip

  // setup event watcher to catch if wifi disconnects
  eventID = WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.print("WiFi lost connection. Reason: ");
    Serial.println(info.disconnected.reason);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("*** connected/disconnected issue!   WiFi disconnected ???...");
      WiFi.disconnect();
    } else {
      Serial.println("*** WiFi disconnected ???...");
    }
  }, WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED);

  if (init_wifi()) { // Connected to WiFi
    Serial.println("Internet connected");

    init_time();
    time(&now);

    // set timezone
    setenv("TZ", TZ_INFO, 1); 
    tzset();
    time(&now);
    localtime_r(&now, &timeinfo);

    Serial.print("After timezone : "); Serial.println(ctime(&now));
  }

/*  
  // SD card init
  esp_err_t card_err = init_sdcard();
  if (card_err != ESP_OK) {
    Serial.printf("SD Card init failed with error 0x%x", card_err);
    major_fail();
    return;
  }
*/

  //uncomment the section below to report CPU load every 60 seconds
/*
  // create tasks for monitoring system load  
  xTaskCreatePinnedToCore(idle_task0, "idle_task0", 1024 * 2, NULL,  0, NULL, 0);
  xTaskCreatePinnedToCore(idle_task1, "idle_task1", 1024 * 2, NULL,  0, NULL, 1);
  xTaskCreate(mon_task, "mon_task", 1024 * 2, NULL, 10, NULL);
*/

  // allocate PSRAM for holding web page content before sending
  the_page = (char*) heap_caps_calloc(8000, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  startWebServer();

  digitalWrite(33, HIGH);  // turn off the red LED on the back of chip

  Serial.print("ESP32 Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// 
unsigned long last_wakeup_1sec = 0;

void loop() {

  if ( (millis() - last_wakeup_1sec) > 1000 ) {       // 1 second
    last_wakeup_1sec = millis();

    if (WiFi.status() != WL_CONNECTED) {
      init_wifi();
      Serial.println("***** WiFi reconnect *****");
    }
  }

  delay(200);
}
