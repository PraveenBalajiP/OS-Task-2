#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
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

#include "monitor_ioctl.h"

/* ================= CONFIG ================= */

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

#define MAX_CONTAINERS 100
#define CONTAINER_ID_LEN 32
#define LOG_CHUNK_SIZE 4096

/* ================= TYPES ================= */

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    char state[16];
    time_t start_time;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;
int monitor_fd = -1;

/* ================= FIND ================= */

container_t* find_container(const char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0)
            return &containers[i];
    }
    return NULL;
}

/* ================= CHILD ================= */

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int log_fd;
} child_config_t;

int child_fn(void *arg) {
    child_config_t *cfg = arg;

    sethostname(cfg->id, strlen(cfg->id));

    chroot(cfg->rootfs);
    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_fd, STDOUT_FILENO);
    dup2(cfg->log_fd, STDERR_FILENO);

    /* Disable buffering for logs */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("exec failed");
    return 1;
}

/* ================= LOG THREAD ================= */

typedef struct {
    int fd;
    char id[CONTAINER_ID_LEN];
} pipe_ctx_t;

void *pipe_reader(void *arg) {
    pipe_ctx_t *ctx = arg;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, ctx->id);

    int logfd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);

    char buf[LOG_CHUNK_SIZE];
    int n;

    while ((n = read(ctx->fd, buf, sizeof(buf))) > 0) {
        write(logfd, buf, n);
    }

    close(logfd);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

/* ================= SIGNAL ================= */

void sigchld_handler(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                if (WIFEXITED(status))
                    strcpy(containers[i].state, "exited");
                else if (WIFSIGNALED(status))
                    strcpy(containers[i].state, "killed");
                else
                    strcpy(containers[i].state, "stopped");
            }
        }
    }
}

void shutdown_handler(int sig) {
    printf("Shutting down supervisor...\n");

    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0) {
            kill(containers[i].pid, SIGTERM);
        }
    }

    unlink(CONTROL_PATH);

    if (monitor_fd >= 0)
        close(monitor_fd);

    exit(0);
}

/* ================= COMMAND ================= */

typedef enum {
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_STOP,
    CMD_LOGS
} cmd_t;

typedef struct {
    cmd_t type;
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    unsigned long soft;
    unsigned long hard;
} request_t;

/* ================= SUPERVISOR ================= */

void run_supervisor() {

    mkdir(LOG_DIR, 0755);

    monitor_fd = open("/dev/container_monitor", O_RDWR);

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    int server = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);
    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client = accept(server, NULL, NULL);

        request_t req;
        read(client, &req, sizeof(req));

        /* ---------- START ---------- */
        if (req.type == CMD_START) {

            int pipefd[2];
            pipe(pipefd);

            child_config_t *cfg = malloc(sizeof(*cfg));
            strcpy(cfg->id, req.id);
            strcpy(cfg->rootfs, req.rootfs);
            strcpy(cfg->command, req.command);
            cfg->log_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              cfg);

            if (pid < 0) {
                perror("clone failed");
                write(client, "CLONE FAILED\n", 13);
                continue;
            }

            struct monitor_request mreq = {
                .pid = pid,
                .soft_limit_bytes = req.soft,
                .hard_limit_bytes = req.hard
            };
            strcpy(mreq.container_id, req.id);
            ioctl(monitor_fd, MONITOR_REGISTER, &mreq);

            close(pipefd[1]);

            pipe_ctx_t *pctx = malloc(sizeof(*pctx));
            pctx->fd = pipefd[0];
            strcpy(pctx->id, req.id);

            pthread_t tid;
            pthread_create(&tid, NULL, pipe_reader, pctx);
            pthread_detach(tid);

            container_t *c = &containers[container_count++];
            strcpy(c->id, req.id);
            c->pid = pid;
            strcpy(c->state, "running");
            c->start_time = time(NULL);

            write(client, "STARTED\n", 8);
        }

        /* ---------- RUN ---------- */
        else if (req.type == CMD_RUN) {

            int pipefd[2];
            pipe(pipefd);

            child_config_t *cfg = malloc(sizeof(*cfg));
            strcpy(cfg->id, req.id);
            strcpy(cfg->rootfs, req.rootfs);
            strcpy(cfg->command, req.command);
            cfg->log_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              cfg);

            if (pid < 0) {
                perror("clone failed");
                write(client, "CLONE FAILED\n", 13);
                continue;
            }

            close(pipefd[1]);

            int status;
            waitpid(pid, &status, 0);

            free(cfg);
            free(stack);

            char resp[64];
            sprintf(resp, "EXIT %d\n", WEXITSTATUS(status));

            write(client, resp, strlen(resp));
        }

        /* ---------- PS ---------- */
        else if (req.type == CMD_PS) {

            char buf[1024] = "";

            for (int i = 0; i < container_count; i++) {
                char line[128];
                sprintf(line, "%s | PID:%d | %s\n",
                        containers[i].id,
                        containers[i].pid,
                        containers[i].state);

                strcat(buf, line);
            }

            write(client, buf, strlen(buf));
        }

        /* ---------- STOP ---------- */
        else if (req.type == CMD_STOP) {

            container_t *c = find_container(req.id);

            if (!c) {
                write(client, "NOT FOUND\n", 10);
            } else {
                kill(c->pid, SIGTERM);
                sleep(1);

                if (strcmp(c->state, "running") == 0)
                    kill(c->pid, SIGKILL);

                struct monitor_request mreq = {0};
                mreq.pid = c->pid;
                ioctl(monitor_fd, MONITOR_UNREGISTER, &mreq);

                strcpy(c->state, "stopped");

                write(client, "STOPPED\n", 8);
            }
        }

        /* ---------- LOGS ---------- */
        else if (req.type == CMD_LOGS) {

            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.id);

            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                write(client, "NO LOGS\n", 8);
            } else {
                char buf[512];
                int n;
                while ((n = read(fd, buf, sizeof(buf))) > 0) {
                    write(client, buf, n);
                }
                close(fd);
            }
        }

        close(client);
    }
}

/* ================= CLIENT ================= */

int send_request(request_t *req) {

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }

    write(sock, req, sizeof(*req));

    char buf[512] = {0};
    read(sock, buf, sizeof(buf));

    printf("%s", buf);

    close(sock);
    return 0;
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {

    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
        return 0;
    }

    request_t req = {0};

    if (strcmp(argv[1], "start") == 0)
        req.type = CMD_START;
    else if (strcmp(argv[1], "run") == 0)
        req.type = CMD_RUN;
    else if (strcmp(argv[1], "ps") == 0)
        req.type = CMD_PS;
    else if (strcmp(argv[1], "stop") == 0)
        req.type = CMD_STOP;
    else if (strcmp(argv[1], "logs") == 0)
        req.type = CMD_LOGS;

    if (req.type != CMD_PS) {
        strcpy(req.id, argv[2]);
    }

    if (req.type == CMD_START || req.type == CMD_RUN) {
        strcpy(req.rootfs, argv[3]);
        strcpy(req.command, argv[4]);
    }

    req.soft = 40UL << 20;
    req.hard = 64UL << 20;

    return send_request(&req);
}
