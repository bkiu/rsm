#include "ui.h"

#include <M5Cardputer.h>
#include <stdarg.h>
#include <vector>

#include "gps.h"
#include "mesh.h"
#include "messages.h"
#include "nodedb.h"
#include "radio.h"
#include "regions.h"
#include "settings.h"

enum class Screen { Nodes, Chat, Regions, Info };

static constexpr int W = 240, H = 135;
static constexpr int HDR_H = 18;
static constexpr uint32_t DIM_AFTER_MS = 60000, OFF_AFTER_MS = 5 * 60000;

static constexpr uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_DIM = 0x8C71, C_HDR = 0x10A6, C_SEL = 0x2A8C,
                          C_OUT = 0x9F3F, C_OK = 0x5EAA, C_WARN = 0xFDA0, C_ERR = 0xF9A6, C_BADGE = 0xE8E4,
                          C_INPUT = 0x18E3;

static M5Canvas canvas(&M5Cardputer.Display);
static Screen screen = Screen::Nodes;
static bool dirty = true;
static uint32_t lastInputAt = 0, lastDrawAt = 0;
static uint8_t brightness = 0;

static int listSel = 0, listTop = 0;
static uint32_t chatThread = CHANNEL_THREAD;
static int chatScroll = 0; // pixels scrolled up from the newest message
static int infoScroll = 0;
static char draft[sizeof(Message::text)];
static size_t draftLen = 0;
static int regionSel = 0;
static char toast[48];
static uint32_t toastUntil = 0;

// ----------------------------------------------------------------------------------------------------------
// helpers

static const lgfx::IFont *bodyFont()
{
    switch (settings.fontSize) {
    case 0: return &fonts::FreeSans9pt7b;
    case 2: return &fonts::DejaVu24;
    default: return &fonts::DejaVu18;
    }
}
static const lgfx::IFont *smallFont()
{
    return settings.fontSize == 2 ? &fonts::DejaVu18 : &fonts::DejaVu12;
}

static void showToast(const char *msg)
{
    strlcpy(toast, msg, sizeof(toast));
    toastUntil = millis() + 2500;
    dirty = true;
}

static void ageText(uint32_t lastHeard, char *out, size_t n)
{
    if (!lastHeard) {
        out[0] = 0;
        return;
    }
    uint32_t s = (millis() - lastHeard) / 1000;
    if (s < 60)
        snprintf(out, n, "now");
    else if (s < 3600)
        snprintf(out, n, "%um", (unsigned)(s / 60));
    else
        snprintf(out, n, "%uh", (unsigned)(s / 3600));
}

// Shortens text with ".." until it fits maxW pixels in the current font.
static String fitText(const char *text, int maxW)
{
    String s(text);
    if (canvas.textWidth(s) <= maxW)
        return s;
    while (s.length() > 0 && canvas.textWidth(s + "..") > maxW)
        s.remove(s.length() - 1);
    return s + "..";
}

// Draws a name at (x, yMid), stepping the font down from the body size until the whole name fits in maxW. Only
// shortens with ".." if it doesn't fit even at the smallest size.
static void drawFittedName(const char *name, int x, int yMid, int maxW)
{
    static const lgfx::IFont *const sizes[] = {&fonts::DejaVu24, &fonts::DejaVu18, &fonts::FreeSans9pt7b,
                                               &fonts::DejaVu12};
    int i = 0;
    while (i < 3 && sizes[i] != bodyFont())
        i++;
    canvas.setFont(sizes[i]);
    while (i < 3 && canvas.textWidth(name) > maxW)
        canvas.setFont(sizes[++i]);
    canvas.setTextDatum(middle_left);
    canvas.drawString(fitText(name, maxW), x, yMid);
}

static void drawHeader(const char *title, const char *right)
{
    canvas.fillRect(0, 0, W, HDR_H, C_HDR);
    canvas.setFont(&fonts::DejaVu12);

    char buf[48];
    int bat = M5.Power.getBatteryLevel();
    if (bat >= 0)
        snprintf(buf, sizeof(buf), "%s %d%%", right ? right : "", bat);
    else
        snprintf(buf, sizeof(buf), "%s", right ? right : "");
    int rightW = canvas.textWidth(buf);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(C_DIM);
    canvas.drawString(buf, W - 4, HDR_H / 2);

    // GPS dot: green = fix, amber = searching, grey = module silent. Hidden when GPS is off.
    if (settings.gpsEnabled) {
        uint16_t c = gpsState.hasFix ? C_OK : gpsState.receiving ? C_WARN : C_DIM;
        rightW += 10;
        canvas.fillCircle(W - 4 - rightW + 3, HDR_H / 2, 3, c);
    }

    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_FG);
    canvas.drawString(fitText(title, W - 8 - rightW - 8), 4, HDR_H / 2);
}

