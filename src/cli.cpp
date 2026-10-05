#include "cli.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <LittleFS.h>
#include <ctype.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include "battery.h"
#include "crypto.h"
#include "gps.h"
#include "mesh.h"
#include "messages.h"
#include "nodedb.h"
#include "radio.h"
#include "regions.h"
#include "settings.h"

static char line[256];
static size_t lineLen = 0;

static int countChannels()
{
    int n = 0;
    for (int i = 0; i < MAX_CHANNELS; i++)
        n += mesh.channelUsed(i);
    return n;
}

// Channel by index ("2") or name ("Family"); -1 if not found.
static int findChannel(const char *s)
{
    if (isdigit((unsigned char)s[0]) && !s[1])
        return mesh.channelUsed(s[0] - '0') ? s[0] - '0' : -1;
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (mesh.channelUsed(i) && strcasecmp(mesh.channelName(i), s) == 0)
            return i;
    return -1;
}

static void pskToBase64(const uint8_t *psk, uint8_t len, char *out, size_t n)
{
    size_t olen = 0;
    mbedtls_base64_encode((uint8_t *)out, n - 1, &olen, psk, len);
    out[olen] = 0;
}

// Accepts base64 (what the Meshtastic apps show), "random" (new 256-bit key), "default" or "none".
static bool parsePsk(const char *arg, uint8_t *psk, uint8_t *len)
{
    if (!strcmp(arg, "default")) {
        psk[0] = 1;
        *len = 1;
    } else if (!strcmp(arg, "none")) {
        psk[0] = 0;
        *len = 1;
    } else if (!strcmp(arg, "random")) {
        esp_fill_random(psk, 32); // true random: the radio is running
        *len = 32;
    } else {
        uint8_t out[48];
        size_t olen = 0;
        if (mbedtls_base64_decode(out, sizeof(out), &olen, (const uint8_t *)arg, strlen(arg)) != 0 || olen == 0 ||
            olen > 32) {
            Serial.println("key must be base64 of 1, 16 or 32 bytes, or random/default/none");
            return false;
        }
        memcpy(psk, out, olen);
        *len = olen;
    }
    return true;
}

static void listChannels()
{
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!mesh.channelUsed(i))
            continue;
        const uint8_t *psk = i == 0 ? settings.psk : settings.channels[i - 1].psk;
        uint8_t len = i == 0 ? settings.pskLen : settings.channels[i - 1].pskLen;
        char b64[64];
        pskToBase64(psk, len, b64, sizeof(b64));
        const char *note = len == 1 ? (psk[0] == 0 ? " (unencrypted)" : psk[0] == 1 ? " (default public key)" : "") : "";
        Serial.printf("%d  %-11s key %s%s%s%s\n", i, mesh.channelName(i), b64, note,
                      mesh.channelSharesPosition(i) ? "  shares location" : "", i == 0 ? "  [primary]" : "");
    }
}

static void printSettings()
{
    const RadioParams &rp = radio.params();
    Serial.printf("node      !%08x\n", (unsigned)myNodeNum);
    Serial.printf("name      %s (%s)\n", settings.longName, settings.shortName);
    Serial.printf("region    %s\n", settings.region[0] ? settings.region : "(not set)");
    Serial.printf("preset    %s\n", presetName(settings.preset));
    Serial.printf("slot      %u%s -> %.3f MHz, %d dBm\n", rp.slot, settings.slot ? "" : " (auto)", rp.freqMHz, rp.power);
    Serial.printf("channels  %d (type 'channels' for names and keys)\n", countChannels());
    Serial.printf("power     %d dBm (requested)\n", settings.txPower);
    Serial.printf("hops      %u\n", settings.hopLimit);
    Serial.printf("relay     %s\n", settings.relay ? "on" : "off");
    Serial.printf("font      %u\n", settings.fontSize);
    if (!settings.gpsEnabled)
        Serial.printf("gps       off\n");
    else if (gpsState.hasFix)
        Serial.printf("gps       fix %.6f, %.6f  alt %d m  %u sats\n", gpsState.latitudeI / 1e7, gpsState.longitudeI / 1e7,
                      (int)gpsState.altitudeM, (unsigned)gpsState.sats);
    else
        Serial.printf("gps       %s (%u sats, %u baud)\n", gpsState.receiving ? "searching" : "no data from module",
                      (unsigned)gpsState.sats, (unsigned)gpsState.baud);
    Serial.printf("share     %s, %u bits, every %u min\n", settings.sharePosition ? "on" : "off",
                  settings.positionPrecision, settings.positionIntervalMin);
    Serial.printf("radio     %s  rx=%u tx=%u bad=%u\n", radio.status(), (unsigned)radio.rxCount, (unsigned)radio.txCount,
                  (unsigned)radio.rxBad);
}

