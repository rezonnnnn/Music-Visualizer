// Audio analysis for Music Visualizer: turns mono samples into column heights.
// No Windows code in here, so it is tested on its own (see tests/analysis_test.cpp).
#pragma once
#include <math.h>
#include <string.h>

namespace mv {

enum { FFT_N = 2048, RING = 4096, MAXC = 96 };

struct Analyzer {
    // ---- results, read by the drawing code
    float lv[MAXC];        // column heights, 0 to 1
    float centerHz[MAXC];  // centre frequency of each column

    // ---- setup
    void init(int columns, int sampleRate) {
        memset(this, 0, sizeof(*this));
        rate = sampleRate > 0 ? sampleRate : 48000;
        peakDb = -40;
        for (int i = 0; i < FFT_N; i++) {
            hann[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (FFT_N - 1));
            unsigned r = 0; for (int b = 0; b < 11; b++) if (i & (1 << b)) r |= 1u << (10 - b);
            brev[i] = (unsigned short)r;
        }
        for (int k = 0; k < FFT_N / 2; k++) { cosT[k] = cosf(6.2831853f * k / FFT_N); sinT[k] = -sinf(6.2831853f * k / FFT_N); }
        setColumns(columns);
    }
    void setColumns(int columns) {
        cols = columns < 1 ? 1 : columns > MAXC ? MAXC : columns;
        for (int i = 0; i <= cols; i++) edge[i] = 30.f * powf(16000.f / 30.f, (float)i / cols) * FFT_N / rate;
        for (int i = 0; i < cols; i++) {
            centerHz[i] = 30.f * powf(16000.f / 30.f, (i + 0.5f) / cols);
            tilt[i] = 3.f * log2f(centerHz[i] / 1000.f);     // lift the highs so they read as loud as the bass
        }
        memset(lv, 0, sizeof(lv)); memset(vel, 0, sizeof(vel));
    }

    // ---- feed audio as it arrives (any chunk size)
    void feed(const float* mono, int n) {
        for (int i = 0; i < n; i++) ring[rpos++ & (RING - 1)] = mono[i];
    }

    // ---- once per displayed frame. live = audio arrived recently
    void frame(float dt, bool live) {
        float raw[MAXC];
        if (!live) { for (int c = 0; c < cols; c++) raw[c] = 0; }
        else {
            unsigned start = rpos - FFT_N;
            for (int i = 0; i < FFT_N; i++) { re[brev[i]] = ring[(start + i) & (RING - 1)] * hann[i]; im[brev[i]] = 0; }
            fft();
            const float norm = 1.f / ((FFT_N / 4.f) * (FFT_N / 4.f));  // a full-scale sine reads 0 dB
            float mx = -200;
            for (int c = 0; c < cols; c++) {
                float b0 = edge[c], b1 = edge[c + 1], p;
                if (b1 - b0 < 1.f) {  // narrower than one FFT bin: interpolate
                    float m = (b0 + b1) * 0.5f; int k = (int)m; float f = m - k;
                    p = power(k) * (1 - f) + power(k + 1) * f;
                } else {
                    int k0 = (int)b0, k1 = (int)ceilf(b1); if (k1 > FFT_N / 2) k1 = FFT_N / 2;
                    p = 0; for (int k = k0; k < k1; k++) p += power(k);
                    p /= (k1 - k0);
                }
                raw[c] = 10.f * log10f(p * norm + 1e-20f) + tilt[c];
                if (raw[c] > mx) mx = raw[c];
            }
            // automatic gain: follow the loudest column, fall 6 dB a second, never below -60 dB
            peakDb = mx > peakDb - 6.f * dt ? mx : peakDb - 6.f * dt;
            if (peakDb < -60) peakDb = -60;
            for (int c = 0; c < cols; c++) { float v = (raw[c] - (peakDb - 42.f)) / 42.f; raw[c] = v < 0 ? 0 : v > 1 ? 1 : v; }
        }
        // Each column glides to its target on a critically damped spring (quick up, slow down), so it never
        // snaps or overshoots. Stepped in 4 ms slices to stay stable at any frame rate.
        if (dt > 0.1f) dt = 0.1f;
        int steps = (int)(dt / 0.004f) + 1; float h = dt / steps;
        for (int s = 0; s < steps; s++) for (int c = 0; c < cols; c++) {
            float w = raw[c] > lv[c] ? 38.f : 14.f;
            vel[c] += (w * w * (raw[c] - lv[c]) - 2.f * w * vel[c]) * h;
            lv[c] += vel[c] * h;
            if (lv[c] < 0) { lv[c] = 0; vel[c] = 0; } else if (lv[c] > 1) { lv[c] = 1; vel[c] = 0; }
        }
    }

private:
    int cols, rate;
    unsigned rpos;
    float ring[RING], hann[FFT_N], cosT[FFT_N / 2], sinT[FFT_N / 2], re[FFT_N], im[FFT_N];
    unsigned short brev[FFT_N];
    float edge[MAXC + 1], tilt[MAXC], vel[MAXC];
    float peakDb;

    float power(int k) const { return re[k] * re[k] + im[k] * im[k]; }
    void fft() {
        for (int len = 2; len <= FFT_N; len <<= 1) {
            int half = len >> 1, step = FFT_N / len;
            for (int i = 0; i < FFT_N; i += len)
                for (int j = 0; j < half; j++) {
                    float wr = cosT[j * step], wi = sinT[j * step];
                    int a = i + j, b = a + half;
                    float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                }
        }
    }
};

}  // namespace mv
