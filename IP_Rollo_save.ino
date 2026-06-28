#include <WiFi.h>
#include <WebServer.h> 
#include <DNSServer.h> 
#include <Preferences.h>
#include <ESP32Time.h> 

Preferences preferences;
WebServer server(80); 
DNSServer dnsServer; 
ESP32Time rtc; 

// ============================================================================
// PIN-KONFIGURATION & KONSTANTEN
// ============================================================================
const int PIN_LS         = 21;    // Lichtschranke (fallende Flanke = Impuls)
const int PIN_RELAIS_HOCH = 16;   // Relais Motor nach OBEN
const int PIN_RELAIS_RUNTER = 17; // Relais Motor nach UNTEN
const int PIN_RUNTER     = 22;    // Wandtaster UNTEN
const int PIN_HOCH       = 23;    // Wandtaster OBEN
const int PIN_BOARD      = 0;     // Board-Taste (Kalibrierung)

const unsigned long TASTER_ENTPRELL_MS = 100;
const unsigned long LS_ENTPRELL_MS = 15;
const unsigned long BOARD_ENTPRELL_MS = 50;
const unsigned long RELAIS_OFF_DELAY_MS = 50;
const unsigned long MOTOR_NACHLAUF_MS = 500;
const unsigned long KALIBRIERUNG_TIMEOUT_MS = 120000;

const long MAX_IMPULSE = 50000;
const long MIN_RANGE = 5;           // Mindestbereich auf 5 Impulse gesetzt (für Tests)
const long DEFAULT_HOCH = 500;      
const long DEFAULT_RUNTER = 0;      
const long EEPROM_NOT_FOUND = -1;

enum SystemState { STATE_BOOT, STATE_NORMAL, STATE_KALIBRIERUNG };
enum MotorState { MOTOR_IDLE, MOTOR_UP, MOTOR_DOWN, MOTOR_STOPPING };
enum KalibrierPhase { KALIBRIERUNG_IDLE, KALIBRIERUNG_REFERENZ, KALIBRIERUNG_OBEN };

SystemState systemState = STATE_BOOT;
MotorState motorState = MOTOR_IDLE;
KalibrierPhase kalibrierPhase = KALIBRIERUNG_IDLE;

volatile long impulsZaehler = 0;
volatile unsigned long lsChangeZeit = 0;

long endpunktHoch = DEFAULT_HOCH;
long endpunktRunter = DEFAULT_RUNTER;
unsigned long motorStoppZeit = 0;

bool tasterHochAktuell = false;
bool tasterRunterAktuell = false;
bool tasterBoardAktuell = true;

bool tasterHochVorher = false;
bool tasterRunterVorher = false;
bool tasterBoardVorher = true;

bool tasterHochGedrueckt = false;
bool tasterRunterGedrueckt = false;
bool tasterBoardGedrueckt = false;

bool webHochGedrueckt = false;
bool webRunterGedrueckt = false;

int aufStunde = 7, aufMinute = 0;
int zuStunde = 20, zuMinute = 0;
bool zeitschaltuhrAusgeloest = false; 
int letzteMinute = -1;

unsigned long tasterChangeZeit = 0;
unsigned long tasterBoardChangeZeit = 0;
unsigned long kalibrierStartZeit = 0;
bool eepromDirty = false;

// Vorwärtsdeklarationen für Funktionen aus Teil 2
bool lade_kalibrierung_aus_eeprom();
void verwalte_motor_nachlauf(unsigned long jetzt);
void verwalte_zeitschaltuhr();
void lese_sensoren(unsigned long jetzt);
void handle_boot_state();
void handle_normal_state(unsigned long jetzt);
void handle_kalibrierung_state(unsigned long jetzt);
void speichere_eeprom_wenn_noetig();
void debug_ausgabe();
bool bootReferenzErforderlich = true; // Erzwingt die einmalige Abwärtsfahrt nach dem Einschalten
bool aufGesperrt = false; // true = Morgen-Timer ist deaktiviert
bool zuGesperrt = false;  // true = Abend-Timer ist deaktiviert
bool webAufSperreGedrueckt = false;
bool webZuSperreGedrueckt = false;

// Generiert die HTML-Seite dynamisch mit Live-Rollladen-Animation