static void help()
{
    Serial.println(
        "Commands:\n"
        "  info                     show settings and radio status\n"
        "  name <long name>         set long name (max 24 chars)\n"
        "  short <name>             set short name (max 4 chars)\n"
        "  region [CODE]            set region (no arg: list regions)\n"
        "  preset [NAME]            LongFast, MediumFast, ShortFast, ... (no arg: list)\n"
        "  slot <n>                 frequency slot, 0 = automatic\n"
        "  channels                 list channels with their keys (to set up other devices)\n"
        "  chadd <name> [key]       add a private channel; key: base64, random (default), default, none\n"
        "  chset <n> name|key|share <value>   change channel n (0 = primary); share on|off\n"
        "  chdel <n>                remove channel n (1-7)\n"
        "  channel <name>|default   rename the primary channel (moves frequency unless slot is set!)\n"
        "  psk <key>                primary channel key\n"
        "  power <dBm>              transmit power\n"
        "  hops <1-7>               hop limit for our packets\n"
        "  relay on|off             rebroadcast other nodes' packets\n"
        "  font <0-2>               text size\n"
        "  gps on|off               power the GPS\n"
        "  share on|off [bits]      broadcast position on the primary (bits: 32 exact, 16 ~350 m, 13 ~3 km)\n"
        "  interval <minutes>       how often to broadcast position\n"
        "  bat                      battery voltage and trend (is it charging?)\n"
        "  log on|off               print every received packet (until reboot)\n"
        "  nodes                    list known nodes\n"
        "  msg <!id|name|ch|#chan> <text>  send a message\n"
        "  keyreq <!id|name>        ask a node for its public key\n"
        "  quick                    list quick messages (Fn+<key> on the node list sends one)\n"
        "  quick <key> <!id|name|ch|#chan> <text>   set one; key is a letter or digit\n"
        "  quick <key> del          remove one\n"
        "  export                   print all settings, keys, nodes and quick messages (see backup.sh)\n"
        "  clearmsgs | clearnodes   wipe history / node list\n"
        "  factoryreset             reset settings and generate a new key pair\n"
        "  reboot");
}

static Node *findNode(const char *arg)
{
    if (arg[0] == '!')
        return nodeDB.get(strtoul(arg + 1, nullptr, 16));
    for (int i = 0; i < nodeDB.count(); i++) {
        Node &n = nodeDB.at(i);
        if (strcasecmp(n.shortName, arg) == 0 || strcasecmp(n.longName, arg) == 0)
            return &n;
    }
    return nullptr;
}

// Message destination: "ch" (primary), "#<channel>", "!<node id>" or a node name. Prints why on failure.
static bool parseTarget(const char *arg, uint32_t *thread)
{
    if (!strcmp(arg, "ch")) {
        *thread = CHANNEL_THREAD;
    } else if (arg[0] == '#') {
        int c = findChannel(arg + 1);
        if (c < 0) {
            Serial.println("unknown channel");
            return false;
        }
        *thread = channelThread(c);
    } else if (Node *n = findNode(arg)) {
        *thread = n->num;
    } else {
        Serial.println("unknown node");
        return false;
    }
    return true;
}

