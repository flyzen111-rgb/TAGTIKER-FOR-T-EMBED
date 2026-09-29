/*
 * TagTinker pour LilyGo T-Embed CC1101 (ESP32-S3)
 *
 * Portage de TagTinker (i12bp8, GPL-3.0) : https://github.com/i12bp8/TagTinker
 * Base : recherches de furrtek sur les etiquettes ESL infrarouges.
 *
 * USAGE : etiquettes que vous possedez legalement. Ne pas modifier des
 * etiquettes de magasin ni interferer avec l'infrastructure d'un tiers.
 *
 * Fonctions portees : saisie du code-barres, detection du profil d'etiquette,
 * envoi d'une image BMP (mono ou couleur accent rouge/jaune) depuis la carte SD.
 * Non portes : tags a segments, SmartTag Color 2.6, broadcast, NFC, Wi-Fi.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <RotaryEncoder.h>
#include <stdlib.h>
#include <string.h>

#include "board_pins.h"
#include "ir_tx.h"
#include "tagtinker_proto.h"

/* ------------------------------------------------------------------------ */
/* Etat global                                                              */
/* ------------------------------------------------------------------------ */

static TFT_eSPI tft;
static RotaryEncoder encoder(ENCODER_INA, ENCODER_INB, RotaryEncoder::LatchMode::TWO03);
static Preferences prefs;

struct Settings {
    uint8_t dataRepeats; /* repetitions des trames de donnees : 1..10 */
    uint8_t compression; /* 0 auto, 1 brut, 2 RLE */
    uint8_t encInvert;   /* 0 normal, 1 inverse */
    uint8_t rotation;    /* 1 ou 3 */
};
static Settings g_set = {2, 0, 0, 3};

static char g_barcode[18] = "A0000000000000000";
static bool g_hasTag = false;
static TagTinkerTagProfile g_profile;
static uint8_t g_plid[4];

static bool g_sdOk = false;
static volatile bool g_cancel = false;

#define COL_BG     TFT_BLACK
#define COL_FG     TFT_WHITE
#define COL_DIM    0x7BEF
#define COL_HEAD   0x2B0F
#define COL_SEL    0x03EF
#define COL_OK     TFT_GREEN
#define COL_ERR    TFT_RED
#define COL_WARN   TFT_YELLOW

#define SCR_W 320
#define SCR_H 170

/* ------------------------------------------------------------------------ */
/* Entrees : encodeur + bouton                                              */
/* ------------------------------------------------------------------------ */

enum Ev { EV_NONE, EV_CW, EV_CCW, EV_CLICK, EV_LONG };

static Ev pollEvent() {
    static long lastPos = 0;
    static bool down = false;
    static bool longSent = false;
    static uint32_t t0 = 0;

    encoder.tick();
    long p = encoder.getPosition();
    if(p != lastPos) {
        bool cw = p > lastPos;
        lastPos += cw ? 1 : -1;
        if(g_set.encInvert) cw = !cw;
        return cw ? EV_CW : EV_CCW;
    }

    bool pressed = (digitalRead(ENCODER_KEY) == LOW);
    if(pressed && !down) {
        down = true;
        longSent = false;
        t0 = millis();
    } else if(pressed && down) {
        if(!longSent && (millis() - t0) > 600) {
            longSent = true;
            return EV_LONG;
        }
    } else if(!pressed && down) {
        down = false;
        if(!longSent && (millis() - t0) > 30) return EV_CLICK;
    }
    return EV_NONE;
}

static Ev waitEvent() {
    for(;;) {
        Ev e = pollEvent();
        if(e != EV_NONE) return e;
        delay(1);
    }
}

static void waitButtonRelease() {
    while(digitalRead(ENCODER_KEY) == LOW) delay(10);
    delay(30);
}

/* ------------------------------------------------------------------------ */
/* Dessin                                                                   */
/* ------------------------------------------------------------------------ */

