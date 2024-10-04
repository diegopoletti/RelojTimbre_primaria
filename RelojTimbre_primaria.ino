#include <WiFi.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TM1637Display.h>
#include <ESP32Time.h>
#include <EEPROM.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

// Definición de pines
#define PIN_CLK 22
#define PIN_DIO 21
#define PIN_BUZZER 19
#define PIN_RELE 18
#define PIN_BOTON 5

// Configuración de WiFi y NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // GMT-3 en segundos
const int daylightOffset_sec = 0;

// Objetos globales
TM1637Display pantalla(PIN_CLK, PIN_DIO);
ESP32Time rtc;
AsyncWebServer servidor(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, gmtOffset_sec, daylightOffset_sec);

// Variables globales
bool alarmaActiva = false;
int patronActual = 0;
unsigned long tiempoUltimaSincronizacion = 0;
const unsigned long intervaloSincronizacion = 4 * 60 * 60 * 1000; // 4 horas en milisegundos

// Estructura para almacenar las alarmas
struct Alarma {
  bool activa;
  int hora;
  int minuto;
  int diasSemana;
  int patron;
};

Alarma alarmas[24];

// Patrones de timbrado
const int PATRONES_TIMBRADO[5][8] = {
  {500, 500, 500, 500, 500, 500, 500, 500},  // Patrón 1: Continuo
  {200, 200, 200, 200, 800, 800, 200, 200},  // Patrón 2: SOS
  {1000, 500, 1000, 500, 1000, 500, 1000, 500},  // Patrón 3: Largo-corto
  {200, 200, 200, 1000, 200, 200, 200, 1000},  // Patrón 4: Tres cortos, uno largo
  {100, 100, 100, 100, 100, 100, 1000, 1000}   // Patrón 5: Seis cortos, dos largos
};

// Variables para autenticación
const char* USUARIO_POR_DEFECTO = "admin";
const char* CONTRASENA_POR_DEFECTO = "password";
char usuarioActual[32] = "admin";
char contrasenaActual[32] = "password";

// Sistema de logging
#define LOG_MAX_ENTRIES 100

struct LogEntry {
  unsigned long timestamp;
  String mensaje;
};

LogEntry logBuffer[LOG_MAX_ENTRIES];
int logIndex = 0;

// Funciones de utilidad
void cargarCredenciales() {
  EEPROM.begin(512);
  EEPROM.get(0, usuarioActual);
  EEPROM.get(32, contrasenaActual);
  EEPROM.end();
}

void guardarCredenciales() {
  EEPROM.begin(512);
  EEPROM.put(0, usuarioActual);
  EEPROM.put(32, contrasenaActual);
  EEPROM.commit();
  EEPROM.end();
}

void registrarLog(String mensaje) {
  logBuffer[logIndex].timestamp = millis();
  logBuffer[logIndex].mensaje = mensaje;
  logIndex = (logIndex + 1) % LOG_MAX_ENTRIES;
  
  Serial.println(mensaje);
}

String obtenerLogsJSON() {
  DynamicJsonDocument doc(4096);
  JsonArray logsArray = doc.createNestedArray("logs");
  
  for (int i = 0; i < LOG_MAX_ENTRIES; i++) {
    int index = (logIndex - 1 - i + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
    if (logBuffer[index].mensaje.length() > 0) {
      JsonObject logObj = logsArray.createNestedObject();
      logObj["timestamp"] = logBuffer[index].timestamp;
      logObj["mensaje"] = logBuffer[index].mensaje;
    }
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

void sincronizarRTC() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime);
  rtc.setTimeStruct(*ptm);
  tiempoUltimaSincronizacion = millis();
  registrarLog("RTC sincronizado con NTP");
}

void actualizarPantalla() {
  int hora = rtc.getHour(true);
  int minuto = rtc.getMinute();
  pantalla.showNumberDecEx(hora * 100 + minuto, 0b01000000, true);
}

void verificarAlarmas() {
  if (alarmaActiva) return;
  
  int horaActual = rtc.getHour(true);
  int minutoActual = rtc.getMinute();
  int diaSemanaActual = rtc.getDayofWeek();
  
  for (int i = 0; i < 24; i++) {
    if (alarmas[i].activa && 
        alarmas[i].hora == horaActual && 
        alarmas[i].minuto == minutoActual &&
        (alarmas[i].diasSemana & (1 << diaSemanaActual))) {
      activarAlarma(i);
      break;
    }
  }
}

void activarAlarma(int indice) {
  alarmaActiva = true;
  patronActual = alarmas[indice].patron;
  registrarLog("Alarma " + String(indice) + " activada");
}

void desactivarAlarma() {
  alarmaActiva = false;
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_RELE, LOW);
  registrarLog("Alarma desactivada");
}