static int footerHeight()
{
    canvas.setFont(smallFont());
    return canvas.fontHeight() + 3;
}

static void drawFooter(const char *hint)
{
    int fh = footerHeight();
    canvas.fillRect(0, H - fh, W, fh, C_HDR);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_FG);
    canvas.drawString(fitText(hint, W - 6), W / 2, H - fh / 2);
}

// Small padlock: filled green when we hold the node's public key, hollow grey when not.
static void drawLock(int x, int y, bool haveKey)
{
    uint16_t c = haveKey ? C_OK : C_DIM;
    canvas.drawRoundRect(x + 1, y, 6, 6, 2, c);
    if (haveKey)
        canvas.fillRect(x, y + 4, 8, 6, c);
    else
        canvas.drawRect(x, y + 4, 8, 6, c);
}

static int badgeWidth(uint32_t count)
{
    if (!count)
        return 0;
    char b[6];
    snprintf(b, sizeof(b), "%u", (unsigned)(count > 99 ? 99 : count));
    canvas.setFont(&fonts::DejaVu12);
    return canvas.textWidth(b) + 8;
}

static void drawBadge(int xRight, int yMid, uint32_t count)
{
    if (!count)
        return;
    char b[6];
    snprintf(b, sizeof(b), "%u", (unsigned)(count > 99 ? 99 : count));
    int w = badgeWidth(count);
    canvas.fillRoundRect(xRight - w, yMid - 7, w, 14, 7, C_BADGE);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_FG);
    canvas.drawString(b, xRight - w / 2, yMid);
}

// Word-wraps text to the current font. Very long words are split.
static void wrapText(const char *text, int width, std::vector<String> &out)
{
    String line;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            out.push_back(line);
            line = "";
            p++;
            continue;
        }
        const char *wordEnd = p;
        while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n')
            wordEnd++;
        String tok(p, wordEnd - p);
        String candidate = line.length() ? line + " " + tok : tok;
        if (canvas.textWidth(candidate) <= width) {
            line = candidate;
        } else {
            if (line.length())
                out.push_back(line);
            line = "";
            // hard-split words wider than the line
            while (canvas.textWidth(tok) > width) {
                int k = tok.length();
                while (k > 1 && canvas.textWidth(tok.substring(0, k)) > width)
                    k--;
                out.push_back(tok.substring(0, k));
                tok = tok.substring(k);
            }
            line = tok;
        }
        p = wordEnd;
        while (*p == ' ')
            p++;
    }
    if (line.length() || out.empty())
        out.push_back(line);
}

// ----------------------------------------------------------------------------------------------------------
// Node list

struct ListItem {
    bool channel;
    Node *node;
    int ch;
};

static int buildList(ListItem *items, int max)
{
    static Node *sortedNodes[NodeDB::MAX_NODES];
    int k = 0;
    for (int c = 0; c < MAX_CHANNELS && k < max; c++)
        if (mesh.channelUsed(c))
            items[k++] = {true, nullptr, c};
    int n = nodeDB.sorted(sortedNodes, NodeDB::MAX_NODES);
    for (int i = 0; i < n && k < max; i++)
        items[k++] = {false, sortedNodes[i], 0};
    return k;
}

