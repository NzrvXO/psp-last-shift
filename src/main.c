#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>

PSP_MODULE_INFO("LAST SHIFT", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(16))) list[262144];

typedef struct
{
    float x, y, z;
} Vertex;

/* Unit cube, centered on X/Z, base at y=0, top at y=1. */
static Vertex __attribute__((aligned(16))) cube_verts[] = {
    /* front */
    {-0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f},
    {-0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, 0.5f},
    /* back */
    {0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, -0.5f},
    {0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, -0.5f},
    /* left */
    {-0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, 0.5f}, {-0.5f, 1.0f, 0.5f},
    {-0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, -0.5f},
    /* right */
    {0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, -0.5f},
    {0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, 0.5f},
    /* top */
    {-0.5f, 1.0f, 0.5f}, {0.5f, 1.0f, 0.5f}, {0.5f, 1.0f, -0.5f},
    {-0.5f, 1.0f, 0.5f}, {0.5f, 1.0f, -0.5f}, {-0.5f, 1.0f, -0.5f},
    /* bottom */
    {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f},
    {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f},
};
#define CUBE_VERT_COUNT (sizeof(cube_verts) / sizeof(Vertex))

/* ---- Audio -------------------------------------------------------------
 *
 * Sounds are square-wave beeps synthesised on the fly rather than sampled
 * assets: a handful of blips doesn't justify shipping and loading audio
 * files. sceAudioOutputBlocking blocks until the hardware drains the
 * buffer, so it runs on its own thread and the game loop only ever posts a
 * request. */

#define SAMPLE_RATE 44100
#define AUDIO_FRAMES 1024
#define AUDIO_AMPLITUDE 5000

typedef struct
{
    short freq; /* Hz, 0 for a rest */
    short ms;   /* 0 terminates the sequence */
} Note;

static const Note snd_pickup[] = {{784, 50}, {0, 0}};
static const Note snd_drop[] = {{392, 50}, {0, 0}};
static const Note snd_deliver[] = {{523, 50}, {784, 70}, {0, 0}};
static const Note snd_order_done[] = {{523, 80}, {659, 80}, {784, 140}, {0, 0}};
static const Note snd_shift_done[] = {{523, 120}, {659, 120}, {784, 120}, {1046, 240}, {0, 0}};
static const Note snd_menu[] = {{660, 35}, {0, 0}};
static const Note snd_horror[] = {{147, 260}, {0, 90}, {110, 420}, {0, 0}};

static int audio_channel = -1;
static short audio_buffer[2][AUDIO_FRAMES * 2];

static const Note *volatile pending_sound = NULL;
static const Note *current_sound = NULL;
static int note_index = 0;
static int note_samples_left = 0;
static int wave_phase = 0;

void play_sound(const Note *sequence)
{
    pending_sound = sequence;
}

static void start_note(void)
{
    note_samples_left = current_sound[note_index].ms * SAMPLE_RATE / 1000;
    wave_phase = 0;
}

static void fill_audio(short *buffer)
{
    /* Picking the request up here keeps all playback state owned by this
     * thread, so the game loop only ever writes the single pointer. */
    if (pending_sound)
    {
        current_sound = (const Note *)pending_sound;
        pending_sound = NULL;
        note_index = 0;
        start_note();
    }

    int i;
    for (i = 0; i < AUDIO_FRAMES; i++)
    {
        short sample = 0;

        if (current_sound)
        {
            if (note_samples_left <= 0)
            {
                note_index++;
                if (current_sound[note_index].ms == 0)
                    current_sound = NULL;
                else
                    start_note();
            }

            if (current_sound)
            {
                int freq = current_sound[note_index].freq;
                if (freq > 0)
                {
                    int period = SAMPLE_RATE / freq;
                    sample = (wave_phase < period / 2) ? AUDIO_AMPLITUDE : -AUDIO_AMPLITUDE;
                    if (++wave_phase >= period)
                        wave_phase = 0;
                }
                note_samples_left--;
            }
        }

        buffer[i * 2] = sample;
        buffer[i * 2 + 1] = sample;
    }
}

int audio_thread(SceSize args, void *argp)
{
    int index = 0;
    while (1)
    {
        fill_audio(audio_buffer[index]);
        sceAudioOutputBlocking(audio_channel, PSP_AUDIO_VOLUME_MAX, audio_buffer[index]);
        index ^= 1;
    }
    return 0;
}

void audio_init(void)
{
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_FRAMES,
                                       PSP_AUDIO_FORMAT_STEREO);
    if (audio_channel < 0)
        return;

    int thid = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x10000, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

/* Top surface of the floor slab; everything standing on the floor sits here. */
#define FLOOR_Y 0.2f

/* The floor is a baked mesh of flat quads rather than one big polygon.
 * The PSP's hardware transform discards whole triangles that reach past
 * the near plane instead of clipping them, so a few huge floor polygons
 * vanish entirely once the camera gets close to them. Small tiles keep
 * every triangle well inside the guard band.
 *
 * It also extends past the walls (which stay at +/-10): the camera trails
 * the player without collision, so near the loading zone it sits beyond
 * Z=10 and would otherwise look out over the floor's edge. */
#define FLOOR_HALF_SIZE 18.0f
#define FLOOR_TILE 1.5f
#define FLOOR_TILES 24
#define FLOOR_VERT_COUNT (FLOOR_TILES * FLOOR_TILES * 6)

int exit_callback(int arg1, int arg2, void *common)
{
    sceKernelExitGame();
    return 0;
}

int callback_thread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setup_callbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
    return thid;
}

