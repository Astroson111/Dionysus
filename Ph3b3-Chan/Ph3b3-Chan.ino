/*
 * stackchan_rung4.ino
 * Ph3b3 / Stack-Chan — Rung 4: Talk / Ph3b3 voice round-trip
 *
 * FQBN: m5stack:esp32:m5stack_cores3
 *
 * NEW in Rung 4 vs Rung 3:
 *   - WiFi connection managed in main sketch (supervisor tick in loop)
 *   - TalkApp (index 1) fully implemented: mic → /transcribe → /chat → speaker
 *   - ISRG Root X1 + X2 cert bundle for Let's Encrypt TLS
 *
 * WiFi setup:
 *   THREE saved networks (slots 0..2, NVS "sc": ssid0..2 / pass0..2). Settings ->
 *   WiFi opens the slot list; tapping a row runs the on-screen SSID + password
 *   keyboard for that slot, long-press clears it. Connect walks the filled slots
 *   0 -> 2 in order, first success wins — so the network you want preferred goes
 *   in the top slot and the away-from-home one goes at the bottom.
 *   The Dio-Setup phone portal is still reachable (slot list footer, and raised
 *   automatically when there are no creds / every slot failed at boot) — it is
 *   also the only place that edits the Ph3b3 host / port / device key.
 *   SC_WIFI_SSID / SC_WIFI_PASS may be baked as a first-boot seed for slot 0 but
 *   are normally blank — provisioning is done on-device, not hardcoded.
 *
 * Mode switching: tap the crescent tab (top-left corner) to open the mode overlay.
 *
 * SD card layout (microSD, CS=GPIO4):
 *   /karaoke/track.wav  /karaoke/track.lrc  /ghost/ev_*.log
 */

// ── First-boot WiFi (blank after NVS is seeded) ───────────────────────────────
#define SC_WIFI_SSID  ""
#define SC_WIFI_PASS  ""

// ── Ph3b3 auth — credentials live in gitignored secrets.h ────────────────────
#include "secrets.h"

#define SC_FACE_BGR   // CoreS3 ILI9342 is BGR; swap R↔B so violet renders violet, not cyan

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>

#include "ph3b3_face.h"
#include "AppBase.h"
#include "AppManager.h"
#include "CrescentMenu.h"
#include "SettingsStore.h"
#include "TouchKeyboard.h"
#include "TalkApp.h"
#include "NetworkApp.h"
#include "KaraokeApp.h"
#include "GhostApp.h"
#include "SettingsApp.h"
#include "CamServer.h"

// ── TLS cert bundle ───────────────────────────────────────────────────────────
// ISRG Root X1 (RSA) + Root X2 (ECDSA) concatenated.
// Let's Encrypt switched to YE2 → Root X2 chain on 2026-06-19; keep both.
const char ISRG_ROOT_X1[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)EOF";

// ── WiFi / NVS ────────────────────────────────────────────────────────────────
// Three saved networks. An empty SSID means the slot is unused and is SKIPPED —
// slot order is preference order, not a packed list, so slot 2 can be the only
// one filled. Key names are frozen: renaming them would orphan the network
// already saved on every shipped device.
static Preferences sPrefs;
static const char  NVS_NS[]      = "sc";
static const char* SSID_KEYS[]   = {"ssid0","ssid1","ssid2"};
static const char* PASS_KEYS[]   = {"pass0","pass1","pass2"};
static const int   SC_MAX_NETS   = 3;
static const uint32_t SC_SLOT_JOIN_MS = 8000;   // association window per slot before we advance

static bool     sWifiConnected = false;
static bool     sSyncDone      = false;
static uint32_t sConnectedAt   = 0;
static bool     sHealthOnline  = false;   // last /health probe reached the server (2xx)
static uint32_t sLastHealth    = 0;       // millis() of the last /health probe
static uint32_t sLastArgusHb   = 0;       // millis() of the last Argus heartbeat POST

// ── Argus fleet heartbeat ─────────────────────────────────────────────────────
// Dio POSTs a tiny status JSON to Ph3b3's /argus/heartbeat every 60s, on the same
// verified check-in path as her other calls (Basic auth + X-Ph3b3-Device:
// stackchan). ARGUS_FW_HASH identifies this build for the panel's drift check.
#define ARGUS_HEARTBEAT_MS  60000UL         // 60 s between heartbeats
#define ARGUS_FW_HASH       "dio-wifi3b"      // 3 saved networks + ladder; keyboard handoff fixed

// ── Server target — ATOMIC NVS record (host+port+user+pass as ONE unit) ───────
// Compile-time SC_PH3B3_* are the first-boot SEED only; runtime always reads NVS.
// Host and port are never stored or edited separately (Iris split-config lesson).
String gSrvHost; int gSrvPort = 0; String gSrvUser; String gSrvPass;
static const char SRV_HOST_KEY[] = "srv_host";
static const char SRV_PORT_KEY[] = "srv_port";
static const char SRV_USER_KEY[] = "srv_user";
static const char SRV_PASS_KEY[] = "srv_pass";

static void _saveServer(const String& h, int p, const String& u, const String& pw) {
    // One transaction — all four fields together, never a partial record.
    sPrefs.begin(NVS_NS, false);
    sPrefs.putString(SRV_HOST_KEY, h);
    sPrefs.putInt   (SRV_PORT_KEY, p);
    sPrefs.putString(SRV_USER_KEY, u);
    sPrefs.putString(SRV_PASS_KEY, pw);
    sPrefs.end();
    gSrvHost = h; gSrvPort = p; gSrvUser = u; gSrvPass = pw;
}

static void _loadServer() {
    sPrefs.begin(NVS_NS, true);
    bool seeded = sPrefs.isKey(SRV_HOST_KEY);
    gSrvHost = sPrefs.getString(SRV_HOST_KEY, SC_PH3B3_HOST);
    gSrvPort = sPrefs.getInt   (SRV_PORT_KEY, SC_PH3B3_PORT);
    gSrvUser = sPrefs.getString(SRV_USER_KEY, SC_PH3B3_USER);
    gSrvPass = sPrefs.getString(SRV_PASS_KEY, SC_PH3B3_PASS);
    sPrefs.end();
    if (!seeded) _saveServer(SC_PH3B3_HOST, SC_PH3B3_PORT, SC_PH3B3_USER, SC_PH3B3_PASS);
    Serial.printf("[srv] target %s:%d\n", gSrvHost.c_str(), gSrvPort);
}

// ── Device settings (Volume / Mic / LED presets) — NVS "sc", index-persisted ──
int   gVolIdx = SET_VOL_DEFAULT, gMicIdx = SET_MIC_DEFAULT, gLedIdx = SET_LED_DEFAULT;
int   gLedColorIdx      = SET_LEDC_DEFAULT;
int   gSpeakerVolume    = SET_VOL_LEVELS[SET_VOL_DEFAULT];
int   gMicMagnification = SET_MIC_LEVELS[SET_MIC_DEFAULT];
float gLedBrightness    = SET_LED_LEVELS[SET_LED_DEFAULT];

// Last head-pat time — stamped in _cueLeds() (LED path, already reads pet), read
// by TalkApp's dead-air exit so petting counts as activity. Timestamp only; it
// never gates the render path (per the sputter-throttle lesson).
volatile uint32_t gLastPetMs = 0;

// Camera (Phase 2). g_faceOwnedByCamera pauses face.update() during a capture —
// wraps the render, never touches it (render-starvation lesson).
volatile bool g_faceOwnedByCamera = false;
CamServer camServer;

// Bridge for TalkApp's native photo loop: TalkApp.h is included before CamServer.h
// (can't see the type), so the vision-capture trigger goes through this free
// function. Declared `extern bool camVisionCapture();` in TalkApp.h.
bool camVisionCapture() { return camServer.captureDisplayPush(); }

static void _settingsApply() {
    gSpeakerVolume    = SET_VOL_LEVELS[gVolIdx];
    gMicMagnification = SET_MIC_LEVELS[gMicIdx];
    gLedBrightness    = SET_LED_LEVELS[gLedIdx];
}

