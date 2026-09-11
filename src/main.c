#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>
#include <math.h>

PSP_MODULE_INFO("LAST SHIFT", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define PI 3.14159265f

/* Fog and the cleared background share a color so distance dissolves into
 * the same darkness the room ends in. */
#define FOG_COLOR 0xFF1A0F08

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
static const Note snd_battery[] = {{880, 40}, {1174, 70}, {0, 0}};
static const Note snd_click[] = {{1500, 25}, {0, 0}};

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

/* ---- Ambient bed -------------------------------------------------------
 *
 * A slow drone rather than a tune: three detuned voices a fifth and an
 * octave apart, each swelling on its own slow cycle so they drift in and
 * out of phase and the pad never audibly loops. A little filtered noise
 * underneath stands in for room tone. Synthesised for the same reason the
 * textures are: no files to ship, and it can react to the game.
 *
 * Sine comes from a table; calling sinf per sample for several voices at
 * 44 kHz would be the most expensive thing in the game. */

#define SINE_TABLE_SIZE 256
#define DRONE_VOICES 3

static short sine_table[SINE_TABLE_SIZE];
static unsigned int drone_phase[DRONE_VOICES];
static unsigned int drone_step[DRONE_VOICES];
static unsigned int swell_phase[DRONE_VOICES];
static unsigned int swell_step[DRONE_VOICES];
static int noise_lowpass = 0;

/* Raised as the shift goes wrong; 1.0 is the normal night-shift hum. */
static volatile float ambient_intensity = 1.0f;

static unsigned int hz_to_step(float hz)
{
    return (unsigned int)(hz * 4294967296.0f / (float)SAMPLE_RATE);
}

void build_ambient(void)
{
    int i;
    for (i = 0; i < SINE_TABLE_SIZE; i++)
        sine_table[i] = (short)(sinf(2.0f * PI * i / SINE_TABLE_SIZE) * 32000.0f);

    /* A, its fifth, and the octave below: an open drone with no third, so
     * it stays ambiguous rather than sounding major or minor. */
    drone_step[0] = hz_to_step(55.0f);
    drone_step[1] = hz_to_step(82.5f);
    drone_step[2] = hz_to_step(27.5f);

    /* Deliberately unrelated periods so the swells never line up. */
    swell_step[0] = hz_to_step(0.041f);
    swell_step[1] = hz_to_step(0.029f);
    swell_step[2] = hz_to_step(0.017f);
}

static int ambient_sample(void)
{
    int mixed = 0;
    int v;

    for (v = 0; v < DRONE_VOICES; v++)
    {
        drone_phase[v] += drone_step[v];
        swell_phase[v] += swell_step[v];

        int tone = sine_table[drone_phase[v] >> 24];
        /* Swell rides from a quarter to full volume. */
        int swell = sine_table[swell_phase[v] >> 24] + 32000;
        mixed += (int)((long long)tone * swell >> 18);
    }

    /* Own generator rather than the one the textures use: this runs on the
     * audio thread and must not share state with the game thread. */
    static unsigned int audio_rng = 0x2468ACE1;
    audio_rng = audio_rng * 1103515245u + 12345u;
    int white = (int)((audio_rng >> 16) & 0x7FF) - 1024;

    /* One-pole lowpass turns the hiss into a rumble. */
    noise_lowpass += (white - noise_lowpass) >> 4;
    mixed += noise_lowpass;

    return (int)(mixed * 0.055f * ambient_intensity);
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

        /* Effects sit on top of the drone; clamp so the sum can't wrap. */
        int mixed = sample + ambient_sample();
        if (mixed > 32767)
            mixed = 32767;
        else if (mixed < -32768)
            mixed = -32768;

        buffer[i * 2] = (short)mixed;
        buffer[i * 2 + 1] = (short)mixed;
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
    build_ambient();

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
    /* Double-buffered: one frame is scanned out while the next is drawn.
     * 0x88000 is exactly one 512x272x4 buffer, so the depth buffer follows
     * both color buffers at 0x110000. */
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0, 65535);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    /* Cube and floor faces are wound counter-clockwise as seen from outside,
     * so culling the rest halves what the rasterizer touches. */
    sceGuFrontFace(GU_CCW);
    /* Smooth shading lets the floor's baked light pools blend across tiles. */
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CULL_FACE);

    /* MODULATE multiplies the texture by the vertex color, so per-face
     * shading and baked lamp light survive texturing. */
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuEnable(GU_TEXTURE_2D);

    /* Distance fades into the dark rather than ending at a hard edge. */
    sceGuFog(9.0f, 30.0f, FOG_COLOR);
    sceGuEnable(GU_FOG);

    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(0, 0);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* ---- Player state ------------------------------------------------------
 *
 * Declared up here because the flashlight is part of the lighting model and
 * needs to know where the player is standing and which way they face. */