void init_graphics(void)
{
    sceGuInit();

    sceGuStart(GU_DIRECT, list);
    /* Single-buffered: draw and display share the same address, so the
     * pspDebugScreen HUD text (written after the 3D frame) always lands
     * in the buffer that's actually shown, with no double-buffer swap
     * to fall out of sync with. */
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)0, BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0, 65535);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    /* Smooth shading lets the floor's baked light pools blend across tiles. */
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(0, 0);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* ---- Lighting ----------------------------------------------------------
 *
 * The GE's hardware lighting needs vertex normals, which would mean carrying
 * a normal through every mesh in the game. Instead light is evaluated on the
 * CPU from the lamp positions: the floor gets it baked per vertex (so lamps
 * cast real pools of light) and objects get it per face. Both feed the same
 * flat-colored vertex path that already works. */

#define LAMP_COUNT 6
static const float lamp_x[LAMP_COUNT] = {-6.0f, 6.0f, -6.0f, 6.0f, 0.0f, 0.0f};
static const float lamp_z[LAMP_COUNT] = {-6.0f, -6.0f, 3.0f, 3.0f, -1.5f, 7.0f};
static int lamp_on[LAMP_COUNT] = {1, 1, 1, 1, 1, 1};

static int terminal_glitched = 0;

#define LIGHT_AMBIENT 0.20f
#define LAMP_STRENGTH 1.10f
#define LAMP_FALLOFF 0.05f

float light_at(float x, float z)
{
    float level = LIGHT_AMBIENT;
    int i;
    for (i = 0; i < LAMP_COUNT; i++)
    {
        if (!lamp_on[i])
            continue;

        float dx = x - lamp_x[i];
        float dz = z - lamp_z[i];
        /* Squared distance is enough for the falloff, so no sqrt per vertex. */
        level += LAMP_STRENGTH / (1.0f + (dx * dx + dz * dz) * LAMP_FALLOFF);
    }
    return level > 1.0f ? 1.0f : level;
}

/* Colors are ABGR; alpha is left alone. */
unsigned int scale_color(unsigned int color, float k)
{
    unsigned int b = (unsigned int)(((color >> 16) & 0xFF) * k);
    unsigned int g = (unsigned int)(((color >> 8) & 0xFF) * k);
    unsigned int r = (unsigned int)((color & 0xFF) * k);
    return (color & 0xFF000000) | (b << 16) | (g << 8) | r;
}

/* Colored world vertex. */
typedef struct
{
    unsigned int color;
    float x, y, z;
} VertexC;

/* Per-face brightness, in cube_verts face order: front, back, left, right,
 * top, bottom. Faces that would catch overhead light are brighter. Without
 * this every box reads as a flat silhouette. */
static const float face_shade[6] = {0.72f, 0.62f, 0.66f, 0.80f, 1.00f, 0.45f};

/* Emits the shared cube already positioned and colored, so the model matrix
 * stays identity and no per-object matrix upload is needed. Vertices come
 * from the display list via sceGuGetMemory, which sidesteps cache writeback. */
void draw_box_shaded(float x, float y, float z, float sx, float sy, float sz,
                      unsigned int color, float light, int shade_faces)
{
    VertexC *v = (VertexC *)sceGuGetMemory(CUBE_VERT_COUNT * sizeof(VertexC));

    unsigned int i;
    for (i = 0; i < CUBE_VERT_COUNT; i++)
    {
        float k = shade_faces ? face_shade[i / 6] * light : light;
        v[i].color = scale_color(color, k);
        v[i].x = cube_verts[i].x * sx + x;
        v[i].y = cube_verts[i].y * sy + y;
        v[i].z = cube_verts[i].z * sz + z;
    }

    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    CUBE_VERT_COUNT, 0, v);
}

/* World objects are lit by the lamps above them. */
void draw_box(float x, float y, float z, float sx, float sy, float sz, unsigned int color)
{
    draw_box_shaded(x, y, z, sx, sy, sz, color, light_at(x, z), 1);
}

/* Lamps and terminal screens emit rather than receive light. */
void draw_box_lit(float x, float y, float z, float sx, float sy, float sz, unsigned int color)
{
    draw_box_shaded(x, y, z, sx, sy, sz, color, 1.0f, 0);
}

/* Floor lighting is baked per vertex and only rebuilt when a lamp changes,
 * so the pools of light cost nothing per frame. */
#define FLOOR_COLOR 0xFF6A6A6A
static VertexC __attribute__((aligned(16))) floor_verts[FLOOR_VERT_COUNT];

static void put_floor_vertex(int n, float x, float z)
{
    floor_verts[n].color = scale_color(FLOOR_COLOR, light_at(x, z));
    floor_verts[n].x = x;
    floor_verts[n].y = FLOOR_Y;
    floor_verts[n].z = z;
}

void build_floor(void)
{
    int i, j, n = 0;
    for (i = 0; i < FLOOR_TILES; i++)
    {
        for (j = 0; j < FLOOR_TILES; j++)
        {
            float x0 = -FLOOR_HALF_SIZE + i * FLOOR_TILE;
            float z0 = -FLOOR_HALF_SIZE + j * FLOOR_TILE;
            float x1 = x0 + FLOOR_TILE;
            float z1 = z0 + FLOOR_TILE;

            put_floor_vertex(n++, x0, z1);
            put_floor_vertex(n++, x1, z1);
            put_floor_vertex(n++, x1, z0);
            put_floor_vertex(n++, x0, z1);
            put_floor_vertex(n++, x1, z0);
            put_floor_vertex(n++, x0, z0);
        }
    }
    /* The GE reads this straight from RAM, so the CPU's writes must land there. */
    sceKernelDcacheWritebackAll();
}

/* Screen-space vertex: 2D transform ignores the camera matrices entirely,
 * so these coordinates are straight pixels. */
