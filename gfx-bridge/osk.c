/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "osk.h"
#include "osk_font.h"

#include <string.h>

/* ------------------------------------------------------------------ layout */

typedef enum { KEY_CHAR, KEY_DELETE, KEY_SHIFT, KEY_SPACE, KEY_OK } KeyKind;
typedef struct {
    KeyKind kind;
    char normal, shifted;
    float weight;
} Key;

static const char *const char_rows[4][2] = {
    {"1234567890-", "!@#$%^&*()_"},
    {"qwertyuiop/", "QWERTYUIOP\\"},
    {"asdfghjkl:'", "ASDFGHJKL;\""},
    {"zxcvbnm,.?!", "ZXCVBNM<>=+"},
};
enum { ROWS = 5, MAX_KEYS = 12 };

static unsigned row_keys(unsigned row, Key *keys)
{
    if (row < 4) {
        unsigned count = 0;
        for (const char *c = char_rows[row][0], *s = char_rows[row][1]; *c; ++c, ++s)
            keys[count++] = (Key){KEY_CHAR, *c, *s, 1.0f};
        if (row == 0)
            keys[count++] = (Key){KEY_DELETE, 0, 0, 1.6f};
        return count;
    }
    keys[0] = (Key){KEY_SHIFT, 0, 0, 2.0f};
    keys[1] = (Key){KEY_SPACE, ' ', ' ', 6.5f};
    keys[2] = (Key){KEY_OK, 0, 0, 2.5f};
    return 3;
}

/* Key rectangles in panel coordinates. */
typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    float scale;
    int margin, top, key_h, gap;
} Metrics;

static Metrics metrics(unsigned width)
{
    const float s = (float)width / 3840.0f;
    return (Metrics){s, (int)(150 * s), (int)(70 * s), (int)(150 * s), (int)(18 * s)};
}

static Rect key_rect(const Metrics *m, unsigned width, unsigned row, unsigned index)
{
    Key keys[MAX_KEYS];
    const unsigned count = row_keys(row, keys);
    float total = 0;
    for (unsigned i = 0; i < count; ++i)
        total += keys[i].weight;
    const float span = (float)(width - 2 * m->margin - (int)(count - 1) * m->gap);
    float x = (float)m->margin;
    for (unsigned i = 0; i < index; ++i)
        x += span * keys[i].weight / total + (float)m->gap;
    return (Rect){(int)x, m->top + (int)row * (m->key_h + m->gap), (int)(span * keys[index].weight / total),
                  m->key_h};
}

/* ------------------------------------------------------------------- input */

void pokopia_osk_init(PokopiaOsk *osk, const char *title, unsigned max_length, unsigned min_length)
{
    memset(osk, 0, sizeof *osk);
    osk->max_length = max_length && max_length <= POKOPIA_OSK_TEXT ? max_length : 16;
    osk->min_length = min_length <= osk->max_length ? min_length : 0;
    strncpy(osk->title, title && *title ? title : "Enter text", POKOPIA_OSK_TITLE);
    osk->row = 1;
}

static void move(PokopiaOsk *osk, int dx, int dy)
{
    Key keys[MAX_KEYS];
    if (dx) {
        const unsigned count = row_keys(osk->row, keys);
        osk->key = (osk->key + count + (unsigned)dx) % count;
    }
    if (dy) {
        const Metrics m = metrics(3840);
        const Rect from = key_rect(&m, 3840, osk->row, osk->key);
        const int center = from.x + from.w / 2;
        osk->row = (osk->row + ROWS + (unsigned)dy) % ROWS;
        const unsigned count = row_keys(osk->row, keys);
        unsigned best = 0;
        int best_distance = 1 << 30;
        for (unsigned i = 0; i < count; ++i) {
            const Rect r = key_rect(&m, 3840, osk->row, i);
            const int d = r.x + r.w / 2 - center;
            if ((d < 0 ? -d : d) < best_distance) {
                best_distance = d < 0 ? -d : d;
                best = i;
            }
        }
        osk->key = best;
    }
}

static void type_char(PokopiaOsk *osk, char c)
{
    if (osk->length < osk->max_length) {
        osk->text[osk->length++] = c;
        osk->text[osk->length] = 0;
    }
}