static float player_x = 0.0f;
static float player_z = 6.0f;
static float player_angle = 0.0f; /* radians; 0 faces -Z */
static float walk_phase = 0.0f;   /* drives the walk cycle */
static float walk_amount = 0.0f;  /* 0 standing, 1 at full speed */

/* Rotates a point from the player's local space into world space. At angle
 * 0 the forward direction is -Z. */
static void rotate_y(float lx, float lz, float angle, float *wx, float *wz)
{
    float c = cosf(angle), s = sinf(angle);
    *wx = lx * c - lz * s;
    *wz = lx * s + lz * c;
}

/* ---- Lighting ----------------------------------------------------------
 *
 * The GE's hardware lighting needs vertex normals, which would mean carrying
 * a normal through every mesh in the game. Instead light is evaluated on the
 * CPU from the lamp positions: the floor gets it baked per vertex (so lamps
 * cast real pools of light) and objects get it per face. Both feed the same
 * flat-colored vertex path that already works. */

#define LAMP_COUNT 4
static const float lamp_x[LAMP_COUNT] = {-6.0f, 6.0f, -6.0f, 6.0f};
static const float lamp_z[LAMP_COUNT] = {-6.0f, -6.0f, 3.0f, 3.0f};
static int lamp_on[LAMP_COUNT] = {1, 1, 1, 1};

/* A lamp the shift's scripted events killed stays dead: the terminal did
 * say maintenance wasn't scheduled. The breaker only brings back the ones
 * that merely tripped. */
static int lamp_dead[LAMP_COUNT] = {0, 0, 0, 0};

static int terminal_glitched = 0;

/* Tuned so a spot directly under a lamp reaches full brightness while the
 * middle of the floor sits near half and the corners fall away: with enough
 * lamps or a weak falloff every point saturates and the room reads as
 * uniformly flat. The ambient floor is low enough that the aisles between
 * the lamps genuinely need the flashlight. */
#define LIGHT_AMBIENT 0.06f
#define LAMP_STRENGTH 0.78f
#define LAMP_FALLOFF 0.17f

/* Flashlight cone carried by the player. It runs on a battery, so the beam
 * shortens and dims as the charge goes, and dies entirely at zero. */
#define FLASH_RANGE 9.0f
#define FLASH_STRENGTH 0.95f
#define FLASH_COS 0.62f     /* cosine of the cone's half angle */
#define FLASH_DRAIN 0.011f /* charge per second: a full battery lasts ~90s */

static int flashlight_on = 1;
static float flashlight_charge = 1.0f;

/* Weak batteries still give some light, so running low is a warning rather
 * than a cliff. */
static float flashlight_power(void)
{
    if (!flashlight_on || flashlight_charge <= 0.0f)
        return 0.0f;
    return 0.35f + 0.65f * flashlight_charge;
}

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

/* The lamps are static and can be baked; this cone moves every frame, so it
 * is kept separate and added on top of the baked level. */
float flashlight_at(float x, float z)
{
    float power = flashlight_power();
    if (power <= 0.0f)
        return 0.0f;

    float range = FLASH_RANGE * power;
    float dx = x - player_x;
    float dz = z - player_z;
    float d2 = dx * dx + dz * dz;
    if (d2 > range * range)
        return 0.0f;

    float d = sqrtf(d2);
    if (d < 0.01f)
        return FLASH_STRENGTH * power;

    float fx = sinf(player_angle);
    float fz = -cosf(player_angle);
    float alignment = (dx * fx + dz * fz) / d;
    if (alignment < FLASH_COS)
        return 0.0f;

    float edge = (alignment - FLASH_COS) / (1.0f - FLASH_COS);
    return FLASH_STRENGTH * power * edge * (1.0f - d / range);
}