static void drawHeader(const char* title) {
    tft.fillRect(0, 0, SCR_W, 22, COL_HEAD);
    tft.setTextColor(COL_FG, COL_HEAD);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(title, 6, 3, 2);
}

static void drawFooter(const char* text) {
    tft.fillRect(0, SCR_H - 16, SCR_W, 16, COL_BG);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(text, 6, SCR_H - 16, 1);
}

static void clearScreen(const char* title) {
    tft.fillScreen(COL_BG);
    drawHeader(title);
}

typedef void (*LabelFn)(int idx, char* out, size_t n);

/* Liste a defilement. Retourne l'index choisi, ou -1 sur appui long. */
static int runList(const char* title, LabelFn label, int count, int start, const char* footer) {
    const int ROW_H = 20;
    const int TOP = 26;
    const int VISIBLE = 6;
    int sel = start;
    if(sel < 0 || sel >= count) sel = 0;
    int first = 0;

    for(;;) {
        if(sel < first) first = sel;
        if(sel >= first + VISIBLE) first = sel - VISIBLE + 1;

        clearScreen(title);
        for(int i = 0; i < VISIBLE && (first + i) < count; i++) {
            int idx = first + i;
            int y = TOP + i * ROW_H;
            char buf[64];
            label(idx, buf, sizeof(buf));
            if(idx == sel) {
                tft.fillRect(0, y, SCR_W, ROW_H, COL_SEL);
                tft.setTextColor(COL_FG, COL_SEL);
            } else {
                tft.setTextColor(COL_FG, COL_BG);
            }
            tft.setTextDatum(TL_DATUM);
            tft.drawString(buf, 8, y + 2, 2);
        }
        if(count > VISIBLE) {
            tft.setTextColor(COL_DIM, COL_BG);
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", sel + 1, count);
            tft.setTextDatum(TR_DATUM);
            tft.drawString(pos, SCR_W - 4, 4, 1);
        }
        drawFooter(footer ? footer : "Tourner: choisir  Clic: valider  Appui long: retour");

        Ev e = waitEvent();
        if(e == EV_CW) sel = (sel + 1) % count;
        else if(e == EV_CCW) sel = (sel + count - 1) % count;
        else if(e == EV_CLICK) return sel;
        else if(e == EV_LONG) return -1;
    }
}

/* Message avec jusqu'a 4 lignes. Attend un clic. */
static void showMessage(const char* title, uint16_t color, const char* l1, const char* l2 = NULL,
                        const char* l3 = NULL, const char* l4 = NULL) {
    clearScreen(title);
    const char* lines[4] = {l1, l2, l3, l4};
    tft.setTextDatum(TL_DATUM);
    for(int i = 0; i < 4; i++) {
        if(!lines[i]) continue;
        tft.setTextColor(i == 0 ? color : COL_FG, COL_BG);
        tft.drawString(lines[i], 8, 34 + i * 22, 2);
    }
    drawFooter("Clic: continuer");
    for(;;) {
        Ev e = waitEvent();
        if(e == EV_CLICK || e == EV_LONG) break;
    }
}

/* ------------------------------------------------------------------------ */
/* Reglages persistants                                                     */
/* ------------------------------------------------------------------------ */

static void settingsLoad() {
    prefs.begin("tagtinker", false);
    g_set.dataRepeats = prefs.getUChar("rep", 2);
    g_set.compression = prefs.getUChar("cmp", 0);
    g_set.encInvert = prefs.getUChar("inv", 0);
    g_set.rotation = prefs.getUChar("rot", 3);
    if(g_set.dataRepeats < 1 || g_set.dataRepeats > 10) g_set.dataRepeats = 2;
    if(g_set.compression > 2) g_set.compression = 0;
    if(g_set.encInvert > 1) g_set.encInvert = 0;
    if(g_set.rotation != 1 && g_set.rotation != 3) g_set.rotation = 3;
    String bc = prefs.getString("bc", "");
    if(bc.length() == 17) strncpy(g_barcode, bc.c_str(), 17);
    g_barcode[17] = '\0';
}