static void drawNodes()
{
    static ListItem items[NodeDB::MAX_NODES + 1];
    int count = buildList(items, NodeDB::MAX_NODES + 1);
    if (listSel >= count)
        listSel = count - 1;

    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%s  %d nodes", radio.ready() ? settings.region : "NO RADIO", nodeDB.count());
    drawHeader(settings.longName, hdr);

    canvas.setFont(bodyFont());
    int rowH = canvas.fontHeight() + 6;
    int top = HDR_H + 1, bottom = H - footerHeight();
    int visible = (bottom - top) / rowH;
    if (visible < 1)
        visible = 1;
    if (listSel < listTop)
        listTop = listSel;
    if (listSel >= listTop + visible)
        listTop = listSel - visible + 1;

    for (int r = 0; r < visible && listTop + r < count; r++) {
        int i = listTop + r;
        int y = top + r * rowH;
        const ListItem &it = items[i];
        if (i == listSel)
            canvas.fillRoundRect(1, y, W - 2, rowH - 1, 4, C_SEL);

        char right[12] = "";
        uint32_t unread = 0;
        canvas.setFont(bodyFont());
        canvas.setTextDatum(middle_left);
        if (it.channel) {
            canvas.setTextColor(C_WARN);
            canvas.drawString("#", 4, y + rowH / 2);
            canvas.setTextColor(C_FG);
            canvas.drawString(fitText(mesh.channelName(it.ch), W - 18 - 40), 18, y + rowH / 2);
            unread = mesh.channelUnread[it.ch];
            if (mesh.channelSharesPosition(it.ch))
                snprintf(right, sizeof(right), "loc");
        } else {
            drawLock(4, y + rowH / 2 - 5, it.node->hasKey);
            ageText(it.node->lastHeard, right, sizeof(right));
            unread = it.node->unread;
            // Leave room on the right for age and badge.
            canvas.setFont(&fonts::DejaVu12);
            int rightW = (right[0] ? canvas.textWidth(right) + 8 : 0) + badgeWidth(unread) + 4;
            canvas.setTextColor(it.node->lastHeard ? C_FG : C_DIM);
            drawFittedName(nodeDB.displayName(it.node->num), 18, y + rowH / 2, W - 5 - rightW - 18);
        }
        canvas.setFont(&fonts::DejaVu12);
        canvas.setTextDatum(middle_right);
        canvas.setTextColor(C_DIM);
        canvas.drawString(right, W - 5, y + rowH / 2);
        int rw = right[0] ? canvas.textWidth(right) + 8 : 0;
        drawBadge(W - 5 - rw, y + rowH / 2, unread);
    }
    if (count == 1) {
        canvas.setFont(&fonts::DejaVu12);
        canvas.setTextDatum(top_center);
        canvas.setTextColor(C_DIM);
        canvas.drawString("No nodes heard yet", W / 2, top + rowH + 8);
    }
    drawFooter("Enter: open   I: help");
}

static void openChat(uint32_t thread)
{
    chatThread = thread;
    chatScroll = 0;
    draftLen = 0;
    draft[0] = 0;
    if (isChannelThread(thread))
        mesh.channelUnread[threadChannel(thread)] = 0;
    else if (Node *n = nodeDB.get(thread))
        n->unread = 0;
    screen = Screen::Chat;
}

static void sendQuick(char key)
{
    char t[48];
    QuickMessage *q = quickFind(key);
    if (!q) {
        snprintf(t, sizeof(t), "Fn+%c not set (serial: quick)", key);
        showToast(t);
        return;
    }
    Message *m = mesh.sendText(q->thread, q->text);
    if (m->status == MsgStatus::Failed)
        snprintf(t, sizeof(t), "Not sent: %s", m->detail);
    else if (isChannelThread(q->thread))
        snprintf(t, sizeof(t), "Sent to #%s", mesh.channelName(threadChannel(q->thread)));
    else
        snprintf(t, sizeof(t), "Sent to %s", nodeDB.displayName(q->thread));
    showToast(t);
}

static void nodesKey(const KeyEvent &e)
{
    static ListItem items[NodeDB::MAX_NODES + 1];
    int count = buildList(items, NodeDB::MAX_NODES + 1);
    bool up = e.key == Key::Up || (e.key == Key::Char && e.ch == ';');
    bool down = e.key == Key::Down || (e.key == Key::Char && e.ch == '.');
    char c = e.key == Key::Char ? tolower(e.ch) : 0;

    if (c && e.fn) {
        sendQuick(c);
        return;
    }
    if (up)
        listSel = listSel > 0 ? listSel - 1 : count - 1;
    else if (down)
        listSel = listSel < count - 1 ? listSel + 1 : 0;
    else if (e.key == Key::Enter && listSel < count)
        openChat(items[listSel].channel ? channelThread(items[listSel].ch) : items[listSel].node->num);
    else if (c == 'k' && listSel < count && !items[listSel].channel) {
        mesh.requestKey(items[listSel].node->num);
        showToast("Key request sent");
    } else if (c == 'i') {
        infoScroll = 0;
        screen = Screen::Info;
    } else if (c == 'f') {
        settings.fontSize = (settings.fontSize + 1) % 3;
        settingsSave();
    } else if (c == 'r') {
        screen = Screen::Regions;
    }
}

