# ESP32 Kyber KEM Self-Test + TCP Server

This project turns an ESP32 into a simple post-quantum key exchange server using a CRYSTALS-Kyber KEM implementation (`kem.h`) and a TCP server on port 5000. It also runs a one-time Kyber self-test in a separate FreeRTOS task.

## How the Code Works (Step by Step)

1. **Includes & Globals**
   - `#include <WiFi.h>`: enables Wi-Fi and TCP networking.
   - `#include "kem.h"`: provides `crypto_kem_keypair`, `crypto_kem_enc`, `crypto_kem_dec` for Kyber.
   - `#include "freertos/FreeRTOS.h"` and `freertos/task.h`: used to create a dedicated self-test task.
   - Creates a `WiFiServer server(SERVER_PORT);` and global Kyber buffers: `pk`, `sk`, `ct`, `ss`, plus extra buffers for the self-test (`ct_test`, `ss_enc`, `ss_dec`).

2. **Hex Helpers**
   - `hexCharToNibble(char c)`: converts a single hex character into a 0–15 value.
   - `bytesToHex(const uint8_t* data, size_t len)`: converts a byte array (public key, ciphertext, shared secret) into a lowercase hex string for printing/sending.
   - `hexToBytes(const String& hex, uint8_t* out, size_t outLen)`: converts a hex string back into a byte array; used to parse the ciphertext sent by the client.

3. **Line Reader**
   - `readLine(WiFiClient& client, String& out, uint32_t timeoutMs)`: reads characters from a TCP client until it sees `\n` or times out; strips `\r`. Used to receive the `CT:<hex>` line from the client.

4. **Kyber Self-Test Task**
   - `kyber_self_test_task(void *pvParameters)` runs **once** in its own FreeRTOS task:
     - Calls `crypto_kem_keypair(pk, sk)` to generate a keypair.
     - Calls `crypto_kem_enc(ct_test, ss_enc, pk)` to encapsulate and create a test ciphertext and shared secret.
     - Calls `crypto_kem_dec(ss_dec, ct_test, sk)` to decapsulate.
     - Compares `ss_enc` vs `ss_dec`. If they match, the Kyber KEM self-test passes.
     - Prints debug info (lengths, first 64 hex chars of pk, ct, and shared secret) to the Serial Monitor.
     - Ends with `vTaskDelete(NULL)` so it does not run again.

5. **`setup()` – Initialization**
   - Starts serial (`Serial.begin(115200)`).
   - Creates the self-test task with `xTaskCreatePinnedToCore(kyber_self_test_task, ..., 16384, ..., 1)` giving it a larger stack (16 KB) to safely run Kyber code.
   - Connects to Wi-Fi using `WIFI_SSID` / `WIFI_PASSWORD` and prints the ESP32 IP.
   - Starts the TCP server with `server.begin()` and logs the listening port.

6. **`loop()` – Per-Client Kyber Handshake**
   - Waits for a client using `WiFiClient client = server.available();`.
   - When a client connects:
     - Generates a fresh Kyber keypair with `crypto_kem_keypair(pk, sk)`.
     - Sends the public key as a single line: `PK:<hex_of_pk>\n` using `bytesToHex`.
     - Receives a line from the client and expects it to start with `CT:`; parses the hex into `ct` with `hexToBytes`.
     - Calls `crypto_kem_dec(ss, ct, sk)` to compute the shared secret `ss` on the server.
     - Prints the shared secret in hex on the Serial Monitor.
     - Closes the connection (`client.stop()`) and waits 5 seconds before accepting a new client.

## Protocol Summary

- Server sends: `PK:<hex_public_key>\n`
- Client responds with: `CT:<hex_ciphertext>\n`
- Both sides derive the same 32-byte shared secret from Kyber (if implemented correctly).

You can use this README in your GitHub repository alongside `esp32_kyber_self_test_server.ino` to document how the ESP32 Kyber server works.
