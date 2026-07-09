#include "setupPortal.h"

// Needs to match the DRD storage used when the detector is created.
#define ESP_DRD_USE_SPIFFS true

#include <DNSServer.h>
#include <ESP_DoubleResetDetector.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "ClockLogic.h"         // tft - on-device status screen (as otaUpdate does)
#include "deviceIdentity.h"     // setup SSID + display identity
#include "logBuffer.h"          // Log
#include "genericBaseProject.h" // drd - stop it before the success/timeout reboot
#include "netCheck.h"           // netCheckNow / NetReachability - online vs captive
#include "projectConfig.h"      // save the picked network's extras

// --- Hotspot identity -------------------------------------------------------
// One SSID for the whole flow, derived from the same device label used by log
// shipping: "<hostname>-<last six MAC hex>". This keeps multiple clocks in
// setup mode distinguishable in the Wi-Fi picker.
static String g_apSsid;
static const char *AP_PASSWORD = "12345678";
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

// --- Timeouts ---------------------------------------------------------------
static const unsigned long PORTAL_TIMEOUT_MS = 15UL * 60UL * 1000UL; // whole session
static const unsigned long STA_CONNECT_TIMEOUT_MS = 20000;           // per join attempt
static const unsigned long NET_POLL_MS = 3000;                       // captive re-check

// --- Portal state -----------------------------------------------------------
enum PortalState
{
    PS_SELECTING,  // AP up, DNS hijacked, serving the config page; awaiting a pick
    PS_CONNECTING, // WiFi.begin() issued, waiting for the STA link
    PS_CAPTIVE,    // associated but behind a login page; NAT up, relaying to the phone
    PS_ONLINE      // internet reachable - about to save + reboot
};

static PortalState g_state = PS_SELECTING;
static String g_ssid, g_pass; // the network the user picked
static String g_message;      // human status shown on the page + screen
static bool g_beginPending = false;
static unsigned long g_connectStartMs = 0;
static unsigned long g_lastNetPollMs = 0;
static bool g_relayUp = false;

static int portalScaleX(int value)
{
    return (value * tft.width() + 160) / 320;
}

static int portalScaleY(int value)
{
    return (value * tft.height() + 120) / 240;
}

static int portalTextColsFromX(int x)
{
    int px = tft.width() - x - portalScaleX(4);
    return max(1, px / 6);
}

static DNSServer g_dns;            // captive DNS during PS_SELECTING (hijack -> AP_IP)
static WebServer g_web(80);        // config page + status API
static WiFiUDP g_dnsFromPhone;     // :53 forwarder listener during PS_CAPTIVE
static WiFiUDP g_dnsToUpstream;    // its query socket

// ---------------------------------------------------------------------------
// NAT + DNS plumbing (mirrors wifiRelay.cpp - NAPT must go through esp_netif,
// not the raw lwIP ip_napt_enable(), or IDF 5.x's thread-safety assert reboots
// the clock).
// ---------------------------------------------------------------------------
static esp_netif_t *apNetif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

// Hand the helper-AP DHCP clients the upstream DNS so a phone that renews its
// lease (most do after the STA-connect channel hop below) resolves names out
// through NAT without needing our forwarder.
static void offerUpstreamDnsToClients()
{
    esp_netif_t *ap = apNetif();
    if (!ap) return;
    IPAddress up = WiFi.dnsIP();
    if ((uint32_t)up == 0) up = IPAddress(8, 8, 8, 8);

    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = (uint32_t)up;

    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offerDns = 0x02; // OFFER_DNS
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offerDns, sizeof(offerDns));
    esp_netif_dhcps_start(ap);
}

