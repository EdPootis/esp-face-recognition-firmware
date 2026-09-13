#ifndef SECRETS_H
#define SECRETS_H

// ============================================================================
// TEMPLATE KONFIGURASI KREDENSIAL & BACKEND SERVER
// ============================================================================
// Salin file ini menjadi 'secrets.h' lalu sesuaikan:
//   cp secrets.example.h secrets.h
// 
// ============================================================================

// Konfigurasi WiFi
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// IP Komputer / Server yang menjalankan Python Face Recognition Backend (port 5050)
const char* pythonServerIp = "192.168.1.100";
const int   pythonServerPort = 5050;

#endif // SECRETS_H