/* What an object standing at (x,z) is actually lit by right now. */
float lit_level(float x, float z)
{
    float level = light_at(x, z) + flashlight_at(x, z);
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

/* ---- Textures ----------------------------------------------------------
 *
 * Generated at boot instead of loaded: three small tiling surfaces cost a
 * few dozen lines here and nothing on disk, and the game ships as a single
 * EBOOT with no data files to find at runtime. They are modulated by the
 * vertex color, so per-face shading and lamp light still come through. */

#define TEX_SIZE 64
/* World units covered by one repeat of a texture. */
#define TEX_WORLD 2.0f

static unsigned int __attribute__((aligned(16))) tex_cardboard[TEX_SIZE * TEX_SIZE];
static unsigned int __attribute__((aligned(16))) tex_concrete[TEX_SIZE * TEX_SIZE];
static unsigned int __attribute__((aligned(16))) tex_metal[TEX_SIZE * TEX_SIZE];

static unsigned int rng_state = 0x13572468;

static int noise(int spread)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % (unsigned int)(spread * 2 + 1)) - spread;
}

static int clamp_byte(int value)
{
    if (value < 0)
        return 0;
    return value > 255 ? 255 : value;
}

static unsigned int rgb(int r, int g, int b)
{
    return 0xFF000000u
           | ((unsigned int)clamp_byte(b) << 16)
           | ((unsigned int)clamp_byte(g) << 8)
           | (unsigned int)clamp_byte(r);
}

void build_textures(void)
{
    int x, y;

    for (y = 0; y < TEX_SIZE; y++)
    {
        for (x = 0; x < TEX_SIZE; x++)
        {
            int i = y * TEX_SIZE + x;

            /* Cardboard: corrugation stripes, speckle, and a darker rim so
             * every box face reads as a panel with edges. */
            int n = noise(10);
            int stripe = (y % 8 == 0) ? -18 : 0;
            int edge = (x < 2 || y < 2 || x > TEX_SIZE - 3 || y > TEX_SIZE - 3) ? -45 : 0;
            tex_cardboard[i] = rgb(172 + n + stripe + edge,
                                   132 + n + stripe + edge,
                                   88 + n + stripe + edge);

            /* Concrete: fine grain with expansion joints on a 32px grid. */
            n = noise(12);
            int joint = (x % 32 == 0 || y % 32 == 0) ? -28 : 0;
            tex_concrete[i] = rgb(118 + n + joint, 118 + n + joint, 120 + n + joint);

            /* Metal: vertical brushing plus a highlight every 16px. */
            n = noise(8);
            int brush = ((x * 7) % 16 < 2) ? 16 : 0;
            tex_metal[i] = rgb(92 + n + brush, 104 + n + brush, 126 + n + brush);
        }
    }

    /* The GE reads textures straight from RAM. */
    sceKernelDcacheWritebackAll();
}

void set_texture(const void *texture)
{
    sceGuTexImage(0, TEX_SIZE, TEX_SIZE, TEX_SIZE, texture);
}

/* Textured, colored world vertex. PSP vertex order is fixed: texture,
 * then color, then position. */
typedef struct
{
    float u, v;
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
void draw_box_rotated(float x, float y, float z, float sx, float sy, float sz,
                       unsigned int color, float light, int shade_faces, float angle)
{
    VertexC *v = (VertexC *)sceGuGetMemory(CUBE_VERT_COUNT * sizeof(VertexC));

    /* The color only changes per face, so resolve it six times rather than
     * once per vertex. */
    unsigned int face_color[6];
    int f;
    for (f = 0; f < 6; f++)
        face_color[f] = scale_color(color, shade_faces ? face_shade[f] * light : light);

    /* UVs are derived from the object's world size so texel density stays
     * constant: a long wall tiles the texture instead of stretching it. */
    float us = sx / TEX_WORLD, vs = sy / TEX_WORLD, ws = sz / TEX_WORLD;

    unsigned int i;
    for (i = 0; i < CUBE_VERT_COUNT; i++)
    {
        float lx = cube_verts[i].x, ly = cube_verts[i].y, lz = cube_verts[i].z;

        switch (i / 6)
        {
        case 0: /* front */
        case 1: /* back */
            v[i].u = (lx + 0.5f) * us;
            v[i].v = ly * vs;
            break;
        case 2: /* left */
        case 3: /* right */
            v[i].u = (lz + 0.5f) * ws;
            v[i].v = ly * vs;
            break;
        default: /* top and bottom */
            v[i].u = (lx + 0.5f) * us;
            v[i].v = (lz + 0.5f) * ws;
            break;
        }

        v[i].color = face_color[i / 6];

        float ox = lx * sx, oz = lz * sz;
        if (angle != 0.0f)
            rotate_y(ox, oz, angle, &ox, &oz);

        v[i].x = ox + x;
        v[i].y = ly * sy + y;
        v[i].z = oz + z;
    }

    sceGuDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    CUBE_VERT_COUNT, 0, v);
}