void settingsLoad() {
    sPrefs.begin(NVS_NS, true);
    gVolIdx      = constrain((int)sPrefs.getInt("vol",  SET_VOL_DEFAULT),  0, 2);
    gMicIdx      = constrain((int)sPrefs.getInt("mic",  SET_MIC_DEFAULT),  0, 2);
    gLedIdx      = constrain((int)sPrefs.getInt("led",  SET_LED_DEFAULT),  0, 2);
    gLedColorIdx = constrain((int)sPrefs.getInt("ledc", SET_LEDC_DEFAULT), 0, SET_LEDC_N - 1);
    sPrefs.end();
    _settingsApply();
    Serial.printf("[settings] vol=%s mic=%s led=%s color=%s\n",
                  SET_VOL_NAMES[gVolIdx], SET_MIC_NAMES[gMicIdx], SET_LED_NAMES[gLedIdx],
                  SET_LEDC_NAMES[gLedColorIdx]);
}

void settingsSetVol(int idx) {
    gVolIdx = constrain(idx, 0, 2);
    sPrefs.begin(NVS_NS, false); sPrefs.putInt("vol", gVolIdx); sPrefs.end();
    _settingsApply();
    M5.Speaker.setVolume(gSpeakerVolume);   // take effect immediately for the next play
}
void settingsSetMic(int idx) {
    gMicIdx = constrain(idx, 0, 2);
    sPrefs.begin(NVS_NS, false); sPrefs.putInt("mic", gMicIdx); sPrefs.end();
    _settingsApply();   // applied on the next M5.Mic.begin() (capture path reads gMicMagnification)
}
void settingsSetLed(int idx) {
    gLedIdx = constrain(idx, 0, 2);
    sPrefs.begin(NVS_NS, false); sPrefs.putInt("led", gLedIdx); sPrefs.end();
    _settingsApply();
}
void settingsSetLedColor(int idx) {
    gLedColorIdx = constrain(idx, 0, SET_LEDC_N - 1);
    sPrefs.begin(NVS_NS, false); sPrefs.putInt("ledc", gLedColorIdx); sPrefs.end();
}

// GET /health against the runtime record. Returns the HTTP code, or a negative
// value on a connection-level failure (no route / TLS / timeout). 2xx = online.
static int _healthCheck() {
    WiFiClientSecure tls;
    tls.setInsecure();               // LAN mkcert self-signed cert
    tls.setTimeout(8000);
    HTTPClient http;
    if (!http.begin(tls, gSrvHost.c_str(), gSrvPort, "/health", true)) return -1000;
    http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
    http.addHeader("X-Ph3b3-Device", "stackchan");
    http.setTimeout(8000);
    int code = http.GET();
    http.end();
    return code;                     // 2xx online; 401/403 denied; <0 no route
}

// POST a tiny status JSON to /argus/heartbeat (fleet observability). Rides the
// verified check-in path (Basic auth + X-Ph3b3-Device: stackchan — the server
// IP-pins Dio via dio_host). Fire-and-forget: any failure is ignored so a flaky
// network never stalls the face. Payload stays well under 200 B.
static void _sendArgusHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(6000);
    HTTPClient http;
    if (!http.begin(tls, gSrvHost.c_str(), gSrvPort, "/argus/heartbeat", true)) return;
    http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
    http.addHeader("X-Ph3b3-Device", "stackchan");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(6000);
    // Battery + charging (M5Unified PMIC). Unavailable → JSON null, never a guess.
    int32_t _lvl = M5.Power.getBatteryLevel();
    char batStr[8];
    if (_lvl < 0 || _lvl > 100) strcpy(batStr, "null");
    else snprintf(batStr, sizeof(batStr), "%ld", (long)_lvl);
    int _chg = (int)M5.Power.isCharging();          // 0=discharging 1=charging 2=unknown
    const char* chgStr = (_chg == 1) ? "true" : (_chg == 0) ? "false" : "null";
    char body[224];
    int n = snprintf(body, sizeof(body),
        "{\"battery\":%s,\"charging\":%s,\"rssi\":%d,\"uptime\":%lu,\"firmware_hash\":\"%s\",\"free_heap\":%u}",
        batStr, chgStr, (int)WiFi.RSSI(),
        (unsigned long)(millis() / 1000UL), ARGUS_FW_HASH, (unsigned)ESP.getFreeHeap());
    http.POST((uint8_t*)body, n);
    http.end();
}

// Probe /health and set the on-screen status word. One quick retry so a slow
// first TLS handshake (marginal association right after WiFi joins) doesn't
// latch a false "no route". Updates sHealthOnline; returns the word.
static String _refreshHealthStatus() {
    int hc = _healthCheck();
    if (hc < 0) { delay(400); hc = _healthCheck(); }
    String st = (hc >= 200 && hc < 300) ? "online"
              : (hc == 401 || hc == 403) ? "denied"
              : (hc < 0)                  ? "no route"
              :                             "away";
    sHealthOnline = (st == "online");
    face.setStatusLine(st);
    Serial.printf("[srv] health=%d -> %s\n", hc, st.c_str());
    return st;
}

static int _loadCreds(String ssids[], String passes[]) {
    sPrefs.begin(NVS_NS, true);
    bool hasAny = false;
    for (int i = 0; i < SC_MAX_NETS; i++) {
        ssids[i]  = sPrefs.getString(SSID_KEYS[i], "");
        passes[i] = sPrefs.getString(PASS_KEYS[i], "");
        if (ssids[i].length()) hasAny = true;
    }
    sPrefs.end();

    if (!hasAny && strlen(SC_WIFI_SSID) > 0) {
        ssids[0]  = SC_WIFI_SSID;
        passes[0] = SC_WIFI_PASS;
        return 1;
    }
    int n = 0;
    for (int i = 0; i < SC_MAX_NETS; i++) if (ssids[i].length()) n = i + 1;
    return n;
}

// Pre-slot builds stored ONE network under un-indexed keys. Lift it into slot 0
// (never overwriting a slot-0 entry) and drop the old keys, so upgrading to the
// multi-slot build doesn't make anyone re-type their home network. No-op — and
// costs one read-only NVS open — once it has run or on a device that never had
// the old layout. Call before the first _loadCreds() of the boot.
static void _migrateLegacyCreds() {
    sPrefs.begin(NVS_NS, true);
    String oldSsid = sPrefs.getString("ssid", "");
    String oldPass = sPrefs.getString("pass", "");
    String slot0   = sPrefs.getString(SSID_KEYS[0], "");
    sPrefs.end();
    if (oldSsid.length() == 0) return;

    sPrefs.begin(NVS_NS, false);
    if (slot0.length() == 0) {
        sPrefs.putString(SSID_KEYS[0], oldSsid);
        sPrefs.putString(PASS_KEYS[0], oldPass);
        Serial.printf("[wifi] migrated legacy creds -> slot 0: %s\n", oldSsid.c_str());
    } else {
        Serial.println("[wifi] legacy creds found but slot 0 is taken — dropping the old keys");
    }
    sPrefs.remove("ssid");
    sPrefs.remove("pass");
    sPrefs.end();
}

// Write / clear one slot. Both open NVS read-write for a single transaction;
// neither touches the other slots or the server record.
static void _saveSlot(int i, const String& ssid, const String& pass) {
    if (i < 0 || i >= SC_MAX_NETS) return;
    sPrefs.begin(NVS_NS, false);
    sPrefs.putString(SSID_KEYS[i], ssid);
    sPrefs.putString(PASS_KEYS[i], pass);
    sPrefs.end();
    Serial.printf("[wifi] slot %d saved: %s\n", i, ssid.c_str());
}

static void _clearSlot(int i) {
    if (i < 0 || i >= SC_MAX_NETS) return;
    sPrefs.begin(NVS_NS, false);
    sPrefs.remove(SSID_KEYS[i]);
    sPrefs.remove(PASS_KEYS[i]);
    sPrefs.end();
    Serial.printf("[wifi] slot %d cleared\n", i);
}

// Next filled slot at or after `from`, or -1 if the pass is out of candidates.
static int _nextFilledSlot(String ssids[], int from) {
    for (int i = (from < 0 ? 0 : from); i < SC_MAX_NETS; i++)
        if (ssids[i].length()) return i;
    return -1;
}

