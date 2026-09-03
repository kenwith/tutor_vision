#define _POSIX_C_SOURCE 200809L

/*
 * agent.c - one-key HDMI screenshot tutor
 *
 * Opens an MJPEG USB capture device and keeps it streaming so the HDMI
 * source remains connected. Press Space to grab a fresh 1280x720 frame,
 * send it to a vision model through OpenRouter, and print the answer.
 * Press q or Ctrl-C to exit.
 */

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <termios.h>
#include <unistd.h>

#define OPENROUTER_URL  "https://openrouter.ai/api/v1/chat/completions"
#define DEFAULT_MODEL   "google/gemini-2.5-flash"
/* API_KEY comes from secret.h (gitignored); see secret.h.example. */
#include "secret.h"
#define VIDEO_DEV       "/dev/video0"
#define CAPTURE_WIDTH   1280
#define CAPTURE_HEIGHT  720
#define CAPTURE_FPS     15
#define REQ_BUFS        4
#define FRESH_FRAMES    (REQ_BUFS + 2)
#define MAX_FRAME_WAIT  5000
#define FRAME_CAPACITY  (4 * 1024 * 1024)

#define USER_PROMPT \
    "You are a visual question-answering tutor. Read all relevant text in " \
    "the screenshot, identify the main question or problem the user wants " \
    "solved, and actually solve it. Do not merely quote, transcribe, " \
    "paraphrase, or restate the question. Start with Answer: followed by a " \
    "direct answer. Then provide a short explanation or the essential work " \
    "that supports it. For multiple-choice questions, include the selected " \
    "choice and its text. For math or logic problems, calculate the result " \
    "before answering. If several questions are visible, answer the newest, " \
    "lowest, or most visually prominent unanswered question. If critical " \
    "text is unreadable or information is missing, say exactly what cannot " \
    "be read or what is missing instead of guessing."

struct buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct camera {
    int fd;
    int nbufs;
    unsigned char **maps;
    size_t *lens;
};

static struct camera cam = {.fd = -1};
static struct termios saved_term;
static int term_is_raw;

static void term_reset(void)
{
    if (term_is_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
        term_is_raw = 0;
    }
}

static void die(const char *msg, int code)
{
    term_reset();
    fprintf(stderr, "agent: %s\n", msg);
    exit(code);
}

static void buf_appn(struct buffer *b, const char *s, size_t n)
{
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        size_t ncap = b->cap ? b->cap : 1024;
        while (ncap < need) {
            if (ncap > (size_t)-1 / 2)
                die("response buffer is too large", 1);
            ncap *= 2;
        }
        char *next = realloc(b->data, ncap);
        if (!next)
            die("out of memory", 1);
        b->data = next;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_app(struct buffer *b, const char *s)
{
    buf_appn(b, s, strlen(s));
}

static size_t on_data(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct buffer *b = userdata;
    size_t total;

    if (size && nmemb > (size_t)-1 / size)
        return 0;
    total = size * nmemb;
    buf_appn(b, ptr, total);
    return total;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *to_base64(const unsigned char *input, size_t len, size_t *outlen)
{
    if (len > ((size_t)-1 - 1) / 4 * 3)
        die("captured frame is too large", 1);

    size_t n = ((len + 2) / 3) * 4;
    char *out = malloc(n + 1);
    if (!out)
        die("out of memory", 1);

    size_t i, o = 0;
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned)input[i] << 16;
        if (i + 1 < len)
            v |= (unsigned)input[i + 1] << 8;
        if (i + 2 < len)
            v |= (unsigned)input[i + 2];
        out[o++] = base64_alphabet[(v >> 18) & 63];
        out[o++] = base64_alphabet[(v >> 12) & 63];
        out[o++] = i + 1 < len ? base64_alphabet[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? base64_alphabet[v & 63] : '=';
    }
    out[o] = '\0';
    if (outlen)
        *outlen = o;
    return out;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

static void cam_init(struct camera *c, int *width, int *height)
{
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers req;

    c->fd = open(VIDEO_DEV, O_RDWR);
    if (c->fd < 0)
        die("cannot open /dev/video0; stop FFmpeg and check the capture device", 1);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAPTURE_WIDTH;
    fmt.fmt.pix.height = CAPTURE_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) != 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
        die("capture device did not accept MJPEG", 1);

    *width = fmt.fmt.pix.width;
    *height = fmt.fmt.pix.height;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CAPTURE_FPS;
    (void)xioctl(c->fd, VIDIOC_S_PARM, &parm);

    memset(&req, 0, sizeof(req));
    req.count = REQ_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2)
        die("VIDIOC_REQBUFS failed", 1);

    c->nbufs = (int)req.count;
    c->maps = calloc((size_t)c->nbufs, sizeof(*c->maps));
    c->lens = calloc((size_t)c->nbufs, sizeof(*c->lens));
    if (!c->maps || !c->lens)
        die("out of memory", 1);

    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b;
        void *mapping;

        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned)i;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) != 0)
            die("VIDIOC_QUERYBUF failed", 1);

        mapping = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, c->fd, b.m.offset);
        if (mapping == MAP_FAILED)
            die("mmap failed for capture buffer", 1);
        c->maps[i] = mapping;
        c->lens[i] = b.length;

        if (xioctl(c->fd, VIDIOC_QBUF, &b) != 0)
            die("VIDIOC_QBUF failed", 1);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &type) != 0)
        die("VIDIOC_STREAMON failed", 1);
}