void draw_box_shaded(float x, float y, float z, float sx, float sy, float sz,
                      unsigned int color, float light, int shade_faces)
{
    draw_box_rotated(x, y, z, sx, sy, sz, color, light, shade_faces, 0.0f);
}

/* World objects are lit by the lamps and by the player's flashlight. */
void draw_box(float x, float y, float z, float sx, float sy, float sz, unsigned int color)
{
    draw_box_shaded(x, y, z, sx, sy, sz, color, lit_level(x, z), 1);
}

/* Lamps and terminal screens emit rather than receive light, and a surface
 * texture would only muddy them, so they draw untextured. */
void draw_box_lit(float x, float y, float z, float sx, float sy, float sz, unsigned int color)
{
    sceGuDisable(GU_TEXTURE_2D);
    draw_box_shaded(x, y, z, sx, sy, sz, color, 1.0f, 0);
    sceGuEnable(GU_TEXTURE_2D);
}

/* The lamps' contribution to the floor is baked once per lamp change, since
 * it never moves. The flashlight is added on top every frame. */
#define FLOOR_COLOR 0xFF6A6A6A
static VertexC __attribute__((aligned(16))) floor_verts[FLOOR_VERT_COUNT];
static float floor_base_light[FLOOR_VERT_COUNT];

static void put_floor_vertex(int n, float x, float z)
{
    floor_base_light[n] = light_at(x, z);

    floor_verts[n].u = x / TEX_WORLD;
    floor_verts[n].v = z / TEX_WORLD;
    floor_verts[n].color = scale_color(FLOOR_COLOR, floor_base_light[n]);
    floor_verts[n].x = x;
    floor_verts[n].y = FLOOR_Y;
    floor_verts[n].z = z;
}

/* Re-colors the floor for the flashlight's current position and heading.
 * Vertices outside the cone's reach just get their baked color back, so the
 * expensive part only runs for the few hundred vertices actually near the
 * player. */
void update_floor_lighting(void)
{
    int i;
    for (i = 0; i < FLOOR_VERT_COUNT; i++)
    {
        float extra = flashlight_at(floor_verts[i].x, floor_verts[i].z);
        float level = floor_base_light[i] + extra;
        if (level > 1.0f)
            level = 1.0f;
        floor_verts[i].color = scale_color(FLOOR_COLOR, level);
    }
    sceKernelDcacheWritebackRange(floor_verts, sizeof(floor_verts));
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
    int row, col;

    /* Runs of lit pixels in a row become one quad instead of one each, which
     * is roughly a third of the geometry for the same glyph. */
    int runs = 0;
    for (row = 0; row < FONT_H; row++)
    {
        int in_run = 0;
        for (col = 0; col < FONT_W; col++)
        {
            int on = glyph[row] & (1 << (FONT_W - 1 - col));
            if (on && !in_run)
                runs++;
            in_run = on;
        }
    }

    if (runs == 0)
        return;

    Vertex2D *v = (Vertex2D *)sceGuGetMemory(runs * 6 * sizeof(Vertex2D));
    int n = 0;

    for (row = 0; row < FONT_H; row++)
    {
        col = 0;
        while (col < FONT_W)
        {
            if (!(glyph[row] & (1 << (FONT_W - 1 - col))))
            {
                col++;
                continue;
            }

            int start = col;
            while (col < FONT_W && (glyph[row] & (1 << (FONT_W - 1 - col))))
                col++;

            short x0 = x + start * scale, y0 = y + row * scale;
            short x1 = x + col * scale, y1 = y0 + scale;

            v[n].color = color; v[n].x = x0; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x0; v[n].y = y1; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y0; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x1; v[n].y = y1; v[n].z = 0; n++;
            v[n].color = color; v[n].x = x0; v[n].y = y1; v[n].z = 0; n++;
        }
    }

    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                    runs * 6, 0, v);
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

