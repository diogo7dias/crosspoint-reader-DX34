#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Credential encryption utilities using the ESP32's unique hardware MAC address.
 *
 * Current scheme (v2): AES-128-CBC with a key derived from SHA-256(salt + MAC).
 * Random IV per encryption ensures identical plaintexts produce different
 * ciphertext. Stored as base64(IV[16] + AES-CBC(PKCS7-padded plaintext)).
 *
 * Decryption tries AES first; falls back to legacy XOR for values written
 * by older firmware versions, providing transparent migration.
 *
 * Not a substitute for hardware key storage (eFuse-based NVS encryption),
 * but significantly stronger than the legacy XOR obfuscation — an attacker
 * must know both the algorithm and key derivation scheme to recover
 * credentials from the SD card.
 */
namespace obfuscation {

// Legacy XOR transform (symmetric). Kept for migration compatibility.
void xorTransform(std::string& data);

// Legacy overload for binary migration (uses old per-store hardcoded keys)
void xorTransform(std::string& data, const uint8_t* key, size_t keyLen);

// Encrypt a plaintext string: AES-128-CBC with derived key, base64-encoded for JSON storage
String obfuscateToBase64(const std::string& plaintext);

// Decode base64 and decrypt back to plaintext. Tries AES first, falls back to legacy XOR.
// Returns empty string on invalid input; sets *ok to false if decode fails.
std::string deobfuscateFromBase64(const char* encoded, bool* ok = nullptr);

// Self-test: verifies round-trip encryption with hardware key. Logs PASS/FAIL.
void selfTest();

}  // namespace obfuscation
