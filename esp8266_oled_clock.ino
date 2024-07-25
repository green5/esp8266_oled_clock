/*
  libs:
  adafruit/Adafruit_SSD1306
  adafruit/RTClib
  aharshac/EasyNTPClient
  todo:
  clock font+
*/
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
extern "C"
{
  #include <lwip/ip.h>
  #include <lwip/icmp.h> // needed for icmp packet definitions
  #include <lwip/raw.h>
}
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <EasyNTPClient.h>

IPAddress syslog;
Adafruit_SSD1306 display(128,64,&Wire,-1);
RTC_PCF8563 rtc;

#define NROW 8
#define NCOL 21
char ss[NROW][NCOL+1];

void screen_puts(int row,const char *t) {
  if(row>=0 && row<NROW) {
    memset(ss[row],0,sizeof(ss[row]));
    strncpy(ss[row],t,NCOL);
  } else {
    memcpy(ss[0],ss[1],(NROW-1)*(NCOL+1));
    strncpy(ss[NROW-1],t,NCOL);
  }
}

void screen_init() {
  memset(ss,0,sizeof(ss));
  Wire.begin(5,4);
  if(!display.begin(SSD1306_SWITCHCAPVCC,0x3c,true,false)) {
    Serial.println("SSD1306 allocation failed");
    for(;;); // Don't proceed, loop forever
  }
  display.setTextColor(WHITE);
  display.display(); 
}

void screen_flush() {
  Wire.begin(5,4);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  for(int i=0;i<NROW;i++) display.println(ss[i]);
  display.display();   
}

void screen_clock(const char *d,const char *t) {
  int x = 15;
  int y = 10;
  x += (rand()-RAND_MAX/2) % 10;
  y += (rand()-RAND_MAX/2) % 10;
  Wire.begin(5,4);
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(x, y);
  display.print(d);
  display.setCursor(x, y+25);
  display.print(t);
  display.display();   
}

WiFiUDP udp;

struct Line {
  int line;
  bool flush,full;
  Line(int line_,bool flush_=true,bool full_=false):line(line_),flush(flush_),full(full_) {
  }
  void plog_(const char *fmt, ...) {
    char t[100];
    va_list arg;
    va_start(arg,fmt);
    vsnprintf(t,sizeof(t),fmt,arg);
    va_end(arg);
    if(full) {
      Serial.printf("%s.%d: %s\n",__FILE__,line,t);
      if(syslog.isSet()) {
        udp.beginPacket(syslog, 514);
        udp.write(t,strlen(t));
        udp.endPacket();        
      }
    }
    screen_puts(-1,t);
    if(flush) screen_flush();
  } 
};
#define plog Line(__LINE__).plog_
#define qlog Line(__LINE__,false).plog_
#define flog Line(__LINE__,true,true).plog_

void rtc_init() {
  Wire.begin(12,14);
  rtc.begin();
  rtc.start();
}

uint32_t now_;

void rtc_update() {
  Wire.begin(12,14);
  DateTime now = rtc.now();
  if(now_!=now.unixtime()) {
    now_ = now.unixtime();
    char d[20],t[20];
    snprintf(d,sizeof(d),"%d.%d.%d",now.day(),now.month(),now.year()-2000);
    snprintf(t,sizeof(t),"%02d:%02d:%02d",now.hour(),now.minute(),now.second());
    screen_clock(d,t);
  }
}

void ntp_init() {
  EasyNTPClient ntpClient(udp, "pool.ntp.org", 3*3600); // https://www.instructables.com/ESP8266-Clock-Module-Development-Board-an-Anatomy/
  DateTime ntp = ntpClient.getUnixTime() + 1;
  Wire.begin(12,14);
  DateTime local = rtc.now(); // \src\Arduino\libraries\RTClib\src\RTClib.h
  rtc.adjust(ntp);
  flog("%d seconds, unix %d",ntp.unixtime()-local.unixtime(),ntp.unixtime());
  flog("%d-%d-%d %02d:%02d:%02d",ntp.day(),ntp.month(),ntp.year()-2000,ntp.hour(),ntp.minute(),ntp.second());
}

bool ntpinit = false;

struct MyPing {
  struct raw_pcb * m_IcmpProtocolControlBlock = 0;
  bool begin() {
    if(m_IcmpProtocolControlBlock == nullptr) {
      m_IcmpProtocolControlBlock = raw_new(IP_PROTO_ICMP);
      if(m_IcmpProtocolControlBlock == nullptr) return false;
      raw_recv(m_IcmpProtocolControlBlock,PingReceivedStatic,(void*)this);
      raw_bind(m_IcmpProtocolControlBlock, IP_ADDR_ANY);
    }
    return true;
  }
  static u8_t PingReceivedStatic(void * pinger, raw_pcb * pcb, pbuf *packetBuffer, const ip_addr_t * addr) {
    union { u32_t addr; u8_t a[4]; } ip;
    ip.addr = addr ? addr->addr : 0;
    qlog("onping %d.%d.%d.%d",ip.a[0],ip.a[1],ip.a[2],ip.a[3]);
    ntpinit = true;
    //pbuf_free(packetBuffer); return 1;
    return 0;
  }
} ping;

void setup() {
  Serial.begin(115200);
  screen_init();
  flog("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);  
  int n = WiFi.scanNetworks();  
  for(int i=0;i<n;i++) {
    String ssid = WiFi.SSID(i);
    if(ssid=="u7") WiFi.begin(ssid.c_str(), "111222111");
    if(ssid=="karlink") WiFi.begin(ssid.c_str(), "12green@carl");
  }
  for(int i=0;WiFi.waitForConnectResult()!=WL_CONNECTED;i++) {
    plog("wifi failed %d",i);
    delay(5000);
    if(i==3) ESP.restart();
  }
  ArduinoOTA.onStart([]() { plog("Start updating"); });
  ArduinoOTA.onEnd([]() { plog("End Flash"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { if(total==progress) plog("Progress: %u%%", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error) {
    plog("OtaError %u", error);
    if (error == OTA_AUTH_ERROR) plog("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) plog("Begin Failed: %s",Update.getErrorString().c_str());
    else if (error == OTA_CONNECT_ERROR) plog("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) plog("Receive Failed");
    else if (error == OTA_END_ERROR) plog("End Failed");
  });
  ArduinoOTA.begin();
  syslog = WiFi.dnsIP();
  flog("DNS %s",WiFi.dnsIP().toString().c_str());
  flog("IP %s",WiFi.localIP().toString().c_str());
  ping.begin();
  rtc_init();
}

void loop() {
  ArduinoOTA.handle();
  if(ntpinit) ntpinit=false,ntp_init();
  rtc_update();
  //screen_flush();
}

