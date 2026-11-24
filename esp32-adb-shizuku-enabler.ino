// Shizuku Universal Auto-Activator for ESP32-S3-Zero (Final Clean Version)
// Wi-Fi AP: "ESP32-ADB-LOG" (Pass: 12345678) -> Plain Text Log: http://192.168.4.1
// Button B (GPIO 0): Short press to restart board & clear log
// LED (GPIO 21, GRB fixed):
//   Blue   = Ready / Waiting USB
//   Cyan   = USB Connected / ADB Handshake
//   Yellow = Waiting Phone Auth Dialog
//   Purple = Authenticated / Running Shizuku
//   Green  = Success!
//   Red    = Error

#include "EspUsbHost.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>

static constexpr uint8_t RGB_LED_PIN = 21;
static constexpr uint8_t BOOT_BTN_PIN = 0;

static constexpr char AP_SSID[] = "ESP32-ADB-LOG";
static constexpr char AP_PASS[] = "12345678";
WebServer server(80);
String webLog = "";

static void logMsg(const String &msg)
{
    Serial.println(msg);
    webLog += "[" + String(millis() / 1000.0, 1) + "s] " + msg + "\n";
    if (webLog.length() > 4000)
    {
        webLog = webLog.substring(webLog.length() - 3500);
    }
}

// 已针对你的 ESP32-S3-Zero 批次修正 GRB 通道顺序
static void setLedColor(uint8_t r, uint8_t g, uint8_t b)
{
    neopixelWrite(RGB_LED_PIN, g, r, b);
}

static constexpr uint8_t ADB_CLASS = 0xff;
static constexpr uint8_t ADB_SUBCLASS = 0x42;
static constexpr uint8_t ADB_PROTOCOL = 0x01;

static constexpr uint32_t A_CNXN = 0x4e584e43;
static constexpr uint32_t A_AUTH = 0x48545541;
static constexpr uint32_t A_OPEN = 0x4e45504f;
static constexpr uint32_t A_OKAY = 0x59414b4f;
static constexpr uint32_t A_CLSE = 0x45534c43;
static constexpr uint32_t A_WRTE = 0x45545257;

static constexpr uint32_t ADB_AUTH_TOKEN = 1;
static constexpr uint32_t ADB_AUTH_SIGNATURE = 2;
static constexpr uint32_t ADB_AUTH_RSAPUBLICKEY = 3;
static constexpr uint32_t A_VERSION = 0x01000000;
static constexpr uint32_t A_MAXDATA = 4096;
static constexpr uint32_t LOCAL_STREAM_ID = 1;
static constexpr uint32_t AUTHORIZATION_GRACE_MS = 60000;
static constexpr uint32_t STABILITY_MS = 300;

static constexpr char SHELL_SERVICE[] = "shell:PKG=$(pm path moe.shizuku.privileged.api | sed 's/package://g' | tr -d '\\r\\n'); DIR=$(dirname \"$PKG\"); \"$DIR/lib/arm64/libshizuku.so\" && echo SHIZUKU_OK || echo SHIZUKU_ERR";
static constexpr char EXPECTED_OUTPUT[] = "SHIZUKU_OK";

class AdbKeyStore
{
public:
    ~AdbKeyStore() { psa_destroy_key(key_); }

    bool begin()
    {
        if (psa_crypto_init() != PSA_SUCCESS) return false;
        if (load())
        {
            logMsg("ADB key: loaded from NVS");
            return true;
        }
        logMsg("ADB key: generating RSA-2048 key...");
        psa_key_attributes_t attributes = keyAttributes();
        const psa_status_t status = psa_generate_key(&attributes, &key_);
        psa_reset_key_attributes(&attributes);
        if (status != PSA_SUCCESS || !save()) return false;
        logMsg("ADB key: generated and saved to NVS");
        return true;
    }

