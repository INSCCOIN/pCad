/* pCad — keyboard 2D CAD for SharkDeck framebuffer. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXE 512
#define NAME "pCad"

enum { T_LINE, T_RECT, T_CIRCLE };
enum { TOOL_SEL, TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_N };
static const char *TOOL[] = {"select", "line", "rect", "circle"};

typedef struct {
    int type;
    double x1, y1, x2, y2;
} Ent;

static int fb = -1;
static unsigned char *map;
static size_t maplen;
static unsigned W, H, BPP, LINE;
static struct termios oldt;
static int raw_on;

static Ent ents[MAXE];
static int nent, sel = -1;
static int tool = TOOL_LINE;
static int grid_on = 1, snap_on = 1, ortho = 1;
static double snap = 0.25;
static double vx, vy, zoom = 28; /* world units per... zoom = px per unit */
static double cx, cy;            /* cursor world */
static int drawing, have_p0;
static double p0x, p0y;
static char status[96] = "arrows move  l line  r rect  c circ  enter point";
static char pathfile[256];

static uint16_t C_BG, C_GRID, C_LINE, C_SEL, C_CUR, C_BAR, C_TXT, C_DIM, C_HI;

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void px(int x, int y, uint16_t c)
{
    unsigned char *p;
    if ((unsigned)x >= W || (unsigned)y >= H)
        return;
    p = map + (size_t)y * LINE + (size_t)x * (BPP / 8);
    if (BPP == 16)
        ((uint16_t *)p)[0] = c;
    else if (BPP == 32) {
        p[0] = (unsigned char)((c & 0x1f) << 3);
        p[1] = (unsigned char)(((c >> 5) & 0x3f) << 2);
        p[2] = (unsigned char)(((c >> 11) & 0x1f) << 3);
        p[3] = 0;
    }
}