// ============================================================================
// WEBSERVER: GENERIERUNG DER WEBSEITE
// ============================================================================
void handleRoot() {
  char auf_buf[10];  sprintf(auf_buf, "%02d:%02d", aufStunde, aufMinute);
  char zu_buf[10];   sprintf(zu_buf, "%02d:%02d", zuStunde, zuMinute);
  
  // Sauberes Auslesen der systemseitigen Sommer-/Winterzeit
  struct tm timeinfo;
  char zeit_str[10] = "00:00:00";
  if (getLocalTime(&timeinfo)) {
    strftime(zeit_str, sizeof(zeit_str), "%H:%M:%S", &timeinfo);
  }

  // Prozentuale Position berechnen (0% = geschlossen/unten, 100% = offen/oben)
  int prozent = 0;
  if (endpunktHoch > 0) {
    prozent = (impulsZaehler * 100) / endpunktHoch;
    if (prozent > 100) prozent = 100;
    if (prozent < 0) prozent = 0;
  }

  // Prüfen, ob sich der Rollladen aktuell bewegt (für die CSS-Animation)
  String animiert = (motorState == MOTOR_UP || motorState == MOTOR_DOWN) ? "true" : "false";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'><meta charset='utf-8'>";
  
  // Automatischer Seiten-Reload alle 2 Sekunden NUR während der Fahrt
  if (animiert == "true") {
    html += "<meta http-equiv='refresh' content='2'>";
  }

  html += "<style>body{font-family:Arial;text-align:center;background:#f4f4f4;margin:0;padding:20px;}";
  html += "h2,h3{color:#333;margin-bottom:5px;}.btn{display:block;width:80%;max-width:300px;margin:15px auto;padding:20px;";
  html += "font-size:22px;font-weight:bold;color:white;border:none;border-radius:10px;cursor:pointer;}";
  html += ".btn-up{background-color:#4CAF50;}.btn-down{background-color:#2196F3;}";
  html += ".box{background:white;padding:15px;max-width:300px;margin:20px auto;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  html += "input[type='time']{font-size:20px;padding:5px;width:80%;text-align:center;margin-bottom:10px;}";
  html += ".btn-save{background-color:#ff9800;font-size:16px;padding:10px;width:85%;}.time-display{font-size:14px;color:#666;margin-bottom:15px;}";
  
  // CSS für das visuelle Fenster und die Rollladen-Animation
  html += ".window-frame{width:120px;height:150px;border:6px solid #555;margin:15px auto;background:#e0f7fa;position:relative;border-radius:5px;overflow:hidden;box-shadow:inset 0 0 10px rgba(0,0,0,0.2);}";
  html += ".shutter{position:absolute;top:0;left:0;width:100%;background:linear-gradient(to bottom, #9e9e9e 70%, #757575 100%);border-bottom:4px solid #424242;transition:height 1s ease-in-out;}";
  html += ".shutter::after{content:'';position:absolute;top:0;left:0;width:100%;height:100%;background:linear-gradient(rgba(0,0,0,0.15) 2px, transparent 2px);background-size:100% 10px;}";
  html += "@keyframes drive { 0% { opacity: 0.95; } 50% { opacity: 1; transform: scaleY(1.005); } 100% { opacity: 0.95; } }";
  html += ".shutter[data-moving='true'] { animation: drive 0.3s infinite linear; border-bottom-color: #ff9800; }";
  
  // CSS für die kleinen Sperr-Buttons und die Inline-Anordnung
  html += ".timer-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}";
  html += ".btn-sperre{padding:8px 12px;font-size:14px;font-weight:bold;color:white;border:none;border-radius:5px;cursor:pointer;margin-left:10px;min-width:70px;}";
  html += ".sperre-off{background-color:#4CAF50;} .sperre-on{background-color:#f44336;}";
  
  html += ".guide-box{background:#222;color:#fff;font-family:monospace;text-align:left;padding:15px;max-width:400px;margin:20px auto;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.3);white-space:pre-wrap;font-size:13px;line-height:1.4;}</style>";
  html += "<script>window.onload=function(){let d = new Date();fetch('/sync?time='+Math.floor(d.getTime()/1000));};</script></head><body>";
  
  html += "<h2>Rollladen Steuerung</h2>";
  
  // Sicherheits-Warnung bei Stromausfall
  if (bootReferenzErforderlich && systemState == STATE_BOOT) {
    html += "<div style='color:red;font-weight:bold;margin-bottom:15px;'>⚠️ STROMAUSFALL! Bitte 1x per Wandtaster ganz nach UNTEN fahren!</div>";
  } else {
    html += "<div class='time-display'>ESP-Uhrzeit: " + String(zeit_str) + "</div>";
  }
  
  int shutterHoehe = 100 - prozent;
  html += "<div class='window-frame'>";
  html += "  <div class='shutter' data-moving='" + animiert + "' style='height: " + String(shutterHoehe) + "%;'></div>";
  html += "</div>";
  html += "<div style='font-weight:bold;color:#333;margin-bottom:10px;'>Status: " + String(prozent) + "% geöffnet</div>";

  // Dynamische Fahr-Buttons (Schalten bei Fahrt auf Rot/Stopp um)
  if (motorState == MOTOR_UP) {
    html += "<button class='btn' style='background-color:#f44336;' onclick=\"location.href='/hoch'\">🛑 STOPP (HOCH)</button>";
    html += "<button class='btn btn-down' onclick=\"location.href='/runter'\">▼ RUNTER STARTEN</button>";
  } 
  else if (motorState == MOTOR_DOWN) {
    html += "<button class='btn btn-up' onclick=\"location.href='/hoch'\">▲ HOCH STARTEN</button>";
    html += "<button class='btn' style='background-color:#f44336;' onclick=\"location.href='/runter'\">🛑 STOPP (RUNTER)</button>";
  } 
  else {
    html += "<button class='btn btn-up' onclick=\"location.href='/hoch'\">▲ HOCH STARTEN</button>";
    html += "<button class='btn btn-down' onclick=\"location.href='/runter'\">▼ RUNTER STARTEN</button>";
  }
  
  // Zeitschaltuhr-Box mit den kleinen, farbwechselnden Sperr-Buttons
  html += "<div class='box'><h3>🕒 Zeitschaltuhr</h3>";
  html += "<form action='/save_timer' method='GET'>";
  
  // Reihe 1: Öffnen-Zeit + Sperrbutton
  String aufKlasse = aufGesperrt ? "sperre-on" : "sperre-off";
  String aufStatusText = aufGesperrt ? "Inaktiv" : "Aktiv";
  html += "<label>Öffnen um:</label><br>";
  html += "<div class='timer-row'>";
  html += "  <input type='time' name='auf' value='" + String(auf_buf) + "' style='width:65%; margin-bottom:0;'>";
  html += "  <button type='button' class='btn-sperre " + aufKlasse + "' onclick=\"location.href='/toggle_auf_sperre'\">Sperre</button>";
  html += "</div>";
  html += "<div style='font-size:11px; text-align:left; color:#666; margin-top:-5px; margin-bottom:10px;'>Status: " + aufStatusText + "</div>";
  
  // Reihe 2: Schließen-Zeit + Sperrbutton
  String zuKlasse = zuGesperrt ? "sperre-on" : "sperre-off";
  String zuStatusText = zuGesperrt ? "Inaktiv" : "Aktiv";
  html += "<label>Schließen um:</label><br>";
  html += "<div class='timer-row'>";
  html += "  <input type='time' name='zu' value='" + String(zu_buf) + "' style='width:65%; margin-bottom:0;'>";
  html += "  <button type='button' class='btn-sperre " + zuKlasse + "' onclick=\"location.href='/toggle_zu_sperre'\">Sperre</button>";
  html += "</div>";
  html += "<div style='font-size:11px; text-align:left; color:#666; margin-top:-5px; margin-bottom:15px;'>Status: " + zuStatusText + "</div>";
  
  html += "<button type='submit' class='btn btn-save' style='margin-top:5px;'>Zeiten speichern</button></form></div>";

  // Kurzanleitung im Monospace-Design
  html += "<div class='guide-box'>";
  html += "=========================================\n";
  html += "     KURZANLEITUNG: ROLLLADEN ANLERNEN   \n";
  html += "=========================================\n\n";
  html += "WICHTIGSTE REGEL:\n";
  html += "-----------------------------------------\n";
  html += "* Motor IMMER erst mit dem Wandtaster\n";
  html += "  stoppen, BEVOR Board-Taste gedrUeckt wird!\n";
  html += "* Die Prozedur muss unter 2 Min. erfolgen.\n\n";
  html += "SCHRITT 1: START\n";
  html += "-----------------------------------------\n";
  html += "* Board-Taste kurz drUecken.\n\n";
  html += "SCHRITT 2: UNTEN (Nullpunkt setzen)\n";
  html += "-----------------------------------------\n";
  html += "* Wandtaster RUNTER kurz einschalten.\n";
  html += "* Mindestens 1 Impuls weit fahren.\n";
  html += "* Per Wandtaster stoppen (Stillstand!).\n";
  html += "* Board-Taste kurz drUecken.\n\n";
  html += "SCHRITT 3: OBEN (Weg einmessen)\n";
  html += "-----------------------------------------\n";
  html += "* Wandtaster HOCH ganz auffahren lassen.\n";
  html += "* Mindestens 6 Impulse weit fahren!\n";
  html += "* Per Wandtaster stoppen (Stillstand!).\n";
  html += "* Board-Taste kurz drUecken.\n\n";
  html += "-> FERTIG! Automatikbetrieb aktiv.\n\n";
  html += "NACH EINEM STROMAUSFALL (Freischalten):\n";
  html += "-----------------------------------------\n";
  html += "* Endpunkte bleiben dauerhaft gesichert.\n";
  html += "* Web-UI und Zeitschaltuhr sind blockiert.\n";
  html += "* FREIGABE-ABLAUF:\n";
  html += "  - Wenn offen: Wandtaster RUNTER drUecken,\n";
  html += "    ganz schliessen & MANUELL STOPPEN!\n";
  html += "  - Wenn schon zu: Wandtaster RUNTER 2x kurz drUecken!\n";
  html += "* 1x mit Smartphone im WLAN einwAhlen,\n";
  html += "  Uhrzeit stellt sich vollautomatisch ein.\n\n";
  html += "ZUM ERNEUTEN NEU-EINLERNEN:\n";
  html += "-----------------------------------------\n";
  html += "* Die Board-Taste dient im Alltag zum\n";
  html += "  bewussten Neu-Einlernen.\n";
  html += "* Ein Klick im Normalbetrieb lOescht\n";
  html += "  sofort alle alten Grenzen.\n";
  html += "* Das System springt direkt in Schritt 1.\n";
  html += "=========================================";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}