static void listQuick()
{
    int n = 0;
    for (auto &q : quickMessages) {
        if (!q.key)
            continue;
        n++;
        if (isChannelThread(q.thread))
            Serial.printf("Fn+%c  #%-11s %s\n", q.key, mesh.channelName(threadChannel(q.thread)), q.text);
        else
            Serial.printf("Fn+%c  %-12s %s\n", q.key, nodeDB.displayName(q.thread), q.text);
    }
    if (!n)
        Serial.println("no quick messages; add one with: quick <key> <#chan|!id|name> <text>");
}

// Prints everything worth keeping as serial commands, so a backup is restored by typing (or piping) it back in.
// backup.sh captures the text between the markers.
static void exportAll()
{
    char b64[64];
    Serial.println("--- BEGIN RSM EXPORT ---");
    Serial.printf("# rsm backup of !%08x. Contains private keys: keep it private.\n", (unsigned)myNodeNum);
    Serial.println("# Every line is an ordinary serial command; restore with ./backup.sh restore <file>.");
    pskToBase64(settings.privateKey, 32, b64, sizeof(b64));
    Serial.printf("identity %s\n", b64);
    Serial.printf("name %s\n", settings.longName);
    Serial.printf("short %s\n", settings.shortName);
    if (settings.region[0])
        Serial.printf("region %s\n", settings.region);
    Serial.printf("preset %s\n", presetName(settings.preset));
    Serial.printf("slot %u\n", settings.slot);
    Serial.printf("channel %s\n", settings.channelName[0] ? settings.channelName : "default");
    pskToBase64(settings.psk, settings.pskLen, b64, sizeof(b64));
    Serial.printf("psk %s\n", b64);
    Serial.printf("power %d\n", settings.txPower);
    Serial.printf("hops %u\n", settings.hopLimit);
    Serial.printf("relay %s\n", settings.relay ? "on" : "off");
    Serial.printf("font %u\n", settings.fontSize);
    Serial.printf("gps %s\n", settings.gpsEnabled ? "on" : "off");
    Serial.printf("share %s %u\n", settings.sharePosition ? "on" : "off", settings.positionPrecision);
    Serial.printf("interval %u\n", settings.positionIntervalMin);
    for (int i = 1; i < MAX_CHANNELS; i++) {
        const ExtraChannel &c = settings.channels[i - 1];
        if (!c.used)
            continue;
        pskToBase64(c.psk, c.pskLen, b64, sizeof(b64));
        Serial.printf("chadd %s %s\n", c.name, b64);
        if (c.sharePosition)
            Serial.printf("chset %s share on\n", c.name);
    }
    for (int i = 0; i < nodeDB.count(); i++) {
        Node &n = nodeDB.at(i);
        if (n.hasKey)
            pskToBase64(n.publicKey, 32, b64, sizeof(b64));
        Serial.printf("nodeadd !%08x %s %s %s\n", (unsigned)n.num, n.hasKey ? b64 : "-", n.shortName[0] ? n.shortName : "-",
                      n.longName);
    }
    for (auto &q : quickMessages) {
        if (!q.key)
            continue;
        if (isChannelThread(q.thread))
            Serial.printf("quick %c #%s %s\n", q.key, mesh.channelName(threadChannel(q.thread)), q.text);
        else
            Serial.printf("quick %c !%08x %s\n", q.key, (unsigned)q.thread, q.text);
    }
    Serial.println("sync");
    Serial.println("# Message history (for reading; not restored):");
    for (int i = 0; i < messages.count(); i++) {
        Message &m = messages.at(i);
        char where[24];
        if (isChannelThread(m.thread))
            snprintf(where, sizeof(where), "#%s", mesh.channelName(threadChannel(m.thread)));
        else
            snprintf(where, sizeof(where), "!%08x", (unsigned)m.thread);
        Serial.printf("# %s %s %s: %s\n", where, m.from == myNodeNum ? "me" : nodeDB.displayName(m.from),
                      m.from == myNodeNum ? statusLabel(m.status) : "", m.text);
    }
    Serial.println("--- END RSM EXPORT ---");
}