void verificarBotonDesactivacion() {
  static unsigned long ultimoTiempoPresionado = 0;
  const unsigned long debounceDelay = 50;
  
  if (digitalRead(PIN_BOTON) == LOW) {
    if (millis() - ultimoTiempoPresionado > debounceDelay) {
      if (alarmaActiva) {
        desactivarAlarma();
      }
    }
    ultimoTiempoPresionado = millis();
  }
}

void cargarAlarmas() {
  EEPROM.begin(sizeof(Alarma) * 24);
  for (int i = 0; i < 24; i++) {
    EEPROM.get(i * sizeof(Alarma), alarmas[i]);
  }
  EEPROM.end();
}

void guardarAlarmas() {
  EEPROM.begin(sizeof(Alarma) * 24);
  for (int i = 0; i < 24; i++) {
    EEPROM.put(i * sizeof(Alarma), alarmas[i]);
  }
  EEPROM.commit();
  EEPROM.end();
}

bool autenticar(AsyncWebServerRequest *request) {
  if (!request->authenticate(usuarioActual, contrasenaActual)) {
    return false;
  }
  return true;
}

String obtenerAlarmasJSON() {
  DynamicJsonDocument doc(1024);
  JsonArray alarmasArray = doc.createNestedArray("alarmas");
  
  for (int i = 0; i < 24; i++) {
    JsonObject alarmaObj = alarmasArray.createNestedObject();
    alarmaObj["id"] = i;
    alarmaObj["activa"] = alarmas[i].activa;
    alarmaObj["hora"] = alarmas[i].hora;
    alarmaObj["minuto"] = alarmas[i].minuto;
    alarmaObj["diasSemana"] = alarmas[i].diasSemana;
    alarmaObj["patron"] = alarmas[i].patron;
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

void configurarAlarma(String alarmaJson) {
  DynamicJsonDocument doc(256);
  deserializeJson(doc, alarmaJson);
  
  int id = doc["id"];
  if (id >= 0 && id < 24) {
    alarmas[id].activa = doc["activa"];
    alarmas[id].hora = doc["hora"];
    alarmas[id].minuto = doc["minuto"];
    alarmas[id].diasSemana = doc["diasSemana"];
    alarmas[id].patron = doc["patron"];
    guardarAlarmas();
    registrarLog("Alarma " + String(id) + " configurada");
  }
}

void cambiarContrasena(String nuevaContrasena) {
  strncpy(contrasenaActual, nuevaContrasena.c_str(), sizeof(contrasenaActual) - 1);
  contrasenaActual[sizeof(contrasenaActual) - 1] = '\0';
  guardarCredenciales();
  registrarLog("Contraseña cambiada");
}

bool verificarCodigoReset(String codigo) {
  // Implementa tu lógica de verificación aquí
  // Por ejemplo, podrías tener un código fijo o generarlo basado en algún algoritmo
  return codigo == "RESET123";
}

void resetearContrasena() {
  strncpy(usuarioActual, USUARIO_POR_DEFECTO, sizeof(usuarioActual) - 1);
  strncpy(contrasenaActual, CONTRASENA_POR_DEFECTO, sizeof(contrasenaActual) - 1);
  usuarioActual[sizeof(usuarioActual) - 1] = '\0';
  contrasenaActual[sizeof(contrasenaActual) - 1] = '\0';
  guardarCredenciales();
  registrarLog("Contraseña reseteada a valores por defecto");
}

void configurarServidorWeb() {
  servidor.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!autenticar(request)) {
      return request->requestAuthentication();
    }
    request->send(SPIFFS, "/index.html", "text/html");
  });

  servidor.on("/alarmas", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!autenticar(request)) {
      return request->requestAuthentication();
    }
    String json = obtenerAlarmasJSON();
    request->send(200, "application/json", json);
  });

  servidor.on("/configurar", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!autenticar(request)) {
      return request->requestAuthentication();
    }
    if (request->hasParam("alarma", true)) {
      String alarmaJson = request->getParam("alarma", true)->value();
      configurarAlarma(alarmaJson);
      request->send(200, "text/plain", "Alarma configurada");
    } else {
      request->send(400, "text/plain", "Parámetro 'alarma' no encontrado");
    }
  });

  servidor.on("/cambiarContrasena", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!autenticar(request)) {
      return request->requestAuthentication();
    }
    if (request->hasParam("nuevaContrasena", true)) {
      String nuevaContrasena = request->getParam("nuevaContrasena", true)->value();
      cambiarContrasena(nuevaContrasena);
      request->send(200, "text/plain", "Contraseña cambiada");
    } else {
      request->send(400, "text/plain", "Parámetro 'nuevaContrasena' no encontrado");
    }
  });

  servidor.on("/resetearContrasena", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("codigoReset", true)) {
      String codigoReset = request->getParam("codigoReset", true)->value();
      if (verificarCodigoReset(codigoReset)) {
        resetearContrasena();
        request->send(200, "text/plain", "Contraseña reseteada a valores por defecto");
      } else {
        request->send(400, "text/plain", "Código de reset inválido");
      }
    } else {
      request->send(400, "text/plain", "Parámetro 'codigoReset' no encontrado");
    }
  });

  servidor.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!autenticar(request)) {
      return request->requestAuthentication();
    }
    String json = obtenerLogsJSON();
    request->send(200, "application/json", json);
  });

  servidor.serveStatic("/", SPIFFS, "/");

  servidor.begin();
}