static size_t cam_capture_one(struct camera *c, unsigned char *out, size_t cap)
{
    struct pollfd pfd = {
        .fd = c->fd,
        .events = POLLIN | POLLERR,
        .revents = 0,
    };
    struct v4l2_buffer b;
    size_t n = 0;

    int polled;
    do {
        polled = poll(&pfd, 1, MAX_FRAME_WAIT);
    } while (polled < 0 && errno == EINTR);
    if (polled <= 0)
        return 0;

    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) != 0)
        return 0;

    if (b.index < (unsigned)c->nbufs && b.bytesused <= cap) {
        n = b.bytesused;
        memcpy(out, c->maps[b.index], n);
    }

    if (xioctl(c->fd, VIDIOC_QBUF, &b) != 0)
        return 0;
    return n;
}

/* Drain buffers that filled while idle, then wait for a newly produced frame. */
static size_t cam_capture_fresh(struct camera *c, unsigned char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < FRESH_FRAMES; i++) {
        n = cam_capture_one(c, out, cap);
        if (!n)
            return 0;
    }
    return n;
}

static void cam_close(struct camera *c)
{
    if (c->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)xioctl(c->fd, VIDIOC_STREAMOFF, &type);
    }
    if (c->maps) {
        for (int i = 0; i < c->nbufs; i++)
            if (c->maps[i])
                munmap(c->maps[i], c->lens[i]);
    }
    free(c->maps);
    free(c->lens);
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void cleanup(void)
{
    term_reset();
    cam_close(&cam);
    curl_global_cleanup();
}

static void on_signal(int signal_number)
{
    ssize_t written;

    (void)signal_number;
    term_reset();
    written = write(STDOUT_FILENO, "\n", 1);
    (void)written;
    _exit(130);
}

static void term_raw(void)
{
    struct termios term;
    struct sigaction action;

    if (tcgetattr(STDIN_FILENO, &term) != 0)
        die("could not read terminal settings", 1);
    saved_term = term;
    term.c_lflag &= ~(ICANON | ECHO);
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) != 0)
        die("could not put terminal into key mode", 1);
    term_is_raw = 1;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

/* Return 1 for Space and 0 for q. */
static int wait_for_command(const char *model)
{
    printf("\nAgent ready (%s). Press SPACE to analyze the screen, q to quit.\n",
           model);
    fflush(stdout);
    for (;;) {
        unsigned char key;
        ssize_t n = read(STDIN_FILENO, &key, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("error reading keyboard", 1);
        }
        if (n == 1 && key == ' ')
            return 1;
        if (n == 1 && (key == 'q' || key == 'Q'))
            return 0;
    }
}