static void backspace(PokopiaOsk *osk)
{
    if (osk->length)
        osk->text[--osk->length] = 0;
}

PokopiaOskState pokopia_osk_input(PokopiaOsk *osk, uint64_t buttons, uint64_t previous, int lx, int ly,
                                  uint64_t now_ms, bool *changed)
{
    const uint64_t pressed = buttons & ~previous;
    *changed = false;
    /* Direction from the d-pad or the left stick, repeating while held. */
    int dx = 0, dy = 0;
    if ((buttons & POKOPIA_OSK_LEFT) || lx < -16000)
        dx = -1;
    else if ((buttons & POKOPIA_OSK_RIGHT) || lx > 16000)
        dx = 1;
    if ((buttons & POKOPIA_OSK_UP) || ly > 16000)
        dy = -1;
    else if ((buttons & POKOPIA_OSK_DOWN) || ly < -16000)
        dy = 1;
    if (dx || dy) {
        const bool fresh = dx != osk->held_dx || dy != osk->held_dy;
        if (fresh || now_ms >= osk->next_repeat_ms) {
            move(osk, dx, dy);
            osk->next_repeat_ms = now_ms + (fresh ? 380 : 90);
            *changed = true;
        }
    }
    osk->held_dx = dx;
    osk->held_dy = dy;

    if (pressed & POKOPIA_OSK_A) {
        Key keys[MAX_KEYS];
        row_keys(osk->row, keys);
        const Key *key = &keys[osk->key];
        switch (key->kind) {
        case KEY_CHAR:
        case KEY_SPACE:
            type_char(osk, osk->shift ? key->shifted : key->normal);
            break;
        case KEY_DELETE:
            backspace(osk);
            break;
        case KEY_SHIFT:
            osk->shift = !osk->shift;
            break;
        case KEY_OK:
            if (osk->length >= osk->min_length)
                return POKOPIA_OSK_DONE;
            break;
        }
        *changed = true;
    }
    if (pressed & POKOPIA_OSK_B) {
        backspace(osk);
        *changed = true;
    }
    if (pressed & POKOPIA_OSK_X) {
        osk->shift = !osk->shift;
        *changed = true;
    }
    if (pressed & POKOPIA_OSK_Y) {
        type_char(osk, ' ');
        *changed = true;
    }
    if ((pressed & POKOPIA_OSK_PLUS) && osk->length >= osk->min_length)
        return POKOPIA_OSK_DONE;
    if (pressed & POKOPIA_OSK_MINUS)
        return POKOPIA_OSK_CANCELLED;
    return POKOPIA_OSK_OPEN;
}

/* ----------------------------------------------------------------- drawing */

typedef struct {
    uint8_t *pixels;
    unsigned width, height;
    bool bgra;
} Canvas;

typedef struct {
    uint8_t r, g, b;
} Color;

static void put(const Canvas *c, int x, int y, Color color, unsigned alpha)
{
    if (x < 0 || y < 0 || (unsigned)x >= c->width || (unsigned)y >= c->height || !alpha)
        return;
    uint8_t *p = c->pixels + ((size_t)y * c->width + (size_t)x) * 4;
    const uint8_t r = c->bgra ? p[2] : p[0], b = c->bgra ? p[0] : p[2];
    const unsigned a = alpha > 255 ? 255 : alpha, ia = 255 - a;
    const uint8_t nr = (uint8_t)((color.r * a + r * ia + 127) / 255);
    const uint8_t ng = (uint8_t)((color.g * a + p[1] * ia + 127) / 255);
    const uint8_t nb = (uint8_t)((color.b * a + b * ia + 127) / 255);
    p[c->bgra ? 2 : 0] = nr;
    p[1] = ng;
    p[c->bgra ? 0 : 2] = nb;
    p[3] = 255;
}

static void fill(const Canvas *c, int x, int y, int w, int h, Color color)
{
    for (int j = y < 0 ? 0 : y; j < y + h && (unsigned)j < c->height; ++j) {
        uint8_t *row = c->pixels + (size_t)j * c->width * 4;
        for (int i = x < 0 ? 0 : x; i < x + w && (unsigned)i < c->width; ++i) {
            uint8_t *p = row + (size_t)i * 4;
            p[c->bgra ? 2 : 0] = color.r;
            p[1] = color.g;
            p[c->bgra ? 0 : 2] = color.b;
            p[3] = 255;
        }
    }
}

