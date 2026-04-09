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
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 4096
#define CONTAINER_ID_LEN 32

/* ===================== TYPES ===================== */

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int running;
    struct container_record *next;
} container_record;

typedef enum {
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

typedef struct {
    int status;
    char message[1024];
} control_response_t;

typedef struct {
    int monitor_fd;
    container_record *containers;
} supervisor_ctx_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int log_fd;
} child_config_t;

/* ===================== SIGNAL HANDLING ===================== */

void handle_sigchld(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void handle_sigint(int sig)
{
    (void)sig;
    unlink(CONTROL_PATH);
    printf("\nSupervisor shutting down...\n");
    exit(0);
}

/* ===================== CHILD ===================== */

int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    sethostname(cfg->id, strlen(cfg->id));
    chdir(cfg->rootfs);
    chroot(cfg->rootfs);

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_fd, STDOUT_FILENO);
    dup2(cfg->log_fd, STDERR_FILENO);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("exec failed");
    return 1;
}

/* ===================== SUPERVISOR ===================== */

static int run_supervisor()
{
    supervisor_ctx_t ctx = {0};

    signal(SIGCHLD, handle_sigchld);
    signal(SIGINT, handle_sigint);

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

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

        control_request_t req = {0};
        read(client, &req, sizeof(req));

        control_response_t res = {0};

        /* ================= START / RUN ================= */
        if (req.kind == CMD_START || req.kind == CMD_RUN) {

            int pipefd[2];
            pipe(pipefd);

            child_config_t cfg;
            strcpy(cfg.id, req.container_id);
            strcpy(cfg.rootfs, req.rootfs);
            strcpy(cfg.command, req.command);
            cfg.log_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              &cfg);

            /* register monitor */
            struct monitor_request mreq = {
                .pid = pid,
                .soft_limit_bytes = req.soft_limit_bytes,
                .hard_limit_bytes = req.hard_limit_bytes
            };
            strcpy(mreq.container_id, req.container_id);
            ioctl(ctx.monitor_fd, MONITOR_REGISTER, &mreq);

            /* add to list */
            container_record *node = malloc(sizeof(*node));
            strcpy(node->id, req.container_id);
            node->pid = pid;
            node->running = 1;
            node->next = ctx.containers;
            ctx.containers = node;

            close(pipefd[1]);

            /* foreground */
            if (req.kind == CMD_RUN) {
                char buf[LOG_CHUNK_SIZE];
                int n;

                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    write(STDOUT_FILENO, buf, n);
                }

                waitpid(pid, NULL, 0);
                node->running = 0;
            } else {
                close(pipefd[0]);
            }

            snprintf(res.message, sizeof(res.message),
                     "Started %s (PID %d)\n", req.container_id, pid);
        }

        /* ================= PS ================= */
        else if (req.kind == CMD_PS) {
            container_record *cur = ctx.containers;

            while (cur) {
                char line[128];
                snprintf(line, sizeof(line),
                         "ID=%s PID=%d STATE=%s\n",
                         cur->id, cur->pid,
                         cur->running ? "RUNNING" : "STOPPED");

                strcat(res.message, line);
                cur = cur->next;
            }
        }

        /* ================= LOGS ================= */
        else if (req.kind == CMD_LOGS) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

            FILE *f = fopen(path, "r");
            if (!f) {
                strcpy(res.message, "No logs found\n");
            } else {
                fread(res.message, 1, sizeof(res.message) - 1, f);
                fclose(f);
            }
        }

        /* ================= STOP ================= */
        else if (req.kind == CMD_STOP) {
            container_record *cur = ctx.containers;

            while (cur) {
                if (strcmp(cur->id, req.container_id) == 0) {
                    kill(cur->pid, SIGTERM);
                    cur->running = 0;
                    snprintf(res.message, sizeof(res.message),
                             "Stopped %s\n", req.container_id);
                    break;
                }
                cur = cur->next;
            }
        }

        write(client, &res, sizeof(res));
        close(client);
    }
}

/* ===================== CLIENT ===================== */

int send_request(control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    write(sock, req, sizeof(*req));

    control_response_t res;
    read(sock, &res, sizeof(res));

    printf("%s", res.message);

    close(sock);
    return 0;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    control_request_t req = {0};

    if (strcmp(argv[1], "start") == 0)
        req.kind = CMD_START;
    else if (strcmp(argv[1], "run") == 0)
        req.kind = CMD_RUN;
    else if (strcmp(argv[1], "ps") == 0)
        req.kind = CMD_PS;
    else if (strcmp(argv[1], "logs") == 0)
        req.kind = CMD_LOGS;
    else if (strcmp(argv[1], "stop") == 0)
        req.kind = CMD_STOP;
    else {
        printf("Unknown command\n");
        return 1;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        if (argc < 5) {
            printf("Usage: %s %s <id> <rootfs> <cmd>\n", argv[0], argv[1]);
            return 1;
        }

        strcpy(req.container_id, argv[2]);
        strcpy(req.rootfs, argv[3]);
        strcpy(req.command, argv[4]);

        req.soft_limit_bytes = 40UL << 20;
        req.hard_limit_bytes = 64UL << 20;
    }

    if (req.kind == CMD_LOGS || req.kind == CMD_STOP) {
        strcpy(req.container_id, argv[2]);
    }

    return send_request(&req);
}