// Ultraschnelle Impulszählung direkt im RAM des ESP32
void IRAM_ATTR lsInterruptISR() {
    unsigned long jetzt = millis();
    
    // Hardware-Entprellung direkt im Interrupt (15ms)
    if ((jetzt - lsChangeZeit) > LS_ENTPRELL_MS) {
        lsChangeZeit = jetzt;
        
        // Da wir auf FALLING (fallende Flanke) triggern, sind wir bereits LOW
        if (motorState == MOTOR_UP) {
            if (systemState == STATE_KALIBRIERUNG || impulsZaehler < MAX_IMPULSE) {
                impulsZaehler++;
            }
        } 
        else if (motorState == MOTOR_DOWN) {
            // Im Boot- oder Kalibrierzustand immer rückwärts zählen
            if (systemState == STATE_KALIBRIERUNG || systemState == STATE_BOOT) {
                impulsZaehler--;
            }
            else if (impulsZaehler > endpunktRunter) {
                impulsZaehler--;
            }
        }
    }
}




void setup() {



  delay(2000);   Serial.begin(115200);

  pinMode(PIN_RELAIS_HOCH, OUTPUT); pinMode(PIN_RELAIS_RUNTER, OUTPUT);
  pinMode(PIN_RUNTER, INPUT_PULLUP); pinMode(PIN_HOCH, INPUT_PULLUP); pinMode(PIN_BOARD, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_LS), lsInterruptISR, FALLING);
  digitalWrite(PIN_RELAIS_HOCH, LOW); digitalWrite(PIN_RELAIS_RUNTER, LOW);
  rtc.setTime(0, 0, 12, 1, 1, 2026); 

  preferences.begin("rolladen", false);
  aufStunde = preferences.getInt("aufH", 7);   aufMinute = preferences.getInt("aufM", 0);
  zuStunde = preferences.getInt("zuH", 20);    zuMinute = preferences.getInt("zuM", 0);
  aufGesperrt = preferences.getBool("aufGesp", false);
  zuGesperrt = preferences.getBool("zuGesp", false);
  preferences.end();

  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP); delay(200); 

  WiFi.setTxPower(WIFI_POWER_7dBm); // Schutz vor extremen Stromspitzen
  delay(100);
  // Feste IP-Konfiguration für den Hotspot definieren
  IPAddress local_ip(192,168,4,1);   // Die Wunsch-IP für dein Rollo
  IPAddress gateway(192,168,4,1);    // Der ESP32 ist sein eigenes Gateway
  IPAddress subnet(255, 255, 255, 0); // Das Standard-Subnetz

