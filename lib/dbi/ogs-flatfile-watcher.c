/*
 * Mongoless: inotify + timerfd file watcher with trailing-edge debounce.
 *
 *   inotify watches the *parent* directory (so editors that save by
 *   write-temp + rename land on a fresh inode without us losing the
 *   watch). Events on names other than ours are filtered.
 *
 *   On each matching event the debounce timerfd is rearmed to
 *   `OGS_FLATFILE_WATCHER_DEBOUNCE_MS` from now — only when the timer
 *   actually expires (no further events for that long) does on_change()
 *   fire. This collapses noisy multi-syscall saves into one reload.
 *
 *   The watcher runs on its own ogs_thread_t and shuts down by writing
 *   an eventfd; the worker exits its poll loop and returns, which
 *   ogs_thread_destroy() observes via the worker's running flag.
 *
 * Isolated from upstream files.
 */

#include "ogs-dbi.h"
#include "ogs-flatfile-watcher.h"

#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <libgen.h>
#include <unistd.h>

typedef struct watcher_self_s {
    bool running;

    ogs_thread_t *thread;

    int inotify_fd;
    int watch_descriptor;
    int timer_fd;
    int shutdown_fd;

    char *dir_path;
    char *base_name;

    void (*on_change)(void);
} watcher_self_t;

static watcher_self_t self;

static void arm_debounce(void)
{
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = OGS_FLATFILE_WATCHER_DEBOUNCE_MS / 1000;
    spec.it_value.tv_nsec =
            (OGS_FLATFILE_WATCHER_DEBOUNCE_MS % 1000) * 1000000L;
    /* it_interval left zero — one-shot, rearmed by each event. */
    if (timerfd_settime(self.timer_fd, 0, &spec, NULL) < 0)
        ogs_error("flatfile-watcher: timerfd_settime failed: %s",
                strerror(errno));
}

static bool drain_inotify_events(void)
{
    /* Per inotify(7): buf must be aligned + large enough for one event. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool matched = false;

    for (;;) {
        ssize_t n = read(self.inotify_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                ogs_error("flatfile-watcher: read(inotify) failed: %s",
                        strerror(errno));
            break;
        }

        ssize_t off = 0;
        while (off < n) {
            const struct inotify_event *ev =
                    (const struct inotify_event *)(buf + off);
            if (ev->len > 0 &&
                    strcmp(ev->name, self.base_name) == 0) {
                matched = true;
            }
            off += sizeof(*ev) + ev->len;
        }
    }
    return matched;
}

static void watcher_worker(void *arg)
{
    (void)arg;

    while (self.running) {
        struct pollfd pfds[3];
        memset(pfds, 0, sizeof(pfds));
        pfds[0].fd = self.inotify_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = self.timer_fd;
        pfds[1].events = POLLIN;
        pfds[2].fd = self.shutdown_fd;
        pfds[2].events = POLLIN;

        int rc = poll(pfds, 3, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            ogs_error("flatfile-watcher: poll failed: %s", strerror(errno));
            break;
        }

        if (pfds[2].revents & POLLIN) {
            /* Drain so a future re-init starts clean. */
            uint64_t throwaway;
            (void)read(self.shutdown_fd, &throwaway, sizeof(throwaway));
            break;
        }

        if (pfds[0].revents & POLLIN) {
            if (drain_inotify_events())
                arm_debounce();
        }

        if (pfds[1].revents & POLLIN) {
            uint64_t expirations;
            ssize_t r = read(self.timer_fd, &expirations, sizeof(expirations));
            (void)r;
            if (self.on_change) {
                ogs_info("flatfile-watcher: debounce settled, reloading %s",
                        self.base_name);
                self.on_change();
            }
        }
    }
}

int ogs_flatfile_watcher_init(const char *path, void (*on_change)(void))
{
    char *path_dup1 = NULL, *path_dup2 = NULL;

    memset(&self, 0, sizeof(self));
    self.inotify_fd = -1;
    self.watch_descriptor = -1;
    self.timer_fd = -1;
    self.shutdown_fd = -1;

    if (!path || !on_change) {
        ogs_error("flatfile-watcher: missing path or callback");
        return OGS_ERROR;
    }

    /* dirname()/basename() may mutate their input — give each its own copy. */
    path_dup1 = ogs_strdup(path);
    path_dup2 = ogs_strdup(path);
    if (!path_dup1 || !path_dup2) goto fail;
    self.dir_path = ogs_strdup(dirname(path_dup1));
    self.base_name = ogs_strdup(basename(path_dup2));
    ogs_free(path_dup1); path_dup1 = NULL;
    ogs_free(path_dup2); path_dup2 = NULL;
    if (!self.dir_path || !self.base_name) goto fail;

    self.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (self.inotify_fd < 0) {
        ogs_error("flatfile-watcher: inotify_init1 failed: %s",
                strerror(errno));
        goto fail;
    }

    /* Watch the directory, not the file itself: editors that rename-over
     * the file would otherwise leave us watching a stale inode. The
     * IN_MOVED_TO branch catches that pattern. */
    self.watch_descriptor = inotify_add_watch(self.inotify_fd, self.dir_path,
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_MODIFY);
    if (self.watch_descriptor < 0) {
        ogs_error("flatfile-watcher: inotify_add_watch(%s) failed: %s",
                self.dir_path, strerror(errno));
        goto fail;
    }

    self.timer_fd = timerfd_create(CLOCK_MONOTONIC,
            TFD_NONBLOCK | TFD_CLOEXEC);
    if (self.timer_fd < 0) {
        ogs_error("flatfile-watcher: timerfd_create failed: %s",
                strerror(errno));
        goto fail;
    }

    self.shutdown_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (self.shutdown_fd < 0) {
        ogs_error("flatfile-watcher: eventfd failed: %s", strerror(errno));
        goto fail;
    }

    self.on_change = on_change;
    self.running = true;

    self.thread = ogs_thread_create(watcher_worker, NULL);
    if (!self.thread) {
        ogs_error("flatfile-watcher: ogs_thread_create failed");
        self.running = false;
        goto fail;
    }

    ogs_info("flatfile-watcher: watching %s/%s (debounce %dms)",
            self.dir_path, self.base_name,
            OGS_FLATFILE_WATCHER_DEBOUNCE_MS);
    return OGS_OK;

fail:
    if (path_dup1) ogs_free(path_dup1);
    if (path_dup2) ogs_free(path_dup2);
    ogs_flatfile_watcher_final();
    return OGS_ERROR;
}

void ogs_flatfile_watcher_final(void)
{
    if (self.thread) {
        self.running = false;
        if (self.shutdown_fd >= 0) {
            uint64_t one = 1;
            ssize_t r = write(self.shutdown_fd, &one, sizeof(one));
            (void)r;
        }
        ogs_thread_destroy(self.thread);
        self.thread = NULL;
    }

    if (self.watch_descriptor >= 0 && self.inotify_fd >= 0) {
        (void)inotify_rm_watch(self.inotify_fd, self.watch_descriptor);
        self.watch_descriptor = -1;
    }
    if (self.inotify_fd >= 0) { close(self.inotify_fd); self.inotify_fd = -1; }
    if (self.timer_fd >= 0)   { close(self.timer_fd);   self.timer_fd = -1; }
    if (self.shutdown_fd >= 0){ close(self.shutdown_fd);self.shutdown_fd = -1; }

    if (self.dir_path)  { ogs_free(self.dir_path);  self.dir_path = NULL; }
    if (self.base_name) { ogs_free(self.base_name); self.base_name = NULL; }
    self.on_change = NULL;
}