// The SSID we are actually associated with (empty when down). Read from the
// radio, not from a cached "we asked for this one" — with three candidates,
// "connected" alone doesn't tell you which network you're on.
String wifiConnectedSsid() {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String();
}

// (Server-side network sync removed 2026-07-16 — Dio provisions WiFi on-device
//  via the Dio-Setup portal; the /stackchan/networks endpoint no longer exists.)

// Robust WiFi join (Iris pattern): full teardown + PMF-capable connect. Bare
// WiFi.begin() doesn't free the WPA supplicant buffers on reason 26/32 and can't
// negotiate PMF-required APs — the likely cause of Dio's flaky association.
static void _wifiJoin(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_OFF);
    delay(400);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    wifi_config_t conf = {};
    strlcpy((char*)conf.sta.ssid,     ssid, sizeof(conf.sta.ssid));
    strlcpy((char*)conf.sta.password, pass, sizeof(conf.sta.password));
    conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    conf.sta.pmf_cfg.capable    = true;
    conf.sta.pmf_cfg.required   = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
    esp_wifi_connect();
}

// ── The ladder ────────────────────────────────────────────────────────────────
// Filled slots, 0 -> 2, first success wins; each slot gets SC_SLOT_JOIN_MS to
// associate before we advance, so a wrong key in slot 0 costs 8s, not the link.
// A pass that reaches the end rests (backoff, growing to 60s) and then starts
// OVER at slot 0 — we never sit re-dialling one SSID, and a mid-session drop
// re-runs the whole ladder instead of blind-reconnecting to the one that fell
// over (the AP that just vanished is the least likely to answer next).
// Non-blocking: one step per supervisor tick, so the face keeps rendering.
static int      sLadderSlot  = -1;   // slot currently being attempted; -1 = between passes
static uint32_t sLadderStart = 0;    // millis() the current attempt began
static int      sLadderPass  = 0;    // consecutive full passes that found nothing
static uint32_t sLadderRest  = 0;    // millis() the current rest period ends

static void _ladderReset() {
    sLadderSlot = -1; sLadderStart = 0; sLadderPass = 0; sLadderRest = 0;
}

// A definitive "this one isn't happening" — wrong key, or the SSID isn't in the
// air — so we can advance without waiting out the window. ADVISORY ONLY: the
// ESP32 driver doesn't reliably latch either status, and a stale one can survive
// the mode cycle, so callers must ignore it until the attempt has had a moment
// to settle and keep SC_SLOT_JOIN_MS as the real backstop.
static const uint32_t SC_JOIN_SETTLE_MS = 1500;
static bool _joinRejected(uint32_t elapsed) {
    if (elapsed < SC_JOIN_SETTLE_MS) return false;
    wl_status_t s = WiFi.status();
    return s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL;
}

static void _ladderTick() {
    uint32_t now = millis();
    // Inside a slot's association window → let it play out, unless the driver
    // has already told us this slot is a dead end.
    if (sLadderSlot >= 0) {
        uint32_t elapsed = now - sLadderStart;
        if (elapsed < SC_SLOT_JOIN_MS && !_joinRejected(elapsed)) return;
    }
    // Resting between passes → don't touch NVS or the radio.
    if (sLadderSlot < 0 && sLadderRest && (int32_t)(now - sLadderRest) < 0) return;

    String ssids[SC_MAX_NETS], passes[SC_MAX_NETS];
    if (_loadCreds(ssids, passes) == 0) {
        Serial.println("[wifi] no saved networks — Settings > WiFi to add one");
        sLadderSlot = -1;
        sLadderRest = now + 30000;   // nothing to climb; look again in 30s
        return;
    }

    int next = _nextFilledSlot(ssids, sLadderSlot + 1);
    if (next < 0) {                  // pass exhausted → back off, then restart at slot 0
        sLadderPass++;
        sLadderSlot = -1;
        uint32_t wait = 10000UL * (uint32_t)sLadderPass;
        if (wait > 60000UL) wait = 60000UL;
        sLadderRest = now + wait;
        Serial.printf("[wifi] ladder pass %d found nothing — resting %us\n",
                      sLadderPass, (unsigned)(wait / 1000));
        return;
    }

    // The WPA supplicant needs room to allocate; cycling the radio is the cheap
    // recovery. Restart the ladder from the top once the heap comes back.
    if (ESP.getMaxAllocHeap() < 80000) {
        Serial.println("[wifi] heap too low to associate — cycling radio");
        WiFi.mode(WIFI_OFF); delay(100); WiFi.mode(WIFI_STA);
        sLadderSlot = -1;
        sLadderRest = now + 5000;
        return;
    }

    sLadderSlot  = next;
    sLadderStart = now;
    Serial.printf("[wifi] ladder slot %d: %s\n", next, ssids[next].c_str());
    _wifiJoin(ssids[next].c_str(), passes[next].c_str());
}

// Violet palette helper, BGR-aware like the keyboard (styling stays consistent).
static uint16_t _pcol(uint8_t r, uint8_t g, uint8_t b) {
    auto& d = M5StackChan.Display();
#ifdef SC_FACE_BGR
    return d.color565(b, g, r);
#else
    return d.color565(r, g, b);
#endif
}


