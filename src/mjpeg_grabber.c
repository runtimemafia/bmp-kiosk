// mjpeg_grabber.c
// Keeps rpicam-vid running (MJPEG to stdout), parses frames in a thread,
// and lets you save the latest JPEG instantly with grab_jpeg().
//
// Usage: ./mjpeg_grabber [width] [height] [fps]
// While running, press Enter to save latest frame as out-<n>.jpg (demo).
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pid_t cam_pid = -1;
static int cam_fd = -1;

static pthread_t reader_th;
static pthread_mutex_t frame_mu = PTHREAD_MUTEX_INITIALIZER;

// latest JPEG buffer (owned here)
static uint8_t *latest = NULL;
static size_t latest_sz = 0;

// scratch accumulation buffer for parsing
static uint8_t *acc = NULL;
static size_t acc_sz = 0, acc_cap = 0;

// simple helper
static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q)
    die("realloc");
  return q;
}

// find next JPEG frame in acc (SOI=FFD8 ... EOI=FFD9)
// returns 1 if a frame was extracted into latest; 0 otherwise
static int extract_one_jpeg_from_acc(void) {
  // search SOI
  size_t i = 0;
  while (i + 1 < acc_sz && !(acc[i] == 0xFF && acc[i + 1] == 0xD8))
    i++;
  if (i + 1 >= acc_sz) { // no SOI
    // discard all but last byte (in case it's 0xFF)
    if (acc_sz > 1) {
      memmove(acc, acc + acc_sz - 1, 1);
      acc_sz = 1;
    }
    return 0;
  }
  // drop leading bytes before SOI
  if (i > 0) {
    memmove(acc, acc + i, acc_sz - i);
    acc_sz -= i;
  }

  // search EOI from position 2 onward
  size_t j = 2;
  while (j + 1 < acc_sz && !(acc[j] == 0xFF && acc[j + 1] == 0xD9))
    j++;
  if (j + 1 >= acc_sz)
    return 0; // need more bytes

  size_t frame_sz = j + 2; // inclusive of EOI

  // publish as latest
  pthread_mutex_lock(&frame_mu);
  latest = (uint8_t *)xrealloc(latest, frame_sz);
  memcpy(latest, acc, frame_sz);
  latest_sz = frame_sz;
  pthread_mutex_unlock(&frame_mu);

  // remove frame from acc
  memmove(acc, acc + frame_sz, acc_sz - frame_sz);
  acc_sz -= frame_sz;
  return 1;
}

static void *reader_thread(void *arg) {
  (void)arg;
  const size_t CHUNK = 64 * 1024;
  uint8_t *buf = (uint8_t *)malloc(CHUNK);
  if (!buf)
    die("malloc buf");

  for (;;) {
    ssize_t r = read(cam_fd, buf, CHUNK);
    if (r == 0)
      break; // EOF
    if (r < 0) {
      if (errno == EINTR)
        continue;
      perror("read cam_fd");
      break;
    }
    // append to acc
    if (acc_sz + (size_t)r > acc_cap) {
      size_t new_cap = acc_cap ? acc_cap * 2 : (256 * 1024);
      while (new_cap < acc_sz + (size_t)r)
        new_cap *= 2;
      acc = (uint8_t *)xrealloc(acc, new_cap);
      acc_cap = new_cap;
    }
    memcpy(acc + acc_sz, buf, (size_t)r);
    acc_sz += (size_t)r;

    // extract as many frames as available
    while (extract_one_jpeg_from_acc()) {
      // no-op; latest updated
    }
  }
  free(buf);
  return NULL;
}

// Start camera stream -> stdout (pipe), parse MJPEG
// Uses rpicam-vid on Raspberry Pi (USE_RPI_CAM) or ffmpeg on Linux
// (development)
int stream_start(int width, int height, int fps) {
  if (cam_fd != -1)
    return 0; // already started

  int pipefd[2];
  if (pipe(pipefd) != 0)
    return -1;

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  // child's stdout -> pipe write end
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
  // child's stderr -> /dev/null to avoid mixing logs
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0)
    posix_spawn_file_actions_adddup2(&fa, devnull, STDERR_FILENO);

  char wbuf[16], hbuf[16], fpsbuf[16];
  snprintf(wbuf, sizeof wbuf, "%d", width > 0 ? width : 640);
  snprintf(hbuf, sizeof hbuf, "%d", height > 0 ? height : 480);
  snprintf(fpsbuf, sizeof fpsbuf, "%d", fps > 0 ? fps : 5);

