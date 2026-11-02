// eyestream - background eye tracking sender for the Pico Neo 3 Pro Eye.
// Runs from adb shell (no APK), keeps running while any app or PC streaming is
// in front, and sends one UDP packet per new eye tracking frame to the PC.
//
//   /data/local/tmp/eyestream <pc_ip> [port=7777]
//   start in background:  setsid nohup /data/local/tmp/eyestream <pc_ip> > /data/local/tmp/eyestream.log 2>&1 &
//
// Packet (same as the Unity PicoEyeStreamer, so pico_eye_receiver.py reads both):
//   PEYE,seq,time_s,ok,comb_status,l_status,r_status,gx,gy,gz,px,py,pz,l_open,r_open
// Converted to Unity axes (+x right, +y up, +z forward); the service uses -z forward.
//
// Data layout (pxr_eyepose_data_v2_0, 0xC8 bytes), found by testing on hardware:
//   0x24 combined gaze origin xyz     0x48 combined gaze direction xyz (unit, -z fwd)
//   0x54 left openness  0x58 right    0x64/0x70 left/right position guide xy
//   0x7c foveated direction xyz       0x8c convergence distance
//   0xa8 int64 timestamp              0xb0/0xb8 left/right pupil position xy

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef void* (*create_fn)(void);
typedef int (*client_fn)(void*);
typedef int (*get_fn)(void*, long, int, void*);

#define V2_SIZE 0xC8

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float f_at(const unsigned char* b, int off) { float f; memcpy(&f, b + off, 4); return f; }

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pc_ip> [port]\n", argv[0]);
        return 2;
    }
    const char* host = argv[1];
    int port = argc > 2 ? atoi(argv[2]) : 7777;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);

    // Only one copy: two clients starting eye tracking at once can leave Pico's
    // eye service stuck until the headset reboots. The lock is released on exit.
    int lock = open("/data/local/tmp/eyestream.lock", O_CREAT | O_RDWR, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        printf("eyestream is already running, not starting a second copy\n");
        return 3;
    }

    void* h = dlopen("libeyetrackingclient.pxr.so", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    create_fn CreateClient = (create_fn)dlsym(h, "CreateClient");
    client_fn StartEyeTracking = (client_fn)dlsym(h, "StartEyeTracking");
    client_fn StopEyeTracking = (client_fn)dlsym(h, "StopEyeTracking");
    get_fn GetV2 = (get_fn)dlsym(h, "GetEyeTrackingData_2_0");
    if (!CreateClient || !StartEyeTracking || !StopEyeTracking || !GetV2) {
        printf("missing symbol\n"); return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { printf("bad ip %s\n", host); return 1; }

    void* c = CreateClient();
    if (!c) { printf("CreateClient failed\n"); return 1; }
    usleep(500 * 1000);
    int r = StartEyeTracking(c);
    printf("eyestream -> %s:%d, StartEyeTracking %d\n", host, port, r);

    unsigned char b[V2_SIZE + 64];
    int64_t last_ts = -1;
    uint32_t seq = 0;
    double t0 = now_s(), last_report = t0, last_start = t0, last_new = t0;
    int sent = 0, valid = 0;

    while (!g_stop) {
        int rr = GetV2(c, 0, 0, b);
        double t = now_s();
        int64_t ts = 0;
        if (rr == 0) memcpy(&ts, b + 0xa8, 8);

        if (rr == 0 && ts != last_ts) {
            last_ts = ts;
            last_new = t;
            float gx = f_at(b, 0x48), gy = f_at(b, 0x4c), gz = -f_at(b, 0x50);
            float px = f_at(b, 0x24), py = f_at(b, 0x28), pz = -f_at(b, 0x2c);
            float lo = f_at(b, 0x54), ro = f_at(b, 0x58);
            int ok = (gx * gx + gy * gy + gz * gz) > 1e-6f;
            char msg[256];
            int n = snprintf(msg, sizeof msg,
                "PEYE,%u,%.4f,%d,%d,%d,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f",
                seq++, t - t0, ok, ok, lo > 0.5f, ro > 0.5f, gx, gy, gz, px, py, pz, lo, ro);
            sendto(sock, msg, n, 0, (struct sockaddr*)&dst, sizeof dst);
            sent++;
            if (ok) valid++;
        }

        // If frames stop for 2 s (service restarted, another app stopped tracking),
        // ask for eye tracking again. At most every 5 s.
        if (t - last_new > 2.0 && t - last_start > 5.0) {
            r = StartEyeTracking(c);
            printf("[%.0fs] no new frames (get=%d), restart -> %d\n", t - t0, rr, r);
            last_start = t;
        }

        if (t - last_report >= 30.0) {
            printf("[%.0fs] %.0f pkt/s, %d valid\n", t - t0, sent / (t - last_report), valid);
            sent = valid = 0;
            last_report = t;
        }
        usleep(2000);  // poll at ~500 Hz; only new frames (~90 Hz) are sent
    }

    printf("stopping, StopEyeTracking %d\n", StopEyeTracking(c));
    close(sock);
    return 0;
}
