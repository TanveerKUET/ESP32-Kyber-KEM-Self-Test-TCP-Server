# ESP32 Kyber KEM Self-Test + TCP Server

This project turns an ESP32 into a simple **post-quantum key exchange server** using:

- A CRYSTALS-Kyber KEM implementation (`kem.h`)
- A TCP server on port `5000`
- A one-time **Kyber self-test** running in a FreeRTOS task
- A per-client Kyber handshake over Wi-Fi

The code is written for the ESP32 Arduino core.

---

## 1. Wi-Fi & Server Configuration

At the top of the sketch you configure Wi-Fi and the TCP port:

```cpp
const char* WIFI_SSID     = "WIFI_SSID";
const char* WIFI_PASSWORD = "WIFI_PASSWORD";

const uint16_t SERVER_PORT = 5000;
```

- `WIFI_SSID` / `WIFI_PASSWORD` – your Wi-Fi credentials.
- `SERVER_PORT` – TCP port where the ESP32 listens for clients.

A global TCP server object is then created:

```cpp
WiFiServer server(SERVER_PORT);
```

---

## 2. Includes & Kyber Buffers

```cpp
#include <WiFi.h>
#include "kem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
```

- `WiFi.h` – Wi-Fi and TCP networking.
- `kem.h` – Kyber KEM API (`crypto_kem_keypair`, `crypto_kem_enc`, `crypto_kem_dec`).
- FreeRTOS headers – used for the self-test task.

Global Kyber buffers:

```cpp
uint8_t pk[CRYPTO_PUBLICKEYBYTES];
uint8_t sk[CRYPTO_SECRETKEYBYTES];
uint8_t ct[CRYPTO_CIPHERTEXTBYTES];
uint8_t ss[CRYPTO_BYTES];

uint8_t ct_test[CRYPTO_CIPHERTEXTBYTES];
uint8_t ss_enc[CRYPTO_BYTES];
uint8_t ss_dec[CRYPTO_BYTES];
```

- `pk`, `sk` – public/secret key.
- `ct`, `ss` – ciphertext and shared secret used in the real TCP handshake.
- `ct_test`, `ss_enc`, `ss_dec` – used only in the Kyber self-test.

Sizes like `CRYPTO_PUBLICKEYBYTES` are defined in `kem.h`/`params.h` and depend on the Kyber parameter set.

---

## 3. Hex Helpers

The code sends keys and ciphertext as hex strings over TCP.

### `hexCharToNibble`

Converts one hex character into a 4‑bit value (0–15):

```cpp
int hexCharToNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}
```

### `bytesToHex`

Converts a byte array to a lowercase hex string:

```cpp
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
```

Used to serialize `pk`, `ct`, and `ss` for printing/sending.

### `hexToBytes`

Converts hex back to bytes, validating length and characters:

```cpp
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
```

---

## 4. Reading a Line from the TCP Client

```cpp
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
```

- Reads characters from a TCP client until it sees `\n` or the timeout expires.
- Strips `\r` characters.
- Used later to receive `CT:<hex>` from the client.

---

## 5. Kyber Self-Test Task

The Kyber self-test runs once at startup in its own FreeRTOS task.

### 5.1 Task Function

```cpp
void kyber_self_test_task(void *pvParameters) {
  (void) pvParameters;

  Serial.println("=== Testing Kyber KEM (keypair + enc + dec self-test) ===");
```

### 5.2 Keypair Generation

```cpp
  int rc = crypto_kem_keypair(pk, sk);
  if (rc != 0) {
    Serial.println("[-] Keypair generation FAILED!");
    vTaskDelete(NULL);
    return;
  }
```

- Calls Kyber to generate `pk` and `sk`.
- On failure, logs and deletes the task.

On success, prints key sizes and a snippet of `pk` in hex:

```cpp
  Serial.println("[+] Keypair generation SUCCESS!");
  Serial.print("PK length: ");
  Serial.println(sizeof(pk));
  Serial.print("SK length: ");
  Serial.println(sizeof(sk));

  String pkHex = bytesToHex(pk, sizeof(pk));
  Serial.println("PK (first 64 hex chars):");
  Serial.println(pkHex.substring(0, 64));
```

### 5.3 Encapsulation

```cpp
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
```

- Encapsulates a shared secret `ss_enc` to the public key `pk` and produces ciphertext `ct_test`.

### 5.4 Decapsulation

```cpp
  rc = crypto_kem_dec(ss_dec, ct_test, sk);
  if (rc != 0) {
    Serial.println("[-] crypto_kem_dec FAILED!");
    vTaskDelete(NULL);
    return;
  }
```

- Uses the secret key `sk` to recover `ss_dec` from `ct_test`.

### 5.5 Compare Shared Secrets