    bool signToken(const uint8_t *token, size_t length, uint8_t *signature, size_t capacity)
    {
        if (key_ == PSA_KEY_ID_NULL || !token || length != 20 || !signature || capacity < MODULUS_BYTES) return false;
        size_t signatureLength = 0;
        return psa_sign_hash(key_, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1), token, length, signature, capacity, &signatureLength) == PSA_SUCCESS && signatureLength == MODULUS_BYTES;
    }

    bool publicKey(uint8_t *output, size_t capacity, size_t &outputLength)
    {
        struct __attribute__((packed)) AndroidRsaPublicKey
        {
            uint32_t modulusSizeWords;
            uint32_t n0inv;
            uint8_t modulus[MODULUS_BYTES];
            uint8_t rr[MODULUS_BYTES];
            uint32_t exponent;
        } encoded = {};

        outputLength = 0;
        uint8_t modulus[MODULUS_BYTES];
        if (!exportModulus(modulus)) return false;

        uint32_t n[WORDS];
        uint32_t rr[WORDS] = {1};
        for (size_t i = 0; i < WORDS; i++)
        {
            const uint8_t *p = modulus + MODULUS_BYTES - 4 * (i + 1);
            n[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }
        for (size_t bit = 0; bit < MODULUS_BYTES * 16; bit++) doubleModulo(rr, n);
        for (size_t i = 0; i < WORDS; i++)
        {
            for (size_t b = 0; b < 4; b++)
            {
                encoded.modulus[4 * i + b] = static_cast<uint8_t>(n[i] >> (8 * b));
                encoded.rr[4 * i + b] = static_cast<uint8_t>(rr[i] >> (8 * b));
            }
        }
        encoded.modulusSizeWords = WORDS;
        uint32_t inverse = 1;
        for (uint8_t i = 0; i < 5; i++) inverse *= 2 - n[0] * inverse;
        encoded.n0inv = 0 - inverse;
        encoded.exponent = 65537;

        size_t base64Length = 0;
        if (mbedtls_base64_encode(output, capacity, &base64Length, reinterpret_cast<const uint8_t *>(&encoded), sizeof(encoded)) != 0) return false;

        static constexpr char COMMENT[] = " shizuku@esp32s3";
        if (base64Length + sizeof(COMMENT) > capacity) return false;
        memcpy(output + base64Length, COMMENT, sizeof(COMMENT));
        outputLength = base64Length + sizeof(COMMENT);
        return true;
    }

private:
    static constexpr size_t MODULUS_BYTES = 256;
    static constexpr size_t WORDS = MODULUS_BYTES / sizeof(uint32_t);
    static constexpr size_t MAX_DER_BYTES = 1600;

    static psa_key_attributes_t keyAttributes()
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
        psa_set_key_bits(&attributes, MODULUS_BYTES * 8);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1));
        return attributes;
    }

    static void doubleModulo(uint32_t *r, const uint32_t *n)
    {
        uint32_t carry = 0;
        for (size_t i = 0; i < WORDS; i++)
        {
            const uint32_t next = r[i] >> 31;
            r[i] = (r[i] << 1) | carry;
            carry = next;
        }
        bool subtract = carry != 0;
        if (!subtract)
        {
            subtract = true;
            for (size_t i = WORDS; i-- > 0;)
            {
                if (r[i] != n[i]) { subtract = r[i] > n[i]; break; }
            }
        }
        if (subtract)
        {
            uint32_t borrow = 0;
            for (size_t i = 0; i < WORDS; i++)
            {
                const uint64_t difference = static_cast<uint64_t>(r[i]) - n[i] - borrow;
                r[i] = static_cast<uint32_t>(difference);
                borrow = (difference >> 32) ? 1 : 0;
            }
        }
    }

    static const uint8_t *derItem(const uint8_t *p, const uint8_t *end, uint8_t tag, size_t &length)
    {
        if (end - p < 2 || *p++ != tag) return nullptr;
        length = *p++;
        if (length & 0x80)
        {
            const size_t count = length & 0x7f;
            if (count == 0 || count > 2 || static_cast<size_t>(end - p) < count) return nullptr;
            length = 0;
            for (size_t i = 0; i < count; i++) length = (length << 8) | *p++;
        }
        return static_cast<size_t>(end - p) >= length ? p : nullptr;
    }

    bool exportModulus(uint8_t *modulus)
    {
        uint8_t der[PSA_EXPORT_PUBLIC_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_RSA_KEY_PAIR, MODULUS_BYTES * 8)];
        size_t derLength = 0;
        if (key_ == PSA_KEY_ID_NULL || psa_export_public_key(key_, der, sizeof(der), &derLength) != PSA_SUCCESS) return false;
        size_t length = 0;
        const uint8_t *p = derItem(der, der + derLength, 0x30, length);
        p = p ? derItem(p, p + length, 0x02, length) : nullptr;
        while (p && length > MODULUS_BYTES && *p == 0) { p++; length--; }
        if (!p || length != MODULUS_BYTES) return false;
        memcpy(modulus, p, MODULUS_BYTES);
        return true;
    }

    bool load()
    {
        Preferences preferences;
        if (!preferences.begin("esp-adb", true)) return false;
        const size_t length = preferences.getBytesLength("rsa-key");
        if (length == 0 || length > MAX_DER_BYTES) { preferences.end(); return false; }
        uint8_t *der = static_cast<uint8_t *>(malloc(length));
        const bool read = der && preferences.getBytes("rsa-key", der, length) == length;
        preferences.end();
        if (!read) { free(der); return false; }
        psa_key_attributes_t attributes = keyAttributes();
        const psa_status_t status = psa_import_key(&attributes, der, length, &key_);
        psa_reset_key_attributes(&attributes);
        free(der);
        return status == PSA_SUCCESS;
    }

    bool save()
    {
        uint8_t der[MAX_DER_BYTES];
        size_t length = 0;
        if (psa_export_key(key_, der, sizeof(der), &length) != PSA_SUCCESS) return false;
        Preferences preferences;
        if (!preferences.begin("esp-adb", false)) return false;
        const bool saved = preferences.putBytes("rsa-key", der, length) == length;
        preferences.end();
        return saved;
    }

    psa_key_id_t key_ = PSA_KEY_ID_NULL;
};

