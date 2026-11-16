#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define NUM_STARS 90
#define NUM_BARS  3

typedef struct {
    char ch;
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
} Cell;

typedef struct {
    float x, y, z;
} Star;

static int term_w = 80;
static int term_h = 24;
static int canvas_h = 22;

static Cell  *grid = NULL;
static float *zbuf = NULL;
static char  *out_buf = NULL;
static Star   stars[NUM_STARS];

#define GRID(y, x) grid[(y) * term_w + (x)]
#define ZBUF(y, x) zbuf[(y) * term_w + (x)]

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t needs_resize = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
}

static void handle_sigwinch(int sig)
{
    (void)sig;
    needs_resize = 1;
}

static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// Smooth HSV-like rainbow helper for the sine scroller and torus tint
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float hp = fmodf(h * 6.0f, 6.0f);
    if (hp < 0.0f) hp += 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0.0f, gf = 0.0f, bf = 0.0f;

    int region = (int)hp;
    switch (region) {
        case 0: rf = c; gf = x; bf = 0; break;
        case 1: rf = x; gf = c; bf = 0; break;
        case 2: rf = 0; gf = c; bf = x; break;
        case 3: rf = 0; gf = x; bf = c; break;
        case 4: rf = x; gf = 0; bf = c; break;
        default:rf = c; gf = 0; bf = x; break;
    }

    *r = clamp_u8((int)((rf + m) * 255.0f));
    *g = clamp_u8((int)((gf + m) * 255.0f));
    *b = clamp_u8((int)((bf + m) * 255.0f));
}

static void init_stars(void)
{
    for (int i = 0; i < NUM_STARS; i++) {
        stars[i].x = ((float)(rand() % 2000) / 1000.0f - 1.0f) * 40.0f;
        stars[i].y = ((float)(rand() % 2000) / 1000.0f - 1.0f) * 20.0f;
        stars[i].z = 1.0f + ((float)(rand() % 1000) / 1000.0f) * 30.0f;
    }
}

