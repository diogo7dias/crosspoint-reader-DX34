#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;
constexpr size_t AES_KEY_LEN = 16;  // AES-128
constexpr size_t AES_IV_LEN = 16;
// Salt prevents the derived key from matching any other use of the raw MAC.
constexpr char kKeySalt[] = "crosspoint-cred-v2";

const uint8_t* getHwKey() {
  static uint8_t key[HW_KEY_LEN] = {};
  static bool initialized = false;
  if (!initialized) {
    esp_efuse_mac_get_default(key);
    initialized = true;
  }
  return key;
}

// Derive a 16-byte AES key from the hardware MAC via SHA-256.
// SHA-256(salt + MAC) → take first 16 bytes as AES-128 key.
void deriveAesKey(uint8_t out[AES_KEY_LEN]) {
  const uint8_t* mac = getHwKey();
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not SHA-224)
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(kKeySalt), strlen(kKeySalt));
  mbedtls_sha256_update(&ctx, mac, HW_KEY_LEN);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  memcpy(out, hash, AES_KEY_LEN);
}

// PKCS7 pad: appends 1..16 bytes so total length is a multiple of 16.
std::string pkcs7Pad(const std::string& data) {
  const uint8_t padLen = AES_IV_LEN - (data.size() % AES_IV_LEN);
  std::string padded = data;
  padded.append(padLen, static_cast<char>(padLen));
  return padded;
}

// PKCS7 unpad. Returns false on invalid padding.
bool pkcs7Unpad(std::string& data) {
  if (data.empty()) return false;
  const uint8_t padLen = static_cast<uint8_t>(data.back());
  if (padLen == 0 || padLen > AES_IV_LEN || padLen > data.size()) return false;
  for (size_t i = data.size() - padLen; i < data.size(); i++) {
    if (static_cast<uint8_t>(data[i]) != padLen) return false;
  }
  data.resize(data.size() - padLen);
  return true;
}
}  // namespace

// --- Legacy XOR (kept for backward-compatible reads) ---

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  if (keyLen == 0 || key == nullptr) return;
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

// --- AES-128-CBC encryption (new) ---

String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return "";

  uint8_t aesKey[AES_KEY_LEN];
  deriveAesKey(aesKey);

  // Random IV
  uint8_t iv[AES_IV_LEN];
  esp_fill_random(iv, AES_IV_LEN);

  // PKCS7 pad plaintext
  const std::string padded = pkcs7Pad(plaintext);

  // Encrypt
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, aesKey, AES_KEY_LEN * 8);

  std::string ciphertext(padded.size(), '\0');
  uint8_t ivCopy[AES_IV_LEN];
  memcpy(ivCopy, iv, AES_IV_LEN);  // CBC modifies IV in-place
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded.size(), ivCopy,
                         reinterpret_cast<const unsigned char*>(padded.data()),
                         reinterpret_cast<unsigned char*>(&ciphertext[0]));
  mbedtls_aes_free(&aes);

  // Output: IV + ciphertext, then base64-encode
  std::string output;
  output.reserve(AES_IV_LEN + ciphertext.size());
  output.append(reinterpret_cast<const char*>(iv), AES_IV_LEN);
  output.append(ciphertext);

  return base64::encode(reinterpret_cast<const uint8_t*>(output.data()), output.size());
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return "";
  }

  // Base64-decode first
  const size_t encodedLen = strlen(encoded);
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen,
                                  reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }

  std::string raw(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&raw[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  raw.resize(decodedLen);

  // Try AES-128-CBC first: valid if length >= IV(16) + at least one block(16) = 32,
  // and (length - 16) is a multiple of 16.
  if (raw.size() >= AES_IV_LEN + AES_IV_LEN && (raw.size() - AES_IV_LEN) % AES_IV_LEN == 0) {
    uint8_t aesKey[AES_KEY_LEN];
    deriveAesKey(aesKey);

    uint8_t iv[AES_IV_LEN];
    memcpy(iv, raw.data(), AES_IV_LEN);

    const size_t cipherLen = raw.size() - AES_IV_LEN;
    std::string plaintext(cipherLen, '\0');

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, aesKey, AES_KEY_LEN * 8);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, iv,
                           reinterpret_cast<const unsigned char*>(raw.data() + AES_IV_LEN),
                           reinterpret_cast<unsigned char*>(&plaintext[0]));
    mbedtls_aes_free(&aes);

    if (pkcs7Unpad(plaintext)) {
      // PKCS7 unpad succeeded — this is a valid AES-encrypted value
      if (ok) *ok = true;
      return plaintext;
    }
    // PKCS7 unpad failed — fall through to legacy XOR
  }

  // Legacy XOR fallback: for values written by older firmware versions
  xorTransform(raw);
  if (ok) *ok = true;
  return raw;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a", "exactly16chars!!"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    String encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  // Verify different ciphertext each time (IV randomness)
  String enc1 = obfuscateToBase64("test123");
  String enc2 = obfuscateToBase64("test123");
  if (enc1 == enc2) {
    LOG_ERR("OBF", "FAIL: two encryptions of same plaintext produced identical output (IV not random?)");
    allPassed = false;
  }
  if (enc1 == "test123") {
    LOG_ERR("OBF", "FAIL: encrypted output identical to plaintext");
    allPassed = false;
  }
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED (AES-128-CBC)");
  }
}

}  // namespace obfuscation