// Turn the AP into a NAT gateway out through the STA link, and switch DNS from
// "hijack to our page" to "reach the real network" so the phone can load the
// upstream captive-portal login. Called once, when the STA associates.
static void startRelay()
{
    if (g_relayUp) return;

    // Stop hijacking DNS; a phone that still points DNS at us (didn't renew)
    // gets its queries forwarded upstream by serviceDnsForward() instead.
    g_dns.stop();
    g_dnsFromPhone.begin(53);
    offerUpstreamDnsToClients();

    esp_netif_t *ap = apNetif();
    esp_err_t err = ap ? esp_netif_napt_enable(ap) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK)
        Log.println("Setup portal: enabling NAT failed (err " + String((int)err) +
                    ") - captive login relay will not work");
    else
        Log.println("Setup portal: NAT relay up - phone can now reach the "
                    "network's login page (clock STA MAC " + WiFi.macAddress() + ")");
    g_relayUp = true;
}

static void stopRelayAndAp()
{
    esp_netif_t *ap = apNetif();
    if (ap) esp_netif_napt_disable(ap);
    g_dns.stop();
    g_dnsFromPhone.stop();
    WiFi.softAPdisconnect(true);
}

// Forward one pending DNS query from a phone (that still uses us as its DNS)
// out to the upstream resolver and relay the answer back. Best-effort; DNS is
// normally allowed through a captive portal even before login.
static void serviceDnsForward()
{
    int len = g_dnsFromPhone.parsePacket();
    if (len <= 0) return;

    uint8_t buf[512];
    if (len > (int)sizeof(buf)) return; // ignore oversized (EDNS) queries
    IPAddress cli = g_dnsFromPhone.remoteIP();
    uint16_t cport = g_dnsFromPhone.remotePort();
    int n = g_dnsFromPhone.read(buf, sizeof(buf));
    if (n <= 0) return;

    IPAddress up = WiFi.dnsIP();
    if ((uint32_t)up == 0) up = IPAddress(8, 8, 8, 8);
    g_dnsToUpstream.beginPacket(up, 53);
    g_dnsToUpstream.write(buf, n);
    g_dnsToUpstream.endPacket();

    unsigned long t0 = millis();
    while (millis() - t0 < 500)
    {
        int rn = g_dnsToUpstream.parsePacket();
        if (rn > 0)
        {
            uint8_t reply[512];
            int m = g_dnsToUpstream.read(reply, sizeof(reply));
            if (m > 0)
            {
                g_dnsFromPhone.beginPacket(cli, cport); // source port stays :53
                g_dnsFromPhone.write(reply, m);
                g_dnsFromPhone.endPacket();
            }
            return;
        }
        delay(2);
    }
}