/* Rounded rectangle with antialiased corners. */
static void round_rect(const Canvas *c, int x, int y, int w, int h, int radius, Color color)
{
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const int cx = i < radius ? radius - i : i >= w - radius ? i - (w - radius - 1) : 0;
            const int cy = j < radius ? radius - j : j >= h - radius ? j - (h - radius - 1) : 0;
            unsigned alpha = 255;
            if (cx && cy) {
                const float d = (float)(cx * cx + cy * cy);
                const float r2 = (float)(radius * radius);
                if (d > r2 + radius)
                    continue;
                if (d > r2 - radius)
                    alpha = (unsigned)(255.0f * (r2 + radius - d) / (2.0f * radius));
            }
            put(c, x + i, y + j, color, alpha);
        }
    }
}

static int text_width(const char *text, float scale)
{
    float w = 0;
    for (const char *t = text; *t; ++t)
        if (*t >= 32 && *t < 127)
            w += osk_glyphs[*t - 32].advance * scale;
    return (int)w;
}

/* Text with its baseline at y (scaled from the atlas by sampling). */
static void draw_text(const Canvas *c, int x, int y, const char *text, float scale, Color color)
{
    float pen = (float)x;
    for (const char *t = text; *t; ++t) {
        if (*t < 32 || *t >= 127)
            continue;
        const unsigned g = (unsigned)(*t - 32);
        const int cell_w = (int)(osk_glyphs[g].width * scale), cell_h = (int)(OSK_FONT_CELL * scale);
        const int left = (int)(pen - osk_glyphs[g].origin * scale);
        const int top = y - (int)(OSK_FONT_BASELINE * scale);
        for (int j = 0; j < cell_h; ++j) {
            const unsigned sy = (unsigned)((float)j / scale);
            for (int i = 0; i < cell_w; ++i) {
                const unsigned sx = (unsigned)((float)i / scale);
                if (sx >= osk_glyphs[g].width || sy >= OSK_FONT_CELL)
                    continue;
                put(c, left + i, top + j, color,
                    osk_font_atlas[sy * OSK_FONT_ATLAS_WIDTH + osk_glyphs[g].x + sx]);
            }
        }
        pen += osk_glyphs[g].advance * scale;
    }
}

/* A small dark disc with a white PlayStation symbol, for the button legend. */
static void button_icon(const Canvas *c, int cx, int cy, int r, char symbol)
{
    const Color dark = {60, 62, 72}, white = {255, 255, 255};
    for (int j = -r - 1; j <= r + 1; ++j)
        for (int i = -r - 1; i <= r + 1; ++i) {
            const float d = (float)(i * i + j * j);
            if (d <= (float)(r * r))
                put(c, cx + i, cy + j, dark, 255);
            else if (d <= (float)((r + 1) * (r + 1)))
                put(c, cx + i, cy + j, dark, 128);
        }
    const int k = r / 2, w = r / 6 > 2 ? r / 6 : 2;
    for (int t = -k; t <= k; ++t)
        for (int s = -w / 2; s <= w / 2; ++s) {
            switch (symbol) {
            case 'x':
                put(c, cx + t + s, cy + t, white, 255);
                put(c, cx + t + s, cy - t, white, 255);
                break;
            case 's':
                put(c, cx + t, cy - k + s, white, 255);
                put(c, cx + t, cy + k + s, white, 255);
                put(c, cx - k + s, cy + t, white, 255);
                put(c, cx + k + s, cy + t, white, 255);
                break;
            default:
                break;
            }
        }
    if (symbol == 'o') {
        for (int j = -k - w; j <= k + w; ++j)
            for (int i = -k - w; i <= k + w; ++i) {
                const float d = (float)(i * i + j * j);
                if (d <= (float)((k + w / 2) * (k + w / 2)) && d >= (float)((k - w / 2) * (k - w / 2)))
                    put(c, cx + i, cy + j, white, 255);
            }
    }
    if (symbol == 't') {
        const int top = cy - k, base = cy + k * 3 / 4;
        for (int y = top; y <= base; ++y) {
            const int half = (y - top) * k / (base - top > 0 ? base - top : 1);
            for (int s = -w / 2; s <= w / 2; ++s) {
                put(c, cx - half + s, y, white, 255);
                put(c, cx + half + s, y, white, 255);
            }
        }
        for (int x = -k; x <= k; ++x)
            for (int s = -w / 2; s <= w / 2; ++s)
                put(c, cx + x, base + s, white, 255);
    }
}