#ifdef USE_RPI_CAM
  // rpicam-vid options for low CPU:
  // - MJPEG (HW ISP)
  // - no preview
  // - run forever (-t 0)
  // - modest size & fps
  // - turn off extra processing
  char *argv[] = {
      "rpicam-vid", "-t",           "0",         "-n",          "--width",
      wbuf,         "--height",     hbuf,        "--framerate", fpsbuf,
      "--codec",    "mjpeg",        "--denoise", "off",         "--sharpness",
      "0",          "--saturation", "0",         "-o",          "-", // stdout
      NULL};
#else
  // ffmpeg with V4L2 for Linux development:
  // -f v4l2: use Video4Linux2 input
  // -video_size: resolution
  // -framerate: fps
  // -i /dev/video0: input device
  // -f mjpeg: output format
  // -: output to stdout
  char size_str[32];
  snprintf(size_str, sizeof size_str, "%dx%d", width > 0 ? width : 640,
           height > 0 ? height : 480);

  char *argv[] = {"ffmpeg",     "-f",   "v4l2", "-video_size", size_str,
                  "-framerate", fpsbuf, "-i",   "/dev/video0", "-f",
                  "mjpeg",      "-",    NULL};
#endif

  int rc = posix_spawnp(&cam_pid, argv[0], &fa, NULL, argv, environ);
  if (devnull >= 0)
    close(devnull);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    perror("posix_spawnp camera command");
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  // parent uses pipe read end
  close(pipefd[1]);
  cam_fd = pipefd[0];

  // start reader thread
  if (pthread_create(&reader_th, NULL, reader_thread, NULL) != 0) {
    perror("pthread_create");
    close(cam_fd);
    cam_fd = -1;
    return -1;
  }
  return 0;
}

// Save the latest JPEG to path; returns 0 on success
int grab_jpeg(const char *path) {
  int rc = -1;
  pthread_mutex_lock(&frame_mu);
  if (latest && latest_sz > 0) {
    FILE *f = fopen(path, "wb");
    if (f) {
      if (fwrite(latest, 1, latest_sz, f) == latest_sz)
        rc = 0;
      fclose(f);
    }
  } else {
    fprintf(stderr, "No frame yet\n");
  }
  pthread_mutex_unlock(&frame_mu);
  return rc;
}

int get_latest_jpeg_copy(uint8_t **out_buf, size_t *out_sz) {
  if (!out_buf || !out_sz)
    return -1;
  *out_buf = NULL;
  *out_sz = 0;

  pthread_mutex_lock(&frame_mu);
  if (latest && latest_sz > 0) {
    uint8_t *buf = (uint8_t *)malloc(latest_sz);
    if (!buf) {
      pthread_mutex_unlock(&frame_mu);
      return -1;
    }
    memcpy(buf, latest, latest_sz);
    *out_buf = buf;
    *out_sz = latest_sz;
    pthread_mutex_unlock(&frame_mu);
    return 0;
  }
  pthread_mutex_unlock(&frame_mu);
  return -1; // no frame yet
}

void stream_stop(void) {
  if (cam_fd != -1) {
    // end reader thread by closing fd
    close(cam_fd);
    cam_fd = -1;
    pthread_join(reader_th, NULL);
  }
  if (cam_pid > 0) {
    // best-effort terminate child
    kill(cam_pid, SIGTERM);
    waitpid(cam_pid, NULL, 0);
    cam_pid = -1;
  }
  free(latest);
  latest = NULL;
  latest_sz = 0;
  free(acc);
  acc = NULL;
  acc_sz = acc_cap = 0;
}

// ------------ Demo main ------------
int mjpeg_grabber_demo_main(int argc, char **argv) {
  int w = (argc > 1) ? atoi(argv[1]) : 640;
  int h = (argc > 2) ? atoi(argv[2]) : 480;
  int f = (argc > 3) ? atoi(argv[3]) : 5;

  if (stream_start(w, h, f) != 0)
    die("stream_start");

  printf(
      "Streaming %dx%d @ %dfps. Press Enter to save frame, Ctrl+C to quit.\n",
      w, h, f);
  int idx = 0;
  for (;;) {
    int c = getchar();
    if (c == EOF)
      break;
    char name[64];
    snprintf(name, sizeof name, "out-%03d.jpg", idx++);
    if (grab_jpeg(name) == 0) {
      printf("Saved %s\n", name);
    } else {
      printf("No frame yet, try again.\n");
    }
  }
  stream_stop();
  return 0;
}