// ---------------------------------------------------------------------------
// On-device status screen (drawn straight on the shared tft, like the OTA
// screen). Only repainted when the text changes, to avoid flicker.
// ---------------------------------------------------------------------------
static void drawScreen()
{
    static String lastKey;
    String key = String((int)g_state) + "|" + g_message;
    if (key == lastKey) return;
    lastKey = key;

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawCentreString("Wi-Fi setup", tft.width() / 2, portalScaleY(6), 2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString(deviceLabel(), tft.width() / 2, portalScaleY(24), 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(deviceMacAddress(), tft.width() / 2, portalScaleY(42), 1);

    if (g_state == PS_SELECTING)
    {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("1. Join this Wi-Fi on your phone:",
                       portalScaleX(5), portalScaleY(58), 2);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString(g_apSsid, portalScaleX(20), portalScaleY(78), 2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("   password: " + String(AP_PASSWORD),
                       portalScaleX(5), portalScaleY(98), 2);
        tft.drawString("2. A setup page opens; pick your",
                       portalScaleX(5), portalScaleY(124), 2);
        tft.drawString("   network and enter its password.",
                       portalScaleX(5), portalScaleY(142), 2);
        tft.drawString("(or browse to " + AP_IP.toString() + ")",
                       portalScaleX(5), portalScaleY(174), 2);
    }
    else
    {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Network: ", portalScaleX(5), portalScaleY(64), 2);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString(g_ssid, portalScaleX(90), portalScaleY(64), 2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        // Wrap the status message across two lines if needed.
        String m = g_message;
        int maxChars = portalTextColsFromX(portalScaleX(5));
        if (m.length() <= maxChars)
        {
            tft.drawString(m, portalScaleX(5), portalScaleY(104), 2);
        }
        else
        {
            int sp = m.lastIndexOf(' ', maxChars);
            if (sp < 0) sp = maxChars;
            tft.drawString(m.substring(0, sp), portalScaleX(5), portalScaleY(104), 2);
            tft.drawString(m.substring(sp + 1), portalScaleX(5), portalScaleY(124), 2);
        }
    }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static const char *stateName()
{
    switch (g_state)
    {
    case PS_SELECTING:  return "selecting";
    case PS_CONNECTING: return "connecting";
    case PS_CAPTIVE:    return "captive";
    default:            return "online";
    }
}

// JSON-escape a Wi-Fi SSID for embedding in the scan / status responses.
static String jsonEscape(const String &s)
{
    String o;
    o.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

static void handleRoot()
{
    // Self-contained page: lists networks, posts a pick to /connect, then polls
    // /status and swaps the message as the clock connects / relays / comes
    // online. No external assets (the phone has no internet yet).
    static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clock Wi-Fi setup</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:1.2em;max-width:32em}
h1{font-size:1.2rem}label{display:block;margin:.6em 0 .2em}select,input,button{width:100%;padding:.6em;font-size:1rem;border-radius:8px;border:1px solid #444;background:#1c1c1e;color:#eee;box-sizing:border-box}
button{background:#0a84ff;border-color:#0a84ff;color:#fff;margin-top:1em;font-weight:600}
#msg{margin:1em 0;padding:.8em;border-radius:8px;background:#1c1c1e}
.ok{color:#30d158}.warn{color:#ff9f0a}.err{color:#ff6961}small{color:#999}
</style></head><body>
<h1>%DEVICE% &mdash; Wi-Fi setup</h1>
<p><small>Device MAC: <code>%MAC%</code><br>Setup Wi-Fi: <code>%APSSID%</code></small></p>
<div id="pick">
<label for="ssid">Network</label>
<select id="ssid"><option>Scanning&hellip;</option></select>
<label for="pw">Password</label>
<input id="pw" type="password" placeholder="network password" autocomplete="off">
<button onclick="conn()">Connect</button>
<p><small>Pick your Wi-Fi and enter its password. If it needs a login page
(hotel/guest Wi-Fi), you'll finish that here on your phone in the next step.</small></p>
</div>
<div id="msg" style="display:none"></div>
<script>
function scan(){fetch('/scan').then(r=>r.json()).then(l=>{
if(!l.length){setTimeout(scan,1500);return;} // async scan still running - retry
var s=document.getElementById('ssid');s.innerHTML='';
l.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;
o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.enc?'':' [open]');s.appendChild(o);});}).catch(()=>setTimeout(scan,1500));}
function conn(){var ssid=document.getElementById('ssid').value,pw=document.getElementById('pw').value;
var b=new URLSearchParams();b.set('ssid',ssid);b.set('pass',pw);
fetch('/connect',{method:'POST',body:b}).then(()=>{document.getElementById('pick').style.display='none';
document.getElementById('msg').style.display='block';poll();});}
function poll(){fetch('/status').then(r=>r.json()).then(s=>{var m=document.getElementById('msg');
if(s.state=='connecting'){m.innerHTML='Connecting to <b>'+s.ssid+'</b>&hellip;';}
else if(s.state=='captive'){m.innerHTML='<b class=warn>Login required.</b><br>'+s.msg+
'<br><br>Open your browser; the network\'s login page should appear. Log in there, '+
'and this page will update automatically. (If nothing opens, visit '+
'<a href="http://neverssl.com">neverssl.com</a>.)';}
else if(s.state=='online'){m.innerHTML='<b class=ok>Setup complete!</b><br>'+s.msg;}
else{m.innerHTML=s.msg||'&hellip;';}
if(s.state!='online')setTimeout(poll,2000);}).catch(()=>setTimeout(poll,2000));}
scan();
</script></body></html>)HTML";
    String page = FPSTR(PAGE);
    page.replace("%DEVICE%", deviceLabel());
    page.replace("%MAC%", deviceMacAddress());
    page.replace("%APSSID%", g_apSsid);
    g_web.send(200, "text/html", page);
}

static void handleScan()
{
    // Only scan while selecting - a scan while the STA is associated (relaying a
    // captive login) would knock the upstream link down.
    if (g_state != PS_SELECTING)
    {
        g_web.send(200, "application/json", "[]");
        return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED || n == -2) // -2 = not started
    {
        WiFi.scanNetworks(true /*async*/); // kick a scan; the page retries
        g_web.send(200, "application/json", "[]");
        return;
    }
    if (n == WIFI_SCAN_RUNNING)
    {
        g_web.send(200, "application/json", "[]");
        return;
    }

    String out = "[";
    int emitted = 0;
    for (int i = 0; i < n && emitted < 24; i++)
    {
        if (WiFi.SSID(i).length() == 0) continue; // skip hidden/blank
        if (emitted) out += ",";
        out += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" +
               String(WiFi.RSSI(i)) + ",\"enc\":" +
               String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
        emitted++;
    }
    out += "]";
    WiFi.scanDelete();
    WiFi.scanNetworks(true); // pre-warm the next refresh
    g_web.send(200, "application/json", out);
}

static void handleConnect()
{
    g_ssid = g_web.arg("ssid");
    g_pass = g_web.arg("pass");
    if (g_ssid.length() == 0)
    {
        g_web.send(400, "text/plain", "no SSID");
        return;
    }
    Log.println("Setup portal: user picked \"" + g_ssid + "\" - connecting");
    g_state = PS_CONNECTING;
    g_message = "Connecting...";
    g_beginPending = true; // the service loop issues WiFi.begin (off the HTTP handler)
    g_web.send(200, "text/plain", "ok");
}

static void handleStatus()
{
    String msg = g_message;
    String j = "{\"state\":\"" + String(stateName()) + "\",\"ssid\":\"" +
               jsonEscape(g_ssid) + "\",\"msg\":\"" + jsonEscape(msg) +
               "\",\"mac\":\"" + deviceMacAddress() + "\",\"device\":\"" +
               jsonEscape(deviceLabel()) + "\",\"setupSsid\":\"" +
               jsonEscape(g_apSsid) + "\"}";
    g_web.send(200, "application/json", j);
}

// Any other host/path (the phone's captive-portal probe) -> our page, so the
// "sign in to Wi-Fi" popup opens the setup page during PS_SELECTING.
static void handleNotFound()
{
    g_web.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
    g_web.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static void evaluateConnection()
{
    // Just associated (or re-checking a captive network). Bring the relay up on
    // first association so a captive login can be completed, then classify.
    startRelay();

    NetReachability net = netCheckNow();
    if (net == NET_ONLINE)
    {
        g_state = PS_ONLINE;
        g_message = "Online - saving and restarting the clock.";
        Log.println("Setup portal: internet reachable on \"" + g_ssid + "\"");
    }
    else
    {
        g_state = PS_CAPTIVE;
        g_message = (net == NET_CAPTIVE)
                        ? "This network wants a browser login."
                        : "Associated, but no internet yet - try the login.";
    }
}

void runSetupPortal(bool forceConfig, ProjectConfig &config)
{
    (void)forceConfig; // the caller already decided we should run

    // AP + STA together so the hotspot survives the upstream connect. (When the
    // STA associates the single radio hops to the STA's channel, briefly
    // dropping AP clients - the phone auto-rejoins the same SSID.)
    //
    // Leave WiFi storage at its FLASH default: WiFi.begin() below must persist
    // the chosen credentials to NVS so the post-setup reboot reconnects to them.
    // (Do NOT call WiFi.persistent(false) here - on the double-reset/forceConfig
    // path WiFi isn't initialised yet, so it would actually switch storage to
    // RAM, the creds wouldn't be saved, and boot would loop back into setup.)
    WiFi.mode(WIFI_AP_STA);
    // Present the (optional) cloned MAC on the STA before we associate, so the
    // captive login authorizes the same address the clock uses after reboot.
    applyStaMacOverride();
    g_apSsid = setupPortalSsid();
    Log.println("Setup portal: starting unified Wi-Fi setup hotspot \"" +
                g_apSsid + "\" for device " + deviceLabel() +
                " (MAC " + deviceMacAddress() + ")");
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD);
    delay(100);

    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(53, "*", AP_IP); // hijack every lookup to us -> captive popup

    g_web.on("/", handleRoot);
    g_web.on("/scan", handleScan);
    g_web.on("/connect", HTTP_POST, handleConnect);
    g_web.on("/status", handleStatus);
    g_web.onNotFound(handleNotFound);
    g_web.begin();

    WiFi.scanNetworks(true); // async pre-warm so the first /scan has results

    g_state = PS_SELECTING;
    g_message = "";
    g_relayUp = false;
    g_beginPending = false;
    unsigned long startedMs = millis();
    g_lastNetPollMs = 0;

    Log.println("Setup portal: join \"" + g_apSsid + "\" (password " +
                AP_PASSWORD + ") on a phone to configure Wi-Fi");

    while (g_state != PS_ONLINE)
    {
        if (!g_relayUp) g_dns.processNextRequest(); // hijack only before the relay owns :53
        g_web.handleClient();
        if (g_relayUp) serviceDnsForward();
        drawScreen();

        unsigned long now = millis();

        if (now - startedMs > PORTAL_TIMEOUT_MS)
        {
            Log.println("Setup portal: timed out - rebooting to retry saved creds");
            stopRelayAndAp();
            if (drd) drd->stop();
            delay(500);
            ESP.restart();
        }

        if (g_beginPending)
        {
            g_beginPending = false;
            WiFi.begin(g_ssid.c_str(), g_pass.c_str());
            g_connectStartMs = now;
        }

        if (g_state == PS_CONNECTING)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                Log.println("Setup portal: associated with \"" + g_ssid +
                            "\", IP " + WiFi.localIP().toString());
                evaluateConnection();
            }
            else if (now - g_connectStartMs > STA_CONNECT_TIMEOUT_MS)
            {
                Log.println("Setup portal: could not join \"" + g_ssid + "\"");
                WiFi.disconnect();
                g_state = PS_SELECTING;
                g_message = "Could not join that network - check the password.";
                // The hijack DNS server kept running through the attempt, so the
                // config page still pops - nothing to restart here.
            }
        }
        else if (g_state == PS_CAPTIVE)
        {
            if (g_lastNetPollMs == 0 || now - g_lastNetPollMs >= NET_POLL_MS)
            {
                g_lastNetPollMs = now;
                if (netCheckNow() == NET_ONLINE)
                {
                    g_state = PS_ONLINE;
                    g_message = "Login complete - saving and restarting.";
                    Log.println("Setup portal: captive login succeeded - online");
                }
            }
        }

        delay(5);
    }

    // Online. The WiFi credentials are already persisted by WiFi.begin(); save
    // the rest of the config, then reboot so the normal boot path applies the
    // hostname / timezone and reconnects (the captive login is bound to our MAC
    // on the router, so it survives the restart).
    drawScreen();
    config.saveConfigFile();
    Log.println("Setup portal: setup complete - rebooting into the clock");
    stopRelayAndAp();
    if (drd) drd->stop();
    delay(1500); // let the phone's "/status" poll show "complete" first
    ESP.restart();
}