/* Every box is the same crate. The type is only readable from the colored
 * label on it, and the label only shows once there is light on it, so a
 * dark aisle has to actually be searched instead of scanned from the door.
 * Different sizes per type would give the game away from across the room. */
#define BOX_SIZE 0.55f
#define BOX_IDENTIFY_LIGHT 0.3f
#define BOX_CARDBOARD 0xFF6A94C0

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

/* Spare batteries lying around the warehouse. Picked up by walking into
 * them rather than with X, so they never compete with grabbing a box. */
#define BATTERY_COUNT 3
#define BATTERY_PICKUP_RANGE 0.8f

typedef struct
{
    float x, z;
    int active;
} Battery;

static Battery batteries[BATTERY_COUNT];

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

    /* Fresh batteries each order, on spawn points the stock didn't take,
     * walking backwards through the list so they land away from the boxes. */
    int b;
    for (b = 0; b < BATTERY_COUNT; b++)
    {
        int point = (SPAWN_POINT_COUNT - 1 - (index * 2 + b * 5)) % SPAWN_POINT_COUNT;
        if (point < 0)
            point += SPAWN_POINT_COUNT;

        batteries[b].x = spawn_x[point];
        batteries[b].z = spawn_z[point];
        batteries[b].active = 1;
    }
}

/* Lamps trip on their own through the shift. Resetting them costs a walk to
 * the breaker, so there is a standing choice between spending time on light
 * and just working the aisles dark. */
#define BREAKER_X -9.3f
#define BREAKER_Z 2.0f
#define BREAKER_RANGE 1.3f
#define LAMP_TRIP_INTERVAL 38.0f

static float lamp_trip_timer = LAMP_TRIP_INTERVAL;

void trip_random_lamp(void)
{
    int candidates[LAMP_COUNT];
    int n = 0, i;

    for (i = 0; i < LAMP_COUNT; i++)
        if (lamp_on[i])
            candidates[n++] = i;

    /* Never leave the room completely black; that isn't tension, just a
     * dead end with a dying flashlight. */
    if (n <= 1)
        return;

    int pick = candidates[(noise(1000) + 1000) % n];
    lamp_on[pick] = 0;
    build_floor();

    play_sound(snd_horror);
    show_message("BREAKER TRIPPED", "RESET PANEL ON WEST WALL", 3.5f);
}

void reset_breaker(void)
{
    int i, restored = 0;
    for (i = 0; i < LAMP_COUNT; i++)
    {
        if (!lamp_on[i] && !lamp_dead[i])
        {
            lamp_on[i] = 1;
            restored++;
        }
    }

    if (restored)
    {
        build_floor();
        play_sound(snd_deliver);
        show_message("POWER RESTORED", "", 2.0f);
    }
    else
        play_sound(snd_click);
}

/* Nothing chases the player and nothing jumps out; the shift just stops
 * adding up. Each event fires as the following order is handed out. */