// IP-Konfiguration aktivieren
  WiFi.softAPConfig(local_ip, gateway, subnet);
  delay(100);

  // KORRIGIERT: Eigenes WLAN mit Ihrem Wunsch-Passwort geschützt
  if(WiFi.softAP("Rollladen-Steuerung", "Cvbn3456+*")) {
    Serial.println("[WLAN] Passwortgeschützter Hotspot erfolgreich gestartet!");
  }
  delay(200);
  
  // DNS-Server reaktiviert: Fängt alle Anfragen für das automatische Pop-up ab
  dnsServer.start(53, "*", IPAddress(192,168,4,1));

  // Webserver Routen zuweisen
  server.on("/", handleRoot);
  server.on("/hoch", [](){ webHochGedrueckt = true; server.sendHeader("Location", "/"); server.send(303); });
  server.on("/runter", [](){ webRunterGedrueckt = true; server.sendHeader("Location", "/"); server.send(303); });

  server.on("/toggle_auf_sperre", [](){ webAufSperreGedrueckt = true; server.sendHeader("Location", "/"); server.send(303); });
  server.on("/toggle_zu_sperre", [](){ webZuSperreGedrueckt = true; server.sendHeader("Location", "/"); server.send(303); });
  
  server.on("/sync", [](){
    if (server.hasArg("time")) {
      // 1. Setzt die reine UTC-Zeit des Smartphones ohne pauschalen Aufschlag
      rtc.setTime(server.arg("time").toInt());
      
      // 2. Aktiviert die offizielle europäische Zeitzonen-Regel (inkl. automatischer Sommer-/Winterzeit)
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
      tzset();
      
      Serial.println("[Uhrzeit] Synchronisiert mit automatischer Sommer-/Winterzeit.");
    }
    server.send(200, "text/plain", "OK");
  });


  server.on("/save_timer", [](){
    if (server.hasArg("auf") && server.hasArg("zu")) {
      String auf = server.arg("auf"); String zu = server.arg("zu");
      aufStunde = auf.substring(0,2).toInt(); aufMinute = auf.substring(3,5).toInt();
      zuStunde = zu.substring(0,2).toInt(); zuMinute = zu.substring(3,5).toInt();
      preferences.begin("rolladen", false);
      preferences.putInt("aufH", aufStunde); preferences.putInt("aufM", aufMinute);
      preferences.putInt("zuH", zuStunde); preferences.putInt("zuM", zuMinute);
      preferences.end();
      Serial.println("[EEPROM] Zeitschaltuhr-Daten gesichert.");
    }
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.onNotFound(handleRoot); // WICHTIG: Erzeugt das automatische Pop-up
  server.begin();

  unsigned long jetzt = millis();
  lsChangeZeit = jetzt; tasterChangeZeit = jetzt; tasterBoardChangeZeit = jetzt;
  tasterHochAktuell = (digitalRead(PIN_HOCH) == HIGH); tasterRunterAktuell = (digitalRead(PIN_RUNTER) == HIGH); tasterBoardAktuell = digitalRead(PIN_BOARD);
  tasterHochVorher = tasterHochAktuell; tasterRunterVorher = tasterRunterAktuell; tasterBoardVorher = tasterBoardAktuell;
  
 

  if (!lade_kalibrierung_aus_eeprom()) {
    preferences.begin("kalib", false); 
    preferences.putLong("hoch", DEFAULT_HOCH); 
    preferences.putLong("runter", DEFAULT_RUNTER); 
    preferences.end();
    endpunktHoch = DEFAULT_HOCH; 
    endpunktRunter = DEFAULT_RUNTER; 
    systemState = STATE_BOOT;
    bootReferenzErforderlich = false; // Falls keine Kalibrierung existiert, greift das normale Anlernen
  } else {
    systemState = STATE_BOOT;         // ZWINGEND im Boot-Zustand starten
    bootReferenzErforderlich = true;  // Referenzfahrt nach unten aktivieren
    impulsZaehler = 9999;             // Temporärer Schutzwert, damit das System weiß, dass es NICHT unten steht
    Serial.println(F("[System] Kalibrierung geladen. WARNUNG: Warte auf Referenzfahrt nach UNTEN..."));
  }
}


void loop() {
  unsigned long jetzt = millis();
  dnsServer.processNextRequest(); // DNS-Server aktiv halten
  server.handleClient();          // Web-Anfragen sicher nacheinander im Hauptthread verarbeiten
  
  verwalte_motor_nachlauf(jetzt);
  if (systemState == STATE_NORMAL) {
    verwalte_zeitschaltuhr(); 
  } 
  lese_sensoren(jetzt);
  

  switch (systemState) {
    case STATE_BOOT:          handle_boot_state(); break;
    case STATE_NORMAL:        handle_normal_state(jetzt); break;
    case STATE_KALIBRIERUNG:  handle_kalibrierung_state(jetzt); break;
  }
  
  speichere_eeprom_wenn_noetig();
  if (jetzt % 2000 < 10) debug_ausgabe();
}

// Die Vorwärtsdeklarationen kommen direkt danach im freien Raum:
void starte_kalibrierung();
void fahre_motor(MotorState richtung);
void stoppe_motor();
void zaehle_impuls();

// ============================================================================
// ZEITSCHALTUHR-VERWALTUNG
// ============================================================================

void verwalte_zeitschaltuhr() {
  // 1. Web-Sperren verwalten (EEPROM Update bei Klick im Webbrowser)
  if (webAufSperreGedrueckt || webZuSperreGedrueckt) {
    if (webAufSperreGedrueckt) { 
      webAufSperreGedrueckt = false; 
      aufGesperrt = !aufGesperrt; 
    }
    if (webZuSperreGedrueckt) { 
      webZuSperreGedrueckt = false; 
      zuGesperrt = !zuGesperrt; 
    }
    
    // Zustand dauerhaft im ESP32-Speicher sichern
    preferences.begin("rolladen", false);
    preferences.putBool("aufGesp", aufGesperrt);
    preferences.putBool("zuGesp", zuGesperrt);
    preferences.end();
    Serial.println(F("[EEPROM] Timer-Sperrstatus aktualisiert."));
  }

  // 2. Zeit-Vergleich (KORRIGIERT auf echte lokale Uhrzeit!)
  if (systemState != STATE_NORMAL) return;
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return; // Holt die echte, korrigierte Lokalzeit
  
  int h = timeinfo.tm_hour; // Echte deutsche Stunde (inkl. Sommerzeit)
  int m = timeinfo.tm_min;  // Echte Minute
  
  if (m == letzteMinute) return; // Verhindert Dauerfeuer innerhalb derselben Minute

  // 3. Automatik-Aktion (Hoch/Runter)
  if (!aufGesperrt && h == aufStunde && m == aufMinute) {
    fahre_motor(MOTOR_UP);
    letzteMinute = m;
  }
  if (!zuGesperrt && h == zuStunde && m == zuMinute) {
    fahre_motor(MOTOR_DOWN);
    letzteMinute = m;
  }
  
  // Setzt den Minutenschutz zurück, sobald die Schaltzeit vorbei ist
  if (m != aufMinute && m != zuMinute) {
    letzteMinute = -1;
  }
}



  
 // ============================================================================
// NORMALBETRIEB-STEUERUNG 
// ============================================================================
void handle_normal_state(unsigned long jetzt) {
  // Statische Variable für die Flankenerkennung der Board-Taste im Normalmodus
  static bool boardTasteLetzterZustandNormal = HIGH;

  // 1. Board-Taste abfragen: Wenn gedrückt, wechsle SOFORT in die Kalibrierung!
  if (tasterBoardAktuell == LOW && boardTasteLetzterZustandNormal == HIGH) {
    boardTasteLetzterZustandNormal = LOW; // Flanke sperren
    Serial.println(F("[SYSTEM] Board-Taste im Normalbetrieb erkannt! Starte Neukalibrierung..."));
    
    starte_kalibrierung(); // Schaltet systemState auf STATE_KALIBRIERUNG um
    return;                // Verlasse die Funktion sofort, um keine Fahrt auszulösen!
  } 
  else if (tasterBoardAktuell == HIGH) {
    boardTasteLetzterZustandNormal = HIGH; // Wieder freigeben beim Loslassen
  }

  // 2. Sicherheits-Stopp bei Erreichen der Endpunkte (NUR im Normalbetrieb!)
  if (systemState == STATE_NORMAL) {
    if (motorState == MOTOR_UP && impulsZaehler >= endpunktHoch) {
      stoppe_motor();
    }
    else if (motorState == MOTOR_DOWN && impulsZaehler <= endpunktRunter) {
      stoppe_motor();
    }
  }
  
  // 3. Taster- und Web-Auswertung für HOCH
  if (tasterHochGedrueckt || webHochGedrueckt) {
    tasterHochGedrueckt = false; webHochGedrueckt = false;
    if (motorState != MOTOR_IDLE) {
      stoppe_motor();
    } else if (impulsZaehler < endpunktHoch) {
      fahre_motor(MOTOR_UP);
    }
  }
  // 4. Taster- und Web-Auswertung für RUNTER
  else if (tasterRunterGedrueckt || webRunterGedrueckt) {
    tasterRunterGedrueckt = false; webRunterGedrueckt = false;
    if (motorState != MOTOR_IDLE) {
      stoppe_motor();
    } else if (impulsZaehler > endpunktRunter) {
      fahre_motor(MOTOR_DOWN);
    }
  }
}

void starte_kalibrierung() {
  systemState = STATE_KALIBRIERUNG; 
  kalibrierPhase = KALIBRIERUNG_REFERENZ;
  kalibrierStartZeit = millis(); 
  
  // KORREKTUR: Wir setzen den Zähler auf einen fiktiven Wert (z.B. -999), 
  // damit das System merkt, dass noch keine reale Fahrt stattgefunden hat!
  impulsZaehler = -999; 
  
  stoppe_motor();
  
  // Die Board-Taste muss nach diesem Aufruf zwingend erst losgelassen werden,
  // bevor handle_kalibrierung_state() die Flanke als "Ziel erreicht" werten darf!
  Serial.println(F("[KALIBRIERUNG] Modus aktiv. Fahre das Rollo nach UNTEN. Erst wenn es STEHT, drücke die Board-Taste!"));
}

// ============================================================================
// HANDLING DER KALIBRIERUNG (WIEDERHERGESTELLT & ABSOLUT SICHER)
// ============================================================================
void handle_kalibrierung_state(unsigned long jetzt) {
  // 1. Timeout-Schutz: Bricht nach Inaktivität ab, um den Motor zu schützen
  if (jetzt - kalibrierStartZeit > KALIBRIERUNG_TIMEOUT_MS) {
    Serial.println(F("[KALIBRIERUNG] Timeout erreicht! Abbruch."));
    stoppe_motor(); 
    systemState = STATE_NORMAL; 
    kalibrierPhase = KALIBRIERUNG_IDLE; 
    return;
  }
  
  webHochGedrueckt = false; 
  webRunterGedrueckt = false;

  // Statische Sperre: Verhindert das Durchschießen der Phasen beim ersten Knopfdruck
  static bool boardTasteMussErstLosgelassenWerden = true;
  static bool boardTasteLetzterZustandKali = HIGH;
  bool boardTasteFlankeKali = false;

  // Board-Taste auswerten (LOW = gedrückt bei GPIO 0)
  if (tasterBoardAktuell == LOW) {
    if (boardTasteLetzterZustandKali == HIGH) {
      boardTasteLetzterZustandKali = LOW;
      // Flanke nur zulassen, wenn die Taste nach dem Start der Kalibrierung mind. 1x losgelassen wurde!
      if (!boardTasteMussErstLosgelassenWerden) {
        boardTasteFlankeKali = true;
        Serial.println(F("[DEBUG] Board-Taste gültige Flanke erkannt!"));
      }
    }
  } else {
    boardTasteLetzterZustandKali = HIGH;
    boardTasteMussErstLosgelassenWerden = false; // Finger wurde weggenommen -> Taste ab jetzt scharf!
  }

  // === PHASE 1: UNTEN ANGEKOMMEN ===
  if (kalibrierPhase == KALIBRIERUNG_REFERENZ) {
    // Wandtaster-Auswertung (Nutzt die korrigierte, synchrone Flankenerkennung aus lese_sensoren)
    if (tasterRunterGedrueckt) { tasterRunterGedrueckt = false; if (motorState == MOTOR_DOWN) stoppe_motor(); else fahre_motor(MOTOR_DOWN); }
    if (tasterHochGedrueckt)   { tasterHochGedrueckt = false;   if (motorState == MOTOR_UP) stoppe_motor(); else fahre_motor(MOTOR_UP); }
    
    if (boardTasteFlankeKali && motorState == MOTOR_IDLE) {
      impulsZaehler = 0; 
      endpunktRunter = 0; 
      kalibrierPhase = KALIBRIERUNG_OBEN;
      boardTasteMussErstLosgelassenWerden = true; // Sperre für die nächste Phase wieder aktivieren!
      Serial.println(F("[KALIBRIERUNG] Nullpunkt unten gesetzt! Jetzt nach OBEN fahren und stoppen."));
      return; // <--- Sehr wichtig! Verhindert das Durchrauschen in Phase 2
    }
  } 
  
  // === PHASE 2: OBEN ANGEKOMMEN ===
  else if (kalibrierPhase == KALIBRIERUNG_OBEN) {
    if (tasterHochGedrueckt)   { tasterHochGedrueckt = false;   if (motorState == MOTOR_UP) stoppe_motor(); else fahre_motor(MOTOR_UP); }
    if (tasterRunterGedrueckt) { tasterRunterGedrueckt = false; if (motorState == MOTOR_DOWN) stoppe_motor(); else fahre_motor(MOTOR_DOWN); }
    
    if (boardTasteFlankeKali && motorState == MOTOR_IDLE) {
      if (impulsZaehler >= MIN_RANGE) {
        endpunktHoch = impulsZaehler; 
        eepromDirty = true; 
        systemState = STATE_NORMAL; 
        kalibrierPhase = KALIBRIERUNG_IDLE;
        Serial.println(F("[KALIBRIERUNG] Erstinbetriebnahme erfolgreich beendet!"));
        
        // Relais-Doppelklick zur Bestätigung
        delay(300); 
        digitalWrite(PIN_RELAIS_HOCH, HIGH); delay(80);
        digitalWrite(PIN_RELAIS_HOCH, LOW);  delay(120);
        digitalWrite(PIN_RELAIS_HOCH, HIGH); delay(80);
        digitalWrite(PIN_RELAIS_HOCH, LOW);
      } else {
        Serial.print(F("[KALIBRIERUNG] Fehler: Zu wenige Impulse! Gezählt: "));
        Serial.print(impulsZaehler); Serial.print(F(" | Benötigt: ")); Serial.println(MIN_RANGE);
        boardTasteMussErstLosgelassenWerden = true; // Bei Fehler Sperre aktiv halten
      }
    }
  }
}

// ============================================================================
// SENSORIK & IMPULSVERARBEITUNG (KLAMMER- UND PEGEL-FIX)
// ============================================================================
void lese_sensoren(unsigned long jetzt) {
  
  
  // Lokale Stoppuhren für getrennte Entprellung der Wandtaster
  static unsigned long tasterHochChangeZeit = 0;
  static unsigned long tasterRunterChangeZeit = 0;

  // --- Taster HOCH einzeln entprellen (LOW = gedrückt bei Öffnern) ---
  bool hoch_neu = (digitalRead(PIN_HOCH) == LOW); 
  if (hoch_neu != tasterHochAktuell) {
      if (jetzt - tasterHochChangeZeit > TASTER_ENTPRELL_MS) {
          tasterHochAktuell = hoch_neu;
          tasterHochChangeZeit = jetzt;
      }
  } else {
      tasterHochChangeZeit = jetzt; 
  }

  // --- Taster RUNTER einzeln entprellen (LOW = gedrückt bei Öffnern) ---
  bool runter_neu = (digitalRead(PIN_RUNTER) == LOW);
  if (runter_neu != tasterRunterAktuell) {
      if (jetzt - tasterRunterChangeZeit > TASTER_ENTPRELL_MS) {
          tasterRunterAktuell = runter_neu;
          tasterRunterChangeZeit = jetzt;
      }
  } else {
      tasterRunterChangeZeit = jetzt;
  }
  
  // Board-Taste entprellen (LOW = gedrückt)
  bool board_neu = digitalRead(PIN_BOARD);
  if (board_neu != tasterBoardAktuell && (jetzt - tasterBoardChangeZeit) > BOARD_ENTPRELL_MS) {
    tasterBoardAktuell = board_neu; 
    tasterBoardChangeZeit = jetzt;
  }

  // Zentrale Flankenerkennung für das Hauptprogramm
  tasterHochGedrueckt = (tasterHochAktuell && !tasterHochVorher);
  tasterRunterGedrueckt = (tasterRunterAktuell && !tasterRunterVorher);

  tasterHochVorher = tasterHochAktuell; 
  tasterRunterVorher = tasterRunterAktuell; 
}


// ============================================================================
// HANDLING DER SYSTEMZUSTÄNDE (ERSTINBETRIEBNAHME FIX)
// ============================================================================
void handle_boot_state() {
  static bool fahrtAktiviert = false;
  static bool boardTasteLetzterZustandBoot = HIGH;
  
  // NEU: Einmaliges Sicherheits-Sperren aller Fahrbefehle beim allerersten Einschalten
  static bool erststartSperre = true;
  if (erststartSperre) {
    erststartSperre = false;
    tasterHochGedrueckt = false;
    tasterRunterGedrueckt = false;
    webHochGedrueckt = false;
    webRunterGedrueckt = false;
    stoppe_motor(); // Stellt sicher, dass alle Relais beim Start physisch AUS sind
    Serial.println(F("[System] Boot-Sicherheitsphase initialisiert. Relais gesperrt."));
    return;
  }

  // 1. Board-Taste abfangen (Wechsel in Kalibrierung)
  if (tasterBoardAktuell == LOW && boardTasteLetzterZustandBoot == HIGH) {
    boardTasteLetzterZustandBoot = LOW;
    Serial.println(F("[ERSTINBETRIEBNAHME] Board-Taste erkannt! Starte Kalibrierung..."));
    bootReferenzErforderlich = false;
    starte_kalibrierung();
    return;
  } 
  else if (tasterBoardAktuell == HIGH) {
    boardTasteLetzterZustandBoot = HIGH;
  }

  // Web-Taster im Boot-Modus eisern sperren
  webHochGedrueckt = false; 
  webRunterGedrueckt = false;

  // 2. Sicherheits-Verriegelung für die Boot-Referenz
  if (bootReferenzErforderlich) {
    
    if (tasterHochGedrueckt) {
      tasterHochGedrueckt = false;
      Serial.println(F("[Sicherheit] Fahrt gesperrt! Nach Stromausfall zuerst ganz nach UNTEN fahren und stoppen."));
    }

    if (tasterRunterGedrueckt) {
      tasterRunterGedrueckt = false;
      
      if (motorState == MOTOR_DOWN) {
        stoppe_motor();
      } else {
        fahre_motor(MOTOR_DOWN);
        fahrtAktiviert = true; // Jetzt erst wurde die Fahrt bewusst durch die Wippe gestartet
      }
    }

    // 3. Abschluss-Bedingung: Nur wenn die Fahrt aktiv gestartet wurde und nun stoppt
    if (fahrtAktiviert && motorState == MOTOR_IDLE) {
      impulsZaehler = 0;                 
      bootReferenzErforderlich = false;   
      systemState = STATE_NORMAL;        
      fahrtAktiviert = false;            
      Serial.println(F("[System] Referenzfahrt erfolgreich beendet. Normalbetrieb AKTIV!"));
    }
  } 
  else {
    systemState = STATE_NORMAL;
  }
}


// ============================================================================
// HARDWARE ENTTRENNUNG & TREIBER
// ============================================================================
void fahre_motor(MotorState richtung) {
  if (motorState == MOTOR_STOPPING) return;
  digitalWrite(PIN_RELAIS_HOCH, LOW); 
  digitalWrite(PIN_RELAIS_RUNTER, LOW);
  delay(RELAIS_OFF_DELAY_MS);
  
  if (richtung == MOTOR_UP) { digitalWrite(PIN_RELAIS_HOCH, HIGH); motorState = MOTOR_UP; }
  else if (richtung == MOTOR_DOWN) { digitalWrite(PIN_RELAIS_RUNTER, HIGH); motorState = MOTOR_DOWN; }
}

void stoppe_motor() {
  if (motorState == MOTOR_IDLE || motorState == MOTOR_STOPPING) return;
  digitalWrite(PIN_RELAIS_HOCH, LOW); 
  digitalWrite(PIN_RELAIS_RUNTER, LOW);
  motorState = MOTOR_STOPPING; 
  motorStoppZeit = millis();
}

void verwalte_motor_nachlauf(unsigned long jetzt) {
  if (motorState == MOTOR_STOPPING && (jetzt - motorStoppZeit >= MOTOR_NACHLAUF_MS)) motorState = MOTOR_IDLE;
}

bool lade_kalibrierung_aus_eeprom() {
  preferences.begin("kalib", true);
  long hoch = preferences.getLong("hoch", EEPROM_NOT_FOUND); 
  long runter = preferences.getLong("runter", EEPROM_NOT_FOUND);
  preferences.end();
  if (hoch == EEPROM_NOT_FOUND || runter == EEPROM_NOT_FOUND || (hoch - runter) < MIN_RANGE) return false;
  endpunktHoch = hoch; 
  endpunktRunter = runter; 
  return true;
}

void speichere_eeprom_wenn_noetig() {
  if (eepromDirty) {
    preferences.begin("kalib", false); 
    preferences.putLong("hoch", endpunktHoch); 
    preferences.putLong("runter", endpunktRunter); 
    preferences.end();
    eepromDirty = false; 
    Serial.println("[EEPROM] Kalibrierdaten dauerhaft gesichert.");
  }
}

void debug_ausgabe() {
  Serial.print("[DEBUG] Zeit: "); Serial.print(rtc.getTime("%H:%M:%S"));
  Serial.print(" | Pos: "); Serial.print(impulsZaehler);
  Serial.print(" / "); Serial.println(endpunktHoch);
}