// ----------------------------------------------------------------------------------------------------------
// Chat

struct ChatLine {
    String text;
    uint16_t color;
    bool small;
    bool right;
    int gapAfter;
};

static void statusText(const Message &m, char *out, size_t n, uint16_t *color)
{
    switch (m.status) {
    case MsgStatus::Delivered: *color = C_OK; break;
    case MsgStatus::Relayed: *color = C_OK; break;
    case MsgStatus::Failed: *color = C_ERR; break;
    case MsgStatus::NoKey: *color = C_WARN; break;
    default: *color = C_DIM; break;
    }
    if (m.detail[0])
        snprintf(out, n, "%s - %s", statusLabel(m.status), m.detail);
    else
        snprintf(out, n, "%s", statusLabel(m.status));
}

static Message *lastRetryable()
{
    static Message *msgs[MessageStore::MAX_MESSAGES];
    int n = messages.thread(chatThread, msgs, MessageStore::MAX_MESSAGES);
    for (int i = n - 1; i >= 0; i--)
        if (msgs[i]->from == myNodeNum &&
            (msgs[i]->status == MsgStatus::Failed || msgs[i]->status == MsgStatus::NoKey ||
             msgs[i]->status == MsgStatus::Relayed))
            return msgs[i];
    return nullptr;
}

static void drawChat()
{
    bool isChan = isChannelThread(chatThread);
    Node *node = isChan ? nullptr : nodeDB.get(chatThread);
    char title[40], right[32] = "";
    if (isChan) {
        int ch = threadChannel(chatThread);
        snprintf(title, sizeof(title), "# %s", mesh.channelName(ch));
        snprintf(right, sizeof(right), ch == 0 ? "primary" : "ch %d", ch);
    } else {
        snprintf(title, sizeof(title), "%s", nodeDB.displayName(chatThread));
        int k = snprintf(right, sizeof(right), "%s", node && node->hasKey ? "key" : "NO KEY");
        if (node && node->hopsAway >= 0)
            k += snprintf(right + k, sizeof(right) - k, " %dhop", node->hopsAway);
        if (node && node->hasPosition && gpsState.hasFix) {
            char d[16];
            gpsFormatDistance(gpsDistanceM(gpsState.latitudeI, gpsState.longitudeI, node->latitudeI, node->longitudeI), d,
                              sizeof(d));
            snprintf(right + k, sizeof(right) - k, " %s", d);
        }
    }
    drawHeader(title, right);

    // input bar
    canvas.setFont(bodyFont());
    int inH = canvas.fontHeight() + 6;
    int inY = H - inH;
    canvas.fillRect(0, inY, W, inH, C_INPUT);
    canvas.setTextDatum(middle_left);
    if (!radio.ready()) {
        canvas.setTextColor(C_ERR);
        canvas.drawString("radio off - press Esc, R", 4, inY + inH / 2);
    } else if (!draftLen) {
        canvas.setTextColor(C_DIM);
        canvas.drawString("type a message...", 4, inY + inH / 2);
    } else {
        canvas.setTextColor(C_FG);
        const char *p = draft;
        while (*p && canvas.textWidth(p) > W - 16)
            p++;
        canvas.drawString(p, 4, inY + inH / 2);
    }
    if ((millis() / 500) % 2 && radio.ready()) {
        int cx = 4 + (draftLen ? min<int>(canvas.textWidth(draft), W - 16) : 0);
        canvas.fillRect(cx + 1, inY + 3, 2, inH - 6, C_FG);
    }

    // message lines, built newest-last
    static Message *msgs[MessageStore::MAX_MESSAGES];
    int n = messages.thread(chatThread, msgs, MessageStore::MAX_MESSAGES);
    std::vector<ChatLine> lines;
    int textW = W - 8;
    for (int i = (n > 30 ? n - 30 : 0); i < n; i++) {
        const Message &m = *msgs[i];
        bool mine = m.from == myNodeNum;
        String body = m.text;
        if (!mine && isChannelThread(chatThread)) {
            Node *s = nodeDB.get(m.from);
            body = String(s && s->shortName[0] ? s->shortName : "?") + ": " + body;
        }
        canvas.setFont(bodyFont());
        std::vector<String> wrapped;
        wrapText(body.c_str(), mine ? textW - 16 : textW, wrapped);
        for (auto &w : wrapped)
            lines.push_back({w, mine ? C_OUT : C_FG, false, mine, 0});
        if (mine) {
            char st[80];
            uint16_t col;
            statusText(m, st, sizeof(st), &col);
            canvas.setFont(smallFont());
            std::vector<String> sw;
            wrapText(st, textW, sw);
            for (auto &w : sw)
                lines.push_back({w, col, true, true, 0});
        }
        lines.back().gapAfter = 5;
    }

    int areaTop = HDR_H + 2, areaBottom = inY - 2;
    int totalH = 0;
    int bodyH, smallH;
    canvas.setFont(bodyFont());
    bodyH = canvas.fontHeight() + 1;
    canvas.setFont(smallFont());
    smallH = canvas.fontHeight() + 1;
    for (auto &l : lines)
        totalH += (l.small ? smallH : bodyH) + l.gapAfter;
    int maxScroll = max(0, totalH - (areaBottom - areaTop));
    if (chatScroll > maxScroll)
        chatScroll = maxScroll;

    canvas.setClipRect(0, areaTop, W, areaBottom - areaTop);
    int y = areaBottom + chatScroll;
    for (int i = (int)lines.size() - 1; i >= 0 && y > areaTop; i--) {
        const ChatLine &l = lines[i];
        int lh = l.small ? smallH : bodyH;
        y -= l.gapAfter + lh;
        if (y > areaBottom)
            continue;
        canvas.setFont(l.small ? smallFont() : bodyFont());
        canvas.setTextColor(l.color);
        canvas.setTextDatum(top_right);
        if (l.right)
            canvas.drawString(l.text, W - 4, y);
        else {
            canvas.setTextDatum(top_left);
            canvas.drawString(l.text, 4, y);
        }
    }
    canvas.clearClipRect();

    if (lines.empty()) {
        canvas.setFont(smallFont());
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_DIM);
        std::vector<String> w;
        wrapText(node && !node->hasKey ? "No key yet - it's fetched when you send" : "No messages yet", W - 16, w);
        int lh = canvas.fontHeight() + 2;
        int y0 = (areaTop + areaBottom) / 2 - (int)(w.size() - 1) * lh / 2;
        for (size_t i = 0; i < w.size(); i++)
            canvas.drawString(w[i], W / 2, y0 + i * lh);
    }
}