static void _runPortal() {
    Serial.println("[wifi] portal — Dio-Setup 192.168.4.1");
    face.setState(Ph3b3Face::CONNECTING);

    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Dio-Setup");
    delay(200);

    DNSServer dns;
    dns.start(53, "*", IPAddress(192, 168, 4, 1));

    WebServer server(80);

    server.on("/", HTTP_GET, [&]() {
        server.send(200, "text/html",
            "<!DOCTYPE html><html><head>"
            "<meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Dio Setup</title>"
            "<style>"
            "body{font-family:sans-serif;background:#0a0318;color:#e0d4ff;"
                 "max-width:480px;margin:0 auto;padding:1rem}"
            "h1{color:#a855f7}"
            "p{color:#a09ab8;font-size:.85rem}"
            "label{font-size:.8rem;color:#a09ab8;display:block;margin-top:.8rem}"
            "input{width:100%;box-sizing:border-box;background:#0d0525;"
                  "border:1px solid #4b2c8a;border-radius:4px;color:#e0d4ff;"
                  "padding:.4rem .5rem;font-size:.95rem;margin-top:2px}"
            "button{width:100%;margin-top:1.2rem;padding:.75rem;background:#7e22ce;"
                   "border:none;border-radius:8px;color:#fff;font-size:1rem;"
                   "font-weight:700;cursor:pointer}"
            "button:hover{background:#9333ea}"
            "</style></head><body>"
            "<h1>Dio Setup</h1>"
            "<p>Enter your WiFi credentials. "
            "Additional networks sync from Ph3b3 after first connect.</p>"
            "<form method='POST' action='/save'>"
            "<label>WiFi Name (SSID)</label>"
            "<input name='ssid' autocomplete='off' autofocus required>"
            "<label>WiFi Password (network key)</label>"
            "<input name='pass' type='password' required minlength='8'>"
            "<label>Ph3b3 host</label>"
            "<input name='host' value='" + gSrvHost + "'>"
            "<label>Ph3b3 port</label>"
            "<input name='port' value='" + String(gSrvPort) + "'>"
            "<label>Ph3b3 device key</label>"
            "<input name='svrkey' type='password' placeholder='(blank keeps saved key)'>"
            "<button type='submit'>Save &amp; Reboot</button>"
            "</form>"
            "<div style='position:fixed;bottom:12px;right:12px;width:110px;opacity:0.75'>"
            "<svg width='100%' viewBox='0 0 680 460' role='img' xmlns='http://www.w3.org/2000/svg'>"
            "<title>Peach, a golden retriever, sleeping in a little bed</title>"
            "<desc>A line-art golden retriever curled up asleep in a small cushioned bed, "
            "with a crescent moon and small stars above, drawn in a soft monochrome style "
            "to match the Ph3b3 crescent art.</desc>"
            "<style>"
            ".ink{fill:none;stroke:#2b2b2b;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"
            ".ink-soft{fill:none;stroke:#6b6b6b;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}"
            ".fur{fill:#e8a85c;opacity:0.22}"
            ".bed{fill:#6b6b6b;opacity:0.10}"
            ".cap{fill:#6b6b6b;font-family:Georgia,serif;font-size:15px;font-style:italic}"
            "</style>"
            "<circle cx='500' cy='95' r='34' fill='none' stroke='#6b6b6b' stroke-width='1.5'/>"
            "<circle cx='512' cy='88' r='34' fill='#ffffff' stroke='none'/>"
            "<circle cx='512' cy='88' r='34' fill='#6b6b6b' opacity='0.10'/>"
            "<path d='M180 150 l3 7 l7 1 l-5 5 l1 7 l-6 -3 l-6 3 l1 -7 l-5 -5 l7 -1 z' fill='#9b9b9b'/>"
            "<path d='M250 110 l2 5 l5 1 l-4 3 l1 5 l-4 -2 l-4 2 l1 -5 l-4 -3 l5 -1 z' fill='#9b9b9b'/>"
            "<path d='M560 175 l2 5 l5 1 l-4 3 l1 5 l-4 -2 l-4 2 l1 -5 l-4 -3 l5 -1 z' fill='#9b9b9b'/>"
            "<ellipse cx='340' cy='370' rx='210' ry='50' class='bed'/>"
            "<path class='ink-soft' d='M140 360 q-8 -34 30 -42 q170 -22 340 0 q38 8 30 42 q-6 30 -50 36 q-150 18 -300 0 q-44 -6 -50 -36 z'/>"
            "<path class='ink-soft' d='M150 358 q180 24 380 0'/>"
            "<path class='fur' d='M250 290 q-70 10 -78 56 q-6 40 60 50 q120 16 230 -4 q70 -14 56 -64 q-14 -44 -90 -48 q-24 24 -64 22 q-46 -2 -54 -28 q-8 6 -16 8 z'/>"
            "<path class='ink' d='M250 292 q-66 12 -74 54 q-6 38 58 48 q116 16 224 -4 q66 -14 54 -60 q-14 -42 -86 -46'/>"
            "<path class='ink' d='M250 292 q12 -22 44 -22 q34 0 42 26'/>"
            "<path class='ink' d='M336 296 q12 18 52 16 q44 -2 56 -24'/>"
            "<path class='ink' d='M444 288 q42 -36 72 -20 q22 12 6 40 q-12 22 -42 28'/>"
            "<path class='ink-soft' d='M470 282 q24 -14 40 -4'/>"
            "<path class='ink' d='M250 292 q-30 -16 -34 -44 q-2 -20 16 -26 q20 -6 30 14'/>"
            "<path class='ink' d='M232 222 q-14 -4 -20 8 q-6 14 8 24'/>"
            "<circle cx='244' cy='258' r='2.5' fill='#2b2b2b'/>"
            "<path class='ink-soft' d='M236 256 q8 -5 16 0'/>"
            "<path class='ink' d='M258 272 q8 6 18 2'/>"
            "<path class='ink' d='M266 268 q2 6 -2 10'/>"
            "<circle cx='270' cy='276' r='3' fill='#2b2b2b'/>"
            "<path class='ink-soft' d='M300 330 q40 10 90 4'/>"
            "<path class='ink-soft' d='M330 360 q30 6 70 0'/>"
            "<path class='ink-soft' d='M540 250 q10 -10 0 -20 q-10 -10 0 -20' opacity='0.7'/>"
            "<path class='ink-soft' d='M556 236 q8 -8 0 -16 q-8 -8 0 -16' opacity='0.5'/>"
            "<text x='340' y='432' text-anchor='middle' class='cap'>Peach &mdash; rest easy, good girl</text>"
            "</svg></div>"
            "</body></html>"
        );
    });

    server.onNotFound([&]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });

    server.on("/save", HTTP_POST, [&]() {
        String ssid = server.arg("ssid"); ssid.trim();
        String pass = server.arg("pass"); pass.trim();
        String host = server.arg("host"); host.trim();
        int    port = server.arg("port").toInt();
        // Require BOTH SSID and key. A blank password saves an unjoinable network
        // and reboots into it — the lockout. This mirrors the form's `required`,
        // for any client that bypasses browser validation.
        if (ssid.length() == 0 || pass.length() == 0) {
            server.send(400, "text/plain",
                ssid.length() == 0 ? "WiFi name (SSID) required"
                                   : "WiFi password (network key) required");
            return;
        }
        sPrefs.begin(NVS_NS, false);
        sPrefs.putString(SSID_KEYS[0], ssid);
        sPrefs.putString(PASS_KEYS[0], pass);
        sPrefs.end();
        // Per-device Ph3b3 key — blank keeps the saved key (parallel to Wi-Fi pass).
        // NOTE: the setup AP is open, so this key (like the Wi-Fi password) crosses
        // it in the clear during the brief setup window — same exposure, local only.
        String svrkey = server.arg("svrkey"); svrkey.trim();
        String newPass = svrkey.length() ? svrkey : gSrvPass;
        // Server target stays ATOMIC: overwrite when host+port are present; a
        // key-only change still persists (reusing the saved host/port).
        if (host.length() > 0 && port > 0)      _saveServer(host, port, gSrvUser, newPass);
        else if (svrkey.length())               _saveServer(gSrvHost, gSrvPort, gSrvUser, newPass);
        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;background:#0a0318;"
            "color:#e0d4ff;text-align:center;padding:2rem'>"
            "<h2 style='color:#a855f7'>Saved!</h2>"
            "<p>Rebooting Dio to join <b>" + ssid + "</b>...</p>"
            "</body></html>"
        );
        delay(1000);
        ESP.restart();
    });

    server.begin();

    // Static setup screen so this mode is OBVIOUS instead of looking like a hang.
    // (The CONNECTING face's "connecting" label read as "stuck connecting".)
    // Drawn once; the loop below no longer calls face.update(), so it persists.
    {
        auto& d = M5StackChan.Display();
        int cx = d.width() / 2;
        d.fillScreen(_pcol(8, 6, 20));
        d.setTextDatum(top_center);
        d.setTextSize(2); d.setTextColor(_pcol(200, 130, 255), _pcol(8, 6, 20));
        d.drawString("WiFi Setup", cx, 14);
        d.setTextSize(1); d.setTextColor(_pcol(215, 200, 245), _pcol(8, 6, 20));
        d.drawString("On your phone, join WiFi:", cx, 56);
        d.setTextSize(2); d.setTextColor(_pcol(150, 240, 170), _pcol(8, 6, 20));
        d.drawString("Dio-Setup", cx, 80);
        d.setTextSize(1); d.setTextColor(_pcol(215, 200, 245), _pcol(8, 6, 20));
        d.drawString("then open in a browser:", cx, 124);
        d.setTextSize(2); d.setTextColor(_pcol(150, 210, 255), _pcol(8, 6, 20));
        d.drawString("192.168.4.1", cx, 148);
        d.setTextSize(1); d.setTextColor(_pcol(150, 140, 190), _pcol(8, 6, 20));
        d.drawString("Enter your WiFi name + key", cx, 198);
    }

    while (true) {
        M5StackChan.update();      // servos/touch stay alive
        dns.processNextRequest();
        server.handleClient();
        delay(5);                  // NO face.update() — keep the static setup screen up
    }
}

// Phone captive portal on demand (slot-list footer). _runPortal() never returns —
// it serves the setup form until /save → ESP.restart(). Still the only surface
// that edits the Ph3b3 host / port / device key.
void launchWifiPortal() { _runPortal(); }

// ── Settings → WiFi: the slot list ───────────────────────────────────────────
// Three rows, each an SSID or "- empty -". Tap a row → the same two-screen entry
// we already had (SSID keyboard → password keyboard) aimed at that slot. Hold a
// row ~1s → clear it, with the row filling red as you hold so a stray thumb
// isn't enough. Right-to-left swipe leaves. No new input surfaces: the keyboard
// is tkPrompt, unchanged, just invoked per slot.
// Blocking screen with its own touch loop (like _runPortal) — it does NOT run
// through the face render loop, per the render-starvation lesson.
static const int      WS_ROW_Y0  = 48;
static const int      WS_ROW_H   = 38;
static const int      WS_FOOT_Y  = 172;
static const int      WS_FOOT_H  = 30;
static const uint32_t WS_HOLD_MS = 900;    // hold-to-clear duration
static const int      WS_SWIPE   = 70;     // right-to-left exit threshold