EspUsbHost usb;
AdbKeyStore adbKey;

static volatile bool connectPending = false;
static uint8_t deviceAddress = 0;
static bool adbOpen = false;
static bool online = false;
static bool signatureSent = false;
static bool publicKeySent = false;
static bool publicKeyOffered = false;
static bool streamOpening = false;
static bool streamOpen = false;
static bool shellDone = false;
static bool finished = false;
static uint32_t remoteStreamId = 0;
static uint32_t signatureSentAtMs = 0;
static uint32_t publicKeySentAtMs = 0;
static uint32_t shellDoneAtMs = 0;
static portMUX_TYPE vendorRxMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t vendorRxRing[(A_MAXDATA + 24) * 2];
static size_t vendorRxHead = 0;
static size_t vendorRxTail = 0;
static size_t vendorRxCount = 0;
static volatile bool vendorRxOverflow = false;
static uint8_t rxBuffer[A_MAXDATA + 24];
static size_t rxLength = 0;
static char shellOutput[512];
static size_t shellOutputLength = 0;

static void putU32(uint8_t *p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

static uint32_t getU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t adbChecksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i++) sum += data[i];
    return sum;
}

static bool adbSend(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t *data = nullptr, uint32_t length = 0)
{
    uint8_t header[24];
    putU32(header + 0, command);
    putU32(header + 4, arg0);
    putU32(header + 8, arg1);
    putU32(header + 12, length);
    putU32(header + 16, adbChecksum(data, length));
    putU32(header + 20, command ^ 0xffffffff);
    if (!usb.vendorWrite(header, sizeof(header), deviceAddress)) return false;
    if (length == 0) return true;
    return usb.vendorWrite(data, length, deviceAddress);
}

static bool openAdbInterface(uint8_t address)
{
    EspUsbHostInterfaceInfo interfaces[ESP_USB_HOST_MAX_INTERFACES];
    const size_t count = usb.getInterfaces(address, interfaces, ESP_USB_HOST_MAX_INTERFACES);
    for (size_t i = 0; i < count; i++)
    {
        const EspUsbHostInterfaceInfo &itf = interfaces[i];
        if (itf.interfaceClass == ADB_CLASS && itf.interfaceSubClass == ADB_SUBCLASS && itf.interfaceProtocol == ADB_PROTOCOL)
        {
            logMsg("ADB interface matched (itf=" + String(itf.number) + ")");
            if (!usb.vendorOpen(address, itf.number)) return false;
            usb.vendorSetAutoZlp(true, address);
            return true;
        }
    }
    return false;
}

static void resetConnectionState()
{
    online = false; signatureSent = false; publicKeySent = false; publicKeyOffered = false;
    streamOpening = false; streamOpen = false; shellDone = false;
    remoteStreamId = 0; signatureSentAtMs = 0; publicKeySentAtMs = 0; rxLength = 0;
    portENTER_CRITICAL(&vendorRxMux);
    vendorRxHead = vendorRxTail = vendorRxCount = 0; vendorRxOverflow = false;
    portEXIT_CRITICAL(&vendorRxMux);
    shellOutputLength = 0; shellOutput[0] = 0;
}

static void bufferVendorData(const EspUsbHostVendorData &event)
{
    if (!adbOpen || event.address != deviceAddress) return;
    portENTER_CRITICAL(&vendorRxMux);
    for (size_t i = 0; i < event.length; i++)
    {
        if (vendorRxCount == sizeof(vendorRxRing)) { vendorRxOverflow = true; break; }
        vendorRxRing[vendorRxHead] = event.data[i];
        vendorRxHead = (vendorRxHead + 1) % sizeof(vendorRxRing);
        vendorRxCount++;
    }
    portEXIT_CRITICAL(&vendorRxMux);
}