void pokopia_osk_draw(const PokopiaOsk *osk, uint8_t *pixels, unsigned width, unsigned top, unsigned bottom,
                      bool bgra, uint64_t now_ms)
{
    const Metrics m = metrics(width);
    const float s = m.scale;
    const Color band = {38, 52, 128}, white = {255, 255, 255}, panel = {214, 217, 224}, key_bg = {247, 248, 250},
                special_bg = {196, 200, 210}, ink = {52, 54, 64}, select = {0, 168, 255}, ok_bg = {48, 112, 232},
                muted = {150, 154, 164};

    /* Title band: the prompt, the text field and its count. */
    const Canvas head = {pixels, width, top, bgra};
    fill(&head, 0, 0, (int)width, (int)top, band);
    draw_text(&head, (int)(140 * s), (int)(110 * s), osk->title, 0.8f * s, white);
    const int field_x = (int)(width / 2 - 900 * s), field_w = (int)(1800 * s), field_y = (int)(260 * s);
    draw_text(&head, field_x + (int)(20 * s), field_y - (int)(20 * s), osk->text, 1.0f * s, white);
    if ((now_ms / 500) % 2 == 0) {
        const int cursor_x = field_x + (int)(20 * s) + text_width(osk->text, s) + (int)(6 * s);
        fill(&head, cursor_x, field_y - (int)(80 * s), (int)(5 * s) + 1, (int)(90 * s), white);
    }
    fill(&head, field_x, field_y + (int)(10 * s), field_w, (int)(5 * s) + 1, white);
    char count[24];
    unsigned n = osk->length, digits[8], d = 0;
    do {
        digits[d++] = n % 10;
        n /= 10;
    } while (n && d < 8);
    unsigned at = 0;
    while (d)
        count[at++] = (char)('0' + digits[--d]);
    count[at++] = '/';
    n = osk->max_length;
    do {
        digits[d++] = n % 10;
        n /= 10;
    } while (n && d < 8);
    while (d)
        count[at++] = (char)('0' + digits[--d]);
    count[at] = 0;
    draw_text(&head, field_x + field_w - text_width(count, 0.6f * s), field_y + (int)(70 * s), count, 0.6f * s, white);

    /* Keyboard panel. */
    const Canvas kb = {pixels + (size_t)width * top * 4, width, bottom, bgra};
    fill(&kb, 0, 0, (int)width, (int)bottom, panel);
    const int radius = (int)(18 * s);
    for (unsigned row = 0; row < ROWS; ++row) {
        Key keys[MAX_KEYS];
        const unsigned count_keys = row_keys(row, keys);
        for (unsigned i = 0; i < count_keys; ++i) {
            const Rect r = key_rect(&m, width, row, i);
            const bool selected = row == osk->row && i == osk->key;
            const bool ok_enabled = osk->length >= osk->min_length;
            Color bg = keys[i].kind == KEY_CHAR || keys[i].kind == KEY_SPACE ? key_bg : special_bg;
            Color fg = ink;
            if (keys[i].kind == KEY_OK) {
                bg = ok_enabled ? ok_bg : special_bg;
                fg = ok_enabled ? white : muted;
            }
            if (keys[i].kind == KEY_SHIFT && osk->shift)
                bg = (Color){150, 190, 255};
            if (selected) {
                round_rect(&kb, r.x - (int)(8 * s), r.y - (int)(8 * s), r.w + (int)(16 * s), r.h + (int)(16 * s),
                           radius + (int)(8 * s), select);
            }
            round_rect(&kb, r.x, r.y, r.w, r.h, radius, bg);
            char label[8] = {0};
            const char *text = label;
            switch (keys[i].kind) {
            case KEY_CHAR:
                label[0] = osk->shift ? keys[i].shifted : keys[i].normal;
                break;
            case KEY_DELETE:
                text = "Del";
                break;
            case KEY_SHIFT:
                text = "Shift";
                break;
            case KEY_SPACE:
                text = "Space";
                break;
            case KEY_OK:
                text = "OK";
                break;
            }
            const float ts = (keys[i].kind == KEY_CHAR ? 0.95f : 0.75f) * s;
            draw_text(&kb, r.x + (r.w - text_width(text, ts)) / 2, r.y + r.h / 2 + (int)(24 * s), text, ts, fg);
        }
    }

    /* Button legend along the bottom. */
    const int legend_y = (int)bottom - (int)(95 * s), icon_r = (int)(30 * s);
    /* R92: the button acting as A (Circle, or Cross when swapped) selects. */
    const bool cross = pokopia_settings.confirm == POKOPIA_CONFIRM_CROSS;
    const struct {
        char symbol;
        const char *label;
    } legend[] = {{cross ? 'x' : 'o', "Select"}, {cross ? 'o' : 'x', "Delete"}, {'t', "Shift"}, {'s', "Space"},
                  {0, "OPTIONS  OK"}, {0, "Touch pad  Cancel"}};
    int x = m.margin;
    for (unsigned i = 0; i < sizeof legend / sizeof legend[0]; ++i) {
        if (legend[i].symbol) {
            button_icon(&kb, x + icon_r, legend_y, icon_r, legend[i].symbol);
            x += 2 * icon_r + (int)(16 * s);
        }
        draw_text(&kb, x, legend_y + (int)(20 * s), legend[i].label, 0.6f * s, ink);
        x += text_width(legend[i].label, 0.6f * s) + (int)(80 * s);
    }
}