// Centred status card. line2 optional (pass "" to omit).
static void _wsMsg(const char* line1, const String& line2, uint16_t accent, uint32_t holdMs) {
    auto& d = M5StackChan.Display();
    d.fillScreen(_pcol(8, 6, 20));
    d.setTextDatum(middle_center);
    d.setTextSize(2); d.setTextColor(accent, _pcol(8, 6, 20));
    d.drawString(line1, d.width() / 2, d.height() / 2 - 14);
    if (line2.length()) {
        d.setTextSize(1); d.setTextColor(_pcol(200, 185, 235), _pcol(8, 6, 20));
        d.drawString(line2.c_str(), d.width() / 2, d.height() / 2 + 14);
    }
    if (holdMs) {
        uint32_t t0 = millis();
        while (millis() - t0 < holdMs) { M5StackChan.update(); delay(10); }
    }
}

// Wait out a touch carried over from another screen — same hazard tkPrompt
// guards against. The keyboard's "done" key overlaps this list's footer button,
// so without this, finishing a password could fall straight through into the
// phone portal.
static void _wsDrainTouch() {
    auto& d = M5StackChan.Display();
    int16_t x = 0, y = 0;
    while (d.getTouch(&x, &y)) { M5StackChan.update(); delay(8); }
    delay(60);
}

// SSIDs can be 32 chars; the row is not. Trim with a visible marker so a
// truncated name never reads as a different (wrong) network.
static String _wsFit(const String& s, int maxChars) {
    if ((int)s.length() <= maxChars) return s;
    return s.substring(0, maxChars - 2) + "..";
}

static void _wsDrawRow(int i, const String& ssid, bool active, float holdFrac) {
    auto& d = M5StackChan.Display();
    const int y = WS_ROW_Y0 + i * WS_ROW_H, w = d.width();
    const int x = 8, rw = w - 16, rh = WS_ROW_H - 6;
    const uint16_t fill = _pcol(18, 10, 44);

    d.fillRoundRect(x, y, rw, rh, 6, fill);
    if (holdFrac > 0.0f) {                       // hold-to-clear progress
        float f = holdFrac > 1.0f ? 1.0f : holdFrac;
        int   fw = (int)((rw - 4) * f);
        if (fw > 0) d.fillRect(x + 2, y + 2, fw, rh - 4, _pcol(110, 25, 45));
    }
    d.drawRoundRect(x, y, rw, rh, 6, active ? _pcol(120, 220, 150) : _pcol(70, 40, 150));

    d.setTextSize(1);
    d.setTextDatum(middle_left);
    d.setTextColor(_pcol(120, 110, 160), fill);
    d.drawString(String(i + 1).c_str(), x + 12, y + rh / 2);   // slot number = preference order
    if (ssid.length()) {
        d.setTextColor(_pcol(220, 205, 255), fill);
        d.drawString(_wsFit(ssid, 26).c_str(), x + 30, y + rh / 2);
    } else {
        d.setTextColor(_pcol(105, 95, 140), fill);
        d.drawString("- empty -", x + 30, y + rh / 2);
    }
    if (active) {                                 // the one we're actually on
        d.setTextDatum(middle_right);
        d.setTextColor(_pcol(120, 220, 150), fill);
        d.drawString("on", x + rw - 12, y + rh / 2);
    }
}

static void _wsDrawList(String ssids[], const String& conn, int holdRow, float holdFrac) {
    auto& d = M5StackChan.Display();
    const int w = d.width();
    d.fillScreen(_pcol(8, 6, 20));

    d.setTextDatum(top_center);
    d.setTextSize(2); d.setTextColor(_pcol(200, 130, 255), _pcol(8, 6, 20));
    d.drawString("WiFi", w / 2, 8);
    d.setTextSize(1);
    if (conn.length()) {
        d.setTextColor(_pcol(120, 220, 150), _pcol(8, 6, 20));
        d.drawString(("on " + _wsFit(conn, 30)).c_str(), w / 2, 30);
    } else {
        d.setTextColor(_pcol(190, 130, 130), _pcol(8, 6, 20));
        d.drawString("not connected", w / 2, 30);
    }

    for (int i = 0; i < SC_MAX_NETS; i++)
        _wsDrawRow(i, ssids[i], conn.length() && ssids[i] == conn,
                   i == holdRow ? holdFrac : 0.0f);

    // Footer: the phone portal is still the only way to edit the server record.
    d.fillRoundRect(8, WS_FOOT_Y, w - 16, WS_FOOT_H, 6, _pcol(26, 16, 58));
    d.drawRoundRect(8, WS_FOOT_Y, w - 16, WS_FOOT_H, 6, _pcol(90, 55, 165));
    d.setTextDatum(middle_center);
    d.setTextColor(_pcol(150, 210, 255), _pcol(26, 16, 58));
    d.drawString("Phone setup (server + WiFi)", w / 2, WS_FOOT_Y + WS_FOOT_H / 2);

    d.setTextDatum(top_center);
    d.setTextColor(_pcol(120, 110, 160), _pcol(8, 6, 20));
    d.drawString("tap edit / hold clear / swipe left back", w / 2, 210);
    d.drawString("top slot is tried first", w / 2, 224);
}

// SSID → password → save, aimed at one slot. Cancelling either keyboard leaves
// NVS untouched. A blank SSID or blank password is REFUSED, not saved: a blank
// key writes an unjoinable network that the ladder then wastes 8s on every pass,
// and on a single-slot device it is the lockout we had to reflash out of.
// Returns true if the slot was written.
static bool _wsEditSlot(int i) {
    String tSsid = "Slot " + String(i + 1) + " - WiFi name (SSID):";
    String ssid  = tkPrompt(tSsid.c_str(), false);
    ssid.trim();
    if (ssid.length() == 0) {
        _wsMsg("Not saved", "cancelled, or no name entered", _pcol(255, 170, 120), 1400);
        return false;
    }
    // Name the network on the password screen — it's the confirmation that the
    // SSID went in, and which of the three slots you're filling.
    String tPass = "Password for " + _wsFit(ssid, 20) + ":";
    String pass  = tkPrompt(tPass.c_str(), true);
    if (pass.length() == 0) {
        _wsMsg("Not saved", "cancelled, or no password entered", _pcol(255, 170, 120), 1400);
        return false;
    }
    _saveSlot(i, ssid, pass);
    _wsMsg("Saved", "slot " + String(i + 1) + ": " + _wsFit(ssid, 28), _pcol(120, 240, 150), 1400);
    // Down? Climb again now, from the top — the new entry may be the one that
    // answers. Already up? Leave the working link alone; the new slot takes
    // effect at the next boot or drop.
    if (WiFi.status() != WL_CONNECTED) _ladderReset();
    return true;
}