static size_t drainVendorData(uint8_t *output, size_t capacity)
{
    size_t copied = 0;
    portENTER_CRITICAL(&vendorRxMux);
    while (copied < capacity && vendorRxCount > 0)
    {
        output[copied++] = vendorRxRing[vendorRxTail];
        vendorRxTail = (vendorRxTail + 1) % sizeof(vendorRxRing);
        vendorRxCount--;
    }
    portEXIT_CRITICAL(&vendorRxMux);
    return copied;
}

static void fail(const char *reason)
{
    if (!finished)
    {
        finished = true;
        setLedColor(40, 0, 0); // 红灯：错误
        logMsg(String("[FAIL] ") + reason);
    }
}

static bool sendConnect()
{
    static const char banner[] = "host::";
    logMsg("ADB send: CNXN");
    return adbSend(A_CNXN, A_VERSION, A_MAXDATA, reinterpret_cast<const uint8_t *>(banner), sizeof(banner));
}

static bool sendAuthSignature(const uint8_t *token, size_t length)
{
    uint8_t signature[256];
    if (!adbKey.signToken(token, length, signature, sizeof(signature))) return false;
    logMsg("ADB send: AUTH SIGNATURE");
    return adbSend(A_AUTH, ADB_AUTH_SIGNATURE, 0, signature, sizeof(signature));
}

static bool sendAuthPublicKey()
{
    uint8_t publicKey[768];
    size_t length = 0;
    if (!adbKey.publicKey(publicKey, sizeof(publicKey), length)) return false;
    logMsg("ADB send: AUTH RSAPUBLICKEY (Waiting for phone dialog...)");
    setLedColor(35, 25, 0); // 黄灯：等待手机点击允许调试
    publicKeySent = true;
    publicKeyOffered = true;
    publicKeySentAtMs = millis();
    return adbSend(A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, publicKey, length);
}

static bool openShell()
{
    logMsg("ADB send: OPEN shell...");
    setLedColor(35, 0, 35); // 紫灯：认证通过，正在执行 Shizuku
    streamOpening = true;
    return adbSend(A_OPEN, LOCAL_STREAM_ID, 0, reinterpret_cast<const uint8_t *>(SHELL_SERVICE), sizeof(SHELL_SERVICE));
}

static void appendShellOutput(const uint8_t *data, size_t length)
{
    const size_t available = sizeof(shellOutput) - 1 - shellOutputLength;
    const size_t copyLength = length < available ? length : available;
    if (copyLength > 0)
    {
        memcpy(shellOutput + shellOutputLength, data, copyLength);
        shellOutputLength += copyLength;
        shellOutput[shellOutputLength] = 0;
    }
}

static void handleMessage(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t *payload, size_t length)
{
    if (command == A_AUTH)
    {
        if (arg0 != ADB_AUTH_TOKEN || length != 20) { fail("invalid AUTH TOKEN"); return; }
        if (!signatureSent)
        {
            if (!sendAuthSignature(payload, length)) fail("AUTH SIGNATURE send failed");
            else { signatureSent = true; signatureSentAtMs = millis(); }
        }
        else if (!publicKeyOffered)
        {
            if (!sendAuthPublicKey()) fail("AUTH RSAPUBLICKEY send failed");
        }
        return;
    }

    if (command == A_CNXN)
    {
        online = true;
        logMsg("ADB connected! Running Shizuku...");
        if (!openShell()) fail("OPEN send failed");
        return;
    }

    if (command == A_OKAY && streamOpening && arg1 == LOCAL_STREAM_ID)
    {
        remoteStreamId = arg0;
        streamOpening = false;
        streamOpen = true;
        return;
    }

    if (command == A_WRTE && streamOpen && arg0 == remoteStreamId && arg1 == LOCAL_STREAM_ID)
    {
        appendShellOutput(payload, length);
        if (!adbSend(A_OKAY, LOCAL_STREAM_ID, remoteStreamId)) fail("OKAY send failed");
        return;
    }

    if (command == A_CLSE && (streamOpening || streamOpen) && (arg1 == LOCAL_STREAM_ID || arg1 == 0))
    {
        logMsg("Shell output:\n" + String(shellOutput));
        if (remoteStreamId != 0) adbSend(A_CLSE, LOCAL_STREAM_ID, remoteStreamId);
        streamOpening = false; streamOpen = false; remoteStreamId = 0;
        if (strstr(shellOutput, EXPECTED_OUTPUT))
        {
            shellDone = true;
            shellDoneAtMs = millis();
        }
        else
        {
            fail("Shizuku returned error or empty output");
        }
    }
}