void apply_shift_events(int index)
{
    if (index >= 2)
    {
        lamp_on[0] = 0;
        lamp_dead[0] = 1; /* this one the breaker won't bring back */
        build_floor();    /* rebake the light pools without that lamp */
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
        float half = BOX_SIZE * 0.5f;
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
    set_texture(tex_cardboard);

    int i;
    for (i = 0; i < box_count; i++)
    {
        Box *b = &boxes[i];
        if (!b->active)
            continue;

        draw_box(b->x, FLOOR_Y, b->z, BOX_SIZE, BOX_SIZE, BOX_SIZE, BOX_CARDBOARD);

        /* The label is what names the box, and it needs light to be read. */
        float light = lit_level(b->x, b->z);
        if (light >= BOX_IDENTIFY_LIGHT)
            draw_box_shaded(b->x, FLOOR_Y + BOX_SIZE * 0.55f, b->z,
                             BOX_SIZE * 1.02f, BOX_SIZE * 0.22f, BOX_SIZE * 1.02f,
                             box_color[b->type], light, 1);
    }
}

void draw_floor(void)
{
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();

    set_texture(tex_concrete);
    sceGuDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
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

void draw_batteries(void)
{
    int i;
    for (i = 0; i < BATTERY_COUNT; i++)
    {
        if (!batteries[i].active)
            continue;

        /* Drawn unlit so they stay findable in a dark aisle. */
        draw_box_lit(batteries[i].x, FLOOR_Y, batteries[i].z,
                     0.16f, 0.34f, 0.16f, 0xFF30D0D0);
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

    set_texture(tex_concrete);
    draw_wall_along_x(-10.0f, 0xFF806040);
    draw_wall_along_z(-10.0f, 0xFF806040);
    draw_wall_along_z(10.0f, 0xFF806040);

    set_texture(tex_metal);

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

    /* Breaker panel on the west wall, with a status light that goes red
     * while anything is tripped so it's findable from across the room. */
    int tripped = 0;
    for (i = 0; i < LAMP_COUNT; i++)
        if (!lamp_on[i] && !lamp_dead[i])
            tripped = 1;

    draw_box(BREAKER_X, FLOOR_Y + 0.8f, BREAKER_Z, 0.25f, 1.0f, 0.9f, 0xFF4A4A52);
    draw_box_lit(BREAKER_X - 0.1f, FLOOR_Y + 1.45f, BREAKER_Z, 0.08f, 0.14f, 0.14f,
                 tripped ? 0xFF3030E0 : 0xFF40D040);

    /* Terminal: cabinet plus a screen that turns red when it starts
     * printing things nobody typed. */
    draw_box(TERMINAL_X, FLOOR_Y, TERMINAL_Z, 1.0f, 1.2f, 0.6f, 0xFF404048);
    draw_box_lit(TERMINAL_X, FLOOR_Y + 1.2f, TERMINAL_Z, 0.9f, 0.7f, 0.1f,
                 terminal_glitched ? 0xFF2020D0 : 0xFF60D060);
}

/* Low-poly worker built from boxes: torso, head, two arms and two legs,
 * turned to face the direction of travel. Limbs swing from the walk phase,
 * arms opposite the legs, and the whole body bobs slightly in step. */
void draw_player(float y, int carrying)
{
    /* Places a body part given in local space (X right, Z forward = -Z). */
    #define PART(lx, ly, lz, sx, sy, sz, col)                                  \
        do {                                                                   \
            float wx, wz;                                                      \
            rotate_y((lx), (lz), player_angle, &wx, &wz);                      \
            draw_box_rotated(player_x + wx, (ly), player_z + wz,               \
                              (sx), (sy), (sz), (col),                         \
                              lit_level(player_x, player_z), 1, player_angle); \
        } while (0)

    float swing = sinf(walk_phase) * 0.22f * walk_amount;
    float bob = sinf(walk_phase * 2.0f) * 0.035f * walk_amount;

    set_texture(tex_metal);

    /* legs */
    PART(-0.13f, y + bob, swing, 0.16f, 0.45f, 0.16f, 0xFF503828);
    PART(0.13f, y + bob, -swing, 0.16f, 0.45f, 0.16f, 0xFF503828);

    /* torso and head */
    PART(0.0f, y + 0.45f + bob, 0.0f, 0.48f, 0.62f, 0.30f, 0xFF3050A0);
    PART(0.0f, y + 1.07f + bob, 0.0f, 0.30f, 0.28f, 0.28f, 0xFFA08058);

    /* arms swing opposite the legs */
    PART(-0.31f, y + 0.52f + bob, -swing, 0.13f, 0.50f, 0.13f, 0xFF2A4488);
    PART(0.31f, y + 0.52f + bob, swing, 0.13f, 0.50f, 0.13f, 0xFF2A4488);

    /* Flashlight in the right hand, pointing the way the body faces. The
     * lens is drawn unlit so it reads as the thing emitting the beam. */
    float hand_z = swing - 0.18f;
    PART(0.31f, y + 0.42f + bob, hand_z, 0.11f, 0.11f, 0.34f, 0xFF303038);

    if (flashlight_power() > 0.0f)
    {
        float wx, wz;
        rotate_y(0.31f, hand_z - 0.20f, player_angle, &wx, &wz);
        draw_box_lit(player_x + wx, y + 0.42f + bob, player_z + wz,
                     0.13f, 0.13f, 0.06f, 0xFF90E8FF);
    }

    /* The carried box is held in front of the chest rather than floating. */
    if (carrying >= 0)
    {
        float size = BOX_SIZE * 0.8f;
        set_texture(tex_cardboard);
        PART(0.0f, y + 0.55f + bob, -0.38f, size, size, size, box_color[carrying]);
    }

    #undef PART
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

#define TURN_RATE 9.0f
#define SPRINT_MULTIPLIER 1.75f
#define STAMINA_DRAIN 0.28f
#define STAMINA_RECOVER 0.18f

static float stamina = 1.0f;
static int sprint_locked = 0; /* forces a rest once stamina bottoms out */

/* Reads the analog stick, moves the player, turns them to face where they
 * are going and advances the walk cycle. */
void update_player(void)
{
    float move_x = (pad.Lx - 128) / 128.0f;
    float move_z = (pad.Ly - 128) / 128.0f;

    if (move_x > -ANALOG_DEADZONE && move_x < ANALOG_DEADZONE)
        move_x = 0.0f;
    if (move_z > -ANALOG_DEADZONE && move_z < ANALOG_DEADZONE)
        move_z = 0.0f;

    float speed = sqrtf(move_x * move_x + move_z * move_z);
    if (speed > 1.0f)
        speed = 1.0f;

    if (speed > 0.0f)
    {
        /* Turn toward the heading by the shortest way round rather than
         * snapping, so direction changes read as the body pivoting. */
        float target = atan2f(move_x, -move_z);
        float diff = target - player_angle;
        while (diff > PI)
            diff -= 2.0f * PI;
        while (diff < -PI)
            diff += 2.0f * PI;

        float step = diff * TURN_RATE * FRAME_DT;
        if ((diff > 0.0f && step > diff) || (diff < 0.0f && step < diff))
            step = diff;
        player_angle += step;

        walk_phase += speed * 9.0f * FRAME_DT;
    }

    /* Sprint on R, but only while there's stamina and after a full rest if
     * it ever ran dry, so mashing it isn't a free speed boost. */
    int wants_sprint = (pad.Buttons & PSP_CTRL_RTRIGGER) && speed > 0.0f;
    int sprinting = wants_sprint && !sprint_locked && stamina > 0.0f;

    if (sprinting)
    {
        stamina -= STAMINA_DRAIN * FRAME_DT;
        if (stamina <= 0.0f)
        {
            stamina = 0.0f;
            sprint_locked = 1;
        }
    }
    else
    {
        stamina += STAMINA_RECOVER * FRAME_DT;
        if (stamina > 1.0f)
            stamina = 1.0f;
        if (stamina > 0.35f)
            sprint_locked = 0;
    }

    float pace = sprinting ? SPRINT_MULTIPLIER : 1.0f;

    /* Ease the animation weight so stopping doesn't freeze mid-stride. */
    walk_amount += (speed * pace - walk_amount) * 0.2f;

    /* Resolve each axis separately so the player slides along walls/crates. */
    float new_x = player_x + move_x * MOVE_SPEED * pace * FRAME_DT;
    if (!check_collision(new_x, player_z))
        player_x = new_x;

    float new_z = player_z + move_z * MOVE_SPEED * pace * FRAME_DT;
    if (!check_collision(player_x, new_z))
        player_z = new_z;

    /* Batteries are collected by walking over them. */
    int i;
    for (i = 0; i < BATTERY_COUNT; i++)
    {
        if (!batteries[i].active)
            continue;

        float dx = player_x - batteries[i].x;
        float dz = player_z - batteries[i].z;
        if (dx * dx + dz * dz < BATTERY_PICKUP_RANGE * BATTERY_PICKUP_RANGE)
        {
            batteries[i].active = 0;
            flashlight_charge = 1.0f;
            play_sound(snd_battery);
        }
    }

    /* The flashlight only drains while it is actually lit, which is what
     * makes switching it off in a lit aisle worth doing. */
    if (flashlight_on && flashlight_charge > 0.0f)
    {
        flashlight_charge -= FLASH_DRAIN * FRAME_DT;
        if (flashlight_charge < 0.0f)
            flashlight_charge = 0.0f;
    }
}

/* X delivers the carried box if standing in the loading zone, otherwise
 * picks up the nearest box in range if empty-handed, or drops the
 * carried box at the player's feet if already carrying one. */
void update_interact(float player_x, float player_z, int *carrying)
{
    if (!(buttons_pressed & PSP_CTRL_CROSS))
        return;

    float breaker_dx = player_x - BREAKER_X;
    float breaker_dz = player_z - BREAKER_Z;
    if (breaker_dx * breaker_dx + breaker_dz * breaker_dz < BREAKER_RANGE * BREAKER_RANGE)
    {
        reset_breaker();
        return;
    }

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

    /* Battery and stamina, stacked under the clock. */
    int bar_y = TIMER_BAR_Y + TIMER_BAR_H + 6;
    draw_text(TIMER_BAR_X - 34, bar_y - 1, 1, 0xFF909090, "BAT");
    draw_rect_2d(TIMER_BAR_X - 2, bar_y - 2, TIMER_BAR_W + 4, TIMER_BAR_H + 4, 0xFF101010);
    draw_rect_2d(TIMER_BAR_X, bar_y, (int)(TIMER_BAR_W * flashlight_charge), TIMER_BAR_H,
                 flashlight_charge > 0.25f ? 0xFF30D0D0 : 0xFF4040E0);

    bar_y += TIMER_BAR_H + 6;
    draw_text(TIMER_BAR_X - 34, bar_y - 1, 1, 0xFF909090, "RUN");
    draw_rect_2d(TIMER_BAR_X - 2, bar_y - 2, TIMER_BAR_W + 4, TIMER_BAR_H + 4, 0xFF101010);
    draw_rect_2d(TIMER_BAR_X, bar_y, (int)(TIMER_BAR_W * stamina), TIMER_BAR_H,
                 sprint_locked ? 0xFF4060A0 : 0xFFC0C060);

    if (flashlight_charge <= 0.0f)
        draw_text_centered(SCR_HEIGHT - 60, 1, 0xFF4040E0, "FLASHLIGHT DEAD - FIND A BATTERY");
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
    player_angle = 0.0f;
    walk_phase = 0.0f;
    walk_amount = 0.0f;
    stamina = 1.0f;
    sprint_locked = 0;
    flashlight_on = 1;
    flashlight_charge = 1.0f;
    ambient_intensity = 1.0f;

    /* The warehouse starts each shift intact, lights and all. */
    int i;
    for (i = 0; i < LAMP_COUNT; i++)
    {
        lamp_on[i] = 1;
        lamp_dead[i] = 0;
    }
    lamp_trip_timer = LAMP_TRIP_INTERVAL;
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
    build_textures();
    build_floor();
    start_order(0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* Third-person camera: fixed angle, trailing the player. The angle stays
     * world-aligned on purpose. Movement is world-relative, so a camera that
     * swung around with the player would keep redefining which way "up" on
     * the stick means. */
    const float cam_height = 3.5f;
    const float cam_distance = 5.0f;
    const float player_y = FLOOR_Y;
    #define CAM_LAG 0.12f

    float cam_follow_x = player_x;
    float cam_follow_z = player_z;

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
            update_player();
            update_interact(player_x, player_z, &carrying);

            if (buttons_pressed & PSP_CTRL_CIRCLE)
            {
                flashlight_on = !flashlight_on;
                play_sound(snd_click);
            }

            /* The drone tightens as the shift wears on. */
            ambient_intensity = 1.0f + order_index * 0.22f;

            lamp_trip_timer -= FRAME_DT;
            if (lamp_trip_timer <= 0.0f)
            {
                trip_random_lamp();
                lamp_trip_timer = LAMP_TRIP_INTERVAL;
            }

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

        /* The camera trails the player instead of being welded to them, so
         * starting and stopping has a little weight to it. */
        cam_follow_x += (player_x - cam_follow_x) * CAM_LAG;
        cam_follow_z += (player_z - cam_follow_z) * CAM_LAG;

        float bob = sinf(walk_phase * 2.0f) * 0.06f * walk_amount;

        ScePspFVector3 cam_pos = {cam_follow_x, player_y + cam_height + bob,
                                   cam_follow_z + cam_distance};
        ScePspFVector3 cam_target = {player_x, player_y + 1.0f, player_z};

        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumLookAt(&cam_pos, &cam_target, &cam_up);

        update_floor_lighting();

        draw_warehouse();
        draw_boxes();
        draw_batteries();
        draw_player(player_y, carrying);

        /* The 2D overlays are flat colored quads: no depth, no texture and
         * no fog, all of which belong to the world pass only. Culling goes
         * too, because screen space has Y pointing down, which makes these
         * quads wind the opposite way from the world's front faces. */
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_CULL_FACE);

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
        sceGuEnable(GU_TEXTURE_2D);
        sceGuEnable(GU_FOG);
        sceGuEnable(GU_CULL_FACE);

        sceGuFinish();
        sceGuSync(0, 0);

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    return 0;
}
