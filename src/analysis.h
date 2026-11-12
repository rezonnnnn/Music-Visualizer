// Audio analysis for Music Visualizer: turns mono samples into column heights, a tempo estimate
// and beat pulses. No Windows code in here, so it is tested on its own
// (see tests/analysis_test.cpp).
#pragma once
#include <math.h>
#include <string.h>

namespace mv {

enum { FFT_N = 2048, RING = 4096, MAXC = 96, ENV_N = 1024, MAX_LAG = 400 };

struct Analyzer {
    // ---- results, read by the drawing code
    float lv[MAXC];        // column heights, 0 to 1
    float centerHz[MAXC];  // centre frequency of each column
    float rmsDb;           // loudness of the last 43 ms, dB below full scale (-99 = silent)
    float bpm;             // tempo estimate, 0 while unknown
    float glitch;          // jumps to 1 on a beat, falls to 0 in about 90 ms

    // ---- setup
    void init(int columns, int sampleRate) {
        memset(this, 0, sizeof(*this));
        rate = sampleRate > 0 ? sampleRate : 48000;
        rmsDb = -99; peakDb = -40;
        for (int i = 0; i < FFT_N; i++) {
            hann[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (FFT_N - 1));
            unsigned r = 0; for (int b = 0; b < 11; b++) if (i & (1 << b)) r |= 1u << (10 - b);
            brev[i] = (unsigned short)r;
        }
        for (int k = 0; k < FFT_N / 2; k++) { cosT[k] = cosf(6.2831853f * k / FFT_N); sinT[k] = -sinf(6.2831853f * k / FFT_N); }
        hop = rate / 100;                                    // tempo is tracked in 10 ms steps
        lpK = 1.f - expf(-6.2831853f * 150.f / rate);        // 150 Hz low-pass picks out kick and bass
        setColumns(columns);
        resetTempo();
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
    void resetTempo() {
        memset(env, 0, sizeof(env)); memset(bpmHist, 0, sizeof(bpmHist));
        envPos = envCount = hopFill = sinceBeatHops = sinceTempoHops = bpmHistN = 0;
        lowAcc = fullAcc = lpState = envMean = 0; prevLow = prevFull = 0; bpm = 0;
    }

    // ---- feed audio as it arrives (any chunk size)
    void feed(const float* mono, int n) {
        for (int i = 0; i < n; i++) {
            float s = mono[i];
            ring[rpos++ & (RING - 1)] = s;
            lpState += (s - lpState) * lpK;
            lowAcc += lpState * lpState; fullAcc += s * s;
            if (++hopFill >= hop) endHop();
        }
    }

    // ---- once per displayed frame. live = audio arrived recently
    void frame(float dt, bool live) {
        float raw[MAXC];
        if (!live) { for (int c = 0; c < cols; c++) raw[c] = 0; rmsDb = -99; }
        else {
            unsigned start = rpos - FFT_N; float ss = 0;
            for (int i = 0; i < FFT_N; i++) {
                float s = ring[(start + i) & (RING - 1)];
                ss += s * s; re[brev[i]] = s * hann[i]; im[brev[i]] = 0;
            }
            fft();
            float rms = sqrtf(ss / FFT_N);
            rmsDb = rms > 1e-5f ? 20.f * log10f(rms) : -99;
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
        glitch -= dt / 0.09f; if (glitch < 0) glitch = 0;
    }

private:
    int cols, rate, hop, hopFill, envPos, envCount, sinceBeatHops, sinceTempoHops, bpmHistN;
    unsigned rpos;
    float ring[RING], hann[FFT_N], cosT[FFT_N / 2], sinT[FFT_N / 2], re[FFT_N], im[FFT_N];
    unsigned short brev[FFT_N];
    float edge[MAXC + 1], tilt[MAXC], vel[MAXC];
    float peakDb, lpK, lpState, lowAcc, fullAcc, prevLow, prevFull, envMean;
    float env[ENV_N];      // onset strength, one value per 10 ms, newest at envPos-1
    float bpmHist[5];

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
    // Every 10 ms: how much louder did the bass and the whole signal just get? That is the "onset strength".
    void endHop() {
        float low = lowAcc / hopFill, full = fullAcc / hopFill;
        lowAcc = fullAcc = 0; hopFill = 0;
        float o = 0;
        if (full > 1e-9f) {  // above about -90 dB; below that it is silence and noise
            if (prevLow > 1e-10f) { float d = 10.f * log10f(low / prevLow + 1e-12f); if (d > 0) o += d > 20 ? 20 : d; }
            if (prevFull > 1e-10f) { float d = 10.f * log10f(full / prevFull + 1e-12f); if (d > 0) o += 0.5f * (d > 20 ? 20 : d); }
        }
        prevLow = low; prevFull = full;
        env[envPos] = o; envPos = (envPos + 1) & (ENV_N - 1);
        if (envCount < ENV_N) envCount++;
        // beat pulse: a jump well above the recent average, at most five a second
        envMean += (o - envMean) * 0.01f;
        sinceBeatHops++;
        if (o > 8.f && o > 4.f * envMean && sinceBeatHops >= 20) { sinceBeatHops = 0; glitch = 1; }
        if (++sinceTempoHops >= 50) { sinceTempoHops = 0; if (envCount >= 400) estimateTempo(); }  // twice a second, once 4 s are in
    }
    // Tempo = the delay at which the onset pattern best lines up with itself.
    void estimateTempo() {
        int n = envCount < 1000 ? envCount : 1000;  // look at the last 10 seconds at most
        static float x[1000];
        float mean = 0;
        for (int i = 0; i < n; i++) { x[i] = env[(envPos - n + i) & (ENV_N - 1)]; mean += x[i]; }
        mean /= n;
        float r0 = 0;
        for (int i = 0; i < n; i++) { x[i] -= mean; r0 += x[i] * x[i]; }
        r0 /= n;
        if (r0 < 0.05f) { pushBpm(0); return; }  // nothing rhythmic going on
        static float r[MAX_LAG + 2];
        int maxLag = n - 150 < MAX_LAG + 1 ? n - 150 : MAX_LAG + 1;  // keep at least 1.5 s of overlap
        for (int lag = 0; lag <= MAX_LAG + 1; lag++) r[lag] = 0;
        for (int lag = 14; lag <= maxLag; lag++) {
            float s = 0; for (int i = 0; i + lag < n; i++) s += x[i] * x[i + lag];
            r[lag] = s / (n - lag);
        }
        // Candidates from 60 to 200 beats a minute. A real beat also lines up two and four beats later
        // (the bar), which a wrong guess like "one and a half beats" does not; tempos near 120 get a gentle nudge.
        int best = 0; float bestScore = 0;
        for (int lag = 30; lag <= 100; lag++) {
            if (r[lag] <= 0 || r[lag] < 0.5f * peakNear(r, 2 * lag)) continue;  // weaker than its double: a subdivision, not the beat
            float oct = log2f(6000.f / lag / 120.f);
            float score = (r[lag] + 0.5f * peakNear(r, 2 * lag) + 0.5f * peakNear(r, 4 * lag)) * expf(-0.5f * (oct / 0.7f) * (oct / 0.7f));
            if (score > bestScore) { bestScore = score; best = lag; }
        }
        if (!best || r[best] < 0.08f * r0) { pushBpm(0); return; }
        // If the pattern also repeats strongly at half that delay, the beat is twice as fast (a kick on every beat).
        int half = (best + 1) / 2;
        if (best >= 68 && peakNear(r, half) > 0.5f * r[best]) { best = r[half] >= r[half - 1] ? (r[half] >= r[half + 1] ? half : half + 1) : half - 1; }
        // refine between whole 10 ms steps with a parabola through the neighbours
        float a = r[best - 1], b = r[best], c = r[best + 1], den = a - 2 * b + c;
        float frac = den < 0 ? 0.5f * (a - c) / den : 0;
        if (frac > 0.5f) frac = 0.5f; if (frac < -0.5f) frac = -0.5f;
        pushBpm(6000.f / (best + frac));
    }
    static float peakNear(const float* r, int lag) {  // strongest value within one step of a delay
        if (lag < 1 || lag > MAX_LAG) return 0;
        float m = r[lag]; if (r[lag - 1] > m) m = r[lag - 1]; if (r[lag + 1] > m) m = r[lag + 1];
        return m > 0 ? m : 0;
    }
    void pushBpm(float v) {  // report the middle of the last five estimates, so one odd reading does not show
        bpmHist[bpmHistN++ % 5] = v;
        int n = bpmHistN < 5 ? bpmHistN : 5; float s[5];
        for (int i = 0; i < n; i++) s[i] = bpmHist[i];
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j] < s[j - 1]; j--) { float t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
        bpm = n >= 3 ? s[n / 2] : 0;
    }
};

}  // namespace mv