static void settingsSave() {
    prefs.putUChar("rep", g_set.dataRepeats);
    prefs.putUChar("cmp", g_set.compression);
    prefs.putUChar("inv", g_set.encInvert);
    prefs.putUChar("rot", g_set.rotation);
}

/* ------------------------------------------------------------------------ */
/* Carte SD                                                                 */
/* ------------------------------------------------------------------------ */

#define DROP_DIR "/tagtinker/dropped"

static bool sdInit() {
    if(g_sdOk) return true;
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(DISPLAY_CS, HIGH);
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    g_sdOk = SD.begin(BOARD_SD_CS, SPI, 4000000);
    if(g_sdOk) {
        SD.mkdir("/tagtinker");
        SD.mkdir(DROP_DIR);
    }
    return g_sdOk;
}

#define MAX_FILES 64
#define NAME_LEN  48
static char g_files[MAX_FILES][NAME_LEN];
static int g_fileCount = 0;

static bool endsWithBmp(const char* s) {
    size_t n = strlen(s);
    if(n < 5) return false;
    const char* e = s + n - 4;
    return e[0] == '.' && (e[1] == 'b' || e[1] == 'B') && (e[2] == 'm' || e[2] == 'M') &&
           (e[3] == 'p' || e[3] == 'P');
}

static void scanFiles() {
    g_fileCount = 0;
    File dir = SD.open(DROP_DIR);
    if(!dir || !dir.isDirectory()) return;
    File e = dir.openNextFile();
    while(e && g_fileCount < MAX_FILES) {
        if(!e.isDirectory()) {
            const char* full = e.name();
            const char* slash = strrchr(full, '/');
            const char* base = slash ? slash + 1 : full;
            if(base[0] != '.' && endsWithBmp(base) && strlen(base) < NAME_LEN) {
                strncpy(g_files[g_fileCount], base, NAME_LEN - 1);
                g_files[g_fileCount][NAME_LEN - 1] = '\0';
                g_fileCount++;
            }
        }
        e.close();
        e = dir.openNextFile();
    }
    dir.close();
}

/* ------------------------------------------------------------------------ */
/* Lecture BMP (meme logique que TagTinker)                                 */
/* ------------------------------------------------------------------------ */

#define BMP_ROW_BUF 128U

struct BmpInfo {
    uint32_t dataOffset;
    uint32_t rowStride;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    bool topDown;
};

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool bmpOpen(File& f, BmpInfo& info) {
    uint8_t h[54];
    if(f.read(h, sizeof(h)) != sizeof(h)) return false;
    if(h[0] != 'B' || h[1] != 'M') return false;

    uint16_t bpp = h[28] | (h[29] << 8);
    /* "2bpp" = deux plans de 1 bit empiles, ecrits par le preparateur d'images web. */
    if(!(bpp == 1 || bpp == 2)) return false;
    info.bpp = bpp;

    uint32_t rawW = le32(&h[18]);
    uint32_t rawH = le32(&h[22]);
    info.topDown = (rawH & 0x80000000U) != 0U;
    uint32_t absH = info.topDown ? (~rawH + 1U) : rawH;
    if(rawW == 0U || rawW > UINT16_MAX || absH == 0U || absH > UINT16_MAX) return false;

    info.width = (uint16_t)rawW;
    info.height = (uint16_t)absH;
    info.dataOffset = le32(&h[10]);
    info.rowStride = ((info.width + 31U) / 32U) * 4U;
    if(info.rowStride > BMP_ROW_BUF) return false;
    return true;
}

static inline uint8_t bmpPixel(const uint8_t* row, uint16_t x) {
    uint8_t bit = (row[x / 8U] >> (7U - (x % 8U))) & 1U;
    return bit ? 0U : 1U;
}

static inline uint16_t mapCoord(uint16_t out, uint16_t outSize, uint16_t srcSize) {
    if(outSize == 0U || srcSize == 0U) return 0U;
    uint32_t v = (uint32_t)out * (uint32_t)srcSize / (uint32_t)outSize;
    if(v >= srcSize) v = srcSize - 1U;
    return (uint16_t)v;
}

