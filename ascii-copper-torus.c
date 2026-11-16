/*
 * ascii-copper-torus
 * Copyright (C) k!M/pizslacker 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define _DEFAULT_SOURCE
#include <math.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <xmp.h>

#define TRACKER_FILE "ascii-copper-torus.it"
#define SAMPLE_RATE  44100
#define AUDIO_FRAMES 512

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

// Shared tracker telemetry updated by the audio thread
typedef struct {
    pthread_mutex_t lock;
    int active;
    int ord;
    int row;
    int bpm;
    float bass_energy;
    float mid_energy;
    float chan_vu[8];
} TrackerSync;

static TrackerSync tracker = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .active = 0
};

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

// Background thread: decodes ascii-copper-torus.it via libxmp and outputs to PulseAudio/PipeWire
static void *tracker_audio_thread(void *arg)
{
    const char *filename = (const char *)arg;
    xmp_context ctx = xmp_create_context();
    if (!ctx) return NULL;

    if (xmp_load_module(ctx, (char *)filename) < 0) {
        xmp_free_context(ctx);
        return NULL;
    }

    if (xmp_start_player(ctx, SAMPLE_RATE, 0) < 0) {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = SAMPLE_RATE,
        .channels = 2
    };

    pa_simple *pa = pa_simple_new(
        NULL, "ascii-copper-torus", PA_STREAM_PLAYBACK, NULL,
        "Impulse Tracker Playback", &ss, NULL, NULL, NULL
    );

    if (!pa) {
        xmp_end_player(ctx);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    pthread_mutex_lock(&tracker.lock);
    tracker.active = 1;
    pthread_mutex_unlock(&tracker.lock);

    int16_t pcm_buf[AUDIO_FRAMES * 2];
    struct xmp_frame_info fi;

    while (keep_running) {
        // Loop module seamlessly (0 = infinite loop count)
        if (xmp_play_buffer(ctx, pcm_buf, sizeof(pcm_buf), 0) < 0) {
            break;
        }

        int pa_err = 0;
        if (pa_simple_write(pa, pcm_buf, sizeof(pcm_buf), &pa_err) < 0) {
            break;
        }

        // Extract real-time pattern and channel telemetry for visual sync
        xmp_get_frame_info(ctx, &fi);

        float bass = 0.0f;
        float mid  = 0.0f;
        float vu[8] = {0};

        for (int c = 0; c < 8; c++) {
            float v = (float)fi.channel_info[c].volume / 64.0f;
            if (v > 1.0f) v = 1.0f;
            vu[c] = v;
            if (c < 2) bass += v * 0.5f;
            else       mid  += v / 6.0f;
        }

        pthread_mutex_lock(&tracker.lock);
        tracker.ord         = fi.pos;
        tracker.row         = fi.row;
        tracker.bpm         = fi.bpm;
        tracker.bass_energy = bass;
        tracker.mid_energy  = mid;
        for (int c = 0; c < 8; c++) {
            tracker.chan_vu[c] = vu[c];
        }
        pthread_mutex_unlock(&tracker.lock);
    }

    pa_simple_drain(pa, NULL);
    pa_simple_free(pa);
    xmp_end_player(ctx);
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    return NULL;
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float hp = fmodf(h * 6.0f, 6.0f);
    if (hp < 0.0f) hp += 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0.0f, gf = 0.0f, bf = 0.0f;

    switch ((int)hp) {
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
    const char *pversion = "1.1";
    const char *scroller =
        "   *** NOW PLAYING: ASCII-COPPER-TORUS.IT *** "
        "REAL-TIME 3D ASCII SURFACE NORMAL SHADING + LIBXMP IMPULSE TRACKER SYNC + "
        "AMIGA RASTER COPPER BARS IN PURE C ***   ";
    const int scroller_len = (int)strlen(scroller);
    const char *lum_chars  = ".,-~:;=!*#$@";
    const char *vu_chars[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

    srand((unsigned int)time(NULL));
    init_stars();

    struct sigaction sa_int = {0}, sa_winch = {0};
    sa_int.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa_int, NULL);

    sa_winch.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &sa_winch, NULL);

    // Start background Impulse Tracker thread for ascii-copper-torus.it
    pthread_t audio_tid;
    int thread_started = (pthread_create(&audio_tid, NULL, tracker_audio_thread, (void *)TRACKER_FILE) == 0);

    printf("\033[?1049h\033[2J\033[?25l");
    resize_viewport();

    float A = 0.0f;
    float B = 0.0f;
    float t = 0.0f;
    float smooth_bass = 0.0f;
    float smooth_mid  = 0.0f;

    while (keep_running) {
        if (needs_resize) {
            resize_viewport();
        }

        // Read latest tracker telemetry
        int trk_active = 0, ord = 0, row = 0, bpm = 125;
        float raw_bass = 0.0f, raw_mid = 0.0f;
        float vu[8] = {0};

        pthread_mutex_lock(&tracker.lock);
        trk_active = tracker.active;
        ord        = tracker.ord;
        row        = tracker.row;
        bpm        = tracker.bpm;
        raw_bass   = tracker.bass_energy;
        raw_mid    = tracker.mid_energy;
        for (int c = 0; c < 8; c++) vu[c] = tracker.chan_vu[c];
        pthread_mutex_unlock(&tracker.lock);

        // Smooth envelope follower for visual reactivity
        smooth_bass = smooth_bass * 0.70f + raw_bass * 0.30f;
        smooth_mid  = smooth_mid  * 0.75f + raw_mid  * 0.25f;

        // 1. Clear character grid and Z-buffer
        for (int y = 0; y < canvas_h; y++) {
            for (int x = 0; x < term_w; x++) {
                GRID(y, x) = (Cell){ ' ', 180, 180, 180, 6, 8, 16 };
                ZBUF(y, x) = 0.0f;
            }
        }

        // 2. Render 3D Perspective Starfield (speeds up on bass hits)
        float cx = (float)term_w * 0.5f;
        float cy = (float)canvas_h * 0.5f;
        float star_speed = 0.28f + smooth_bass * 0.45f;

        for (int i = 0; i < NUM_STARS; i++) {
            stars[i].z -= star_speed;
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

        // 3. Render Beat-Reactive Amiga Raster Copper Bars
        const uint8_t bar_colors[NUM_BARS][3] = {
            { 220, 40,  90  },
            { 40,  190, 240 },
            { 240, 175, 40  }
        };

        float bar_half_span = 2.4f + smooth_mid * 1.6f;

        for (int b = 0; b < NUM_BARS; b++) {
            float bar_y = cy + sinf(t * 1.9f + (float)b * 1.15f) * ((float)canvas_h * 0.34f);

            for (int y = 0; y < canvas_h; y++) {
                float dist = fabsf((float)y - bar_y);
                if (dist < bar_half_span) {
                    float intensity = 1.0f - (dist / bar_half_span);
                    intensity = intensity * intensity * (0.85f + smooth_bass * 0.45f);

                    char raster_ch = (dist < 0.65f) ? '=' : ((dist < 1.4f) ? ':' : '.');
                    for (int x = 0; x < term_w; x++) {
                        Cell *c = &GRID(y, x);
                        c->bg_r = clamp_u8(c->bg_r + (int)(bar_colors[b][0] * intensity * 0.48f));
                        c->bg_g = clamp_u8(c->bg_g + (int)(bar_colors[b][1] * intensity * 0.48f));
                        c->bg_b = clamp_u8(c->bg_b + (int)(bar_colors[b][2] * intensity * 0.48f));

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

        // 4. Render 3D Surface-Lit ASCII Torus (pulses with tracker bass)
        const float R1 = 0.95f + smooth_bass * 0.28f;
        const float R2 = 2.10f + smooth_mid  * 0.20f;
        const float K2 = 5.5f;
        const float K1 = (float)canvas_h * K2 * 3.0f / (8.0f * (1.25f + 2.30f));

        float cosA = cosf(A), sinA = sinf(A);
        float cosB = cosf(B), sinB = sinf(B);

        for (float theta = 0.0f; theta < 6.28318f; theta += 0.06f) {
            float costheta = cosf(theta), sintheta = sinf(theta);

            for (float phi = 0.0f; phi < 6.28318f; phi += 0.02f) {
                float cosphi = cosf(phi), sinphi = sinf(phi);

                float circlex = R2 + R1 * costheta;
                float circley = R1 * sintheta;

                float x = circlex * (cosB * cosphi + sinA * sinB * sinphi) - circley * cosA * sinB;
                float y = circlex * (sinB * cosphi - sinA * cosB * sinphi) + circley * cosA * cosB;
                float z = K2 + cosA * circlex * sinphi + circley * sinA;
                float ooz = 1.0f / z;

                int xp = (int)(cx + 2.0f * K1 * ooz * x);
                int yp = (int)(cy - K1 * ooz * y);

                if (xp < 0 || xp >= term_w || yp < 0 || yp >= canvas_h) {
                    continue;
                }

                float L = cosphi * costheta * sinB
                        - cosA * costheta * sinphi
                        - sinA * sintheta
                        + cosB * (cosA * sintheta - costheta * sinA * sinphi);

                if (L > -0.15f && ooz > ZBUF(yp, xp)) {
                    ZBUF(yp, xp) = ooz;

                    float norm_L = (L + 0.15f) / 1.56f;
                    if (norm_L < 0.0f) norm_L = 0.0f;
                    if (norm_L > 1.0f) norm_L = 1.0f;

                    int lum_idx = (int)(norm_L * 11.99f);

                    uint8_t r, g, b;
                    float hue = fmodf(phi / 6.28318f + t * 0.12f + smooth_bass * 0.15f, 1.0f);
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

            float wave = sinf((float)x * 0.16f + t * 4.2f) * (2.0f + smooth_bass * 1.2f);
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

        // 6. Flush Framebuffer + Tracker Status Bar to Terminal
        char *p = out_buf;
        if (trk_active) {
            p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m | \033[1;32m%s\033[0m [Ord:%02X Row:%02X BPM:%3d] VU:",
                         pname, pversion, TRACKER_FILE, ord, row, bpm);
            for (int c = 0; c < 8; c++) {
                int v_idx = (int)(vu[c] * 8.0f);
                if (v_idx > 8) v_idx = 8;
                p += sprintf(p, "\033[1;33m%s", vu_chars[v_idx]);
            }
            p += sprintf(p, "\033[0m\033[K\n");
        } else {
            p += sprintf(p, "\033[H\033[1;36m%s v%s\033[0m [%dx%d] - (%s not found, visual-only mode)\033[K\n",
                         pname, pversion, term_w, canvas_h, TRACKER_FILE);
        }

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

        A += 0.050f + smooth_bass * 0.035f;
        B += 0.030f + smooth_mid  * 0.025f;
        t += 0.045f;
        usleep(30000); // ~33 FPS
    }

    if (thread_started) {
        pthread_join(audio_tid, NULL);
    }

    printf("\033[0m\033[?25h\033[?1049l");
    free(grid);
    free(zbuf);
    free(out_buf);
    printf("Exited %s v%s cleanly.\n", pname, pversion);
    return 0;
}