void launchWifiSlots() {
    auto& d = M5StackChan.Display();
    String ssids[SC_MAX_NETS], passes[SC_MAX_NETS];
    String conn;
    bool     dirty = true, wasTouch = false, holdFired = false;
    int      downRow = -2;               // -2 = none, 0..2 = slot row, -1 = footer
    int      downX = 0, downY = 0, lastX = 0, lastY = 0;
    uint32_t downMs = 0, lastConnCheck = 0, lastHoldDraw = 0;

    _wsDrainTouch();   // don't inherit the tap that opened this screen
    for (;;) {
        M5StackChan.update();

        // Refresh the "on <ssid>" header if the link changes while we sit here.
        if (millis() - lastConnCheck > 1000) {
            lastConnCheck = millis();
            String now = wifiConnectedSsid();
            if (now != conn) { conn = now; dirty = true; }
        }
        if (dirty) {
            _loadCreds(ssids, passes);
            _wsDrawList(ssids, conn, -1, 0.0f);
            dirty = false;
        }

        int16_t tx = 0, ty = 0;
        bool touching = d.getTouch(&tx, &ty);

        if (touching && !wasTouch) {                      // press
            wasTouch = true; holdFired = false;
            downX = lastX = tx; downY = lastY = ty; downMs = millis();
            downRow = -2;
            if (ty >= WS_ROW_Y0 && ty < WS_ROW_Y0 + SC_MAX_NETS * WS_ROW_H)
                downRow = (ty - WS_ROW_Y0) / WS_ROW_H;
            else if (ty >= WS_FOOT_Y && ty < WS_FOOT_Y + WS_FOOT_H)
                downRow = -1;
        } else if (touching) {                            // held
            lastX = tx; lastY = ty;
            bool moved = abs(lastX - downX) > 14 || abs(lastY - downY) > 14;
            if (downRow >= 0 && !holdFired && !moved) {
                uint32_t heldMs = millis() - downMs;
                if (heldMs >= WS_HOLD_MS) {               // hold complete → clear
                    holdFired = true;
                    if (ssids[downRow].length()) {
                        _clearSlot(downRow);
                        _wsMsg("Cleared", "slot " + String(downRow + 1),
                               _pcol(255, 150, 150), 1000);
                    }
                    dirty = true;
                } else if (ssids[downRow].length() && millis() - lastHoldDraw > 60) {
                    lastHoldDraw = millis();              // fill the row as they hold (throttled)
                    _wsDrawRow(downRow, ssids[downRow],
                               conn.length() && ssids[downRow] == conn,
                               (float)heldMs / (float)WS_HOLD_MS);
                }
            }
        } else if (wasTouch) {                            // release
            wasTouch = false;
            int dx = lastX - downX, dy = lastY - downY;
            int row = downRow;
            downRow = -2;
            if (holdFired) { delay(8); continue; }        // hold already acted
            if (dx < -WS_SWIPE && abs(dy) < 55) return;   // right-to-left → back to Settings
            if (abs(dx) < 14 && abs(dy) < 14) {
                if (row == -1) { _runPortal(); return; }  // never returns (reboots on save)
                if (row >= 0)  { _wsEditSlot(row); _wsDrainTouch(); dirty = true; }
            } else {
                dirty = true;                             // a stray drag smudged a row
            }
        }
        delay(8);
    }
}


static void wifiSupervisorTick() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!sWifiConnected) {
            sWifiConnected = true;
            sConnectedAt   = millis();
            _ladderReset();
            Serial.printf("[wifi] connected: %s (%s)\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        }
        if (!sSyncDone && millis() - sConnectedAt > 10000) {
            sSyncDone = true;   // connection settled (gates the health re-probe below)
        }
        // Recover a stale non-online status word: re-probe /health every 20s ONLY
        // while not online. Stops once online — no probe cost or face stutter when
        // healthy. Covers a transient failure latched by the one-shot boot probe.
        if (sSyncDone && millis() - sLastArgusHb > ARGUS_HEARTBEAT_MS) {
            sLastArgusHb = millis();
            _sendArgusHeartbeat();
        }
        if (sSyncDone && !sHealthOnline && millis() - sLastHealth > 20000) {
            sLastHealth = millis();
            _refreshHealthStatus();
        }
        return;
    }
    if (sWifiConnected) {
        sWifiConnected = false;
        sSyncDone      = false;
        sConnectedAt   = 0;
        sHealthOnline  = false;   // re-probe health once the link comes back
        _ladderReset();           // a drop re-runs the ladder from slot 0
        Serial.println("[wifi] disconnected — re-running the ladder");
    }
    // Iris watchdog: climb the ladder, retry FOREVER; the portal is boot-onboarding
    // only, never raised on a runtime drop.
    _ladderTick();
}

// ── Globals ───────────────────────────────────────────────────────────────────
Ph3b3Face    face;
AppManager   appMgr;
bool         g_overlayOpen   = false;  // set by CrescentMenu; suppresses app touch handling
bool         g_crescentTapped = false;  // set by CrescentMenu on crescent tap; consumed by active app

static CrescentMenu crescentMenu;
static TalkApp      talkApp;
static NetworkApp   networkApp;
static KaraokeApp   karaokeApp;
static GhostApp     ghostApp;
static SettingsApp  settingsApp;

// ── Servo constants ───────────────────────────────────────────────────────────
static const int TILT_HOME    = 450;   // 45.0° — physical center for pitch
static const int TILT_MIN     =  50;   // 5.0°  — rated floor (BSP allows 0, but 5° is spec)
static const int TILT_MAX     = 850;   // 85.0° — rated ceiling (BSP allows 900 = 90°)
// YAW_SAFE_MAX: BSP allows ±1280 (±128°). Physical build may be tighter due to cable routing.
// 700 (70°) is a conservative safe default; tune up after verifying mechanical stop on your unit.
static const int YAW_SAFE_MAX = 700;

// ── Rung 0 — lookAtNormalized safe wrapper ────────────────────────────────────
// BSP maps [-1,+1] linearly to its own angle limits: pitch 0–900 (0°–90°), yaw ±1280 (±128°).
// That overshoots rated pitch by 5° at both ends. This wrapper pre-scales inputs so
// ±1.0 arrives at exactly the rated limits (TILT_MIN–TILT_MAX for pitch, ±YAW_SAFE_MAX for yaw).
//
// Verified endpoints (TILT_MIN=50, TILT_MAX=850, BSP pitch range=900):
//   y=+1.0 → pre-scaled y=+0.889 → BSP pitch = 850 (85.0°) ✓
//   y=-1.0 → pre-scaled y=-0.889 → BSP pitch =  50 (5.0°)  ✓
//   y= 0.0 → pre-scaled y= 0.0   → BSP pitch = 450 (45.0°) ✓  (centers align)
//   x=+1.0 → pre-scaled x=+0.547 → BSP yaw   = 700 (70.0°) ✓
//   x=-1.0 → pre-scaled x=-0.547 → BSP yaw   =-700 (-70.0°) ✓
static void lookAtNormalizedSafe(float x, float y, int speed = 500) {
    constexpr float pitchScale = (float)(TILT_MAX - TILT_MIN) / 900.0f;   // 800/900 ≈ 0.889
    constexpr float yawScale   = (float)YAW_SAFE_MAX / 1280.0f;            // 700/1280 ≈ 0.547
    M5StackChan.Motion.lookAtNormalized(x * yawScale, y * pitchScale, speed);
}

// ── Rung 2 — listening orient target (settable; Beast 2 / camera swaps source) ─
// Units: decidegrees. Call setListenTarget() from vision/camera code when available.
static int _listenYaw   = 0;          // 0 = front-center
static int _listenPitch = TILT_HOME;  // 45° = level gaze

void setListenTarget(int yawDec, int pitchDec) {
    _listenYaw   = constrain(yawDec,   -YAW_SAFE_MAX, YAW_SAFE_MAX);
    _listenPitch = constrain(pitchDec, TILT_MIN,       TILT_MAX);
}

// ── Body language state ───────────────────────────────────────────────────────
static Ph3b3Face::State _lastBLState = Ph3b3Face::BOOT;
static uint32_t _scanNextMs  = 0;   // Rung 1: next idle waypoint time
static uint32_t _thinkNextMs = 0;   // Rung 3: next thinking drift time
static uint32_t _speakNextMs = 0;   // Rung 4: next speaking emphasis time

// applyBodyLanguage — fires once on state transition; sets initial pose + scan timers.
static void applyBodyLanguage(Ph3b3Face::State s) {
    if (face.isGazeLocked()) return;
    if (s == _lastBLState) return;
    _lastBLState = s;
    uint32_t now = millis();
    switch (s) {
        case Ph3b3Face::LISTENING:
            // Rung 2: stop scan, orient to speaker target (default = front-center).
            M5StackChan.Motion.moveX(_listenYaw,   350);
            M5StackChan.Motion.moveY(_listenPitch, 350);
            break;
        case Ph3b3Face::THINKING:
            // Rung 3: initial contemplative pose — slight up-right gaze, then drift.
            M5StackChan.Motion.moveX(160, 200);    // 16° off-axis
            M5StackChan.Motion.moveY(390, 180);    // 39° — soft upward look
            _thinkNextMs = now + 1800;             // let pose settle before first drift
            break;
        case Ph3b3Face::SPEAKING:
            // Rung 4: near-home for speaking; subtle life via updateBodyLanguageScan.
            M5StackChan.Motion.moveX(0, 280);
            M5StackChan.Motion.moveY(TILT_HOME, 280);
            _speakNextMs = now + 800;              // first emphasis after 800ms
            break;
        default:  // IDLE, CONNECTING, ERROR, BOOT
            // Rung 1: home on entry, then scan begins after brief grace.
            M5StackChan.Motion.moveX(0, 220);
            M5StackChan.Motion.moveY(TILT_HOME, 200);
            _scanNextMs = now + 1800;              // let home move complete before scanning
            break;
    }
}