static void resize_viewport(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        term_w = (ws.ws_col >= 40) ? ws.ws_col : 40;
        term_h = (ws.ws_row >= 14) ? ws.ws_row : 14;
    } else {
        term_w = 80;
        term_h = 24;
    }

    // Reserve 1 top row for header and 1 bottom newline margin
    canvas_h = term_h - 2;

    free(grid);
    free(zbuf);
    free(out_buf);

    grid    = calloc((size_t)(term_w * canvas_h), sizeof(Cell));
    zbuf    = malloc((size_t)(term_w * canvas_h) * sizeof(float));
    out_buf = malloc((size_t)(term_w * canvas_h * 48 + 1024));

    if (!grid || !zbuf || !out_buf) {
        fprintf(stderr, "Memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }

    printf("\033[2J");
    needs_resize = 0;
}

int main(void)
{
    const char *pname    = "ascii-copper-torus";
    const char *pversion = "1.0";
    const char *scroller =
        "   *** GREETINGS TO ALL LINUX & DEMOSCENE CODERS! *** "
        "REAL-TIME 3D ASCII SURFACE NORMAL SHADING + RASTER COPPER BARS + "
        "3D PARALLAX STARFIELD IN PURE C ***   ";
    const int scroller_len = (int)strlen(scroller);

    // 12-level ASCII luminance ramp from darkest to brightest
    const char *lum_chars = ".,-~:;=!*#$@";

    srand((unsigned int)time(NULL));
    init_stars();

    struct sigaction sa_int = {0}, sa_winch = {0};
    sa_int.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa_int, NULL);

    sa_winch.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &sa_winch, NULL);

    // Enter alternate screen buffer and hide cursor
    printf("\033[?1049h\033[2J\033[?25l");
    resize_viewport();

    float A = 0.0f; // Torus rotation around X axis
    float B = 0.0f; // Torus rotation around Z axis
    float t = 0.0f; // Global demo timer

    while (keep_running) {
        if (needs_resize) {
            resize_viewport();
        }

        // 1. Clear character grid and reset Z-buffer
        for (int y = 0; y < canvas_h; y++) {
            for (int x = 0; x < term_w; x++) {
                GRID(y, x) = (Cell){ ' ', 180, 180, 180, 6, 8, 16 };
                ZBUF(y, x) = 0.0f;
            }
        }

        // 2. Render 3D Perspective Starfield in Background
        float cx = (float)term_w * 0.5f;
        float cy = (float)canvas_h * 0.5f;

        for (int i = 0; i < NUM_STARS; i++) {
            stars[i].z -= 0.32f;
            if (stars[i].z <= 0.5f) {
                stars[i].x = ((float)(rand() % 2000) / 1000.0f - 1.0f) * 40.0f;
                stars[i].y = ((float)(rand() % 2000) / 1000.0f - 1.0f) * 20.0f;
                stars[i].z = 30.0f;
            }

            int sx = (int)(cx + (stars[i].x / stars[i].z) * 28.0f);
            int sy = (int)(cy + (stars[i].y / stars[i].z) * 14.0f);

            if (sx >= 0 && sx < term_w && sy >= 0 && sy < canvas_h) {
                float depth_norm = 1.0f - (stars[i].z / 30.0f);
                int bright = clamp_u8((int)(70.0f + depth_norm * 185.0f));
                char s_ch = (stars[i].z < 8.0f) ? '*' : ((stars[i].z < 18.0f) ? '+' : '.');
                GRID(sy, sx).ch   = s_ch;
                GRID(sy, sx).fg_r = (uint8_t)bright;
                GRID(sy, sx).fg_g = (uint8_t)bright;
                GRID(sy, sx).fg_b = clamp_u8(bright + 30);
            }
        }

        // 3. Render Amiga-Style Scrolling Raster Copper Bars
        const uint8_t bar_colors[NUM_BARS][3] = {
            { 220, 40,  90  }, // Crimson Copper
            { 40,  190, 240 }, // Cyber Cyan
            { 240, 175, 40  }  // Gold Amber
        };

        for (int b = 0; b < NUM_BARS; b++) {
            float bar_y = cy + sinf(t * 1.9f + (float)b * 1.15f) * ((float)canvas_h * 0.34f);

            for (int y = 0; y < canvas_h; y++) {
                float dist = fabsf((float)y - bar_y);
                if (dist < 2.6f) {
                    float intensity = 1.0f - (dist / 2.6f);
                    intensity = intensity * intensity; // Smooth quadratic falloff

                    char raster_ch = (dist < 0.65f) ? '=' : ((dist < 1.4f) ? ':' : '.');
                    for (int x = 0; x < term_w; x++) {
                        Cell *c = &GRID(y, x);
                        c->bg_r = clamp_u8(c->bg_r + (int)(bar_colors[b][0] * intensity * 0.48f));
                        c->bg_g = clamp_u8(c->bg_g + (int)(bar_colors[b][1] * intensity * 0.48f));
                        c->bg_b = clamp_u8(c->bg_b + (int)(bar_colors[b][2] * intensity * 0.48f));

                        // Add subtle ASCII raster scanline texture where empty
                        if (c->ch == ' ') {
                            c->ch   = raster_ch;
                            c->fg_r = clamp_u8((int)(bar_colors[b][0] * intensity * 0.95f));
                            c->fg_g = clamp_u8((int)(bar_colors[b][1] * intensity * 0.95f));
                            c->fg_b = clamp_u8((int)(bar_colors[b][2] * intensity * 0.95f));
                        }
                    }
                }
            }
        }

        // 4. Render 3D Surface-Lit ASCII Torus with Z-Buffer
        const float R1 = 1.0f;  // Tube radius
        const float R2 = 2.15f; // Distance from torus center to tube center
        const float K2 = 5.5f;  // Distance from viewer to torus
        // Scale K1 dynamically to terminal height (accounting for ~2:1 char aspect ratio)
        const float K1 = (float)canvas_h * K2 * 3.0f / (8.0f * (R1 + R2));

        float cosA = cosf(A), sinA = sinf(A);
        float cosB = cosf(B), sinB = sinf(B);

        for (float theta = 0.0f; theta < 6.28318f; theta += 0.06f) {
            float costheta = cosf(theta), sintheta = sinf(theta);

            for (float phi = 0.0f; phi < 6.28318f; phi += 0.02f) {
                float cosphi = cosf(phi), sinphi = sinf(phi);

                // Circle coordinates before 3D rotation
                float circlex = R2 + R1 * costheta;
                float circley = R1 * sintheta;

                // Final 3D coordinates (x, y, z) and reciprocal depth (ooz = 1/z)
                float x = circlex * (cosB * cosphi + sinA * sinB * sinphi) - circley * cosA * sinB;
                float y = circlex * (sinB * cosphi - sinA * cosB * sinphi) + circley * cosA * cosB;
                float z = K2 + cosA * circlex * sinphi + circley * sinA;
                float ooz = 1.0f / z;

                // 2D perspective projection (multiply X by 2.0 for terminal font aspect ratio)
                int xp = (int)(cx + 2.0f * K1 * ooz * x);
                int yp = (int)(cy - K1 * ooz * y);

                if (xp < 0 || xp >= term_w || yp < 0 || yp >= canvas_h) {
                    continue;
                }

                // Compute surface normal luminance L against light direction (0, 1, -1)
                float L = cosphi * costheta * sinB
                        - cosA * costheta * sinphi
                        - sinA * sintheta
                        + cosB * (cosA * sintheta - costheta * sinA * sinphi);

                // Only draw front-facing / lit surfaces that are closer than current ZBUF
                if (L > -0.15f && ooz > ZBUF(yp, xp)) {
                    ZBUF(yp, xp) = ooz;

                    float norm_L = (L + 0.15f) / 1.56f;
                    if (norm_L < 0.0f) norm_L = 0.0f;
                    if (norm_L > 1.0f) norm_L = 1.0f;

                    int lum_idx = (int)(norm_L * 11.99f);

                    // Colorize the ASCII character based on angle phi + surface brightness
                    uint8_t r, g, b;
                    float hue = fmodf(phi / 6.28318f + t * 0.12f, 1.0f);
                    hsv_to_rgb(hue, 0.55f, 0.35f + norm_L * 0.65f, &r, &g, &b);

                    GRID(yp, xp).ch   = lum_chars[lum_idx];
                    GRID(yp, xp).fg_r = r;
                    GRID(yp, xp).fg_g = g;
                    GRID(yp, xp).fg_b = b;
                }
            }
        }

        // 5. Render Demoscene Sine-Wave Text Scroller at the Bottom
        int scroll_char_offset = (int)(t * 14.0f) % scroller_len;
        int base_scroll_y = canvas_h - 4;
        if (base_scroll_y < 4) base_scroll_y = canvas_h - 2;

        for (int x = 0; x < term_w; x++) {
            char ch = scroller[(scroll_char_offset + x) % scroller_len];
            if (ch == ' ') continue;

            float wave = sinf((float)x * 0.16f + t * 4.2f) * 2.2f;
            int sy = base_scroll_y + (int)roundf(wave);

            if (sy >= 0 && sy < canvas_h) {
                uint8_t r, g, b;
                float hue = fmodf((float)x / (float)term_w + t * 0.35f, 1.0f);
                hsv_to_rgb(hue, 0.85f, 1.0f, &r, &g, &b);

                GRID(sy, x).ch   = ch;
                GRID(sy, x).fg_r = r;
                GRID(sy, x).fg_g = g;
                GRID(sy, x).fg_b = b;
            }
        }

        // 6. Flush Framebuffer to Terminal in a Single fwrite() Call
        char *p = out_buf;
        p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m [%dx%d] - 3D ASCII Z-Buffer + Copper Bars (Ctrl+C to quit)\033[K\n",
                     pname, pversion, term_w, canvas_h);

        for (int y = 0; y < canvas_h; y++) {
            for (int x = 0; x < term_w; x++) {
                Cell c = GRID(y, x);
                p += sprintf(p, "\033[38;2;%d;%d;%d;48;2;%d;%d;%dm%c",
                             c.fg_r, c.fg_g, c.fg_b,
                             c.bg_r, c.bg_g, c.bg_b,
                             c.ch);
            }
            p += sprintf(p, "\033[0m\n");
        }

        fwrite(out_buf, 1, (size_t)(p - out_buf), stdout);
        fflush(stdout);

        A += 0.055f;
        B += 0.032f;
        t += 0.045f;
        usleep(30000); // ~33 FPS
    }

    // Restore cursor, leave alternate screen buffer, and free heap memory
    printf("\033[0m\033[?25h\033[?1049l");
    free(grid);
    free(zbuf);
    free(out_buf);
    printf("Exited %s v%s cleanly.\n", pname, pversion);
    return 0;
}