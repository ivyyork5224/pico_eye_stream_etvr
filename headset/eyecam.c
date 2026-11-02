// eyecam - streams the Pico Neo 3 Pro Eye's infrared eye cameras to the PC as MJPEG,
// for EyeTrackVR / Baballonia. Runs from adb shell like eyestream.
//
//   /data/local/tmp/eyecam [quality=75] [max_fps=90] [min_fps=30]
//   left camera:  http://<headset-ip>:8080/     right camera: http://<headset-ip>:8081/
//   any port also serves /left, /right, /both and /snap.jpg (single image)
//
// Only views someone is watching get encoded. The frame rate follows the headset's load:
// above LOAD_HIGH it steps down towards min_fps, below LOAD_LOW back up towards max_fps,
// so the eye cameras give way to head tracking when the headset gets busy.
//
// Frames come from Pico's eye tracking debug dump: with debug.pxr.et.tobii.dumpimg=1 the
// eye service writes every combined frame to /sdcard/pvreyetracking/<n>.raw
// (416x200, 8-bit gray, the two cameras side by side). We take the newest frame and delete
// every file right away, so the data rarely leaves the page cache. The property is reset
// on exit. Eye tracking must be running (eyestream starts it).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../third_party/stb_image_write.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DUMP_DIR "/sdcard/pvreyetracking"
#define DUMP_PROP "debug.pxr.et.tobii.dumpimg"
#define FULL_W 416
#define H 200
#define HALF_W 208
#define FRAME_BYTES (FULL_W * H)

enum { VIEW_LEFT, VIEW_RIGHT, VIEW_BOTH, VIEW_COUNT };

typedef struct {
    unsigned char* data;
    int len, cap;
} jpg_buf;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static jpg_buf g_jpg[VIEW_COUNT];
static unsigned long g_frame_no = 0;
static volatile sig_atomic_t g_stop = 0;
static int g_quality = 75;
static int g_max_fps = 90;
static int g_min_fps = 30;
static volatile int g_clients[VIEW_COUNT];

// load average (1 min) thresholds for the 8-core headset, checked every LOAD_CHECK_S
#define LOAD_HIGH 9.0
#define LOAD_LOW 6.0
#define LOAD_CHECK_S 5.0
#define FPS_STEP 15

static double load1(void) {
    double l = 0;
    FILE* f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &l) != 1) l = 0; fclose(f); }
    return l;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void jpg_append(void* ctx, void* data, int size) {
    jpg_buf* b = ctx;
    if (b->len + size > b->cap) {
        b->cap = (b->len + size) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, data, size);
    b->len += size;
}

static void encode(jpg_buf* out, const unsigned char* px, int w, int stride) {
    static unsigned char tmp[FRAME_BYTES];
    for (int y = 0; y < H; y++) memcpy(tmp + y * w, px + y * stride, w);
    out->len = 0;
    stbi_write_jpg_to_func(jpg_append, out, w, H, 1, tmp, g_quality);
}

// Removes every dump file and returns the newest complete frame in buf (1) or none (0).
static int grab_newest(unsigned char* buf) {
    DIR* d = opendir(DUMP_DIR);
    if (!d) return 0;
    unsigned long best = 0;
    struct dirent* e;
    char path[256];
    while ((e = readdir(d))) {
        char* end;
        unsigned long n = strtoul(e->d_name, &end, 10);
        if (end == e->d_name || strcmp(end, ".raw") != 0) continue;
        if (n > best) {
            // the newest may still be being written; only take it if complete
            snprintf(path, sizeof path, DUMP_DIR "/%s", e->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size == FRAME_BYTES) best = n;
        }
    }
    int got = 0;
    if (best) {
        snprintf(path, sizeof path, DUMP_DIR "/%lu.raw", best);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            got = read(fd, buf, FRAME_BYTES) == FRAME_BYTES;
            close(fd);
        }
    }
    // delete everything up to the frame we took (newer files are left for next time)
    rewinddir(d);
    while ((e = readdir(d))) {
        char* end;
        unsigned long n = strtoul(e->d_name, &end, 10);
        if (end == e->d_name || strcmp(end, ".raw") != 0) continue;
        if (!best || n <= best) {
            snprintf(path, sizeof path, DUMP_DIR "/%s", e->d_name);
            unlink(path);
        }
    }
    closedir(d);
    return got;
}

