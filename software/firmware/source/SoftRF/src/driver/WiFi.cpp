/*
 * WiFiHelper.cpp
 * Copyright (C) 2016-2022 Linar Yusupov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../system/SoC.h"

#if defined(EXCLUDE_WIFI)
void WiFi_setup()   {}
void WiFi_loop()    {}
void WiFi_fini()    {}
#else

#include <FS.h>
#include <TimeLib.h>

#include "../../SoftRF.h"
#include "../system/OTA.h"
#include "GNSS.h"
#include "Settings.h"
#include "WiFi.h"
#include "tcpip_adapter.h"
#include "../TrafficHelper.h"
#include "RF.h"
#include "../ui/Web.h"
#include "../protocol/data/NMEA.h"
#include "Battery.h"

String station_ssid = MY_ACCESSPOINT_SSID ;
String station_psk  = MY_ACCESSPOINT_PSK ;

String host_name = HOSTNAME;   // "SoftRF"

IPAddress local_IP(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

/**
 * Default WiFi connection information.
 *
 */
const char* ap_default_psk = "12345678";   // Default PSK.

#if defined(USE_DNS_SERVER)
#include <DNSServer.h>

const byte DNS_PORT = 53;
DNSServer dnsServer;
bool dns_active = false;
#endif

// A UDP instance to let us send and receive packets over UDP
WiFiUDP Uni_Udp;
bool udp_is_ready = 0;
char UDPpacketBuffer[UDP_PACKET_BUFSIZE];

#if defined(POWER_SAVING_WIFI_TIMEOUT)
static unsigned long WiFi_No_Clients_Time_ms = 0;
#endif

#if 0
/**
 * @brief Read WiFi connection information from file system.
 * @param ssid String pointer for storing SSID.
 * @param pass String pointer for storing PSK.
 * @return True or False.
 * 
 * The config file have to containt the WiFi SSID in the first line
 * and the WiFi PSK in the second line.
 * Line seperator can be \r\n (CR LF) \r or \n.
 */
bool loadConfig(String *ssid, String *pass)
{
  // open file for reading.
  File configFile = SPIFFS.open("/cl_conf.txt", "r");
  if (!configFile)
  {
    Serial.println(F("Failed to open cl_conf.txt."));

    return false;
  }

  // Read content from config file.
  String content = configFile.readString();
  configFile.close();

  content.trim();

  // Check if ther is a second line available.
  int8_t pos = content.indexOf("\r\n");
  uint8_t le = 2;
  // check for linux and mac line ending.
  if (pos == -1)
  {
    le = 1;
    pos = content.indexOf("\n");
    if (pos == -1)
    {
      pos = content.indexOf("\r");
    }
  }

  // If there is no second line: Some information is missing.
  if (pos == -1)
  {
    Serial.println(F("Invalid content."));
    Serial.println(content);

    return false;
  }

  // Store SSID and PSK into string vars.
  *ssid = content.substring(0, pos);
  *pass = content.substring(pos + le);

  ssid->trim();
  pass->trim();

#ifdef SERIAL_VERBOSE
  Serial.println("----- file content -----");
  Serial.println(content);
  Serial.println("----- file content -----");
  Serial.println("ssid: " + *ssid);
  Serial.println("psk:  " + *pass);
#endif

  return true;
} // loadConfig


/**
 * @brief Save WiFi SSID and PSK to configuration file.
 * @param ssid SSID as string pointer.
 * @param pass PSK as string pointer,
 * @return True or False.
 */
bool saveConfig(String *ssid, String *pass)
{
  // Open config file for writing.
  File configFile = SPIFFS.open("/cl_conf.txt", "w");
  if (!configFile)
  {
    Serial.println(F("Failed to open cl_conf.txt for writing"));

    return false;
  }

  // Save SSID and PSK.
  configFile.println(*ssid);
  configFile.println(*pass);

  configFile.close();

  return true;
} // saveConfig
#endif

// general UDP receiving (code from SkyView):
size_t WiFi_Receive_UDP(uint8_t *buf, size_t max_size)
{
  int noBytes = Uni_Udp.parsePacket();
  if ( noBytes ) {

    if (noBytes > max_size) {
      noBytes = max_size;
    }

    // We've received a packet, read the data from it
    Uni_Udp.read(buf,noBytes); // read the packet into the buffer

    return (size_t) noBytes;
  } else {
    return 0;
  }
}