/* ------------------------------------------------------------ R92: port menu */

void pokopia_menu_init(PokopiaMenu *menu, const PokopiaSettings *settings)
{
    *menu = (PokopiaMenu){.values = *settings};
}

/* Moves a value by step (-1/+1); toggles flip either way. */
static void menu_change(PokopiaMenu *menu, int step)
{
    PokopiaSettings *v = &menu->values;
    switch (menu->row) {
    case POKOPIA_MENU_FPS:
        v->fps_counter = !v->fps_counter;
        break;
    case POKOPIA_MENU_DEBUG:
        v->debug_info = !v->debug_info;
        break;
    case POKOPIA_MENU_PROMPTS:
        v->prompts = v->prompts == POKOPIA_PROMPTS_PLAYSTATION ? POKOPIA_PROMPTS_NINTENDO : POKOPIA_PROMPTS_PLAYSTATION;
        break;
    case POKOPIA_MENU_CONFIRM:
        v->confirm = v->confirm == POKOPIA_CONFIRM_CIRCLE ? POKOPIA_CONFIRM_CROSS : POKOPIA_CONFIRM_CIRCLE;
        break;
    case POKOPIA_MENU_CAP:
        v->frame_cap = v->frame_cap == 60 ? 30 : 60;
        break;
    case POKOPIA_MENU_UPSCALER:
        v->upscaler = v->upscaler == POKOPIA_UPSCALER_FSR ? POKOPIA_UPSCALER_BILINEAR : POKOPIA_UPSCALER_FSR;
        break;
    case POKOPIA_MENU_SHARPNESS: {
        int level = (int)v->sharpness + step;
        if (level < 0)
            level = step < 0 ? 0 : POKOPIA_SHARPNESS_LEVELS - 1;
        if (level >= POKOPIA_SHARPNESS_LEVELS)
            level = 0; /* A cycles round; Right stops at the top below */
        v->sharpness = (unsigned)level;
        break;
    }
    default:
        break;
    }
}