static bool bmpReadRow(File& f, const BmpInfo& info, uint16_t srcY, uint16_t planeOffRows, uint8_t* row) {
    uint16_t actual = info.topDown ? srcY : (uint16_t)(info.height - 1U - srcY);
    uint32_t off = info.dataOffset + ((uint32_t)actual + (uint32_t)planeOffRows) * info.rowStride;
    if(!f.seek(off)) return false;
    return f.read(row, info.rowStride) == info.rowStride;
}

/* Lit un plan complet, redimensionne au plus proche voisin vers la taille de l'etiquette. */
static bool bmpLoadPlane(File& f, const BmpInfo& info, uint16_t w, uint16_t h, uint16_t planeOffRows,
                         uint8_t* out) {
    uint8_t row[BMP_ROW_BUF];
    uint16_t cached = UINT16_MAX;
    for(uint16_t y = 0; y < h; y++) {
        uint16_t sy = mapCoord(y, h, info.height);
        if(sy != cached) {
            if(!bmpReadRow(f, info, sy, planeOffRows, row)) return false;
            cached = sy;
        }
        uint8_t* dst = out + (size_t)y * w;
        for(uint16_t x = 0; x < w; x++) {
            dst[x] = bmpPixel(row, mapCoord(x, w, info.width));
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Envoi                                                                    */
/* ------------------------------------------------------------------------ */

static bool stopCb() {
    if(digitalRead(ENCODER_KEY) == LOW) g_cancel = true;
    return g_cancel;
}

static void drawTx(const char* stage, size_t cur, size_t total) {
    tft.fillRect(0, 30, SCR_W, 90, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawString(stage, 8, 34, 2);

    const int bx = 8, by = 70, bw = SCR_W - 16, bh = 18;
    tft.drawRect(bx, by, bw, bh, COL_DIM);
    if(total > 0) {
        int fill = (int)(((uint64_t)cur * (uint64_t)(bw - 4)) / total);
        tft.fillRect(bx + 2, by + 2, fill, bh - 4, COL_OK);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%u / %u trames", (unsigned)cur, (unsigned)total);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(buf, 8, 96, 2);
}

/* Sequence identique a TagTinker : ping -> parametres -> donnees -> rafraichissement. */
static bool txSequence(const TagTinkerImagePayload& pl, uint8_t page, uint16_t w, uint16_t h) {
    uint8_t frame[TAGTINKER_MAX_FRAME_SIZE];
    size_t frames = pl.byte_count / TAGTINKER_IMAGE_DATA_BYTES_PER_FRAME;

    drawTx("Reveil de l'etiquette...", 0, frames);
    size_t len = tagtinker_make_ping_frame(frame, g_plid);
    if(!ir_transmit(frame, len, 80, 1, stopCb)) return false;
    delay(50);

    drawTx("Envoi des parametres...", 0, frames);
    len = tagtinker_make_image_param_frame(
        frame, g_plid, (uint16_t)pl.byte_count, pl.comp_type, page, w, h, 0, 0);
    if(!ir_transmit(frame, len, 15, 1, stopCb)) return false;
    delay(50);

    drawTx("Envoi de l'image...", 0, frames);
    for(size_t i = 0; i < frames; i++) {
        len = tagtinker_make_image_data_frame(
            frame, g_plid, (uint16_t)i, &pl.data[i * TAGTINKER_IMAGE_DATA_BYTES_PER_FRAME]);
        if(!ir_transmit(frame, len, g_set.dataRepeats, 1, stopCb)) return false;
        if(((i + 1U) % 32U) == 0U && (i + 1U) < frames) delay(1);
        if((i % 8U) == 0U) drawTx("Envoi de l'image...", i, frames);
    }
    drawTx("Envoi de l'image...", frames, frames);
    delay(50);

    drawTx("Rafraichissement...", frames, frames);
    len = tagtinker_make_refresh_frame(frame, g_plid);
    if(!ir_transmit(frame, len, 20, 1, stopCb)) return false;
    return true;
}

static bool profileSendable(const TagTinkerTagProfile& p, const char** why) {
    if(!p.known) {
        *why = "Type d'etiquette inconnu";
        return false;
    }
    if(p.type_code == TAGTINKER_TYPE_SMARTAG_COLOR_26) {
        *why = "SmartTag Color 2.6 non porte";
        return false;
    }
    if(p.kind != TagTinkerTagKindDotMatrix || p.width == 0 || p.height == 0) {
        *why = "Tag a segments: pas d'image";
        return false;
    }
    return true;
}

/* Prepare l'image puis l'envoie. Retourne un message d'erreur, ou NULL si tout s'est bien passe. */
static const char* sendBmp(const char* fileName, uint8_t page) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", DROP_DIR, fileName);

    File f = SD.open(path, FILE_READ);
    if(!f) return "Ouverture du fichier impossible";

    BmpInfo info;
    memset(&info, 0, sizeof(info));
    if(!bmpOpen(f, info)) {
        f.close();
        return "BMP invalide (1 ou 2 bits requis)";
    }

    const uint16_t w = g_profile.width;
    const uint16_t h = g_profile.height;
    const size_t count = (size_t)w * h;
    const bool accent =
        g_profile.color == TagTinkerTagColorRed || g_profile.color == TagTinkerTagColorYellow;

    uint8_t* p1 = (uint8_t*)malloc(count);
    uint8_t* p2 = accent ? (uint8_t*)malloc(count) : NULL;
    if(!p1 || (accent && !p2)) {
        free(p1);
        free(p2);
        f.close();
        return "Memoire insuffisante";
    }

    bool ok = bmpLoadPlane(f, info, w, h, 0, p1);
    if(ok && accent) {
        if(info.bpp == 2) ok = bmpLoadPlane(f, info, w, h, info.height, p2);
        else memset(p2, 1, count);
    }
    f.close();
    if(!ok) {
        free(p1);
        free(p2);
        return "Lecture du BMP echouee";
    }

    TagTinkerCompressionMode mode = TagTinkerCompressionAuto;
    if(g_set.compression == 1) mode = TagTinkerCompressionRaw;
    else if(g_set.compression == 2) mode = TagTinkerCompressionRle;

    TagTinkerImagePayload pl;
    memset(&pl, 0, sizeof(pl));
    ok = tagtinker_encode_planes_payload(p1, p2, count, mode, &pl);
    free(p1);
    free(p2);
    if(!ok) return "Encodage impossible (memoire)";

    /* Le champ "nombre d'octets" de la trame de parametres fait 16 bits. */
    if(pl.byte_count == 0 || pl.byte_count > 65535U) {
        tagtinker_free_image_payload(&pl);
        return "Image trop lourde pour ce protocole";
    }

    clearScreen("Envoi en cours");
    drawFooter("Appuyez sur le bouton pour annuler");
    waitButtonRelease();
    g_cancel = false;

    if(!ir_init()) {
        tagtinker_free_image_payload(&pl);
        return "Init IR (RMT) echouee";
    }
    bool sent = txSequence(pl, page, w, h);
    ir_deinit();
    tagtinker_free_image_payload(&pl);
    waitButtonRelease();

    if(!sent) return g_cancel ? "Envoi annule" : "Echec de l'envoi IR";
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Ecrans                                                                   */
/* ------------------------------------------------------------------------ */

static void updateTagFromBarcode() {
    g_hasTag = false;
    memset(&g_profile, 0, sizeof(g_profile));
    if(!tagtinker_is_barcode_valid(g_barcode)) return;
    if(!tagtinker_barcode_to_plid(g_barcode, g_plid)) return;
    tagtinker_barcode_to_profile(g_barcode, &g_profile);
    g_hasTag = true;
}

static const char* colorName(TagTinkerTagColor c) {
    switch(c) {
    case TagTinkerTagColorRed: return "noir/blanc/rouge";
    case TagTinkerTagColorYellow: return "noir/blanc/jaune";
    default: return "noir/blanc";
    }
}

/* Editeur de code-barres : 1 lettre + 16 chiffres. */
static bool editBarcode() {
    char buf[18];
    strncpy(buf, g_barcode, 17);
    buf[17] = '\0';
    /* garantit un format valide pour l'edition */
    if(buf[0] < 'A' || buf[0] > 'Z') buf[0] = 'A';
    for(int i = 1; i < 17; i++)
        if(buf[i] < '0' || buf[i] > '9') buf[i] = '0';

    int cur = 0; /* 0..16 = caracteres, 17 = bouton OK */
    for(;;) {
        clearScreen("Code-barres de l'etiquette");
        tft.setTextDatum(TL_DATUM);
        for(int i = 0; i < 17; i++) {
            int x = 8 + i * 18;
            char s[2] = {buf[i], 0};
            if(i == cur) {
                tft.fillRect(x - 2, 46, 18, 30, COL_SEL);
                tft.setTextColor(COL_FG, COL_SEL);
            } else {
                tft.setTextColor(COL_FG, COL_BG);
            }
            tft.drawString(s, x, 48, 4);
        }
        if(cur == 17) tft.fillRect(100, 100, 120, 26, COL_OK);
        else tft.drawRect(100, 100, 120, 26, COL_DIM);
        tft.setTextColor(cur == 17 ? COL_BG : COL_FG, cur == 17 ? COL_OK : COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("VALIDER", 160, 113, 2);
        drawFooter("Tourner: valeur  Clic: suivant  Appui long: precedent/annuler");

        Ev e = waitEvent();
        if(e == EV_CW || e == EV_CCW) {
            int d = (e == EV_CW) ? 1 : -1;
            if(cur == 0) {
                buf[0] = (char)('A' + ((buf[0] - 'A' + d + 26) % 26));
            } else if(cur < 17) {
                buf[cur] = (char)('0' + ((buf[cur] - '0' + d + 10) % 10));
            }
        } else if(e == EV_CLICK) {
            if(cur < 17) {
                cur++;
            } else {
                strncpy(g_barcode, buf, 17);
                g_barcode[17] = '\0';
                prefs.putString("bc", g_barcode);
                return true;
            }
        } else if(e == EV_LONG) {
            if(cur == 0) return false;
            cur--;
        }
    }
}

static void screenBarcode() {
    if(!editBarcode()) return;
    updateTagFromBarcode();
    if(!g_hasTag) {
        showMessage("Code-barres", COL_ERR, "Code-barres invalide.");
        return;
    }
    if(!g_profile.known) {
        char l2[40];
        snprintf(l2, sizeof(l2), "Type de tag: %u", (unsigned)g_profile.type_code);
        showMessage("Etiquette non reconnue", COL_WARN, "Aucun profil pour ce type.", l2,
                    "Verifiez le code-barres.");
        return;
    }
    char l1[48], l2[48], l3[48];
    snprintf(l1, sizeof(l1), "%s", g_profile.model_name ? g_profile.model_name : "?");
    if(g_profile.width && g_profile.height)
        snprintf(l2, sizeof(l2), "Ecran: %ux%u px", (unsigned)g_profile.width, (unsigned)g_profile.height);
    else
        snprintf(l2, sizeof(l2), "Ecran a segments");
    snprintf(l3, sizeof(l3), "Couleurs: %s", colorName(g_profile.color));
    const char* why = NULL;
    if(!profileSendable(g_profile, &why)) {
        showMessage("Etiquette detectee", COL_WARN, l1, l2, l3, why);
    } else {
        showMessage("Etiquette detectee", COL_OK, l1, l2, l3, "Pret pour l'envoi d'image.");
    }
}

static uint8_t pickPage() {
    uint8_t page = 0;
    for(;;) {
        clearScreen("Page de l'etiquette");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COL_FG, COL_BG);
        char s[4];
        snprintf(s, sizeof(s), "%u", page);
        tft.drawString(s, 160, 80, 7);
        drawFooter("Tourner: page 0-7  Clic: valider  Appui long: annuler");
        Ev e = waitEvent();
        if(e == EV_CW) page = (page + 1) & 7;
        else if(e == EV_CCW) page = (page + 7) & 7;
        else if(e == EV_CLICK) return page;
        else if(e == EV_LONG) return 0xFF;
    }
}

static void fileLabel(int idx, char* out, size_t n) {
    snprintf(out, n, "%s", g_files[idx]);
}

static void screenSend() {
    if(!g_hasTag) {
        showMessage("Envoyer une image", COL_WARN, "Saisissez d'abord un", "code-barres d'etiquette.");
        return;
    }
    const char* why = NULL;
    if(!profileSendable(g_profile, &why)) {
        showMessage("Envoyer une image", COL_ERR, "Etiquette non prise en charge:", why);
        return;
    }
    if(!sdInit()) {
        showMessage("Carte SD", COL_ERR, "Carte SD introuvable.", "Utilisez une SD FAT32",
                    "(SanDisk 32 Go max conseille).");
        return;
    }
    scanFiles();
    if(g_fileCount == 0) {
        showMessage("Aucun BMP", COL_WARN, "Copiez vos images BMP dans:", DROP_DIR, "puis reessayez.");
        return;
    }

    int pick = runList("Choisir une image BMP", fileLabel, g_fileCount, 0, NULL);
    if(pick < 0) return;

    uint8_t page = pickPage();
    if(page == 0xFF) return;

    char l1[56], l2[56];
    snprintf(l1, sizeof(l1), "%s", g_files[pick]);
    snprintf(l2, sizeof(l2), "Vers: %s (page %u)", g_profile.model_name ? g_profile.model_name : "?", page);
    clearScreen("Confirmer l'envoi");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawString(l1, 8, 40, 2);
    tft.drawString(l2, 8, 64, 2);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Clic = ENVOYER", 8, 96, 2);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Appui long = annuler", 8, 120, 2);
    for(;;) {
        Ev e = waitEvent();
        if(e == EV_LONG) return;
        if(e == EV_CLICK) break;
    }

    const char* err = sendBmp(g_files[pick], page);
    if(err) showMessage("Envoi", COL_ERR, err);
    else showMessage("Envoi termine", COL_OK, "Image envoyee.", "L'ecran e-ink se rafraichit",
                     "dans quelques secondes.");
}

/* --- Reglages --- */

#define SET_COUNT 6

static void settingLabel(int idx, char* out, size_t n) {
    static const char* cmp[] = {"auto", "brut", "RLE"};
    switch(idx) {
    case 0: snprintf(out, n, "Repetition donnees: x%u", g_set.dataRepeats); break;
    case 1: snprintf(out, n, "Compression: %s", cmp[g_set.compression]); break;
    case 2: snprintf(out, n, "Sens encodeur: %s", g_set.encInvert ? "inverse" : "normal"); break;
    case 3: snprintf(out, n, "Orientation ecran: %u", g_set.rotation); break;
    case 4: snprintf(out, n, "Test LED IR (camera telephone)"); break;
    default: snprintf(out, n, "Retour"); break;
    }
}

static void irLedTest() {
    clearScreen("Test LED IR");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawString("Filmez la LED IR avec un", 8, 36, 2);
    tft.drawString("telephone: elle doit apparaitre", 8, 58, 2);
    tft.drawString("allumee (lueur blanche/violette).", 8, 80, 2);
    drawFooter("Appuyez sur le bouton pour arreter");
    waitButtonRelease();
    g_cancel = false;

    if(!ir_init()) {
        showMessage("Test LED IR", COL_ERR, "Init IR (RMT) echouee.");
        return;
    }
    uint8_t pattern[24];
    memset(pattern, 0x1B, sizeof(pattern)); /* 0x1B = symboles 3,2,1,0 */
    uint32_t t0 = millis();
    while(!g_cancel && (millis() - t0) < 6000U) {
        if(!ir_transmit(pattern, sizeof(pattern), 20, 1, stopCb)) break;
    }
    ir_deinit();
    waitButtonRelease();
    showMessage("Test LED IR", COL_OK, "Test termine.", "Si la LED reste eteinte sur",
                "la camera, voir le README.");
}

static void screenSettings() {
    int sel = 0;
    for(;;) {
        sel = runList("Reglages", settingLabel, SET_COUNT, sel,
                      "Clic: changer / lancer   Appui long: retour");
        if(sel < 0 || sel == SET_COUNT - 1) return;
        switch(sel) {
        case 0: g_set.dataRepeats = (g_set.dataRepeats >= 10) ? 1 : g_set.dataRepeats + 1; break;
        case 1: g_set.compression = (g_set.compression + 1) % 3; break;
        case 2: g_set.encInvert = g_set.encInvert ? 0 : 1; break;
        case 3:
            g_set.rotation = (g_set.rotation == 3) ? 1 : 3;
            tft.setRotation(g_set.rotation);
            break;
        case 4: irLedTest(); break;
        }
        settingsSave();
    }
}

static void screenAbout() {
    showMessage("A propos", COL_FG, "TagTinker T-Embed (GPL-3.0)",
                "Base: i12bp8/TagTinker",
                "Protocole: recherches de furrtek",
                "Usage: vos propres etiquettes");
}

static void screenWarning() {
    showMessage("Avertissement", COL_WARN, "Recherche / etiquettes que vous",
                "possedez uniquement. Ne modifiez", "pas les etiquettes d'un magasin.",
                "Piles retirees = tag inutilisable.");
}

/* --- Menu principal --- */

#define MAIN_COUNT 4

static void mainLabel(int idx, char* out, size_t n) {
    switch(idx) {
    case 0:
        if(g_hasTag && g_profile.known)
            snprintf(out, n, "Tag: %s", g_profile.model_name ? g_profile.model_name : "?");
        else if(g_hasTag)
            snprintf(out, n, "Tag: type %u (inconnu)", (unsigned)g_profile.type_code);
        else
            snprintf(out, n, "Saisir le code-barres");
        break;
    case 1: snprintf(out, n, "Envoyer une image BMP"); break;
    case 2: snprintf(out, n, "Reglages"); break;
    default: snprintf(out, n, "A propos"); break;
    }
}

/* ------------------------------------------------------------------------ */
/* setup / loop                                                             */
/* ------------------------------------------------------------------------ */

void setup() {
    /* Alimentation des peripheriques + bus SPI partage: CS au repos */
    pinMode(BOARD_PWR_EN, OUTPUT);
    digitalWrite(BOARD_PWR_EN, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(DISPLAY_CS, OUTPUT);
    digitalWrite(DISPLAY_CS, HIGH);
    pinMode(DISPLAY_BL, OUTPUT);
    digitalWrite(DISPLAY_BL, HIGH);

    /* La broche IR reste au repos tant qu'on n'emet pas */
    pinMode(BOARD_IR_TX, OUTPUT);
    digitalWrite(BOARD_IR_TX, LOW);

    pinMode(ENCODER_KEY, INPUT_PULLUP);
    delay(50);

    settingsLoad();

    tft.init();
    tft.setRotation(g_set.rotation);
    tft.fillScreen(COL_BG);

    updateTagFromBarcode();
    /* Au premier demarrage le code par defaut n'est pas une vraie etiquette */
    if(prefs.getString("bc", "").length() != 17) g_hasTag = false;

    screenWarning();
}

void loop() {
    static int sel = 0;
    sel = runList("TagTinker - T-Embed CC1101", mainLabel, MAIN_COUNT, sel,
                  "Tourner: choisir   Clic: valider");
    switch(sel) {
    case 0: screenBarcode(); break;
    case 1: screenSend(); break;
    case 2: screenSettings(); break;
    case 3: screenAbout(); break;
    default: break; /* appui long au menu principal: rien */
    }
    if(sel < 0) sel = 0;
}