#if 0
// was used only in bridge mode, was called from SoftRF.ino,
//   - replaced with WiFi_Receive_UDP(buf, MAX_PKT_SIZE)
size_t Raw_Receive_UDP(uint8_t *buf)
{
  int noBytes = Uni_Udp.parsePacket();
  if ( noBytes ) {

    if (noBytes > MAX_PKT_SIZE) {
      noBytes = MAX_PKT_SIZE;
    }

    // We've received a packet, read the data from it
    Uni_Udp.read(buf,noBytes); // read the packet into the buffer

    return (size_t) noBytes;
  } else {
    return 0;
  }
}
#endif

void Raw_Transmit_UDP()
{
    size_t rx_size = RF_Payload_Size(settings->rf_protocol);
    rx_size = rx_size > sizeof(fo_raw) ? sizeof(fo_raw) : rx_size;
    String str = Bin2Hex(fo_raw, rx_size);
    size_t len = str.length();
    // ASSERT(sizeof(UDPpacketBuffer) > 2 * PKT_SIZE + 1)
    str.toCharArray(UDPpacketBuffer, sizeof(UDPpacketBuffer));
    UDPpacketBuffer[len] = '\n';
    SoC->WiFi_transmit_UDP(RELAY_DST_PORT, (byte *)UDPpacketBuffer, len + 1);
}

// Extend DHCP Lease time - check and set
#if defined(ESP32)
void printLeaseTime(){
    uint32_t leaseTime = 0;
    if(!tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_GET,
            TCPIP_ADAPTER_IP_ADDRESS_LEASE_TIME, (void*)&leaseTime, 4)){
      Serial.printf("DHCPS Lease Time: %u\r\n", leaseTime);
    }
}
void setLeaseTime(){
    uint32_t lease_time = 24*60; // 24 hours
    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    tcpip_adapter_dhcps_option(
      (tcpip_adapter_dhcp_option_mode_t)TCPIP_ADAPTER_OP_SET,
      (tcpip_adapter_dhcp_option_id_t)TCPIP_ADAPTER_IP_ADDRESS_LEASE_TIME,
      (void*)&lease_time, sizeof(uint32_t)
    );
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
}
#endif

/**
 * @brief Arduino setup function.
 */