typedef struct
{
    unsigned int color;
    short x, y, z;
} Vertex2D;

/* Filled screen-space rectangle. Vertices come from sceGuGetMemory so they
 * live inside the current display list, which avoids the cache-writeback
 * dance a per-frame CPU-written buffer would need. */
void draw_rect_2d(int x, int y, int w, int h, unsigned int color)
{
    if (w <= 0 || h <= 0)
        return;

    Vertex2D *v = (Vertex2D *)sceGuGetMemory(6 * sizeof(Vertex2D));
    short x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    v[0].color = color; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].color = color; v[1].x = x1; v[1].y = y0; v[1].z = 0;
    v[2].color = color; v[2].x = x0; v[2].y = y1; v[2].z = 0;
    v[3].color = color; v[3].x = x1; v[3].y = y0; v[3].z = 0;
    v[4].color = color; v[4].x = x1; v[4].y = y1; v[4].z = 0;
    v[5].color = color; v[5].x = x0; v[5].y = y1; v[5].z = 0;

    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                    6, 0, v);
}

/* 5x7 bitmap font. Each glyph is 7 rows; the low 5 bits of each row are the
 * pixels, most significant bit leftmost. Glyphs are drawn as one small quad
 * per lit pixel, batched into a single draw call per character. That is more
 * geometry than a font texture would need, but it reuses the plain 2D path
 * that already works instead of introducing texture and blend state that has
 * to coexist with the 3D pass. */
#define FONT_W 5
#define FONT_H 7

static const unsigned char font_data[][FONT_H] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, /* # */
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}, /* / */
    {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}, /* : */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, /* . */
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, /* ! */
};

int font_index(char c)
{
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 1 + (c - 'a');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    switch (c)
    {
    case '#': return 37;
    case '/': return 38;
    case ':': return 39;
    case '-': return 40;
    case '+': return 41;
    case '.': return 42;
    case '!': return 43;
    default: return -1;
    }
}

void draw_char(int x, int y, int scale, unsigned int color, char c)
{
    int idx = font_index(c);
    if (idx < 0)
        return;

    const unsigned char *glyph = font_data[idx];
    int row, col, lit = 0;

    for (row = 0; row < FONT_H; row++)
        for (col = 0; col < FONT_W; col++)
            if (glyph[row] & (1 << (FONT_W - 1 - col)))
                lit++;

    if (lit == 0)
        return;

    Vertex2D *v = (Vertex2D *)sceGuGetMemory(lit * 6 * sizeof(Vertex2D));
    int n = 0;

    for (row = 0; row < FONT_H; row++)
    {
        for (col = 0; col < FONT_W; col++)
        {
            if (!(glyph[row] & (1 << (FONT_W - 1 - col))))
                continue;

            short x0 = x + col * scale, y0 = y + row * scale;
            short x1 = x0 + scale, y1 = y0 + scale;

            v[n].color = color; v[n].x = x0; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x0; v[n].y = y1; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y1; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x0; v[n].y = y1; v[n].z = 0; n++;
        }
    }

    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                    lit * 6, 0, v);
}

void draw_text(int x, int y, int scale, unsigned int color, const char *text)
{
    int cursor = x;
    while (*text)
    {
        draw_char(cursor, y, scale, color, *text);
        cursor += (FONT_W + 1) * scale;
        text++;
    }
}

int text_width(const char *text, int scale)
{
    int n = 0;
    while (text[n])
        n++;
    return n * (FONT_W + 1) * scale;
}

/* Static obstacles as axis-aligned boxes in the XZ plane (walls + crates). */
typedef struct
{
    float x, z, hx, hz; /* center and half-extents */
} Obstacle;

/* Shelving runs in two rows with a corridor down the middle and a gap at
 * each wall, so nothing in the warehouse is reachable in a straight line. */
#define SHELF_COUNT 4
static const Obstacle shelves[SHELF_COUNT] = {
    {-5.0f, -4.0f, 3.5f, 0.6f},
    {5.0f, -4.0f, 3.5f, 0.6f},
    {-5.0f, 1.0f, 3.5f, 0.6f},
    {5.0f, 1.0f, 3.5f, 0.6f},
};

static Obstacle obstacles[] = {
    {0.0f, -10.0f, 10.0f, 0.2f}, /* back wall */
    {-10.0f, 0.0f, 0.2f, 10.0f}, /* left wall */
    {10.0f, 0.0f, 0.2f, 10.0f},  /* right wall */
    {6.0f, 7.0f, 0.5f, 0.3f},    /* terminal */
    {-5.0f, -4.0f, 3.5f, 0.6f},  /* shelves, mirroring the table above */
    {5.0f, -4.0f, 3.5f, 0.6f},
    {-5.0f, 1.0f, 3.5f, 0.6f},
    {5.0f, 1.0f, 3.5f, 0.6f},
};
#define OBSTACLE_COUNT (sizeof(obstacles) / sizeof(Obstacle))
#define PLAYER_RADIUS 0.35f

/* Pickable box types: differ in size and color. */
typedef enum
{
    BOX_A = 0,
    BOX_B,
    BOX_C
} BoxType;
#define BOX_TYPE_COUNT 3

/* An order asks for a number of boxes of each type. Delivered counts go up
 * when the player hands a box over at the loading zone. */
typedef struct
{
    int number;
    int required[BOX_TYPE_COUNT];
    int delivered[BOX_TYPE_COUNT];
} Order;

static Order current_order = {1, {2, 1, 3}, {0, 0, 0}};

/* One shift is five orders. Every order has to be fillable from what's
 * stocked in the warehouse (2x A, 1x B, 3x C), which restocks between
 * orders. */