static void applyRadio()
{
    settingsSave();
    radio.reconfigure();
    mesh.applyChannels();
    mesh.uiDirty = true;
    Serial.printf("radio: %s\n", radio.status());
}

static void runCommand(char *cmd)
{
    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
    } else {
        arg = cmd + strlen(cmd);
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        help();
    } else if (!strcmp(cmd, "info")) {
        printSettings();
    } else if (!strcmp(cmd, "name") && *arg) {
        strlcpy(settings.longName, arg, sizeof(settings.longName));
        settingsSave();
        Serial.println("ok (announced at next NodeInfo broadcast)");
    } else if (!strcmp(cmd, "short") && *arg) {
        strlcpy(settings.shortName, arg, sizeof(settings.shortName));
        settingsSave();
        Serial.println("ok");
    } else if (!strcmp(cmd, "region")) {
        if (!*arg) {
            for (int i = 0; i < regionCount(); i++)
                Serial.printf("%s ", regionAt(i)->name);
            Serial.println();
        } else if (const Region *r = findRegion(arg)) {
            strlcpy(settings.region, r->name, sizeof(settings.region));
            applyRadio();
        } else {
            Serial.println("unknown region");
        }
    } else if (!strcmp(cmd, "preset")) {
        uint8_t p;
        if (!*arg)
            Serial.println("LongFast LongSlow LongMod LongTurbo MediumSlow MediumFast MediumTurbo ShortSlow ShortFast ShortTurbo");
        else if (presetFromName(arg, &p)) {
            settings.preset = p;
            applyRadio();
        } else
            Serial.println("unknown preset");
    } else if (!strcmp(cmd, "slot")) {
        settings.slot = atoi(arg);
        applyRadio();
    } else if (!strcmp(cmd, "channel") && *arg) {
        if (!strcmp(arg, "default"))
            settings.channelName[0] = 0;
        else
            strlcpy(settings.channelName, arg, sizeof(settings.channelName));
        applyRadio();
    } else if (!strcmp(cmd, "psk") && *arg) {
        if (!parsePsk(arg, settings.psk, &settings.pskLen))
            return;
        applyRadio();
    } else if (!strcmp(cmd, "power") && *arg) {
        settings.txPower = atoi(arg);
        applyRadio();
    } else if (!strcmp(cmd, "hops") && *arg) {
        int h = atoi(arg);
        settings.hopLimit = h < 1 ? 1 : (h > 7 ? 7 : h);
        settingsSave();
        Serial.println("ok");
    } else if (!strcmp(cmd, "relay") && *arg) {
        settings.relay = !strcmp(arg, "on");
        settingsSave();
        Serial.printf("relay %s\n", settings.relay ? "on" : "off");
    } else if (!strcmp(cmd, "font") && *arg) {
        settings.fontSize = constrain(atoi(arg), 0, 2);
        settingsSave();
        mesh.uiDirty = true;
    } else if (!strcmp(cmd, "gps") && *arg) {
        settings.gpsEnabled = !strcmp(arg, "on");
        gpsSetEnabled(settings.gpsEnabled);
        settingsSave();
        Serial.printf("gps %s\n", settings.gpsEnabled ? "on" : "off");
    } else if (!strcmp(cmd, "share") && *arg) {
        char *bits = strchr(arg, ' ');
        if (bits)
            *bits++ = 0;
        settings.sharePosition = !strcmp(arg, "on");
        if (bits)
            settings.positionPrecision = constrain(atoi(bits), 10, 32);
        settingsSave();
        mesh.uiDirty = true;
        Serial.printf("position sharing %s (%u bits)\n", settings.sharePosition ? "on" : "off", settings.positionPrecision);
    } else if (!strcmp(cmd, "interval") && *arg) {
        settings.positionIntervalMin = constrain(atoi(arg), 1, 720);
        settingsSave();
        Serial.println("ok");
    } else if (!strcmp(cmd, "log") && *arg) {
        mesh.logPackets = !strcmp(arg, "on");
        Serial.printf("packet log %s\n", mesh.logPackets ? "on" : "off");
    } else if (!strcmp(cmd, "bat")) {
        int mv = batteryMv();
        Serial.printf("battery   %d.%02d V, %d%%\n", mv / 1000, (mv % 1000) / 10, (int)M5.Power.getBatteryLevel());
        int mins = batteryHistoryMinutes();
        if (mins < 5) {
            Serial.printf("trend     need 5 min of samples (have %d); check again soon\n", mins);
        } else {
            int t5 = batteryTrendMv(5), t = batteryTrendMv(mins > 20 ? 20 : mins);
            Serial.printf("trend     %+d mV over 5 min, %+d mV over %d min\n", t5, t, mins > 20 ? 20 : mins);
            Serial.println(t > 15   ? "=> voltage rising: charging"
                           : t < -15 ? "=> voltage falling: NOT charging (or using more than the charger supplies)"
                                     : "=> roughly flat: full, or charging about as fast as it is used");
        }
    } else if (!strcmp(cmd, "channels")) {
        listChannels();
    } else if (!strcmp(cmd, "chadd") && *arg) {
        char *key = strchr(arg, ' ');
        if (key) {
            *key++ = 0;
            while (*key == ' ')
                key++;
        }
        if (strlen(arg) > 11) {
            Serial.println("channel names are at most 11 characters");
            return;
        }
        if (findChannel(arg) >= 0) {
            Serial.println("a channel with that name already exists");
            return;
        }
        int slot = -1;
        for (int i = 1; i < MAX_CHANNELS && slot < 0; i++)
            if (!settings.channels[i - 1].used)
                slot = i;
        if (slot < 0) {
            Serial.println("all 8 channels are in use; remove one with chdel");
            return;
        }
        ExtraChannel &c = settings.channels[slot - 1];
        memset(&c, 0, sizeof(c));
        if (!parsePsk(key && *key ? key : "random", c.psk, &c.pskLen))
            return;
        strlcpy(c.name, arg, sizeof(c.name));
        c.used = true;
        settingsSave();
        mesh.applyChannels();
        char b64[64];
        pskToBase64(c.psk, c.pskLen, b64, sizeof(b64));
        Serial.printf("added channel %d \"%s\" key %s\n", slot, c.name, b64);
        Serial.println("Other devices need exactly this name and key (add it as a secondary channel in the app).");
    } else if (!strcmp(cmd, "chset") && *arg) {
        // chset <n> name|key|share <value>
        char *what = strchr(arg, ' ');
        char *value = what ? strchr(what + 1, ' ') : nullptr;
        if (!what || !value) {
            Serial.println("usage: chset <n> name|key|share <value>");
            return;
        }
        *what++ = 0;
        *value++ = 0;
        int ch = findChannel(arg);
        if (ch < 0) {
            Serial.println("unknown channel");
            return;
        }
        ExtraChannel *c = ch ? &settings.channels[ch - 1] : nullptr;
        if (!strcmp(what, "name")) {
            if (strlen(value) > 11) {
                Serial.println("channel names are at most 11 characters");
                return;
            }
            strlcpy(c ? c->name : settings.channelName, value, sizeof(settings.channelName));
            if (!c)
                Serial.println("note: renaming the primary moves the frequency unless 'slot' is set");
        } else if (!strcmp(what, "key")) {
            if (!parsePsk(value, c ? c->psk : settings.psk, c ? &c->pskLen : &settings.pskLen))
                return;
        } else if (!strcmp(what, "share")) {
            bool on = !strcmp(value, "on");
            if (c)
                c->sharePosition = on;
            else
                settings.sharePosition = on;
        } else {
            Serial.println("usage: chset <n> name|key|share <value>");
            return;
        }
        applyRadio(); // the primary's name/key can move the frequency; harmless for the others
        listChannels();
    } else if (!strcmp(cmd, "chdel") && *arg) {
        int ch = findChannel(arg);
        if (ch <= 0) {
            Serial.println(ch == 0 ? "the primary channel can't be removed" : "unknown channel");
            return;
        }
        memset(&settings.channels[ch - 1], 0, sizeof(ExtraChannel));
        settingsSave();
        for (auto &q : quickMessages)
            if (q.key && q.thread == channelThread(ch)) {
                Serial.printf("removed quick message Fn+%c\n", q.key);
                q.key = 0;
            }
        quickSave();
        mesh.applyChannels();
        Serial.printf("removed channel %d\n", ch);
    } else if (!strcmp(cmd, "nodes")) {
        for (int i = 0; i < nodeDB.count(); i++) {
            Node &n = nodeDB.at(i);
            char dist[16] = "-";
            if (n.hasPosition && gpsState.hasFix)
                gpsFormatDistance(gpsDistanceM(gpsState.latitudeI, gpsState.longitudeI, n.latitudeI, n.longitudeI), dist, sizeof(dist));
            Serial.printf("!%08x %-4s %-24s key=%s hops=%d heard=%s dist=%s\n", (unsigned)n.num, n.shortName, n.longName,
                          n.hasKey ? "yes" : "no", n.hopsAway,
                          n.lastHeard ? String((millis() - n.lastHeard) / 1000).c_str() : "-", dist);
        }
    } else if (!strcmp(cmd, "msg") && *arg) {
        char *text = strchr(arg, ' ');
        if (!text) {
            Serial.println("usage: msg <!id|name|ch|#chan> <text>");
            return;
        }
        *text++ = 0;
        uint32_t thread;
        if (!parseTarget(arg, &thread))
            return;
        Message *m = mesh.sendText(thread, text);
        Serial.printf("queued id=%08x status=%s\n", (unsigned)m->id, statusLabel(m->status));
    } else if (!strcmp(cmd, "quick")) {
        // quick | quick <key> del | quick <key> <target> <text>
        if (!*arg) {
            listQuick();
            return;
        }
        char key = tolower((unsigned char)arg[0]);
        if (arg[1] != ' ' || !isalnum((unsigned char)key)) {
            Serial.println("usage: quick <key> <!id|name|ch|#chan> <text>   (key: one letter or digit)");
            return;
        }
        char *target = arg + 2;
        while (*target == ' ')
            target++;
        QuickMessage *q = quickFind(key);
        if (!strcmp(target, "del")) {
            if (q) {
                q->key = 0;
                quickSave();
            }
            Serial.println(q ? "removed" : "no quick message on that key");
            return;
        }
        char *text = strchr(target, ' ');
        if (!text) {
            Serial.println("usage: quick <key> <!id|name|ch|#chan> <text>");
            return;
        }
        *text++ = 0;
        while (*text == ' ')
            text++;
        uint32_t thread;
        if (!*text || !parseTarget(target, &thread))
            return;
        if (strlen(text) >= sizeof(q->text)) {
            Serial.printf("text is at most %u characters\n", (unsigned)sizeof(q->text) - 1);
            return;
        }
        if (!q)
            for (auto &e : quickMessages)
                if (!e.key) {
                    q = &e;
                    break;
                }
        if (!q) {
            Serial.printf("all %d quick messages are in use; remove one with: quick <key> del\n", MAX_QUICK);
            return;
        }
        q->key = key;
        q->thread = thread;
        strlcpy(q->text, text, sizeof(q->text));
        quickSave();
        listQuick();
    } else if (!strcmp(cmd, "export")) {
        exportAll();
    } else if (!strcmp(cmd, "sync")) {
        settingsSave();
        quickSave();
        nodeDB.save();
        messages.save();
        Serial.println("saved");
    } else if (!strcmp(cmd, "identity") && *arg) {
        // Restores our key pair from a backup. Only meant for this same device: the node number comes from the chip.
        uint8_t key[48];
        size_t olen = 0;
        uint8_t pub[32];
        if (mbedtls_base64_decode(key, sizeof(key), &olen, (const uint8_t *)arg, strlen(arg)) != 0 || olen != 32 ||
            !cryptoPublicKey(key, pub)) {
            Serial.println("identity needs the base64 private key from an export");
            return;
        }
        memcpy(settings.privateKey, key, 32);
        memcpy(settings.publicKey, pub, 32);
        settingsSave();
        Serial.println("key pair restored");
    } else if (!strcmp(cmd, "nodeadd") && *arg) {
        // nodeadd !<id> <public key|-> <short|-> [long name]   (lines written by 'export')
        char *tok[3];
        char *rest = arg;
        int k = 0;
        for (; k < 3 && rest && *rest; k++) {
            tok[k] = rest;
            rest = strchr(rest, ' ');
            if (rest) {
                *rest++ = 0;
                while (*rest == ' ')
                    rest++;
            }
        }
        if (k < 3 || tok[0][0] != '!') {
            Serial.println("usage: nodeadd !<id> <public key|-> <short|-> [long name]");
            return;
        }
        uint32_t num = strtoul(tok[0] + 1, nullptr, 16);
        uint8_t key[48];
        size_t olen = 0;
        bool hasKey = strcmp(tok[1], "-") != 0;
        if (hasKey && (mbedtls_base64_decode(key, sizeof(key), &olen, (const uint8_t *)tok[1], strlen(tok[1])) != 0 ||
                       olen != 32)) {
            Serial.println("bad public key");
            return;
        }
        Node *n = nodeDB.getOrCreate(num);
        if (strcmp(tok[2], "-"))
            strlcpy(n->shortName, tok[2], sizeof(n->shortName));
        if (rest && *rest) {
            strlcpy(n->longName, rest, sizeof(n->longName));
            n->hasUser = true;
        }
        if (hasKey) {
            memcpy(n->publicKey, key, 32);
            n->hasKey = true;
        }
        nodeDB.saveSoon(); // 'sync' (the last line of an export) writes it out
        mesh.uiDirty = true;
        Serial.println("ok");
    } else if (!strcmp(cmd, "keyreq") && *arg) {
        if (Node *n = findNode(arg)) {
            mesh.requestKey(n->num);
            Serial.println("sent");
        } else
            Serial.println("unknown node");
    } else if (!strcmp(cmd, "clearmsgs")) {
        LittleFS.remove("/messages.bin");
        ESP.restart();
    } else if (!strcmp(cmd, "clearnodes")) {
        LittleFS.remove("/nodes.bin");
        ESP.restart();
    } else if (!strcmp(cmd, "factoryreset")) {
        settingsFactoryReset();
        LittleFS.remove("/nodes.bin");
        LittleFS.remove("/messages.bin");
        ESP.restart();
    } else if (!strcmp(cmd, "reboot")) {
        ESP.restart();
    } else if (*cmd) {
        Serial.println("unknown command; type 'help'");
    }
}

void cliPoll()
{
    // Echo as we go: serial monitors (pio device monitor, screen, minicom) don't echo locally.
    static bool lastWasCR = false;
    while (Serial.available()) {
        int c = Serial.read();
        if (c == '\n' && lastWasCR) {
            lastWasCR = false; // second half of CRLF
            continue;
        }
        lastWasCR = c == '\r';
        if (c == '\r' || c == '\n') {
            Serial.print("\r\n");
            if (lineLen) {
                line[lineLen] = 0;
                runCommand(line);
                lineLen = 0;
            }
            Serial.print("> ");
        } else if (c == 8 || c == 127) {
            if (lineLen) {
                lineLen--;
                Serial.print("\b \b");
            }
        } else if (c >= 32 && c != 127 && lineLen < sizeof(line) - 1) {
            line[lineLen++] = c;
            Serial.write((uint8_t)c);
        }
    }
}