static int print_json_string(const char *p)
{
    if (!p || *p != '"')
        return 0;
    p++;
    while (*p && *p != '"') {
        if (*p != '\\') {
            putchar((unsigned char)*p++);
            continue;
        }
        p++;
        if (!*p)
            return 0;
        switch (*p) {
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'b': putchar('\b'); break;
        case 'f': putchar('\f'); break;
        case '"': putchar('"'); break;
        case '\\': putchar('\\'); break;
        case '/': putchar('/'); break;
        default:
            putchar('\\');
            putchar((unsigned char)*p);
            break;
        }
        p++;
    }
    return *p == '"';
}

static int print_reply(const char *json)
{
    const char *p = strstr(json, "\"choices\"");
    if (p)
        p = strstr(p, "\"message\"");
    if (p)
        p = strstr(p, "\"content\"");
    if (!p)
        return 0;

    p = strchr(p, ':');
    if (!p)
        return 0;
    do {
        p++;
    } while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

    if (!print_json_string(p))
        return 0;
    putchar('\n');
    return 1;
}

static int analyze_frame(const unsigned char *frame, size_t frame_len,
                         const char *api_key, const char *model)
{
    struct buffer body = {0};
    struct buffer response = {0};
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    char *base64 = NULL;
    char *authorization = NULL;
    size_t base64_len = 0;
    long status = 0;
    int ok = 0;

    base64 = to_base64(frame, frame_len, &base64_len);
    buf_app(&body, "{\"model\":\"");
    buf_app(&body, model);
    buf_app(&body, "\",\"messages\":[{\"role\":\"user\",\"content\":[");
    buf_app(&body, "{\"type\":\"text\",\"text\":\"");
    buf_app(&body, USER_PROMPT);
    buf_app(&body, "\"},{\"type\":\"image_url\",\"image_url\":{");
    buf_app(&body, "\"url\":\"data:image/jpeg;base64,");
    buf_appn(&body, base64, base64_len);
    buf_app(&body, "\"}}]}],\"max_tokens\":800}");
    free(base64);

    authorization = malloc(strlen(api_key) + 24);
    if (!authorization)
        die("out of memory", 1);
    sprintf(authorization, "Authorization: Bearer %s", api_key);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authorization);
    curl = curl_easy_init();
    if (!curl)
        die("libcurl failed to initialize", 1);

    curl_easy_setopt(curl, CURLOPT_URL, OPENROUTER_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tutor-vision-agent/0.2");

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        fprintf(stderr, "agent: request failed: %s\n",
                curl_easy_strerror(result));
        goto done;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        fprintf(stderr, "agent: API returned HTTP %ld\n", status);
        if (response.data)
            fprintf(stderr, "%s\n", response.data);
        goto done;
    }

    printf("\nAnswer:\n");
    if (!print_reply(response.data ? response.data : "")) {
        fprintf(stderr, "agent: could not parse the model response\n");
        if (response.data)
            fprintf(stderr, "%s\n", response.data);
        goto done;
    }
    ok = 1;

done:
    free(authorization);
    free(body.data);
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

int main(void)
{
    const char *api_key = API_KEY;
    const char *model = getenv("TUTOR_VISION_MODEL");
    unsigned char *frame;
    int width = 0, height = 0;

    if (!model || !*model)
        model = DEFAULT_MODEL;

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
        die("libcurl global initialization failed", 1);
    if (atexit(cleanup) != 0)
        die("could not register cleanup", 1);

    cam_init(&cam, &width, &height);
    printf("Camera streaming: %dx%d MJPEG at up to %d fps (%s)\n",
           width, height, CAPTURE_FPS, VIDEO_DEV);
    if (width != CAPTURE_WIDTH || height != CAPTURE_HEIGHT)
        printf("Note: capture device substituted its closest supported size.\n");

    frame = malloc(FRAME_CAPACITY);
    if (!frame)
        die("out of memory", 1);

    term_raw();
    while (wait_for_command(model)) {
        printf("Capturing a fresh frame...");
        fflush(stdout);
        size_t frame_len = cam_capture_fresh(&cam, frame, FRAME_CAPACITY);
        if (!frame_len) {
            printf(" failed. Check that the HDMI source is active.\n");
            continue;
        }
        printf(" %zu bytes.\nSending image to the model...\n", frame_len);
        (void)analyze_frame(frame, frame_len, api_key, model);
    }

    free(frame);
    printf("Exiting.\n");
    return 0;
}