#define ORDER_COUNT 5

static const int order_requirements[ORDER_COUNT][BOX_TYPE_COUNT] = {
    {2, 1, 3},
    {1, 1, 1},
    {2, 0, 2},
    {1, 1, 3},
    {2, 1, 2},
};

#define ORDER_TIME_LIMIT 180.0f
#define SCORE_PER_BOX 100
#define TIME_BONUS_PER_SECOND 2
#define ORDER_COMPLETE_HOLD 2.0f

static float time_left = ORDER_TIME_LIMIT;
static int score = 0;
static int order_complete = 0;
static int order_index = 0;
static int shift_complete = 0;
static float complete_hold = 0.0f;

/* Snapshot taken when an order is filled, for the ORDER COMPLETE screen. */
static int last_bonus = 0;
static float last_time_left = 0.0f;

typedef enum
{
    STATE_MENU,
    STATE_PLAYING,
    STATE_ORDER_DONE,
    STATE_SHIFT_DONE,
    STATE_PAUSED
} GameState;

static GameState state = STATE_MENU;
static int menu_selection = 0;

/* Player state lives here so the state machine can reset it. */
static float player_x = 0.0f;
static float player_z = 6.0f;
static int carrying = -1; /* -1 = empty-handed, else BoxType */

int order_is_complete(void)
{
    int i;
    for (i = 0; i < BOX_TYPE_COUNT; i++)
        if (current_order.delivered[i] < current_order.required[i])
            return 0;
    return 1;
}

const char *box_type_name(BoxType type)
{
    switch (type)
    {
    case BOX_A: return "BOX A";
    case BOX_B: return "BOX B";
    default: return "BOX C";
    }
}

typedef struct
{
    float x, z;
    BoxType type;
    int active; /* 0 once picked up */
} Box;

static const float box_size[] = {0.7f, 0.55f, 0.4f};
static const unsigned int box_color[] = {0xFF2040D0, 0xFF30C040, 0xFFD0C020};

/* Spots in the aisles where stock can sit: clear of the shelving, the
 * loading zone, the terminal and the spot the player starts on. */
static const float spawn_x[] = {
    -7.0f, -4.0f, -1.0f,  4.0f,  7.0f,
    -7.0f, -3.0f,  3.0f,  7.0f,
    -7.0f, -3.0f,  3.0f,  7.0f,
     0.0f,  0.0f, -2.0f,
};
static const float spawn_z[] = {
    -2.0f, -2.0f, -2.0f, -2.0f, -2.0f,
     3.5f,  3.5f,  3.5f,  3.5f,
    -6.5f, -6.5f, -6.5f, -6.5f,
    -6.0f,  0.0f,  5.0f,
};
#define SPAWN_POINT_COUNT (sizeof(spawn_x) / sizeof(float))

#define MAX_BOXES 16
#define PICKUP_RANGE 1.1f

static Box boxes[MAX_BOXES];
static int box_count = 0;

/* Transient on-screen message, used by the shift's small unexplained events. */
static const char *message_line1 = NULL;
static const char *message_line2 = NULL;
static float message_timer = 0.0f;

void show_message(const char *line1, const char *line2, float seconds)
{
    message_line1 = line1;
    message_line2 = line2;
    message_timer = seconds;
}

static int extra_box_present = 0;

static void place_box(BoxType type, int spawn_slot)
{
    if (box_count >= MAX_BOXES)
        return;

    boxes[box_count].x = spawn_x[spawn_slot];
    boxes[box_count].z = spawn_z[spawn_slot];
    boxes[box_count].type = type;
    boxes[box_count].active = 1;
    box_count++;
}

/* Restocks for a given order. The starting slot rotates per order and the
 * stride skips around the aisles, so each order's stock lands somewhere new;
 * the surplus on top of what's required varies too, so the shelves never
 * hold exactly the same thing twice. */
void restock_warehouse(int index)
{
    static const int surplus[ORDER_COUNT][BOX_TYPE_COUNT] = {
        {1, 1, 0},
        {2, 0, 2},
        {0, 2, 1},
        {2, 1, 1},
        {1, 1, 2},
    };

    box_count = 0;

    int slot = (index * 7) % SPAWN_POINT_COUNT;
    int type;
    for (type = 0; type < BOX_TYPE_COUNT; type++)
    {
        int wanted = current_order.required[type] + surplus[index][type];
        int i;
        for (i = 0; i < wanted; i++)
        {
            place_box((BoxType)type, slot);
            /* 3 and 16 share no factors, so slots never repeat. */
            slot = (slot + 3) % SPAWN_POINT_COUNT;
        }
    }

    /* Nobody stocked this one, and it turns up right where you were standing. */
    if (extra_box_present && box_count < MAX_BOXES)
    {
        boxes[box_count].x = 0.0f;
        boxes[box_count].z = 5.0f;
        boxes[box_count].type = BOX_A;
        boxes[box_count].active = 1;
        box_count++;
    }
}

/* Nothing chases the player and nothing jumps out; the shift just stops
 * adding up. Each event fires as the following order is handed out. */
void apply_shift_events(int index)
{
    if (index >= 2)
    {
        lamp_on[0] = 0;
        lamp_on[4] = 0;
        build_floor(); /* rebake the light pools without those lamps */
    }
    if (index >= 3)
        extra_box_present = 1;
    if (index >= 4)
        terminal_glitched = 1;

    if (index == 2)
    {
        play_sound(snd_horror);
        show_message("LAMP 01 OFFLINE", "MAINTENANCE NOT SCHEDULED", 4.0f);
    }
    else if (index == 3)
    {
        play_sound(snd_horror);
        show_message("INVENTORY MISMATCH", "ONE ITEM UNACCOUNTED FOR", 4.0f);
    }
    else if (index == 4)
    {
        play_sound(snd_horror);
        show_message("TERMINAL", "YOU ARE NOT ON THE ROSTER", 4.0f);
    }
}