```cpp
  bool match = true;
  for (size_t i = 0; i < CRYPTO_BYTES; ++i) {
    if (ss_enc[i] != ss_dec[i]) {
      match = false;
      break;
    }
  }
```

If they match, the self-test passes:

```cpp
  if (match) {
    Serial.println("[✓] KEM self-test PASSED: shared secrets match!");
    String ssHex = bytesToHex(ss_enc, sizeof(ss_enc));
    Serial.print("Shared secret (first 64 hex chars): ");
    Serial.println(ssHex.substring(0, 64));
  } else {
    Serial.println("[✗] KEM self-test FAILED: shared secrets DO NOT match!");
    ...
  }

  Serial.println("=== Kyber self-test DONE ===");
  vTaskDelete(NULL);
}
```

- On success, prints part of the shared secret.
- Always ends with `vTaskDelete(NULL)` so the self-test task exits cleanly.

---

## 6. `setup()` – Serial, Task, Wi-Fi, and Server

```cpp
void setup() {
  Serial.begin(115200);
  delay(2000);
```

- Starts the Serial Monitor for logging.

### 6.1 Create the Self-Test Task

```cpp
  xTaskCreatePinnedToCore(
    kyber_self_test_task,
    "kyber_self_test",
    16384,                  // stack size in bytes
    NULL,
    1,
    NULL,
    1                       // run on core 1
  );
```

- Creates a FreeRTOS task for the Kyber self-test.
- Gives it a 16 KB stack to safely handle Kyber’s large local arrays.

### 6.2 Wi-Fi and TCP Server

```cpp
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
```

- Connects to your Wi-Fi network.
- Starts the TCP server on `SERVER_PORT`.
- Prints the assigned IP address for the ESP32.

---

## 7. `loop()` – Per-Client Kyber Handshake

The main loop handles incoming clients and performs the Kyber KEM handshake.

### 7.1 Wait for Client

```cpp
void loop() {
  WiFiClient client = server.available();
  if (!client) {
    delay(50);
    return;
  }

  Serial.println("\n[+] Client connected");
```

- Checks for a new TCP client.
- If none, waits briefly and returns.

### 7.2 Generate Keypair for This Connection

```cpp
  int rc = crypto_kem_keypair(pk, sk);
  if (rc != 0) {
    Serial.println("[-] kyber512_keypair failed!");
    client.stop();
    return;
  }
  Serial.println("[*] Kyber keypair generated");
```

- Generates a fresh public/secret keypair for each client connection.

### 7.3 Send Public Key

```cpp
  String pkHex = bytesToHex(pk, sizeof(pk));
  client.print("PK:");
  client.print(pkHex);
  client.print("\n");
  Serial.println("[*] Sent public key to client");
```

- Sends the public key as a single line:
  ```text
  PK:<hex_of_public_key>

  ```

### 7.4 Receive Ciphertext

```cpp
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
```

- Expects the client to send:
  ```text
  CT:<hex_of_ciphertext>

  ```
- Parses the hex ciphertext into the `ct` buffer.

### 7.5 Decapsulate Shared Secret

```cpp
  rc = crypto_kem_dec(ss, ct, sk);
  if (rc != 0) {
    Serial.println("[-] kyber512_dec failed!");
    client.stop();
    return;
  }

  String ssHex = bytesToHex(ss, sizeof(ss));
  Serial.print("[+] Shared secret (server): ");
  Serial.println(ssHex);
```

- Uses the secret key `sk` and ciphertext `ct` to compute the shared secret `ss`.
- Prints the shared secret in hex (demo/debug only).

Finally, close the connection and wait before serving another client:

```cpp
  Serial.println("[*] Handshake complete, closing connection");
  client.stop();

  // For demo, wait 5s before accepting another client
  delay(5000);
}
```

---

## 8. Output
```c
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:1
load:0x3fff0030,len:4744
load:0x40078000,len:15672
load:0x40080400,len:3164
entry 0x4008059c

=== ESP32 Kyber-512 SERVER (network mode) ===
=== Testing Kyber KEM (keypair + enc + dec self-test) ===
[+] Keypair generation SUCCESS!
PK length: 800
SK length: 1632
PK (first 64 hex chars):
793b532f53047f3918823212ac9a864c686743570cca6c68ffacb6a5d59561c6
[+] crypto_kem_enc SUCCESS!
CT (first 64 hex chars):
2b7ad58a88229512046fdcc555f86054e44b624820d49c6a2fe4febb2b1b24cd
[✓] KEM self-test PASSED: shared secrets match!
Shared secret (first 64 hex chars): a2bdafb7bb5119cf5621529611033dfe86da897721332338e02176076524d47e
=== Kyber self-test DONE ===
Connecting to WiFi...
Connected, IP address: 192.168.137.93
Server listening on port 5000
```