static void chatKey(const KeyEvent &e)
{
    switch (e.key) {
    case Key::Esc:
    case Key::Left:
        screen = Screen::Nodes;
        break;
    case Key::Up:
        chatScroll += 20;
        break;
    case Key::Down:
        chatScroll = max(0, chatScroll - 20);
        break;
    case Key::Backspace:
        if (draftLen)
            draft[--draftLen] = 0;
        break;
    case Key::Enter:
        if (draftLen && radio.ready()) {
            mesh.sendText(chatThread, draft);
            draftLen = 0;
            draft[0] = 0;
            chatScroll = 0;
        }
        break;
    case Key::Tab:
        if (Message *m = lastRetryable()) {
            mesh.retry(m->id);
            showToast("Retrying");
        } else if (!isChannelThread(chatThread)) {
            mesh.requestKey(chatThread);
            showToast("Key request sent");
        }
        break;
    case Key::Char:
        if (draftLen < sizeof(draft) - 1 && e.ch >= 32 && e.ch < 127) {
            draft[draftLen++] = e.ch;
            draft[draftLen] = 0;
        }
        break;
    default:
        break;
    }
}

// ----------------------------------------------------------------------------------------------------------
// Region picker

static void drawRegions()
{
    drawHeader("LoRa region", "");
    canvas.setFont(bodyFont());
    int rowH = canvas.fontHeight() + 4;
    int top = HDR_H + 2, bottom = H - footerHeight();
    int visible = max(1, (bottom - top) / rowH);
    int first = regionSel - visible / 2;
    if (first < 0)
        first = 0;
    if (first > regionCount() - visible)
        first = max(0, regionCount() - visible);
    for (int r = 0; r < visible && first + r < regionCount(); r++) {
        int i = first + r;
        int y = top + r * rowH;
        if (i == regionSel)
            canvas.fillRoundRect(1, y, W - 2, rowH - 1, 4, C_SEL);
        canvas.setTextDatum(middle_left);
        canvas.setTextColor(strcmp(regionAt(i)->name, settings.region) == 0 ? C_OK : C_FG);
        canvas.drawString(regionAt(i)->name, 8, y + rowH / 2);
    }
    drawFooter("; . move   Enter: pick");
}