/* Loads an order and resets the clock and the warehouse stock for it. */
void start_order(int index)
{
    int i;
    order_index = index;
    current_order.number = index + 1;
    for (i = 0; i < BOX_TYPE_COUNT; i++)
    {
        current_order.required[i] = order_requirements[index][i];
        current_order.delivered[i] = 0;
    }
    time_left = ORDER_TIME_LIMIT;
    order_complete = 0;
    complete_hold = 0.0f;
    apply_shift_events(index);
    restock_warehouse(index);
}

/* True if a circle of PLAYER_RADIUS at (x,z) overlaps any obstacle or box. */
int check_collision(float x, float z)
{
    int i;
    for (i = 0; i < (int)OBSTACLE_COUNT; i++)
    {
        Obstacle *o = &obstacles[i];
        float min_x = o->x - o->hx, max_x = o->x + o->hx;
        float min_z = o->z - o->hz, max_z = o->z + o->hz;

        float closest_x = x < min_x ? min_x : (x > max_x ? max_x : x);
        float closest_z = z < min_z ? min_z : (z > max_z ? max_z : z);

        float dx = x - closest_x;
        float dz = z - closest_z;
        if (dx * dx + dz * dz < PLAYER_RADIUS * PLAYER_RADIUS)
            return 1;
    }

    for (i = 0; i < box_count; i++)
    {
        Box *b = &boxes[i];
        if (!b->active)
            continue;
        float half = box_size[b->type] * 0.5f;
        float min_x = b->x - half, max_x = b->x + half;
        float min_z = b->z - half, max_z = b->z + half;

        float closest_x = x < min_x ? min_x : (x > max_x ? max_x : x);
        float closest_z = z < min_z ? min_z : (z > max_z ? max_z : z);

        float dx = x - closest_x;
        float dz = z - closest_z;
        if (dx * dx + dz * dz < PLAYER_RADIUS * PLAYER_RADIUS)
            return 1;
    }
    return 0;
}

void draw_boxes(void)
{
    int i;
    for (i = 0; i < box_count; i++)
    {
        Box *b = &boxes[i];
        if (!b->active)
            continue;
        float size = box_size[b->type];
        draw_box(b->x, FLOOR_Y, b->z, size, size, size, box_color[b->type]);
    }
}

void draw_floor(void)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();

    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    FLOOR_VERT_COUNT, 0, floor_verts);
}

/* Walls are drawn as short segments for the same reason the floor is tiled:
 * one 20-unit polygon gets dropped outright when the camera comes near it. */
#define WALL_SEGMENT 2.0f
#define WALL_SEGMENTS 10
#define WALL_HEIGHT 5.0f
#define WALL_THICKNESS 0.4f

void draw_wall_along_x(float z, unsigned int color)
{
    int i;
    for (i = 0; i < WALL_SEGMENTS; i++)
    {
        float cx = -10.0f + WALL_SEGMENT * (i + 0.5f);
        draw_box(cx, FLOOR_Y, z, WALL_SEGMENT, WALL_HEIGHT, WALL_THICKNESS, color);
    }
}

void draw_wall_along_z(float x, unsigned int color)
{
    int i;
    for (i = 0; i < WALL_SEGMENTS; i++)
    {
        float cz = -10.0f + WALL_SEGMENT * (i + 0.5f);
        draw_box(x, FLOOR_Y, cz, WALL_THICKNESS, WALL_HEIGHT, WALL_SEGMENT, color);
    }
}

/* A shelf is an uprights-and-decks rack, drawn in segments along its long
 * axis so no single polygon is big enough to be dropped near the camera. */
#define SHELF_HEIGHT 2.4f

void draw_shelf(const Obstacle *s)
{
    float length = s->hx > s->hz ? s->hx * 2.0f : s->hz * 2.0f;
    int segments = (int)(length / 1.75f);
    if (segments < 1)
        segments = 1;

    float step = length / segments;
    int along_x = s->hx > s->hz;
    int i;

    for (i = 0; i < segments; i++)
    {
        float offset = -length * 0.5f + step * (i + 0.5f);
        float cx = along_x ? s->x + offset : s->x;
        float cz = along_x ? s->z : s->z + offset;
        float sx = along_x ? step : s->hx * 2.0f;
        float sz = along_x ? s->hz * 2.0f : step;

        /* Uprights, then the two decks they carry. */
        draw_box(cx, FLOOR_Y, cz, sx * 0.12f, SHELF_HEIGHT, sz, 0xFF404860);
        draw_box(cx, FLOOR_Y + 0.9f, cz, sx, 0.12f, sz, 0xFF2F5A78);
        draw_box(cx, FLOOR_Y + SHELF_HEIGHT - 0.15f, cz, sx, 0.12f, sz, 0xFF2F5A78);
    }
}

/* Pallets are flat enough to step over, so they are decoration only and
 * deliberately absent from the collision table. */
static const float pallet_x[] = {-8.5f, 8.5f, -2.0f, 2.5f, 8.0f};
static const float pallet_z[] = {6.5f, -8.5f, 6.0f, -6.5f, 5.0f};
#define PALLET_COUNT (sizeof(pallet_x) / sizeof(float))

void draw_pallets(void)
{
    unsigned int i;
    for (i = 0; i < PALLET_COUNT; i++)
        draw_box(pallet_x[i], FLOOR_Y, pallet_z[i], 1.4f, 0.14f, 1.1f, 0xFF3A6480);
}

