/* test_audio.c — WAV sound registration + playback plumbing.
 *
 * Synthesizes a tiny PCM16 WAV in memory, registers it (twice — ids must be
 * distinct and monotonic), rejects malformed bytes with a last-error, and
 * exercises tessera_play_sound including bad ids and a re-trigger. Playback
 * itself is environment-dependent (headless CI has no output device), so the
 * test only asserts the API contract, not that audio was audible.
 */
#include "tessera.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

/* A minimal 16-bit mono PCM WAV: RIFF header + `frames` samples of a sine. */
static unsigned char* make_wav(int rate, int frames, float freq, int* out_len) {
    int data_len = frames * 2;
    int len = 44 + data_len;
    unsigned char* d = (unsigned char*)calloc(1, (size_t)len);
    if (!d) return NULL;
    memcpy(d, "RIFF", 4);
    unsigned riff = (unsigned)(36 + data_len);
    d[4] = riff & 0xFF; d[5] = (riff >> 8) & 0xFF; d[6] = (riff >> 16) & 0xFF; d[7] = (riff >> 24) & 0xFF;
    memcpy(d + 8, "WAVEfmt ", 8);
    d[16] = 16;                       /* fmt chunk size */
    d[20] = 1;                        /* PCM */
    d[22] = 1;                        /* mono */
    d[24] = rate & 0xFF; d[25] = (rate >> 8) & 0xFF; d[26] = (rate >> 16) & 0xFF;
    unsigned br = (unsigned)rate * 2; /* byte rate */
    d[28] = br & 0xFF; d[29] = (br >> 8) & 0xFF; d[30] = (br >> 16) & 0xFF;
    d[32] = 2;                        /* block align */
    d[34] = 16;                       /* bits per sample */
    memcpy(d + 36, "data", 4);
    d[40] = data_len & 0xFF; d[41] = (data_len >> 8) & 0xFF;
    d[42] = (data_len >> 16) & 0xFF; d[43] = (data_len >> 24) & 0xFF;
    for (int i = 0; i < frames; ++i) {
        double s = sin(2.0 * 3.14159265358979 * freq * i / rate) * 0.25 * 32767.0;
        int v = (int)s;
        d[44 + i * 2] = (unsigned char)(v & 0xFF);
        d[44 + i * 2 + 1] = (unsigned char)((v >> 8) & 0xFF);
    }
    *out_len = len;
    return d;
}

int main(void) {
    TesseraConfig cfg = { .width = 64, .height = 64, .pixel_density = 1.0f, .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("create NULL\n"); return 1; }
    /* Sounds are GPU-independent: keep going even with no render backend. */

    int len = 0;
    unsigned char* wav = make_wav(22050, 2205, 440.0f, &len);
    CHECK(wav != NULL);

    TesseraBytes bytes = { .data = wav, .size = (size_t)len, .debug_name = "beep" };
    TesseraSoundId a = tessera_register_sound(e, &bytes);
    TesseraSoundId b = tessera_register_sound(e, &bytes);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);

    /* Malformed bytes must fail with a message, not crash. */
    TesseraBytes junk = { .data = "not a wav at all", .size = 16, .debug_name = "junk" };
    TesseraSoundId c = tessera_register_sound(e, &junk);
    CHECK(c == 0);
    CHECK(tessera_last_error(e)[0] != 0);

    /* Contract: bad ids are false; good ids depend on a device being present,
     * so both plays must simply agree with each other (and not crash). */
    CHECK(!tessera_play_sound(e, 0, 1.0f));
    CHECK(!tessera_play_sound(e, 999, 1.0f));
    bool p1 = tessera_play_sound(e, a, 1.0f);
    bool p2 = tessera_play_sound(e, a, 0.5f);   /* re-trigger restarts */
    bool p3 = tessera_play_sound(e, b, -2.0f);  /* negative gain clamps */
    CHECK(p1 == p2);
    CHECK(p1 == p3);

    /* NULL engine / NULL bytes are safe no-ops. */
    CHECK(tessera_register_sound(NULL, &bytes) == 0);
    CHECK(tessera_register_sound(e, NULL) == 0);
    CHECK(!tessera_play_sound(NULL, a, 1.0f));

    free(wav);
    tessera_destroy(e);
    printf(g_fail ? "test_audio: %d FAILURES\n" : "test_audio: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