// updateBodyLanguageScan — called every loop(); drives continuous per-state motion.
// State-machine approach: each state has its own timer and waypoint picker.
static void updateBodyLanguageScan(Ph3b3Face::State s) {
    if (face.isGazeLocked()) return;
    uint32_t now = millis();

    if (s == Ph3b3Face::IDLE || s == Ph3b3Face::CONNECTING) {
        // Rung 1 — mellow curious scan: slow pan with varied dwell + occasional tilt.
        // Range: ±35° yaw (350 dec), 42°–49° pitch (420–490 dec). Calm, unhurried.
        if (now < _scanNextMs) return;
        int yTgt = random(-350, 351);
        int pTgt = random(420, 491);
        M5StackChan.Motion.moveX(yTgt, 160);    // slow pan
        M5StackChan.Motion.moveY(pTgt, 130);    // very slow tilt
        _scanNextMs = now + 2500 + random(3000); // 2.5–5.5s dwell (not metronome)

    } else if (s == Ph3b3Face::THINKING) {
        // Rung 3 — contemplative drift: small slow movements after initial pose.
        if (now < _thinkNextMs) return;
        int yTgt = 160 + random(-100, 101);     // ±10° around the off-axis position
        int pTgt = 390 + random(-25, 36);       // 36.5°–42.5° gentle wander
        M5StackChan.Motion.moveX(yTgt, 130);
        M5StackChan.Motion.moveY(pTgt, 110);
        _thinkNextMs = now + 1200 + random(1000); // 1.2–2.2s

    } else if (s == Ph3b3Face::SPEAKING) {
        // Rung 4 — speaking emphasis: gentle pan life on phrase boundaries.
        // NOT syllable-synced — just enough motion to read as alive, not a statue.
        if (now < _speakNextMs) return;
        int yTgt = random(-200, 201);           // ±20° gentle sweep
        int pTgt = TILT_HOME + random(-25, 26); // ±2.5° subtle nod
        M5StackChan.Motion.moveX(yTgt, 210);
        M5StackChan.Motion.moveY(pTgt, 190);
        _speakNextMs = now + 900 + random(800);  // 0.9–1.7s between emphasis beats
    }
}

// ── Safe homing ───────────────────────────────────────────────────────────────
static void waitAxis(bool (*fn)()) {
    uint32_t t0 = millis();
    while (fn() && millis() - t0 < 5000) delay(10);
}

// safeHome — used on power-off path (side-button). Speed 200 is acceptable there.
static void safeHome() {
    M5StackChan.Motion.moveY(TILT_HOME, 200);
    waitAxis([]() { return M5StackChan.Motion.isYMoving(); });
    delay(200);
    M5StackChan.Motion.moveX(0, 200);
    waitAxis([]() { return M5StackChan.Motion.isXMoving(); });
}