/* Against the back wall: keeps it out of the camera's trailing path, which
 * is where it would fill half the screen. */
#define LOADING_ZONE_X 0.0f
#define LOADING_ZONE_Z -8.0f
#define LOADING_ZONE_RADIUS 1.5f

/* The terminal the shift's orders come in on. */
#define TERMINAL_X 6.0f
#define TERMINAL_Z 7.0f

void draw_warehouse(void)
{
    draw_floor();

    draw_wall_along_x(-10.0f, 0xFF806040);
    draw_wall_along_z(-10.0f, 0xFF806040);
    draw_wall_along_z(10.0f, 0xFF806040);

    /* loading zone marker, slightly raised to avoid z-fighting with the floor.
     * Colors are ABGR here, so this is gold rather than the cyan the same
     * digits would give in RGB. */
    draw_box(LOADING_ZONE_X, FLOOR_Y + 0.01f, LOADING_ZONE_Z, 3.0f, 0.02f, 3.0f, 0xFF30A0C0);

    int s;
    for (s = 0; s < SHELF_COUNT; s++)
        draw_shelf(&shelves[s]);

    draw_pallets();

    int i;
    for (i = 0; i < LAMP_COUNT; i++)
    {
        /* A lit lamp keeps its color while the room around it dims. */
        if (lamp_on[i])
            draw_box_lit(lamp_x[i], 4.3f, lamp_z[i], 1.2f, 0.15f, 1.2f, 0xFFC0F0FF);
        else
            draw_box(lamp_x[i], 4.3f, lamp_z[i], 1.2f, 0.15f, 1.2f, 0xFF303030);
    }

    /* Terminal: cabinet plus a screen that turns red when it starts
     * printing things nobody typed. */
    draw_box(TERMINAL_X, FLOOR_Y, TERMINAL_Z, 1.0f, 1.2f, 0.6f, 0xFF404048);
    draw_box_lit(TERMINAL_X, FLOOR_Y + 1.2f, TERMINAL_Z, 0.9f, 0.7f, 0.1f,
                 terminal_glitched ? 0xFF2020D0 : 0xFF60D060);
}

/* Simple low-poly placeholder player: body + head, facing -Z.
 * If carrying a box (carrying >= 0), shows a small floating indicator above the head. */
void draw_player(float x, float y, float z, int carrying)
{
    draw_box(x, y, z, 0.6f, 1.0f, 0.4f, 0xFF3050A0);
    draw_box(x, y + 1.0f, z, 0.4f, 0.4f, 0.4f, 0xFFC09060);

    if (carrying >= 0)
    {
        float size = 0.3f;
        draw_box(x, y + 1.6f, z, size, size, size, box_color[carrying]);
    }
}

#define ANALOG_DEADZONE 0.25f
#define MOVE_SPEED 4.0f
#define FRAME_DT (1.0f / 60.0f)

/* Pad state is read once per frame and shared: every state needs it, and
 * edge detection has to happen in exactly one place to stay correct. */
static SceCtrlData pad;
static unsigned int buttons_pressed;

void update_input(void)
{
    static unsigned int prev_buttons = 0;
    sceCtrlReadBufferPositive(&pad, 1);
    buttons_pressed = pad.Buttons & ~prev_buttons;
    prev_buttons = pad.Buttons;
}

/* Reads the analog stick and moves the player, sliding along obstacles. */
void update_player(float *player_x, float *player_z)
{
    float move_x = (pad.Lx - 128) / 128.0f;
    float move_z = (pad.Ly - 128) / 128.0f;

    if (move_x > -ANALOG_DEADZONE && move_x < ANALOG_DEADZONE)
        move_x = 0.0f;
    if (move_z > -ANALOG_DEADZONE && move_z < ANALOG_DEADZONE)
        move_z = 0.0f;

    /* Resolve each axis separately so the player slides along walls/crates. */
    float new_x = *player_x + move_x * MOVE_SPEED * FRAME_DT;
    if (!check_collision(new_x, *player_z))
        *player_x = new_x;

    float new_z = *player_z + move_z * MOVE_SPEED * FRAME_DT;
    if (!check_collision(*player_x, new_z))
        *player_z = new_z;
}

/* X delivers the carried box if standing in the loading zone, otherwise
 * picks up the nearest box in range if empty-handed, or drops the
 * carried box at the player's feet if already carrying one. */
void update_interact(float player_x, float player_z, int *carrying)
{
    if (!(buttons_pressed & PSP_CTRL_CROSS))
        return;

    float zone_dx = player_x - LOADING_ZONE_X;
    float zone_dz = player_z - LOADING_ZONE_Z;
    int in_loading_zone = zone_dx * zone_dx + zone_dz * zone_dz < LOADING_ZONE_RADIUS * LOADING_ZONE_RADIUS;

    if (in_loading_zone && *carrying >= 0)
    {
        BoxType type = (BoxType)*carrying;
        if (current_order.delivered[type] < current_order.required[type])
        {
            current_order.delivered[type]++;
            score += SCORE_PER_BOX;

            if (order_is_complete() && !order_complete)
            {
                order_complete = 1;
                last_bonus = (int)time_left * TIME_BONUS_PER_SECOND;
                last_time_left = time_left;
                score += last_bonus;
                play_sound(snd_order_done);
            }
            else
                play_sound(snd_deliver);
        }
        *carrying = -1;
        return;
    }

    if (*carrying < 0)
    {
        int i;
        for (i = 0; i < box_count; i++)
        {
            Box *b = &boxes[i];
            if (!b->active)
                continue;
            float dx = player_x - b->x;
            float dz = player_z - b->z;
            if (dx * dx + dz * dz < PICKUP_RANGE * PICKUP_RANGE)
            {
                b->active = 0;
                *carrying = b->type;
                play_sound(snd_pickup);
                break;
            }
        }
    }
    else
    {
        int i;
        for (i = 0; i < box_count; i++)
        {
            if (!boxes[i].active)
            {
                boxes[i].active = 1;
                boxes[i].type = (BoxType)*carrying;
                /* Offset so the dropped box doesn't trap the player against it. */
                boxes[i].x = player_x;
                boxes[i].z = player_z + PICKUP_RANGE;
                break;
            }
        }
        *carrying = -1;
        play_sound(snd_drop);
    }
}