static void regionsKey(const KeyEvent &e)
{
    bool up = e.key == Key::Up || (e.key == Key::Char && e.ch == ';');
    bool down = e.key == Key::Down || (e.key == Key::Char && e.ch == '.');
    if (up)
        regionSel = regionSel > 0 ? regionSel - 1 : regionCount() - 1;
    else if (down)
        regionSel = (regionSel + 1) % regionCount();
    else if (e.key == Key::Esc && settings.region[0])
        screen = Screen::Nodes;
    else if (e.key == Key::Enter) {
        strlcpy(settings.region, regionAt(regionSel)->name, sizeof(settings.region));
        settingsSave();
        radio.reconfigure();
        mesh.applyChannels();
        screen = Screen::Nodes;
        showToast(radio.ready() ? "Radio on" : radio.status());
    }
}

// ----------------------------------------------------------------------------------------------------------
// Info

static void drawInfo()
{
    drawHeader("Info & help", "");
    struct Item {
        char text[128];
        uint16_t color;
    };
    static Item items[44];
    int n = 0;
    auto add = [&](uint16_t color, const char *fmt, ...) {
        if (n >= 44)
            return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(items[n].text, sizeof(items[n].text), fmt, ap);
        va_end(ap);
        items[n++].color = color;
    };

    const RadioParams &rp = radio.params();
    add(C_FG, "%s (%s)", settings.longName, settings.shortName);
    add(C_FG, "ID !%08x", (unsigned)myNodeNum);
    add(radio.ready() ? C_FG : C_ERR, "%s", radio.status());
    add(C_FG, "%s slot %u, %d dBm", presetName(settings.preset), rp.slot, rp.power);
    for (int c = 0; c < MAX_CHANNELS; c++)
        if (mesh.channelUsed(c))
            add(C_FG, "Ch %d: %s%s", c, mesh.channelName(c), mesh.channelSharesPosition(c) ? " (shares location)" : "");
    add(C_FG, "Relay %s, hops %u", settings.relay ? "on" : "off", settings.hopLimit);
    add(C_FG, "RX %u TX %u bad %u", (unsigned)radio.rxCount, (unsigned)radio.txCount, (unsigned)radio.rxBad);
    int keys = 0;
    for (int i = 0; i < nodeDB.count(); i++)
        keys += nodeDB.at(i).hasKey;
    add(C_FG, "%d nodes, %d keys", nodeDB.count(), keys);

    if (!settings.gpsEnabled)
        add(C_DIM, "GPS off");
    else if (gpsState.hasFix)
        add(C_OK, "GPS %.5f %.5f, %u sats", gpsState.latitudeI / 1e7, gpsState.longitudeI / 1e7, (unsigned)gpsState.sats);
    else if (gpsState.receiving)
        add(C_WARN, "GPS searching, %u sats", (unsigned)gpsState.sats);
    else
        add(C_ERR, "GPS: no data from module");

    add(C_WARN, "Node list");
    add(C_FG, "; . move, Enter open");
    add(C_FG, "K get key, F text size");
    add(C_FG, "R region, I this page");
    add(C_FG, "Fn+letter/digit: quick message");
    bool anyQuick = false;
    for (auto &q : quickMessages)
        if (q.key) {
            if (!anyQuick)
                add(C_WARN, "Quick messages (node list)");
            anyQuick = true;
            if (isChannelThread(q.thread))
                add(C_FG, "Fn+%c #%s: %s", q.key, mesh.channelName(threadChannel(q.thread)), q.text);
            else
                add(C_FG, "Fn+%c %s: %s", q.key, nodeDB.displayName(q.thread), q.text);
        }
    add(C_WARN, "Chat");
    add(C_FG, "Enter send, Esc back");
    add(C_FG, "Fn+; Fn+. scroll");
    add(C_FG, "Tab retry failed message");
    add(C_WARN, "Settings");
    add(C_FG, "USB serial 115200, type help");

    canvas.setFont(smallFont());
    int lh = canvas.fontHeight() + 2;
    std::vector<std::pair<String, uint16_t>> lines;
    for (int i = 0; i < n; i++) {
        std::vector<String> w;
        wrapText(items[i].text, W - 8, w);
        for (auto &s : w)
            lines.push_back({s, items[i].color});
    }
    int maxScroll = max(0, (int)lines.size() * lh - (H - HDR_H - 4));
    if (infoScroll > maxScroll)
        infoScroll = maxScroll;
    canvas.setClipRect(0, HDR_H + 1, W, H - HDR_H - 1);
    canvas.setTextDatum(top_left);
    for (size_t i = 0; i < lines.size(); i++) {
        canvas.setTextColor(lines[i].second);
        canvas.drawString(lines[i].first, 4, HDR_H + 3 + (int)i * lh - infoScroll);
    }
    canvas.clearClipRect();
}

