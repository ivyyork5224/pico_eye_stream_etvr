// eyeprobe - talks to Pico's eye tracking service through the public
// libeyetrackingclient.pxr.so and dumps what comes back, so we can work out
// the data layout. Run from adb shell:  /data/local/tmp/eyeprobe [seconds] [nostart]
//
// C API found by disassembly (all take the client pointer first):
//   void* CreateClient(void)
//   int   StartEyeTracking(void* c)          -> StartAlgorithm(5, mode, 0)
//   int   StopEyeTracking(void* c)
//   int   GetSupportedTrackingMode(void* c)
//   int   GetEyeTrackingData(void* c, long ts, int flags, void* out)      out = 0xA8 bytes
//   int   GetEyeTrackingData_2_0(void* c, long ts, int flags, void* out)  out = 0xC8 bytes
//   returns 0 ok, -1 no service yet, -2 shared memory failed, -3 no data

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void* (*create_fn)(void);
typedef int (*client_fn)(void*);
typedef int (*get_fn)(void*, long, int, void*);

#define V1_SIZE 0xA8
#define V2_SIZE 0xC8
#define BUF_SIZE 512

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void dump(const char* name, const unsigned char* b, int size) {
    printf("--- %s (%d bytes): offset: int32 | float\n", name, size);
    for (int off = 0; off < size; off += 4) {
        int i; float f;
        memcpy(&i, b + off, 4);
        memcpy(&f, b + off, 4);
        printf("  0x%03x: %11d | %12.5f\n", off, i, f);
    }
    // canary check: anything written past the expected size?
    for (int off = size; off < BUF_SIZE; off++) {
        if (b[off] != 0xCD) { printf("  !! byte written past size at 0x%03x\n", off); break; }
    }
}

static void line(const char* name, const unsigned char* b, int size) {
    printf("%s:", name);
    for (int off = 0; off < size; off += 4) {
        float f; memcpy(&f, b + off, 4);
        printf(" %.3f", f);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int do_start = !(argc > 2 && strcmp(argv[2], "nostart") == 0);
    // "every" as 3rd arg: print every new frame as "t,<50 floats>" for analysis
    int every = argc > 3 && strcmp(argv[3], "every") == 0;
    setvbuf(stdout, NULL, _IOLBF, 0);

    void* h = dlopen("libeyetrackingclient.pxr.so", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    create_fn CreateClient = (create_fn)dlsym(h, "CreateClient");
    client_fn StartEyeTracking = (client_fn)dlsym(h, "StartEyeTracking");
    client_fn StopEyeTracking = (client_fn)dlsym(h, "StopEyeTracking");
    client_fn GetSupportedTrackingMode = (client_fn)dlsym(h, "GetSupportedTrackingMode");
    get_fn GetV1 = (get_fn)dlsym(h, "GetEyeTrackingData");
    get_fn GetV2 = (get_fn)dlsym(h, "GetEyeTrackingData_2_0");
    if (!CreateClient || !StartEyeTracking || !StopEyeTracking || !GetV1 || !GetV2) {
        printf("missing symbol\n"); return 1;
    }

    void* c = CreateClient();
    printf("CreateClient -> %p\n", c);
    if (!c) return 1;

    // the client looks up the service on a thread; give it a moment
    usleep(500 * 1000);
    printf("GetSupportedTrackingMode -> %d\n", GetSupportedTrackingMode ? GetSupportedTrackingMode(c) : -99);

    if (do_start) {
        int r = StartEyeTracking(c);
        printf("StartEyeTracking -> %d\n", r);
        usleep(1000 * 1000);
    }

    unsigned char b1[BUF_SIZE], b2[BUF_SIZE];
    int dumped = 0;
    double t0 = now_s(), last = 0;
    int ok1 = 0, ok2 = 0, calls = 0;
    while (now_s() - t0 < seconds) {
        memset(b1, 0xCD, sizeof b1);
        memset(b2, 0xCD, sizeof b2);
        int r1 = GetV1(c, 0, 0, b1);
        int r2 = GetV2(c, 0, 0, b2);
        calls++;
        if (r1 == 0) ok1++;
        if (r2 == 0) ok2++;

        if (!dumped && r1 == 0 && r2 == 0) {
            dump("v1", b1, V1_SIZE);
            dump("v2", b2, V2_SIZE);
            dumped = 1;
        }
        double t = now_s();
        if (every) {
            static int64_t last_ts = -1;
            int64_t ts = 0;
            memcpy(&ts, b2 + 0xa8, 8);
            if (r2 == 0 && ts != last_ts) {
                last_ts = ts;
                // raw 32-bit words in hex, so int and float fields both survive
                printf("F,%.4f", t - t0);
                for (int off = 0; off < V2_SIZE; off += 4) {
                    unsigned int w; memcpy(&w, b2 + off, 4);
                    printf(",%08x", w);
                }
                printf("\n");
            }
            usleep(2000);
            continue;
        }
        if (t - last >= 0.5) {
            printf("[%.1fs] r1=%d r2=%d ok1=%d ok2=%d/%d\n", t - t0, r1, r2, ok1, ok2, calls);
            if (r2 == 0) line("v2", b2, V2_SIZE);
            last = t;
        }
        usleep(11 * 1000);
    }

    if (do_start) printf("StopEyeTracking -> %d\n", StopEyeTracking(c));
    return 0;
}