void WiFi_setup()
{
#if 0
  // Initialize file system.
  //if (!SPIFFS.begin()) {
  //  Serial.println(F("Failed to mount file system"));
  //  return;
  //}

  // Load wifi connection information.
  if (! loadConfig(&station_ssid, &station_psk))
  {
    station_ssid = MY_ACCESSPOINT_SSID ;
    station_psk = MY_ACCESSPOINT_PSK ;

    Serial.println(F("No WiFi connection information available."));
  }
#endif

  // Check WiFi connection
  // ... check mode
  if (WiFi.getMode() != WIFI_STA)
  {
    WiFi.mode(WIFI_STA);
    delay(100);
  }

  bool try_external = false;

  if (settings->ssid[0] != '\0') {

    if (settings->psk[0] != '\0') {
        station_ssid = settings->ssid;    // SSID and PSK of external network from settings
        station_psk  = settings->psk;
        try_external = true;
    } else {
        // no password - use settings->ssid as a custom SSID for AP mode
        strcpy(settings->myssid, settings->ssid);
    }

    // ... Compare file config with sdk config.
    if (try_external && (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk))
    {
      //Serial.println(F("WiFi config changed."));
      // ... Try to connect to WiFi station.
      WiFi.begin(station_ssid.c_str(), station_psk.c_str());
      // ... Print new SSID
      Serial.print(F("new SSID: "));
      Serial.println(WiFi.SSID());
      // ... Uncomment this for debugging output.
      //WiFi.printDiag(Serial);
    } else {
      // ... Begin with sdk config.
      WiFi.begin();
    }

  } else {   // empty SSID from settings
      WiFi.begin();
  }

  // Set Hostname.
  if (settings->myssid[0] != '\0') {
      host_name = settings->myssid;
  } else {
      host_name += "-";
      char chipID[8];
      snprintf(chipID, 8, "%06x", (SoC->getChipId() & 0xFFFFFF));
      host_name += chipID;           // SoftRF-xxxxxx
  }
  SoC->WiFi_hostname(host_name);

  // Print hostname.
  Serial.println("Hostname: " + host_name);

  if (try_external) {    // first try and connect to external network

      Serial.println(F("Wait for WiFi connection."));

      // ... Give ESP 10 seconds to connect to station.
      unsigned long startTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000)
      {
        Serial.write('.');
        //Serial.print(WiFi.status());
        delay(500);
      }
      Serial.println();

  }

  // Check connection
  if(WiFi.status() == WL_CONNECTED)
  {
    // ... print IP Address
    Serial.print(F("Connected to: "));
    Serial.println(WiFi.SSID());
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println(F("Can not connect to WiFi station. Go into AP mode."));
    
    // Go into software AP mode.  Use default password even if custom SSID.
    WiFi.mode(WIFI_AP);
    SoC->WiFi_set_param(WIFI_PARAM_TX_POWER, WIFI_TX_POWER_MED); // 10 dBm
    SoC->WiFi_set_param(WIFI_PARAM_DHCP_LEASE_TIME, WIFI_DHCP_LEASE_HRS);
    delay(10);

    Serial.print(F("Setting soft-AP configuration ... "));
    Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ?
      F("Ready") : F("Failed!"));

    Serial.print(F("Setting soft-AP ... "));
    Serial.print(host_name);
    Serial.print(" ");
    Serial.print(ap_default_psk);
    Serial.println(WiFi.softAP(host_name.c_str(), ap_default_psk) ?
      F(" - Ready") : F(" - Failed!"));
#if defined(USE_DNS_SERVER)
    // if DNSServer is started with "*" for domain name, it will reply with
    // provided IP to all DNS request
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    dns_active = true;
#endif
    Serial.print(F("IP address: "));
    Serial.println(WiFi.softAPIP());

#if defined(ESP32)
    // Extend DHCP lease time
    printLeaseTime();
    setLeaseTime();
    printLeaseTime();
#endif
  }

  unsigned int UDP_Input_Port = NMEA_UDP_PORT;   // local port to listen for UDP packets
  if (settings->gdl90_in == DEST_UDP)
      UDP_Input_Port = GDL90_DST_PORT;
  else if (settings->mode == SOFTRF_MODE_BRIDGE)
      UDP_Input_Port = RELAY_SRC_PORT;
  else if (settings->nmea_out==DEST_UDP || settings->nmea_out2==DEST_UDP)
      UDP_Input_Port = ALT_UDP_PORT;  // <<< or use NMEA_UDP_PORT2 (10111)?
  if (Uni_Udp.begin(UDP_Input_Port)) {
      Serial.print(F("UDP server started, listening at port: "));
      Serial.println(UDP_Input_Port);
      udp_is_ready = 1;
  }

#if defined(POWER_SAVING_WIFI_TIMEOUT)
  WiFi_No_Clients_Time_ms = millis();
#endif
}

void WiFi_loop()
{
#if defined(USE_DNS_SERVER)
  if (dns_active) {
    dnsServer.processNextRequest();
  }
#endif

#if defined(POWER_SAVING_WIFI_TIMEOUT)
  if ((settings->power_save & POWER_SAVE_WIFI) && WiFi.getMode() == WIFI_AP) {
    if (SoC->WiFi_clients_count() == 0) {
      if ((millis() - WiFi_No_Clients_Time_ms) > POWER_SAVING_WIFI_TIMEOUT) {
        //NMEA_fini();   // keep serial & BT NMEA going
        Web_fini();
        WiFi_fini();

        if (settings->nmea_p) {
          StdOut.println(F("$PSRFS,WIFI_OFF"));
        }
      }
    } else {
      WiFi_No_Clients_Time_ms = millis();
    }
  }
#endif
}

void WiFi_fini()
{
  udp_is_ready = 0;
  Uni_Udp.stop();

  WiFi.mode(WIFI_OFF);
}

#endif /* EXCLUDE_WIFI */