void setup() {
  Serial.begin(115200);
  
  // Configuración de pines
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  
  // Inicialización de la pantalla
  pantalla.setBrightness(0x0f);
  
  // Inicialización de SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("Error al montar SPIFFS");
    return;
  }
  
  // Configuración de WiFi
  WiFiManager wifiManager;
  wifiManager.autoConnect("ESP32_Reloj_AP");
  
  // Configuración de NTP
  timeClient.begin();
  
  // Sincronización inicial del RTC
  sincronizarRTC();
  
  // Cargar alarmas desde EEPROM
  cargarAlarmas();
  
  // Cargar credenciales
  cargarCredenciales();
  
  // Configuración del servidor web
  configurarServidorWeb();
  
  registrarLog("Sistema iniciado");
}

void loop() {
  // Actualizar la hora en la pantalla
  actualizarPantalla();
  
  // Verificar y activar alarmas
  verificarAlarmas();
  
  // Verificar botón para desactivar alarma
  verificarBotonDesactivacion();
  
  // Sincronizar RTC si es necesario
  if (millis() - tiempoUltimaSincronizacion >= intervaloSincronizacion) {
    sincronizarRTC();
  }
  
  // Mane
  if (alarmaActiva) {
    static unsigned long ultimoCambio = 0;
    static bool estadoBuzzer = false;
    static int pasoActual = 0;
    
    unsigned long tiempoActual = millis();
    if (tiempoActual - ultimoCambio >= PATRONES_TIMBRADO[patronActual][pasoActual]) {
      estadoBuzzer = !estadoBuzzer;
      digitalWrite(PIN_BUZZER, estadoBuzzer);
      digitalWrite(PIN_RELE, estadoBuzzer);
      ultimoCambio = tiempoActual;
      pasoActual = (pasoActual + 1) % 8;
    }
  }
  
  delay(10); // Pequeña pausa para evitar sobrecarga del CPU
}