static void* grab_thread(void* arg) {
    (void)arg;
    static unsigned char frame[FRAME_BYTES];
    jpg_buf work[VIEW_COUNT] = {{0}};
    int fps = g_max_fps;
    double min_dt = 1.0 / fps, last = 0, last_report = now_s(), last_frame = now_s(), last_load = 0;
    int count = 0;
    while (!g_stop) {
        double t = now_s();
        if (t - last_load >= LOAD_CHECK_S) {
            last_load = t;
            double l = load1();
            int want = fps;
            if (l > LOAD_HIGH) want = fps - FPS_STEP < g_min_fps ? g_min_fps : fps - FPS_STEP;
            else if (l < LOAD_LOW) want = fps + FPS_STEP > g_max_fps ? g_max_fps : fps + FPS_STEP;
            if (want != fps) {
                printf("[eyecam] load %.1f: %d -> %d fps\n", l, fps, want);
                fps = want;
                min_dt = 1.0 / fps;
            }
        }
        if (t - last < min_dt || !grab_newest(frame)) {
            // re-arm the dump if it stopped (e.g. the eye service restarted)
            if (t - last_frame > 3.0) {
                __system_property_set(DUMP_PROP, "1");
                last_frame = t;
            }
            usleep(3000);
            continue;
        }
        last = last_frame = t;
        // only encode what someone is watching (a new viewer gets the next frame)
        int want[VIEW_COUNT];
        for (int v = 0; v < VIEW_COUNT; v++) want[v] = g_clients[v] > 0;
        if (want[VIEW_LEFT]) encode(&work[VIEW_LEFT], frame, HALF_W, FULL_W);
        if (want[VIEW_RIGHT]) encode(&work[VIEW_RIGHT], frame + HALF_W, HALF_W, FULL_W);
        if (want[VIEW_BOTH]) encode(&work[VIEW_BOTH], frame, FULL_W, FULL_W);

        pthread_mutex_lock(&g_lock);
        for (int v = 0; v < VIEW_COUNT; v++) {
            if (!want[v]) { g_jpg[v].len = 0; continue; }  // no stale image for a later viewer
            jpg_buf tmp = g_jpg[v];
            g_jpg[v] = work[v];
            work[v] = tmp;
        }
        g_frame_no++;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);

        count++;
        if (t - last_report >= 30.0) {
            printf("[eyecam] %.1f fps (limit %d, load %.1f), jpeg %d+%d bytes, clients left %d right %d\n",
                   count / (t - last_report), fps, load1(), g_jpg[VIEW_LEFT].len, g_jpg[VIEW_RIGHT].len,
                   g_clients[VIEW_LEFT], g_clients[VIEW_RIGHT]);
            count = 0;
            last_report = t;
        }
    }
    return NULL;
}