// gentleHome — boot-only homing sequence. Servo::init() leaves torque disabled;
// this re-enables it explicitly and eases to neutral over ~1.5s so the first
// visible motion is a glide, not a snap.
// speed=50 → spring stiffness≈11.6 (critically damped) → 98% settle ≈ 1.5s.
// Tilt first (heavier axis, gravity bias), then pan.
static void gentleHome() {
    M5StackChan.Motion.setAutoAngleSyncEnabled(true);  // spring starts from physical pos
    M5StackChan.Motion.setTorqueEnabled(true);         // re-engage motor output
    M5StackChan.Motion.moveY(TILT_HOME, 50);
    waitAxis([]() { return M5StackChan.Motion.isYMoving(); });
    delay(100);
    M5StackChan.Motion.moveX(0, 50);
    waitAxis([]() { return M5StackChan.Motion.isXMoving(); });
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("[rung4] boot");

    M5StackChan.begin();

    // Boot splash
    auto& d = M5StackChan.Display();
    d.fillScreen(TFT_BLACK);
    d.setTextDatum(middle_center);
    d.setTextSize(2);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.drawString("Ph3b3", d.width() / 2, d.height() / 2 - 20);
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString("homing servos...", d.width() / 2, d.height() / 2 + 16);

    gentleHome();
    // Brownout mitigation (audit #2 / README landmine f3767ae): once homed, drop
    // servo HOLD current so it isn't stacked on the WiFi-connect current spike +
    // LED ring — the AXP2101 brown-out that stops WiFi joining on battery. The
    // head simply rests (same as the power-off state) until re-engaged after the
    // WiFi step below. Do NOT add torque to "fix" homing — more torque = more
    // current = worse brown-out.
    M5StackChan.Motion.setTorqueEnabled(false);
    randomSeed(micros());   // vary scan waypoints across boots
    Serial.println("[rung4] homed");

    // Init face before WiFi so the on-screen setup can animate while waiting for creds.
    face.begin();
    face.setStatusVisible(false);   // face-only mode: state via expression, no text
    face.setCrescentTabVisible(true);  // corner crescent is the only control affordance

    // WiFi — blocking connect up to 12 s on first boot cred
    WiFi.mode(WIFI_STA);
    String ssids[SC_MAX_NETS], passes[SC_MAX_NETS];
    // One-time NVS wipe: clears stale creds written before the captive portal existed.
    // Flag "wiped_v1" in the same namespace prevents this from ever running again.
    {
        sPrefs.begin(NVS_NS, false);
        if (!sPrefs.getBool("wiped_v1", false)) {
            sPrefs.clear();
            sPrefs.putBool("wiped_v1", true);
            Serial.println("[wifi] one-time NVS wipe done");
        }
        sPrefs.end();
    }

    _loadServer();   // atomic server record — seeds NVS on first boot, else reads it
    settingsLoad();  // Volume / Mic / LED presets from NVS (seeds defaults on first boot)
    face.setStatusVisible(true);
    face.setStatusLine("connecting");   // on-screen IP:port removed (Astro)

    _migrateLegacyCreds();   // pre-slot single-entry config → slot 0 (no-op after the first run)
    int n = _loadCreds(ssids, passes);
    Serial.printf("[wifi] _loadCreds → n=%d\n", n);
    if (n == 0) {
        _runPortal();  // no creds — captive setup portal (Dio-Setup)
    } else {
        // Climb the ladder on the boot splash — same screen as "homing servos...".
        // Empty slots are skipped; each candidate gets SC_SLOT_JOIN_MS, and the
        // SSID being tried is named on screen so a slot that fails is VISIBLE
        // rather than looking like a generic hang.
        const int LY = d.height() / 2 + 32;
        d.drawString("connecting wifi...", d.width() / 2, LY);
        for (int slot = 0; slot < SC_MAX_NETS && WiFi.status() != WL_CONNECTED; slot++) {
            if (!ssids[slot].length()) continue;          // unused slot
            Serial.printf("[wifi] boot slot %d: %s\n", slot, ssids[slot].c_str());
            d.fillRect(0, LY + 12, d.width(), 12, TFT_BLACK);
            d.drawString((String(slot + 1) + ": " + ssids[slot]).c_str(), d.width() / 2, LY + 18);
            _wifiJoin(ssids[slot].c_str(), passes[slot].c_str());
            uint32_t t0 = millis();
            while (millis() - t0 < SC_SLOT_JOIN_MS && WiFi.status() != WL_CONNECTED) {
                if (_joinRejected(millis() - t0)) break;   // bad key / not in the air → next slot now
                delay(100);
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            sWifiConnected = true;
            sConnectedAt   = millis();
            // Name the network we actually landed on — with three candidates,
            // "connected" on its own tells you nothing.
            d.fillRect(0, LY - 6, d.width(), 30, TFT_BLACK);
            d.setTextColor(TFT_GREEN, TFT_BLACK);
            d.drawString(("connected: " + WiFi.SSID()).c_str(), d.width() / 2, LY);
            d.setTextColor(TFT_WHITE, TFT_BLACK);
            Serial.printf("[wifi] connected: %s (%s)\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            // Boot health probe → status word. Refreshed in the loop until online
            // so a transient hiccup here doesn't stick. (Iris lesson)
            _refreshHealthStatus();
            sLastHealth = millis();
        } else {
            // Every filled slot failed at boot → captive setup portal (onboarding).
            // Runtime drops are the watchdog's job (retry forever), not this.
            // Name what was tried first — otherwise the portal appearing looks
            // like Dio forgot the networks rather than none of them answering.
            d.fillRect(0, LY - 6, d.width(), 30, TFT_BLACK);
            d.setTextColor(TFT_RED, TFT_BLACK);
            String tried;
            for (int i = 0; i < SC_MAX_NETS; i++)
                if (ssids[i].length()) tried += (tried.length() ? ", " : "") + ssids[i];
            d.drawString("no answer from:", d.width() / 2, LY);
            d.drawString(tried.c_str(), d.width() / 2, LY + 14);
            d.setTextColor(TFT_WHITE, TFT_BLACK);
            Serial.printf("[wifi] boot join failed (%s) — opening Dio-Setup portal\n",
                          tried.c_str());
            delay(2500);
            _runPortal();
        }
    }

    // WiFi current spike is past — re-engage the servos for the idle scan.
    // autoAngleSync so torque springs gently from the rested position (no snap),
    // then ease back to neutral. On battery this lifts fine; USB-only the tilt may
    // stay low (documented current limit, landmine f3767ae) — not a fault.
    M5StackChan.Motion.setAutoAngleSyncEnabled(true);
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.moveY(TILT_HOME, 50);
    M5StackChan.Motion.moveX(0, 50);

    camServer.begin();   // Phase 2: init CoreS3 camera (shares M5 I2C) + :8080 control server

    appMgr.registerApp(&talkApp);     // 0
    appMgr.registerApp(&networkApp);  // 1
    appMgr.registerApp(&karaokeApp);  // 2
    // appMgr.registerApp(&ghostApp);    // 3 — UNREGISTERED: a stray top-left crescent-tap trapped her in Ghost (FOCUSED/frown) with no voice; Ghost is a Rung-3 stub, so drop it from the menu
    appMgr.registerApp(&settingsApp); // last — Settings (WiFi / Mic / Volume / LED)

    appMgr.begin(0);  // boot into Talk
    Serial.println("[rung4] setup done");
}

// Base-LED cue authority — one place drives the 12 base LEDs, by priority:
//   PET (pink) > LISTENING (LED Color) > leave alone.
// Head-pat (Si12T @ I2C 0x68) is read via the BSP TouchSensor, which M5.begin()/
// M5StackChan.update() already init + poll every loop — we only CONSUME it here,
// in ANY state; pink brightness scales with pet intensity. LED-only: reads
// TouchSensor + face.getState(); never touches servo/VAD/PTT/state/karaoke.
// Color contract: pet = PINK (locked); listening = the LED Color setting (default
// purple). Both scaled by the LED brightness setting (Off → dark).
static int _cueLeds() {   // returns pet intensity 0..9 (for the pet-smile trigger in loop)
    static int      lastMode = 0;      // 0 none, 1 listen, 2 pet
    static uint32_t lastMs   = 0;
    const auto& ints = M5StackChan.TouchSensor.getIntensities();  // 3 ch, 0..3 each
    int  pet       = (int)ints[0] + (int)ints[1] + (int)ints[2];  // 0..9
    if (pet > 0) gLastPetMs = millis();   // activity signal for the dead-air exit (timestamp only)
    bool listening = (face.getState() == Ph3b3Face::LISTENING);
    int  mode      = pet > 0 ? 2 : (listening ? 1 : 0);

    if (mode == 2) {                   // PET → pink (locked), brighter the harder you pet
        float b = (0.55f + 0.45f * (pet >= 6 ? 1.0f : pet / 6.0f)) * gLedBrightness;
        uint8_t r = (uint8_t)(255 * b), g = (uint8_t)(45 * b), bl = (uint8_t)(140 * b);
        for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, bl);
        M5StackChan.refreshRgb();
    } else if (mode == 1) {            // LISTENING → LED Color breathe (~25 fps)
        if (millis() - lastMs >= 40) {
            lastMs = millis();
            float b = (0.6f + 0.4f * (0.5f + 0.5f * sinf(millis() / 300.0f))) * gLedBrightness;
            const uint8_t* c = SET_LEDC_RGB[gLedColorIdx];   // Settings → LED Color
            uint8_t r = (uint8_t)(c[0] * b), g = (uint8_t)(c[1] * b), bl = (uint8_t)(c[2] * b);
            for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, bl);
            M5StackChan.refreshRgb();
        }
    } else if (lastMode != 0) {        // fell to none → clear once
        for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, 0, 0, 0);
        M5StackChan.refreshRgb();
    }
    lastMode = mode;
    return pet;
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    M5StackChan.update();

    // Side-button deliberate hold → safe-home servos then PMIC power-off.
    // Power = 1000ms deliberate hold (AXP2101 hardware floor, register 0x27 bits[3:2]=0b00).
    // Intentional: prevents accidental power-off on the demo body. Tap does not trigger power.
    // AXP2101 mapping: wasHold() = long press (>=1s fired while held); wasClicked() = short tap (<1s, on release).
    if (M5.BtnPWR.wasHold()) {
        safeHome();
        M5.Power.powerOff();
    }

    wifiSupervisorTick();
    camServer.handle();     // service the camera control server (:8080) + monitor tick

    crescentMenu.update();  // updates g_overlayOpen + handles mode-switch taps
    appMgr.update();        // app logic (checks g_overlayOpen before processing touches)
    // Skip the per-frame face push when a full-screen app owns the screen OR the
    // crescent drawer is open — otherwise the push flickers behind the overlay
    // (the drawer redraws over a static frame instead; face resumes on close).
    bool appOwnsScreen = appMgr.active() && appMgr.active()->ownsScreen();
    bool faceLive = !appOwnsScreen && !g_overlayOpen && !g_faceOwnedByCamera;
    if (faceLive) face.update();  // renders face + crescent tab, pushes (paused during camera capture)
    int pet = _cueLeds();   // base-LED cue: pet=pink (any state) > listening=LED Color (LED-only)
    // Pet → warm smile, on the live face in a resting state (IDLE/FOCUSED) AND while
    // she wears the ERROR "bad news" frown — a good petting comforts her out of it
    // (frown → smile + brighten, see Ph3b3Face::render). Never while an app/overlay/
    // keyboard/camera owns the screen, nor during recording/speaking. Guardrail:
    // petting can't trigger PTT/karaoke.
    if (pet > 0 && faceLive &&
        (face.getState() == Ph3b3Face::IDLE || face.getState() == Ph3b3Face::FOCUSED ||
         face.getState() == Ph3b3Face::ERROR)) {
        face.petTouch();
        // Soft purr — ONLY when the mic is off (talkApp PH_IDLE) so it can't grab the
        // shared I2S bus from an active mic, and never over TTS (purr() guards that).
        // Cooldown so sustained petting purrs every few seconds, not every frame.
        static uint32_t sLastPurrMs = 0;
        if (talkApp.micIdle() && millis() - sLastPurrMs > 3500) {
            talkApp.purr();
            sLastPurrMs = millis();
        }
    }
    appMgr.draw();          // app overlays (e.g. karaoke lyrics)
    crescentMenu.draw();    // mode panel slides over everything when open

    applyBodyLanguage(face.getState());
    updateBodyLanguageScan(face.getState());

    // Servo gaze consumer — fires one command on lock/unlock edge; same target as pupils.
    // Uses safe wrapper so ±1.0 stays within rated servo travel.
    {
        static bool sPrevGazeLocked = false;
        bool gazeLocked = face.isGazeLocked();
        if (gazeLocked && !sPrevGazeLocked)
            lookAtNormalizedSafe(face.getGazeYaw(), face.getGazePitch(), 300);
        else if (!gazeLocked && sPrevGazeLocked)
            M5StackChan.Motion.goHome(300);
        sPrevGazeLocked = gazeLocked;
    }

    delay(16);
}
