// Runs a WAV file through the analyzer the way the app does (10 ms audio packets, 60 frames a second)
// and prints what it saw as one line of JSON. Driven by tests/run_tests.py.
#include <stdio.h>
#include <stdlib.h>
#include "../src/analysis.h"

static mv::Analyzer an;

static unsigned rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: analysis_test file.wav [columns]\n"); return 2; }
    int columns = argc > 2 ? atoi(argv[2]) : 48;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc(size);
    if (fread(d, 1, size, f) != (size_t)size) return 2;
    fclose(f);
    // minimal WAV reader: 16-bit PCM only
    int chans = 0, rate = 0; long pos = 12, dataAt = 0, dataLen = 0;
    while (pos + 8 <= size) {
        unsigned len = rd32(d + pos + 4);
        if (!memcmp(d + pos, "fmt ", 4)) { chans = d[pos + 10]; rate = (int)rd32(d + pos + 12); }
        if (!memcmp(d + pos, "data", 4)) { dataAt = pos + 8; dataLen = len; break; }
        pos += 8 + len + (len & 1);
    }
    if (!chans || !dataAt) { fprintf(stderr, "not a 16-bit PCM wav\n"); return 2; }
    long frames = dataLen / (2 * chans);
    const short* pcm = (const short*)(d + dataAt);

    an.init(columns, rate);
    int packet = rate / 100;
    double frameEvery = rate / 60.0, nextFrame = frameEvery;
    static float mono[4096];
    static float prev[mv::MAXC]; float maxStep = 0, top = 0;  // biggest change of any column in one frame; tallest column seen
    for (long i = 0; i < frames; i += packet) {
        int n = (int)(frames - i < packet ? frames - i : packet);
        for (int k = 0; k < n; k++) {
            float m = 0; for (int c = 0; c < chans; c++) m += pcm[(i + k) * chans + c] / 32768.f;
            mono[k] = m / chans;
        }
        an.feed(mono, n);
        while (i + n >= nextFrame) {
            an.frame(1 / 60.f, true);
            for (int c = 0; c < columns; c++) {
                float d = an.lv[c] - prev[c]; if (d < 0) d = -d;
                if (d > maxStep) maxStep = d;
                if (an.lv[c] > top) top = an.lv[c];
                prev[c] = an.lv[c];
            }
            nextFrame += frameEvery;
        }
    }
    int best = 0; float lo = 1, hi = 0;
    for (int c = 0; c < columns; c++) {
        if (an.lv[c] > an.lv[best]) best = c;
        if (an.lv[c] < lo) lo = an.lv[c];
        if (an.lv[c] > hi) hi = an.lv[c];
    }
    printf("{\"seconds\":%.2f,\"peak_col\":%d,\"peak_hz\":%.1f,\"lv_max\":%.3f,\"lv_min\":%.3f,\"lv_top\":%.3f,\"max_step\":%.3f,\"lv\":[",
           (double)frames / rate, best, an.centerHz[best], hi, lo, top, maxStep);
    for (int c = 0; c < columns; c++) printf("%s%.3f", c ? "," : "", an.lv[c]);
    printf("]}\n");
    return 0;
}