static int send_all(int fd, const void* data, int len) {
    const char* p = data;
    while (len > 0) {
        int n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

typedef struct { int fd; int default_view; } client_arg;

static void* client_thread(void* arg) {
    client_arg c = *(client_arg*)arg;
    free(arg);
    char req[1024] = {0};
    int n = recv(c.fd, req, sizeof req - 1, 0);
    if (n <= 0) { close(c.fd); return NULL; }

    int view = c.default_view, snap = 0;
    if (strstr(req, "GET /left")) view = VIEW_LEFT;
    else if (strstr(req, "GET /right")) view = VIEW_RIGHT;
    else if (strstr(req, "GET /both")) view = VIEW_BOTH;
    if (strstr(req, "GET /snap")) snap = 1;

    int one = 1;
    setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // A viewer that stops reading (Wi-Fi hiccup, half-open connection) is dropped
    // after 2 s instead of lingering, so its reconnect gets a clean stream.
    struct timeval tv = {2, 0};
    setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(c.fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    const char* vname = view == VIEW_LEFT ? "left" : view == VIEW_RIGHT ? "right" : "both";
    __sync_fetch_and_add(&g_clients[view], 1);
    if (!snap) printf("[eyecam] %s viewer connected (%d)\n", vname, g_clients[view]);
    jpg_buf local = {0};
    unsigned long seen = 0;
    char hdr[256];

    if (!snap) {
        const char* h = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
        if (send_all(c.fd, h, strlen(h))) goto done;
    }
    while (!g_stop) {
        pthread_mutex_lock(&g_lock);
        while (g_frame_no == seen && !g_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cond, &g_lock, &ts);
        }
        seen = g_frame_no;
        jpg_buf* src = &g_jpg[view];
        if (local.cap < src->len) { local.cap = src->len * 2; local.data = realloc(local.data, local.cap); }
        local.len = src->len;
        if (src->len) memcpy(local.data, src->data, src->len);
        pthread_mutex_unlock(&g_lock);
        if (!local.len) continue;

        if (snap) {
            int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                              "Content-Length: %d\r\nConnection: close\r\n\r\n", local.len);
            send_all(c.fd, hdr, hl);
            send_all(c.fd, local.data, local.len);
            break;
        }
        int hl = snprintf(hdr, sizeof hdr, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", local.len);
        if (send_all(c.fd, hdr, hl) || send_all(c.fd, local.data, local.len) || send_all(c.fd, "\r\n", 2)) break;
    }
done:
    __sync_fetch_and_sub(&g_clients[view], 1);
    if (!snap) printf("[eyecam] %s viewer disconnected (%d left)\n", vname, g_clients[view]);
    free(local.data);
    close(c.fd);
    return NULL;
}

typedef struct { int port; int view; } server_arg;

static void* server_thread(void* arg) {
    server_arg s = *(server_arg*)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(s.port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 8)) {
        printf("[eyecam] can't listen on %d: %s\n", s.port, strerror(errno));
        return NULL;
    }
    printf("[eyecam] port %d -> %s camera\n", s.port, s.view == VIEW_LEFT ? "left" : "right");
    while (!g_stop) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        client_arg* c = malloc(sizeof *c);
        c->fd = fd;
        c->default_view = s.view;
        pthread_t t;
        pthread_create(&t, NULL, client_thread, c);
        pthread_detach(t);
    }
    close(ls);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc > 1) g_quality = atoi(argv[1]);
    if (argc > 2) g_max_fps = atoi(argv[2]);
    if (argc > 3) g_min_fps = atoi(argv[3]);
    if (g_max_fps < 1) g_max_fps = 1;
    if (g_min_fps < 1) g_min_fps = 1;
    if (g_min_fps > g_max_fps) g_min_fps = g_max_fps;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    // only one copy (a second one would fight over the dump files and ports)
    int lock = open("/data/local/tmp/eyecam.lock", O_CREAT | O_RDWR, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        printf("[eyecam] already running, not starting a second copy\n");
        return 3;
    }

    __system_property_set(DUMP_PROP, "1");
    printf("[eyecam] dump on, quality %d, %d-%d fps by load\n", g_quality, g_min_fps, g_max_fps);

    pthread_t grab, srv[2];
    pthread_create(&grab, NULL, grab_thread, NULL);
    static server_arg sa[2] = {{8080, VIEW_LEFT}, {8081, VIEW_RIGHT}};
    for (int i = 0; i < 2; i++) {
        pthread_create(&srv[i], NULL, server_thread, &sa[i]);
        pthread_detach(srv[i]);
    }

    while (!g_stop) sleep(1);

    __system_property_set(DUMP_PROP, "0");
    pthread_join(grab, NULL);
    unsigned char scratch[FRAME_BYTES];
    usleep(200 * 1000);
    grab_newest(scratch);  // final cleanup of leftover dump files
    rmdir(DUMP_DIR);
    printf("[eyecam] stopped, dump off\n");
    return 0;
}
