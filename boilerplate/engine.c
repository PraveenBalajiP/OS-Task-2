#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= UI COLORS ================= */

#define RESET   "\033[0m"
#define GREEN   "\033[32m"
#define RED     "\033[31m"
#define YELLOW  "\033[33m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"

/* ================= CORE ================= */

#define SOCK_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define STACK_SIZE (1024 * 1024)

#define MAX_CONTAINERS 64
#define ID_LEN 32

/* ================= PROTOCOL ================= */

typedef struct {
    int cmd;
    char id[ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
} request_t;

typedef struct {
    int status;
    char msg[256];
} response_t;

enum {
    CMD_START = 1,
    CMD_PS,
    CMD_STOP,
    CMD_LOGS
};

/* ================= CONTAINERS ================= */

typedef struct {
    char id[ID_LEN];
    pid_t pid;
    time_t start;
    int alive;
} container_t;

static container_t containers[MAX_CONTAINERS];
static int container_count = 0;

/* ================= UTIL ================= */

static container_t* find(const char *id) {
    for (int i = 0; i < container_count; i++)
        if (!strcmp(containers[i].id, id))
            return &containers[i];
    return NULL;
}

static void uptime(time_t s, char *out) {
    int t = time(NULL) - s;
    sprintf(out, "%02d:%02d:%02d", t/3600, (t%3600)/60, t%60);
}

static const char* state_color(int alive) {
    return alive ? GREEN "RUNNING" RESET : RED "STOPPED" RESET;
}

/* ================= CHILD ================= */

typedef struct {
    char id[ID_LEN];
    char rootfs[PATH_MAX];
    char cmd[256];
} child_t;

static int child_fn(void *arg) {
    child_t *c = arg;

    sethostname(c->id, strlen(c->id));

    if (chroot(c->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    execl("/bin/sh", "sh", "-c", c->cmd, NULL);

    perror("exec");
    return 1;
}

/* ================= SAFE READ ================= */

static int read_full(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = read(fd, buf + got, len - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* ================= SUPERVISOR ================= */

static void supervisor() {

    mkdir(LOG_DIR, 0755);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, SOCK_PATH);

    unlink(SOCK_PATH);
    bind(s, (struct sockaddr*)&a, sizeof(a));
    listen(s, 10);

    printf(BOLD CYAN "\n[SUPERVISOR STARTED]\n" RESET);

    while (1) {

        int c = accept(s, NULL, NULL);

        request_t r;
        response_t res;

        if (read_full(c, &r, sizeof(r)) < 0) {
            close(c);
            continue;
        }

        /* ================= START ================= */
        if (r.cmd == CMD_START) {

            void *stack = malloc(STACK_SIZE);
            child_t *ch = malloc(sizeof(child_t));

            strcpy(ch->id, r.id);
            strcpy(ch->rootfs, r.rootfs);
            strcpy(ch->cmd, r.command);

            pid_t pid = clone(child_fn,
                              stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              ch);

            container_t *ct = &containers[container_count++];
            strcpy(ct->id, r.id);
            ct->pid = pid;
            ct->start = time(NULL);
            ct->alive = 1;

            printf(GREEN "[OK]" RESET " started %s pid=%d\n", r.id, pid);

            sprintf(res.msg, "started %s", r.id);
            write(c, &res, sizeof(res));
        }

        /* ================= PS ================= */
        else if (r.cmd == CMD_PS) {

            printf(BOLD CYAN "\n%-12s %-8s %-10s %-10s\n" RESET,
                   "ID", "PID", "STATE", "UPTIME");

            for (int i = 0; i < container_count; i++) {

                char up[32];
                uptime(containers[i].start, up);

                printf("%-12s %-8d %-10s %-10s\n",
                       containers[i].id,
                       containers[i].pid,
                       state_color(containers[i].alive),
                       up);
            }

            printf("\n");
        }

        /* ================= STOP ================= */
        else if (r.cmd == CMD_STOP) {

            container_t *ct = find(r.id);

            if (!ct) {
                printf(RED "[ERROR] not found\n" RESET);
            } else {
                kill(ct->pid, SIGKILL);
                ct->alive = 0;

                printf(YELLOW "[STOPPED]" RESET " %s\n", r.id);
            }
        }

        /* ================= LOGS ================= */
        else if (r.cmd == CMD_LOGS) {

            char p[PATH_MAX];
            sprintf(p, "%s/%s.log", LOG_DIR, r.id);

            int fd = open(p, O_RDONLY);

            if (fd < 0) {
                printf(RED "no logs\n" RESET);
            } else {
                char b[512];
                int n;
                while ((n = read(fd, b, sizeof(b))) > 0)
                    write(1, b, n);
                close(fd);
            }
        }

        close(c);
    }
}

/* ================= CLIENT ================= */

static void send_req(request_t *r) {

    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, SOCK_PATH);

    connect(s, (struct sockaddr*)&a, sizeof(a));

    write(s, r, sizeof(*r));

    char buf[4096];
    int n;
    while ((n = read(s, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(s);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {

    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor")) {
        supervisor();
        return 0;
    }

    request_t r;
    memset(&r, 0, sizeof(r));

    if (!strcmp(argv[1], "start")) r.cmd = CMD_START;
    else if (!strcmp(argv[1], "ps")) r.cmd = CMD_PS;
    else if (!strcmp(argv[1], "stop")) r.cmd = CMD_STOP;
    else if (!strcmp(argv[1], "logs")) r.cmd = CMD_LOGS;

    if (r.cmd != CMD_PS)
        strcpy(r.id, argv[2]);

    if (r.cmd == CMD_START) {
        strcpy(r.rootfs, argv[3]);
        strcpy(r.command, argv[4]);
    }

    send_req(&r);
    return 0;
}