#define TIMER_BAR_W 150
#define TIMER_BAR_H 8
#define TIMER_BAR_X (SCR_WIDTH - TIMER_BAR_W - 10)
#define TIMER_BAR_Y 12

void format_time(char *out, float seconds)
{
    int total = (int)seconds;
    sprintf(out, "%02d:%02d", total / 60, total % 60);
}

void draw_text_centered(int y, int scale, unsigned int color, const char *text)
{
    draw_text((SCR_WIDTH - text_width(text, scale)) / 2, y, scale, color, text);
}

/* Opaque backing panel for overlay text. Blending is never enabled, so an
 * alpha-dimmed overlay would just come out solid black; a panel keeps the
 * surrounding 3D scene visible instead. */
void draw_panel(int x, int y, int w, int h)
{
    draw_rect_2d(x - 2, y - 2, w + 4, h + 4, 0xFF808080);
    draw_rect_2d(x, y, w, h, 0xFF201510);
}

void draw_hud(void)
{
    char line[32];

    sprintf(line, "ORDER #%02d", current_order.number);
    draw_text(8, 8, 2, 0xFFFFFFFF, line);

    int i;
    for (i = 0; i < BOX_TYPE_COUNT; i++)
    {
        sprintf(line, "%s %d/%d", box_type_name((BoxType)i),
                current_order.delivered[i], current_order.required[i]);
        int done = current_order.delivered[i] >= current_order.required[i];
        draw_text(8, 32 + i * 16, 2, done ? 0xFF60E060 : 0xFFC0C0C0, line);
    }

    if (carrying >= 0)
    {
        sprintf(line, "CARRYING: %s", box_type_name((BoxType)carrying));
        draw_text(8, 96, 2, box_color[carrying], line);
    }

    format_time(line, time_left);
    draw_text(8, SCR_HEIGHT - 40, 2, time_left < 30.0f ? 0xFF4040E0 : 0xFFFFFFFF, line);

    sprintf(line, "SCORE: %d", score);
    draw_text(8, SCR_HEIGHT - 20, 2, 0xFFFFFFFF, line);

    /* Timer bar mirrors the clock for a read at a glance. */
    float fraction = time_left / ORDER_TIME_LIMIT;
    draw_rect_2d(TIMER_BAR_X - 2, TIMER_BAR_Y - 2,
                 TIMER_BAR_W + 4, TIMER_BAR_H + 4, 0xFF101010);
    draw_rect_2d(TIMER_BAR_X, TIMER_BAR_Y,
                 (int)(TIMER_BAR_W * fraction), TIMER_BAR_H,
                 fraction > 0.3f ? 0xFF40C040 : 0xFF4040E0);
}

/* Triangle holds the full order card up on screen. */
void draw_order_card(void)
{
    char line[32];
    int x = 130, y = 60, w = 220, h = 150;

    draw_panel(x, y, w, h);

    sprintf(line, "ORDER #%02d", current_order.number);
    draw_text_centered(y + 12, 2, 0xFFFFFFFF, line);

    int i;
    for (i = 0; i < BOX_TYPE_COUNT; i++)
    {
        sprintf(line, "%s X%d", box_type_name((BoxType)i), current_order.required[i]);
        draw_text(x + 20, y + 44 + i * 18, 2, box_color[i], line);
    }

    draw_text(x + 20, y + 106, 1, 0xFFA0A0A0, "DELIVERY: LOADING BAY");
    format_time(line, time_left);
    draw_text(x + 20, y + 124, 1, 0xFFA0A0A0, line);
}

static const char *menu_items[] = {"START", "OPTIONS", "EXIT"};
#define MENU_ITEM_COUNT 3

void draw_menu(void)
{
    draw_panel(110, 50, 260, 170);

    draw_text_centered(70, 4, 0xFFFFFFFF, "LAST SHIFT");
    draw_text_centered(104, 1, 0xFF808080, "NIGHT SHIFT WAREHOUSE");

    int i;
    for (i = 0; i < MENU_ITEM_COUNT; i++)
    {
        int selected = (i == menu_selection);
        draw_text_centered(140 + i * 22, 2,
                           selected ? 0xFF40E0E0 : 0xFF909090, menu_items[i]);
    }

    draw_text_centered(SCR_HEIGHT - 24, 1, 0xFF707070, "X SELECT");
}

void draw_order_done_screen(void)
{
    char line[32];
    draw_panel(140, 80, 200, 110);

    draw_text_centered(96, 2, 0xFF60E060, "ORDER COMPLETE");

    format_time(line, last_time_left);
    draw_text_centered(130, 2, 0xFFFFFFFF, line);

    sprintf(line, "BONUS: +%d", last_bonus);
    draw_text_centered(154, 2, 0xFFFFFFFF, line);
}