PokopiaMenuState pokopia_menu_input(PokopiaMenu *menu, uint64_t buttons, uint64_t previous, int lx, int ly,
                                    uint64_t now_ms, bool *changed)
{
    const uint64_t pressed = buttons & ~previous;
    *changed = false;
    if (pressed & (POKOPIA_OSK_B | POKOPIA_OSK_PLUS | POKOPIA_OSK_MINUS))
        return POKOPIA_MENU_CLOSED;
    int dx = 0, dy = 0;
    if ((buttons & POKOPIA_OSK_LEFT) || lx < -16000)
        dx = -1;
    else if ((buttons & POKOPIA_OSK_RIGHT) || lx > 16000)
        dx = 1;
    if ((buttons & POKOPIA_OSK_UP) || ly > 16000)
        dy = -1;
    else if ((buttons & POKOPIA_OSK_DOWN) || ly < -16000)
        dy = 1;
    if (dx || dy) {
        const bool fresh = dx != menu->held_dx || dy != menu->held_dy;
        if (fresh || now_ms >= menu->next_repeat_ms) {
            if (dy)
                menu->row = (menu->row + POKOPIA_MENU_ROWS + (unsigned)(dy + POKOPIA_MENU_ROWS)) % POKOPIA_MENU_ROWS;
            else if (menu->row != POKOPIA_MENU_CLOSE &&
                     !(menu->row == POKOPIA_MENU_SHARPNESS &&
                       ((dx > 0 && menu->values.sharpness + 1 >= POKOPIA_SHARPNESS_LEVELS) ||
                        (dx < 0 && menu->values.sharpness == 0))))
                menu_change(menu, dx);
            menu->next_repeat_ms = now_ms + (fresh ? 380 : (dy ? 120 : 250));
            *changed = true;
        }
    }
    menu->held_dx = dx;
    menu->held_dy = dy;
    if (pressed & POKOPIA_OSK_A) {
        if (menu->row == POKOPIA_MENU_CLOSE)
            return POKOPIA_MENU_CLOSED;
        menu_change(menu, 1);
        *changed = true;
    }
    return POKOPIA_MENU_OPEN;
}

static const char *menu_value(const PokopiaSettings *v, unsigned row, char *buffer, size_t size)
{
    switch (row) {
    case POKOPIA_MENU_FPS:
        return v->fps_counter ? "On" : "Off";
    case POKOPIA_MENU_DEBUG:
        return v->debug_info ? "On" : "Off";
    case POKOPIA_MENU_PROMPTS:
        return v->prompts == POKOPIA_PROMPTS_PLAYSTATION ? "PlayStation" : "Nintendo";
    case POKOPIA_MENU_CONFIRM:
        return v->confirm == POKOPIA_CONFIRM_CIRCLE ? "Circle" : "Cross";
    case POKOPIA_MENU_CAP:
        return v->frame_cap == 30 ? "30 fps" : "60 fps";
    case POKOPIA_MENU_UPSCALER:
        return v->upscaler == POKOPIA_UPSCALER_FSR ? "FSR 1" : "Bilinear";
    case POKOPIA_MENU_SHARPNESS: {
        static const char *const names[POKOPIA_SHARPNESS_LEVELS] = {"Soft", "Low", "Medium", "High", "Max"};
        (void)buffer;
        (void)size;
        return names[v->sharpness < POKOPIA_SHARPNESS_LEVELS ? v->sharpness : 3];
    }
    default:
        return "";
    }
}