static void processReceived()
{
    while (rxLength >= 24 && !finished)
    {
        const uint32_t command = getU32(rxBuffer + 0);
        const uint32_t arg0 = getU32(rxBuffer + 4);
        const uint32_t arg1 = getU32(rxBuffer + 8);
        const uint32_t dataLength = getU32(rxBuffer + 12);
        const uint32_t checksum = getU32(rxBuffer + 16);
        const uint32_t magic = getU32(rxBuffer + 20);
        if (magic != (command ^ 0xffffffff)) { fail("ADB header magic mismatch"); return; }
        if (dataLength > A_MAXDATA) { fail("ADB payload exceeds maxdata"); return; }
        if (rxLength < 24 + dataLength) return;
        if (adbChecksum(rxBuffer + 24, dataLength) != checksum) { fail("ADB checksum mismatch"); return; }

        handleMessage(command, arg0, arg1, rxBuffer + 24, dataLength);
        const size_t consumed = 24 + dataLength;
        memmove(rxBuffer, rxBuffer + consumed, rxLength - consumed);
        rxLength -= consumed;
    }
}

void handleRoot()
{
    server.send(200, "text/plain; charset=utf-8", webLog);
}

void setup()
{
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(200);

    setLedColor(0, 0, 35); // 蓝灯：待命

    WiFi.softAP(AP_SSID, AP_PASS);
    server.on("/", handleRoot);
    server.begin();
    logMsg("Wi-Fi AP ready: " + String(AP_SSID) + " -> http://192.168.4.1");

    if (!adbKey.begin())
    {
        fail("ADB key initialization failed");
        return;
    }

    usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device)
                          {
        logMsg("USB Connected: addr=" + String(device.address) + " vid=0x" + String(device.vid, HEX) + " pid=0x" + String(device.pid, HEX));
        if (!adbOpen && openAdbInterface(device.address))
        {
            deviceAddress = device.address;
            adbOpen = true;
            finished = false;
            resetConnectionState();
            setLedColor(0, 35, 35); // 青灯：已识别手机，开始 ADB 握手
            connectPending = true;
        } });

    usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device)
                             {
        logMsg("USB Disconnected: addr=" + String(device.address));
        if (device.address == deviceAddress)
        {
            adbOpen = false;
            connectPending = false;
            finished = false;
            deviceAddress = 0;
            resetConnectionState();
            setLedColor(0, 0, 35); // 手机拔出后恢复蓝灯待命
        } });

    usb.onVendorData([](const EspUsbHostVendorData &event)
                     { bufferVendorData(event); });

    if (!usb.begin())
    {
        fail("usb.begin() failed");
        return;
    }
    logMsg("Shizuku ESP32-S3-Zero Ready (No timeout).");
}

void loop()
{
    server.handleClient();

    // 短按 B (BOOT) 键软重启板子并清空日志
    if (digitalRead(BOOT_BTN_PIN) == LOW)
    {
        delay(50);
        if (digitalRead(BOOT_BTN_PIN) == LOW)
        {
            setLedColor(35, 35, 35); // 白灯闪一下提示重启
            delay(200);
            ESP.restart();
        }
    }

    if (!finished && adbOpen && connectPending)
    {
        connectPending = false;
        if (!sendConnect()) fail("CNXN send failed");
    }

    if (!finished && vendorRxOverflow) fail("ADB receive ring overflow");

    if (!finished && adbOpen && rxLength < sizeof(rxBuffer))
    {
        const size_t received = drainVendorData(rxBuffer + rxLength, sizeof(rxBuffer) - rxLength);
        if (received > 0)
        {
            rxLength += received;
            processReceived();
        }
    }

    if (!finished && adbOpen && signatureSent && !publicKeySent && !publicKeyOffered && !online && millis() - signatureSentAtMs >= 1500)
    {
        if (!sendAuthPublicKey()) fail("AUTH RSAPUBLICKEY fallback send failed");
    }

    if (!finished && adbOpen && publicKeyOffered && !online)
    {
        const uint32_t waitingSince = publicKeySent ? publicKeySentAtMs : signatureSentAtMs;
        if (waitingSince != 0 && millis() - waitingSince >= AUTHORIZATION_GRACE_MS)
        {
            fail("USB debugging authorization timeout");
        }
    }

    if (!finished && shellDone && adbOpen && millis() - shellDoneAtMs >= STABILITY_MS)
    {
        finished = true;
        setLedColor(0, 50, 0); // 绿灯：Shizuku 激活成功
        logMsg("[PASS] Shizuku started!");
    }

    delay(10);
}