void draw_shift_done_screen(void)
{
    char line[32];
    draw_panel(130, 60, 220, 150);

    draw_text_centered(76, 3, 0xFF40E0E0, "SHIFT COMPLETE");

    sprintf(line, "ORDERS: %d/%d", ORDER_COUNT, ORDER_COUNT);
    draw_text_centered(118, 2, 0xFFFFFFFF, line);

    sprintf(line, "SCORE: %d", score);
    draw_text_centered(142, 2, 0xFFFFFFFF, line);

    draw_text_centered(166, 2, 0xFF4040E0, "WARNING");
    draw_text_centered(184, 1, 0xFF4040E0, "UNAUTHORIZED EMPLOYEE DETECTED");

    draw_text_centered(SCR_HEIGHT - 26, 2, 0xFFA0A0A0, "PRESS X");
}

void draw_message_overlay(void)
{
    if (message_timer <= 0.0f)
        return;

    int w = 300, h = 46, x = (SCR_WIDTH - w) / 2, y = SCR_HEIGHT - 92;
    draw_panel(x, y, w, h);
    draw_text_centered(y + 8, 2, 0xFF4040E0, message_line1);
    draw_text_centered(y + 28, 1, 0xFFC0C0C0, message_line2);
}

void draw_pause_screen(void)
{
    draw_panel(160, 110, 160, 60);
    draw_text_centered(126, 3, 0xFFFFFFFF, "PAUSED");
    draw_text_centered(152, 1, 0xFFA0A0A0, "START TO RESUME");
}

void reset_game(void)
{
    score = 0;
    shift_complete = 0;
    carrying = -1;
    player_x = 0.0f;
    player_z = 6.0f;

    /* The warehouse starts each shift intact, lights and all. */
    int i;
    for (i = 0; i < LAMP_COUNT; i++)
        lamp_on[i] = 1;
    build_floor();
    terminal_glitched = 0;
    extra_box_present = 0;
    message_timer = 0.0f;

    start_order(0);
}

int main(void)
{
    setup_callbacks();
    init_graphics();
    audio_init();
    build_floor();
    start_order(0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* Third-person camera: fixed offset above and behind the player. */
    const float cam_height = 3.5f;
    const float cam_distance = 5.0f;
    const float player_y = FLOOR_Y;

    ScePspFVector3 cam_up = {0.0f, 1.0f, 0.0f};

    while (1)
    {
        update_input();

        switch (state)
        {
        case STATE_MENU:
            if (buttons_pressed & PSP_CTRL_UP)
            {
                menu_selection = (menu_selection + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
                play_sound(snd_menu);
            }
            if (buttons_pressed & PSP_CTRL_DOWN)
            {
                menu_selection = (menu_selection + 1) % MENU_ITEM_COUNT;
                play_sound(snd_menu);
            }
            if (buttons_pressed & PSP_CTRL_CROSS)
            {
                play_sound(snd_menu);
                if (menu_selection == 0)
                {
                    reset_game();
                    state = STATE_PLAYING;
                }
                else if (menu_selection == 2)
                    sceKernelExitGame();
            }
            break;

        case STATE_PLAYING:
            update_player(&player_x, &player_z);
            update_interact(player_x, player_z, &carrying);

            time_left -= FRAME_DT;
            if (time_left < 0.0f)
                time_left = 0.0f;

            if (message_timer > 0.0f)
                message_timer -= FRAME_DT;

            if (order_complete)
            {
                complete_hold = 0.0f;
                state = STATE_ORDER_DONE;
            }
            else if (buttons_pressed & PSP_CTRL_START)
                state = STATE_PAUSED;
            break;

        case STATE_ORDER_DONE:
            complete_hold += FRAME_DT;
            if (complete_hold >= ORDER_COMPLETE_HOLD)
            {
                carrying = -1;
                if (order_index + 1 < ORDER_COUNT)
                {
                    start_order(order_index + 1);
                    state = STATE_PLAYING;
                }
                else
                {
                    shift_complete = 1;
                    state = STATE_SHIFT_DONE;
                    play_sound(snd_shift_done);
                }
            }
            break;

        case STATE_SHIFT_DONE:
            if (buttons_pressed & PSP_CTRL_CROSS)
            {
                menu_selection = 0;
                state = STATE_MENU;
            }
            break;

        case STATE_PAUSED:
            if (buttons_pressed & PSP_CTRL_START)
                state = STATE_PLAYING;
            break;
        }

        sceGuStart(GU_DIRECT, list);

        sceGuClearColor(0xFF1A0F08);
        sceGuClearDepth(65535);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        /* A 0.5 near plane against a 500 far plane wastes almost all of the
         * 16-bit depth buffer and pushes the transform into bad precision;
         * this range covers the whole room with room to spare. */
        sceGumPerspective(60.0f, 480.0f / 272.0f, 1.0f, 100.0f);

        ScePspFVector3 cam_pos = {player_x, player_y + cam_height, player_z + cam_distance};
        ScePspFVector3 cam_target = {player_x, player_y + 1.0f, player_z};

        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumLookAt(&cam_pos, &cam_target, &cam_up);

        draw_warehouse();
        draw_boxes();
        draw_player(player_x, player_y, player_z, carrying);

        /* All 2D overlays share one depth-test-off block. */
        sceGuDisable(GU_DEPTH_TEST);

        switch (state)
        {
        case STATE_MENU:
            draw_menu();
            break;
        case STATE_PLAYING:
            draw_hud();
            draw_message_overlay();
            if (pad.Buttons & PSP_CTRL_TRIANGLE)
                draw_order_card();
            break;
        case STATE_ORDER_DONE:
            draw_hud();
            draw_order_done_screen();
            break;
        case STATE_SHIFT_DONE:
            draw_shift_done_screen();
            break;
        case STATE_PAUSED:
            draw_hud();
            draw_pause_screen();
            break;
        }

        sceGuEnable(GU_DEPTH_TEST);

        sceGuFinish();
        sceGuSync(0, 0);

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    return 0;
}
