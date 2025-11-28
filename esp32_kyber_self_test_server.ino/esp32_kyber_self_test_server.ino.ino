#include <WiFi.h>
#include "kem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ================== USER CONFIG ==================
const char* WIFI_SSID     = "WIFI_SSID";
const char* WIFI_PASSWORD = "WIFI_PASSWORD";

const uint16_t SERVER_PORT = 5000;
// =================================================

WiFiServer server(SERVER_PORT);

// ---- Helpers: hex encoding/decoding ----
int hexCharToNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

String bytesToHex(const uint8_t* data, size_t len) {
  const char* hexmap = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    out += hexmap[b >> 4];
    out += hexmap[b & 0x0F];
  }
  return out;
}

bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
  if (hex.length() != outLen * 2) return false;
  for (size_t i = 0; i < outLen; ++i) {
    int hi = hexCharToNibble(hex[2 * i]);
    int lo = hexCharToNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (hi << 4) | lo;
  }
  return true;
}

// Read a single line terminated by '\n' (strip '\r') with timeout
bool readLine(WiFiClient& client, String& out, uint32_t timeoutMs = 10000) {
  uint32_t start = millis();
  out = "";
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        return true;  // line complete
      }
      if (c != '\r') {
        out += c;
      }
    }
    delay(10);
  }
  return false; // timeout
}

// ---- Kyber buffers ----
uint8_t pk[CRYPTO_PUBLICKEYBYTES];
uint8_t sk[CRYPTO_SECRETKEYBYTES];
uint8_t ct[CRYPTO_CIPHERTEXTBYTES];
uint8_t ss[CRYPTO_BYTES];

// Extra buffers for self-test
uint8_t ct_test[CRYPTO_CIPHERTEXTBYTES];
uint8_t ss_enc[CRYPTO_BYTES];
uint8_t ss_dec[CRYPTO_BYTES];

// --------- Kyber self-test task (runs once) ---------
void kyber_self_test_task(void *pvParameters) {
  (void) pvParameters;

  Serial.println("=== Testing Kyber KEM (keypair + enc + dec self-test) ===");

  // 1) Keypair
  int rc = crypto_kem_keypair(pk, sk);
  if (rc != 0) {
    Serial.println("[-] Keypair generation FAILED!");
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[+] Keypair generation SUCCESS!");
  Serial.print("PK length: ");
  Serial.println(sizeof(pk));
  Serial.print("SK length: ");
  Serial.println(sizeof(sk));

  String pkHex = bytesToHex(pk, sizeof(pk));
  Serial.println("PK (first 64 hex chars):");
  Serial.println(pkHex.substring(0, 64));

  // 2) Encapsulation
  rc = crypto_kem_enc(ct_test, ss_enc, pk);
  if (rc != 0) {
    Serial.println("[-] crypto_kem_enc FAILED!");
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[+] crypto_kem_enc SUCCESS!");
  String ctHex = bytesToHex(ct_test, sizeof(ct_test));
  Serial.println("CT (first 64 hex chars):");
  Serial.println(ctHex.substring(0, 64));

  // 3) Decapsulation
  rc = crypto_kem_dec(ss_dec, ct_test, sk);
  if (rc != 0) {
    Serial.println("[-] crypto_kem_dec FAILED!");
    vTaskDelete(NULL);
    return;
  }

  // 4) Compare shared secrets
  bool match = true;
  for (size_t i = 0; i < CRYPTO_BYTES; ++i) {
    if (ss_enc[i] != ss_dec[i]) {
      match = false;
      break;
    }
  }

  if (match) {
    Serial.println("[✓] KEM self-test PASSED: shared secrets match!");
    String ssHex = bytesToHex(ss_enc, sizeof(ss_enc));
    Serial.print("Shared secret (first 64 hex chars): ");
    Serial.println(ssHex.substring(0, 64));
  } else {
    Serial.println("[✗] KEM self-test FAILED: shared secrets DO NOT match!");
    String ssEncHex = bytesToHex(ss_enc, sizeof(ss_enc));
    String ssDecHex = bytesToHex(ss_dec, sizeof(ss_dec));
    Serial.print("ss_enc (first 64 hex chars): ");
    Serial.println(ssEncHex.substring(0, 64));
    Serial.print("ss_dec (first 64 hex chars): ");
    Serial.println(ssDecHex.substring(0, 64));
  }

  Serial.println("=== Kyber self-test DONE ===");
  vTaskDelete(NULL);   // task finished
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Create a dedicated task with a larger stack for Kyber self-test
  // Stack size here is 16384 bytes (you can go higher if needed).
  xTaskCreatePinnedToCore(
    kyber_self_test_task,   // task function
    "kyber_self_test",      // name
    16384,                  // stack size in bytes (Arduino-ESP32 uses bytes)
    NULL,                   // parameter
    1,                      // priority
    NULL,                   // task handle
    1                       // core (1 = same core as loop usually)
  );

  Serial.println();
  Serial.println("=== ESP32 Kyber-512 SERVER (network mode) ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  server.begin();
  Serial.print("Server listening on port ");
  Serial.println(SERVER_PORT);
}

void loop() {
  WiFiClient client = server.available();
  if (!client) {
    delay(50);
    return;
  }

  Serial.println("\n[+] Client connected");

  // For simplicity, still generating a fresh keypair per client.
  // If this also overflows, we can move this into a task too.
  int rc = crypto_kem_keypair(pk, sk);
  if (rc != 0) {
    Serial.println("[-] kyber512_keypair failed!");
    client.stop();
    return;
  }
  Serial.println("[*] Kyber keypair generated");

  // 2) Send public key as "PK:<hex>\n"
  String pkHex = bytesToHex(pk, sizeof(pk));
  client.print("PK:");
  client.print(pkHex);
  client.print("\n");
  Serial.println("[*] Sent public key to client");

  // 3) Receive ciphertext as "CT:<hex>\n"
  String line;
  if (!readLine(client, line)) {
    Serial.println("[-] Timeout waiting for CT line");
    client.stop();
    return;
  }

  if (!line.startsWith("CT:")) {
    Serial.print("[-] Invalid message from client: ");
    Serial.println(line);
    client.stop();
    return;
  }

  String ctHex = line.substring(3);
  if (!hexToBytes(ctHex, ct, sizeof(ct))) {
    Serial.println("[-] Failed to parse ciphertext hex");
    client.stop();
    return;
  }
  Serial.println("[*] Received ciphertext from client");

  // 4) Decapsulate to get shared secret
  rc = crypto_kem_dec(ss, ct, sk);
  if (rc != 0) {
    Serial.println("[-] kyber512_dec failed!");
    client.stop();
    return;
  }

  String ssHex = bytesToHex(ss, sizeof(ss));
  Serial.print("[+] Shared secret (server): ");
  Serial.println(ssHex);

  Serial.println("[*] Handshake complete, closing connection");
  client.stop();

  // For demo, wait 5s before accepting another client
  delay(5000);
}