void pokopia_menu_draw(const PokopiaMenu *menu, uint8_t *pixels, unsigned width, unsigned height, bool bgra)
{
    /* Laid out for a 1600-pixel-wide panel and scaled. */
    const float s = (float)width / 1600.0f;
    const Color band = {38, 52, 128}, white = {255, 255, 255}, panel = {214, 217, 224}, row_bg = {247, 248, 250},
                ink = {52, 54, 64}, select = {0, 168, 255}, muted = {150, 154, 164}, value_bg = {228, 231, 238},
                close_bg = {48, 112, 232}, frame = {24, 32, 80};
    const Canvas c = {pixels, width, height, bgra};
    fill(&c, 0, 0, (int)width, (int)height, frame);
    const int border = (int)(6 * s) + 1, head = (int)(150 * s);
    fill(&c, border, border, (int)width - 2 * border, head, band);
    draw_text(&c, (int)(60 * s), (int)(100 * s), "Port settings", 0.9f * s, white);
    const char *const hint = "PS5 port only - the game's own settings are unchanged";
    draw_text(&c, (int)width - (int)(60 * s) - text_width(hint, 0.45f * s), (int)(98 * s), hint, 0.45f * s,
              (Color){190, 200, 240});
    fill(&c, border, border + head, (int)width - 2 * border, (int)height - 2 * border - head, panel);

    static const char *const labels[POKOPIA_MENU_ROWS] = {
        "FPS counter", "Debug info", "Button prompts", "Confirm button", "Frame rate cap", "Upscaler",
        "FSR sharpness", "Close",
    };
    const int row_h = (int)(104 * s), gap = (int)(14 * s), x = (int)(60 * s), w = (int)width - 2 * x;
    int y = border + head + (int)(40 * s);
    const int radius = (int)(16 * s);
    for (unsigned row = 0; row < POKOPIA_MENU_ROWS; ++row) {
        const bool selected = row == menu->row;
        const bool closing = row == POKOPIA_MENU_CLOSE;
        const bool inactive = row == POKOPIA_MENU_SHARPNESS && menu->values.upscaler != POKOPIA_UPSCALER_FSR;
        if (closing)
            y += (int)(16 * s);
        if (selected)
            round_rect(&c, x - (int)(8 * s), y - (int)(8 * s), w + (int)(16 * s), row_h + (int)(16 * s),
                       radius + (int)(8 * s), select);
        if (closing) {
            const int cw = (int)(360 * s), cx = x + (w - cw) / 2;
            round_rect(&c, cx, y, cw, row_h, radius, close_bg);
            draw_text(&c, cx + (cw - text_width(labels[row], 0.7f * s)) / 2, y + row_h / 2 + (int)(22 * s), labels[row],
                      0.7f * s, white);
            if (selected) /* the highlight is the button itself */
                ;
            y += row_h + gap;
            continue;
        }
        round_rect(&c, x, y, w, row_h, radius, row_bg);
        draw_text(&c, x + (int)(36 * s), y + row_h / 2 + (int)(22 * s), labels[row], 0.7f * s, inactive ? muted : ink);
        char buffer[32];
        const char *value = menu_value(&menu->values, row, buffer, sizeof buffer);
        const int vw = (int)(420 * s), vx = x + w - vw - (int)(24 * s), vy = y + (int)(14 * s), vh = row_h - (int)(28 * s);
        round_rect(&c, vx, vy, vw, vh, (int)(12 * s), value_bg);
        const Color value_ink = inactive ? muted : ink;
        draw_text(&c, vx + (vw - text_width(value, 0.65f * s)) / 2, vy + vh / 2 + (int)(20 * s), value, 0.65f * s,
                  value_ink);
        if (selected) {
            draw_text(&c, vx + (int)(18 * s), vy + vh / 2 + (int)(20 * s), "<", 0.65f * s, value_ink);
            draw_text(&c, vx + vw - (int)(18 * s) - text_width(">", 0.65f * s), vy + vh / 2 + (int)(20 * s), ">",
                      0.65f * s, value_ink);
        }
        y += row_h + gap;
    }

    /* Legend: the button acting as A changes, the one acting as B closes. */
    const bool cross = menu->values.confirm == POKOPIA_CONFIRM_CROSS;
    const int legend_y = (int)height - border - (int)(60 * s), icon_r = (int)(26 * s);
    int lx = x;
    button_icon(&c, lx + icon_r, legend_y, icon_r, cross ? 'x' : 'o');
    lx += 2 * icon_r + (int)(14 * s);
    draw_text(&c, lx, legend_y + (int)(18 * s), "Change", 0.55f * s, ink);
    lx += text_width("Change", 0.55f * s) + (int)(60 * s);
    button_icon(&c, lx + icon_r, legend_y, icon_r, cross ? 'o' : 'x');
    lx += 2 * icon_r + (int)(14 * s);
    draw_text(&c, lx, legend_y + (int)(18 * s), "Close", 0.55f * s, ink);
    lx += text_width("Close", 0.55f * s) + (int)(60 * s);
    draw_text(&c, lx, legend_y + (int)(18 * s), "Left/Right  Change    Up/Down  Select", 0.55f * s, ink);
}