static void infoKey(const KeyEvent &e)
{
    bool up = e.key == Key::Up || (e.key == Key::Char && e.ch == ';');
    bool down = e.key == Key::Down || (e.key == Key::Char && e.ch == '.');
    if (up)
        infoScroll = max(0, infoScroll - 16);
    else if (down)
        infoScroll += 16;
    else
        screen = Screen::Nodes;
}

// ----------------------------------------------------------------------------------------------------------

static void setBrightness(uint8_t b)
{
    if (b != brightness) {
        brightness = b;
        M5Cardputer.Display.setBrightness(b);
    }
}

void uiNotifyIncoming(uint32_t thread)
{
    lastInputAt = millis(); // wake the screen
    if (screen == Screen::Chat && chatThread == thread) {
        if (isChannelThread(thread))
            mesh.channelUnread[threadChannel(thread)] = 0;
        else if (Node *n = nodeDB.get(thread))
            n->unread = 0;
        chatScroll = 0;
    }
    M5Cardputer.Speaker.tone(isChannelThread(thread) ? 1800 : 2600, 80);
    dirty = true;
}

void uiBegin()
{
    M5Cardputer.Display.setRotation(1);
    canvas.setColorDepth(16);
    canvas.createSprite(W, H);
    lastInputAt = millis();
    setBrightness(160);
    for (int i = 0; i < regionCount(); i++)
        if (strcmp(regionAt(i)->name, settings.region) == 0)
            regionSel = i;
    if (!settings.region[0])
        screen = Screen::Regions;
}

void uiTick()
{
    KeyEvent e;
    if (M5Cardputer.BtnA.wasPressed()) {
        if (brightness >= 100 && screen != Screen::Regions)
            screen = Screen::Nodes; // the side button always goes home
        lastInputAt = millis();
        dirty = true;
    }
    while (inputNext(e)) {
        bool wasAsleep = brightness < 100;
        lastInputAt = millis();
        dirty = true;
        if (wasAsleep)
            continue; // first key only wakes the screen
        switch (screen) {
        case Screen::Nodes: nodesKey(e); break;
        case Screen::Chat: chatKey(e); break;
        case Screen::Regions: regionsKey(e); break;
        case Screen::Info: infoKey(e); break;
        }
    }

    uint32_t idle = millis() - lastInputAt;
    setBrightness(idle > OFF_AFTER_MS ? 0 : idle > DIM_AFTER_MS ? 30 : 160);
    if (brightness == 0)
        return;

    if (mesh.uiDirty) {
        mesh.uiDirty = false;
        dirty = true;
    }
    uint32_t now = millis();
    if (toastUntil && (int32_t)(now - toastUntil) >= 0) {
        toastUntil = 0;
        dirty = true;
    }
    if (!dirty && now - lastDrawAt < (screen == Screen::Chat ? 500u : 5000u))
        return;
    dirty = false;
    lastDrawAt = now;

    canvas.fillSprite(C_BG);
    switch (screen) {
    case Screen::Nodes: drawNodes(); break;
    case Screen::Chat: drawChat(); break;
    case Screen::Regions: drawRegions(); break;
    case Screen::Info: drawInfo(); break;
    }
    if (toastUntil && (int32_t)(now - toastUntil) < 0) {
        canvas.setFont(&fonts::DejaVu12);
        String t = fitText(toast, W - 24);
        int w = canvas.textWidth(t) + 16;
        canvas.fillRoundRect((W - w) / 2, H / 2 - 12, w, 24, 6, C_SEL);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_FG);
        canvas.drawString(t, W / 2, H / 2);
    }
    canvas.pushSprite(0, 0);
}