static void clear(uint16_t c)
{
    unsigned y, x;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            px(x, y, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px(x + i, y + j, c);
}

static const unsigned char FONT[96][5] = {
    {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0},{0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},
    {8,8,0x3e,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2},{0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},
    {2,4,8,0x10,0x20},{0,0x41,0x41,0x7f,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},
    {8,0x7e,9,1,2},{0x0c,0x52,0x52,0x52,0x3e},{0x7f,8,4,4,0x78},
    {0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},{0x7f,0x10,0x28,0x44,0},
    {0,0x41,0x7f,0x40,0},{0x7c,4,0x18,4,0x78},{0x7c,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7c},
    {0x7c,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},
};

static void glyph(int x, int y, char ch, uint16_t c)
{
    int gx, gy;
    unsigned char col;
    if ((unsigned char)ch < 32)
        ch = '?';
    if ((unsigned char)ch > 126)
        ch = '?';
    for (gx = 0; gx < 5; gx++) {
        col = FONT[ch - 32][gx];
        for (gy = 0; gy < 7; gy++)
            if (col & (1 << gy))
                px(x + gx, y + gy, c);
    }
}

static void text(int x, int y, const char *s, uint16_t c)
{
    while (*s) {
        glyph(x, y, *s++, c);
        x += 6;
    }
}

static int world_to_sx(double x) { return (int)((W * 0.5) + (x - vx) * zoom); }
static int world_to_sy(double y) { return (int)((H * 0.5) - (y - vy) * zoom); }

static double snapx(double x)
{
    if (!snap_on)
        return x;
    return floor(x / snap + 0.5) * snap;
}

static void circle(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 0;
    if (r < 1)
        return;
    while (x >= y) {
        px(cx + x, cy + y, c);
        px(cx + y, cy + x, c);
        px(cx - y, cy + x, c);
        px(cx - x, cy + y, c);
        px(cx - x, cy - y, c);
        px(cx - y, cy - x, c);
        px(cx + y, cy - x, c);
        px(cx + x, cy - y, c);
        y++;
        if (err <= 0)
            err += 2 * y + 1;
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

static void draw_ent(const Ent *e, uint16_t c)
{
    int x1 = world_to_sx(e->x1), y1 = world_to_sy(e->y1);
    int x2 = world_to_sx(e->x2), y2 = world_to_sy(e->y2);
    if (e->type == T_LINE)
        line(x1, y1, x2, y2, c);
    else if (e->type == T_RECT) {
        line(x1, y1, x2, y1, c);
        line(x2, y1, x2, y2, c);
        line(x2, y2, x1, y2, c);
        line(x1, y2, x1, y1, c);
    } else if (e->type == T_CIRCLE) {
        double r = hypot(e->x2 - e->x1, e->y2 - e->y1);
        circle(x1, y1, (int)(r * zoom + 0.5), c);
    }
}

static void draw_grid(void)
{
    double g = snap > 0 ? snap : 0.25;
    double x, y;
    int sx, sy;
    if (!grid_on)
        return;
    while (g * zoom < 8)
        g *= 2;
    for (x = floor((vx - W / (2 * zoom)) / g) * g; x < vx + W / (2 * zoom); x += g) {
        sx = world_to_sx(x);
        line(sx, 12, sx, (int)H - 22, C_GRID);
    }
    for (y = floor((vy - H / (2 * zoom)) / g) * g; y < vy + H / (2 * zoom); y += g) {
        sy = world_to_sy(y);
        line(0, sy, (int)W, sy, C_GRID);
    }
    line(world_to_sx(0), 12, world_to_sx(0), (int)H - 22, C_DIM);
    line(0, world_to_sy(0), (int)W, world_to_sy(0), C_DIM);
}

static void add_ent(int type, double x1, double y1, double x2, double y2)
{
    if (nent >= MAXE)
        return;
    ents[nent].type = type;
    ents[nent].x1 = x1;
    ents[nent].y1 = y1;
    ents[nent].x2 = x2;
    ents[nent].y2 = y2;
    sel = nent++;
}

static void cancel_draw(void)
{
    drawing = 0;
    have_p0 = 0;
}

static int hit_ent(double x, double y)
{
    int i, best = -1;
    double bd = 1e9, t = 12.0 / zoom;
    for (i = 0; i < nent; i++) {
        Ent *e = &ents[i];
        double d = 1e9;
        if (e->type == T_CIRCLE) {
            double r = hypot(e->x2 - e->x1, e->y2 - e->y1);
            d = fabs(hypot(x - e->x1, y - e->y1) - r);
        } else if (e->type == T_LINE) {
            double dx = e->x2 - e->x1, dy = e->y2 - e->y1;
            double len2 = dx * dx + dy * dy;
            double u = len2 > 0 ? ((x - e->x1) * dx + (y - e->y1) * dy) / len2 : 0;
            if (u < 0)
                u = 0;
            if (u > 1)
                u = 1;
            d = hypot(x - (e->x1 + u * dx), y - (e->y1 + u * dy));
        } else {
            double xa = e->x1 < e->x2 ? e->x1 : e->x2;
            double xb = e->x1 > e->x2 ? e->x1 : e->x2;
            double ya = e->y1 < e->y2 ? e->y1 : e->y2;
            double yb = e->y1 > e->y2 ? e->y1 : e->y2;
            if (x >= xa - t && x <= xb + t && y >= ya - t && y <= yb + t) {
                double dx = x < xa ? xa - x : x > xb ? x - xb : 0;
                double dy = y < ya ? ya - y : y > yb ? y - yb : 0;
                d = hypot(dx, dy);
                if (d == 0)
                    d = fmin(fmin(x - xa, xb - x), fmin(y - ya, yb - y));
            }
        }
        if (d < t && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void ensure_dir(const char *path)
{
    char tmp[256], *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void save_file(void)
{
    FILE *f;
    int i;
    ensure_dir(pathfile);
    f = fopen(pathfile, "w");
    if (!f) {
        snprintf(status, sizeof status, "save fail");
        return;
    }
    fprintf(f, "# pCad\n");
    for (i = 0; i < nent; i++)
        fprintf(f, "%d %.6f %.6f %.6f %.6f\n", ents[i].type, ents[i].x1, ents[i].y1, ents[i].x2, ents[i].y2);
    fclose(f);
    snprintf(status, sizeof status, "saved %d", nent);
}

static void load_file(void)
{
    FILE *f;
    char line[256];
    f = fopen(pathfile, "r");
    if (!f) {
        snprintf(status, sizeof status, "new drawing");
        return;
    }
    nent = 0;
    while (fgets(line, sizeof line, f) && nent < MAXE) {
        int t;
        double a, b, c, d;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%d %lf %lf %lf %lf", &t, &a, &b, &c, &d) == 5) {
            ents[nent].type = t;
            ents[nent].x1 = a;
            ents[nent].y1 = b;
            ents[nent].x2 = c;
            ents[nent].y2 = d;
            nent++;
        }
    }
    fclose(f);
    snprintf(status, sizeof status, "loaded %d", nent);
}

static void render(void)
{
    int i, sx, sy;
    char bar[128], coord[64];
    clear(C_BG);
    draw_grid();
    for (i = 0; i < nent; i++)
        draw_ent(&ents[i], i == sel ? C_SEL : C_LINE);
    if (have_p0) {
        Ent tmp;
        tmp.type = tool == TOOL_CIRCLE ? T_CIRCLE : tool == TOOL_RECT ? T_RECT : T_LINE;
        tmp.x1 = p0x;
        tmp.y1 = p0y;
        tmp.x2 = cx;
        tmp.y2 = cy;
        if (ortho && tmp.type == T_LINE) {
            if (fabs(cx - p0x) >= fabs(cy - p0y))
                tmp.y2 = p0y;
            else
                tmp.x2 = p0x;
        }
        draw_ent(&tmp, C_CUR);
    }
    sx = world_to_sx(cx);
    sy = world_to_sy(cy);
    line(sx - 6, sy, sx + 6, sy, C_CUR);
    line(sx, sy - 6, sx, sy + 6, C_CUR);

    fill_rect(0, 0, (int)W, 12, C_BAR);
    snprintf(bar, sizeof bar, "pCad  %s  snap%s  grid%s  ortho%s  %d",
             TOOL[tool], snap_on ? "*" : "-", grid_on ? "*" : "-", ortho ? "*" : "-", nent);
    text(2, 3, bar, C_HI);

    fill_rect(0, (int)H - 20, (int)W, 20, C_BAR);
    snprintf(coord, sizeof coord, "x %.2f  y %.2f", cx, cy);
    text(2, (int)H - 17, coord, C_TXT);
    text(2, (int)H - 9, status, C_TXT);
}

static void place_point(void)
{
    double x = snapx(cx), y = snapx(cy);
    cx = x;
    cy = y;
    if (tool == TOOL_SEL) {
        sel = hit_ent(x, y);
        snprintf(status, sizeof status, sel >= 0 ? "selected %d" : "none", sel);
        return;
    }
    if (!have_p0) {
        p0x = x;
        p0y = y;
        have_p0 = 1;
        drawing = 1;
        snprintf(status, sizeof status, "second point");
        return;
    }
    if (ortho && tool == TOOL_LINE) {
        if (fabs(x - p0x) >= fabs(y - p0y))
            y = p0y;
        else
            x = p0x;
    }
    if (tool == TOOL_LINE)
        add_ent(T_LINE, p0x, p0y, x, y);
    else if (tool == TOOL_RECT)
        add_ent(T_RECT, p0x, p0y, x, y);
    else if (tool == TOOL_CIRCLE)
        add_ent(T_CIRCLE, p0x, p0y, x, y);
    have_p0 = 0;
    drawing = 0;
    snprintf(status, sizeof status, "ok  %d ents", nent);
}

static void raw_term(int on)
{
    struct termios t;
    if (on) {
        tcgetattr(0, &oldt);
        t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        raw_on = 1;
    } else if (raw_on) {
        tcsetattr(0, TCSANOW, &oldt);
        raw_on = 0;
    }
}

static int fb_open(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0)
        return -1;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) < 0)
        return -1;
    if (ioctl(fb, FBIOGET_FSCREENINFO, &f) < 0)
        return -1;
    W = v.xres;
    H = v.yres;
    BPP = v.bits_per_pixel;
    LINE = f.line_length;
    maplen = f.smem_len ? f.smem_len : (size_t)LINE * H;
    map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    return map == MAP_FAILED ? -1 : 0;
}

int main(void)
{
    int running = 1;
    double step;

    if (fb_open() < 0) {
        fprintf(stderr, "pCad: /dev/fb0: %s\n", strerror(errno));
        return 1;
    }
    C_BG = rgb565(12, 14, 18);
    C_GRID = rgb565(28, 32, 40);
    C_LINE = rgb565(210, 214, 220);
    C_SEL = rgb565(80, 200, 255);
    C_CUR = rgb565(255, 180, 40);
    C_BAR = rgb565(22, 24, 30);
    C_TXT = rgb565(200, 205, 210);
    C_DIM = rgb565(70, 80, 95);
    C_HI = rgb565(140, 220, 180);

    if (!access("/home/working", W_OK))
        snprintf(pathfile, sizeof pathfile, "/home/working/pcad/drawing.pcd");
    else
        snprintf(pathfile, sizeof pathfile, "%s/drawing.pcd", getenv("HOME") ? getenv("HOME") : "/tmp");

    raw_term(1);
    load_file();
    render();

    while (running) {
        unsigned char ch = 0;
        fd_set rf;
        struct timeval tv = {0, 40000};
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        if (select(1, &rf, NULL, NULL, &tv) > 0)
            if (read(0, &ch, 1) != 1)
                ch = 0;
        if (!ch)
            continue;
        step = snap_on ? snap : 0.1;
        if (ch == 0x1b) {
            unsigned char seq[8] = {0};
            struct timeval t2 = {0, 80000};
            FD_ZERO(&rf);
            FD_SET(0, &rf);
            if (select(1, &rf, NULL, NULL, &t2) > 0)
                read(0, seq, 6);
            if (seq[0] == '[' && seq[1] == 'A')
                cy += step;
            else if (seq[0] == '[' && seq[1] == 'B')
                cy -= step;
            else if (seq[0] == '[' && seq[1] == 'C')
                cx += step;
            else if (seq[0] == '[' && seq[1] == 'D')
                cx -= step;
            else {
                cancel_draw();
                snprintf(status, sizeof status, "cancel");
            }
            if (snap_on) {
                cx = snapx(cx);
                cy = snapx(cy);
            }
            render();
            continue;
        }
        if (ch == 'q')
            running = 0;
        else if (ch == 'l' || ch == 'L') {
            tool = TOOL_LINE;
            cancel_draw();
            snprintf(status, sizeof status, "LINE first point");
        } else if (ch == 'r' || ch == 'R') {
            tool = TOOL_RECT;
            cancel_draw();
            snprintf(status, sizeof status, "RECT first corner");
        } else if (ch == 'c' || ch == 'C') {
            tool = TOOL_CIRCLE;
            cancel_draw();
            snprintf(status, sizeof status, "CIRCLE center then rim");
        } else if (ch == 'v' || ch == 'V') {
            tool = TOOL_SEL;
            cancel_draw();
            snprintf(status, sizeof status, "SELECT");
        } else if (ch == 10 || ch == 13 || ch == ' ')
            place_point();
        else if (ch == 8 || ch == 127) {
            if (nent) {
                nent--;
                sel = nent ? nent - 1 : -1;
                snprintf(status, sizeof status, "undo last");
            }
        } else if (ch == 'd' || ch == 'D') {
            if (sel >= 0 && sel < nent) {
                memmove(&ents[sel], &ents[sel + 1], (nent - sel - 1) * sizeof(Ent));
                nent--;
                sel = -1;
                snprintf(status, sizeof status, "deleted");
            }
        } else if (ch == 'g' || ch == 'G')
            grid_on ^= 1;
        else if (ch == 's' || ch == 'S') {
            snap_on ^= 1;
            snprintf(status, sizeof status, snap_on ? "snap on" : "snap off");
        } else if (ch == 'o' || ch == 'O') {
            ortho ^= 1;
            snprintf(status, sizeof status, ortho ? "ortho on" : "ortho off");
        } else if (ch == '+' || ch == '=')
            zoom *= 1.15;
        else if (ch == '-' || ch == '_') {
            zoom /= 1.15;
            if (zoom < 4)
                zoom = 4;
        } else if (ch == 'w' || ch == 'W')
            save_file();
        else if (ch == 'h')
            vx -= 4 / zoom;
        else if (ch == ';')
            vx += 4 / zoom;
        else if (ch == 'k')
            vy += 4 / zoom;
        else if (ch == 'j')
            vy -= 4 / zoom;
        else if (ch == '0') {
            vx = vy = 0;
            zoom = 28;
        }
        render();
    }
    save_file();
    raw_term(0);
    munmap(map, maplen);
    close(fb);
    return 0;
}
