/* Browsered's native launcher and loopback-only HTTP server.
 * Portable POSIX, with macOS-only parts guarded by __APPLE__.
 * The browser runs the editor itself.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
CFDataRef browsered_create_pdf(const char *, size_t, const char *);
#endif
extern char **environ;
char *browsered_run_assistant(const char *, size_t, const char *);
char *browsered_run_claude(const char *, size_t);
char *browsered_ask_naia(const char *, size_t);
char *browsered_autoclave(const char *, size_t);
#define PORT 8766
#define EDITOR_URL "http://127.0.0.1:8766/"

struct PublicFile {
    const char *url;
    const char *filename;
    const char *content_type;
};

/* No directory browsing or arbitrary file paths: serve only editor assets. */
static const struct PublicFile public_files[] = {
    {"/", "Browsered.html", "text/html; charset=utf-8"},
    {"/Browsered.html", "Browsered.html", "text/html; charset=utf-8"},
    {"/browser-tests.html", "browser-tests.html", "text/html; charset=utf-8"},
    {"/browser-tests.js", "browser-tests.js", "text/javascript; charset=utf-8"},
    {"/keys.js", "keys.js", "text/javascript; charset=utf-8"},
    {"/femtoscheme/wasm/femtoscheme.js", "femtoscheme.js", "text/javascript; charset=utf-8"},
    {"/scheme.js", "scheme.js", "text/javascript; charset=utf-8"},
    {"/editor.scm", "editor.scm", "text/plain; charset=utf-8"},
    /* The user's init.scm next to Browsered.app, else the bundled default. */
    {"/init.scm", "init.scm", "text/plain; charset=utf-8"}
};

static bool send_all(int socket_fd, const char *bytes, size_t length)
{
    while (length > 0) {
        ssize_t sent = send(socket_fd, bytes, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void send_error(int client, const char *status)
{
    char response[256];
    int length = snprintf(response, sizeof(response),
        "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    send_all(client, response, (size_t)length);
}

/* Accept requests only from the editor's loopback origin. */
static void serve_action(int client, const char *url, const char *request,
                         size_t received, const char *resources)
{
    const char *body_start = strstr(request, "\r\n\r\n") + 4;
    size_t header_size = (size_t)(body_start - request);
    char headers[8192];
    if (header_size >= sizeof(headers)) { send_error(client, "400 Bad Request"); return; }
    memcpy(headers, request, header_size);
    headers[header_size] = '\0';
    if (!strstr(headers, "\r\nOrigin: http://127.0.0.1:8766\r\n")) {
        send_error(client, "403 Forbidden");
        return;
    }
    const char *length_header = strstr(headers, "\r\nContent-Length:");
    if (!length_header) { send_error(client, "411 Length Required"); return; }
    size_t length = (size_t)strtoull(length_header + strlen("\r\nContent-Length:"), NULL, 10);
    if (length == 0 || length > 2 * 1024 * 1024) {
        send_error(client, "413 Content Too Large");
        return;
    }
    size_t already_read = received - (size_t)(body_start - request);
    if (already_read > length) already_read = length;
    char *body = malloc(length + 1);
    if (!body) { send_error(client, "500 Internal Server Error"); return; }
    memcpy(body, body_start, already_read);
    size_t used = already_read;
    while (used < length) {
        ssize_t count = recv(client, body + used, length - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { free(body); return; }
        used += (size_t)count;
    }
    body[length] = '\0';

    const unsigned char *bytes = NULL;
    size_t output_length = 0;
    const char *content_type = NULL;
    char *answer = NULL;
#ifdef __APPLE__
    CFDataRef pdf = NULL;
#endif
    if (strcmp(url, "/assistant") == 0) {
        answer = browsered_run_assistant(body, length, resources);
        if (answer) {
            bytes = (const unsigned char *)answer;
            output_length = strlen(answer);
            content_type = "application/json; charset=utf-8";
        }
    } else if (strcmp(url, "/claude") == 0) {
        /* Claude Code's JSON envelope; the editor extracts the reply. */
        answer = browsered_run_claude(body, length);
        if (answer) {
            bytes = (const unsigned char *)answer;
            output_length = strlen(answer);
            content_type = "application/json; charset=utf-8";
        }
    } else if (strcmp(url, "/naia") == 0) {
        /* One question to NAIA in its Chrome tab; {"answer"} or {"error"}. */
        answer = browsered_ask_naia(body, length);
        if (answer) {
            bytes = (const unsigned char *)answer;
            output_length = strlen(answer);
            content_type = "application/json; charset=utf-8";
        }
    } else if (strcmp(url, "/autoclave") == 0) {
        /* The mechanical quote check of an autoclave test: a plain-text report. */
        answer = browsered_autoclave(body, length);
        if (answer) {
            bytes = (const unsigned char *)answer;
            output_length = strlen(answer);
            content_type = "text/plain; charset=utf-8";
        }
    } else if (strcmp(url, "/pdf") == 0) {
#ifdef __APPLE__
        char rules_path[PATH_MAX];
        snprintf(rules_path, sizeof(rules_path), "%s/../../../formatting.xml", resources);
        if (access(rules_path, R_OK) != 0) {
            snprintf(rules_path, sizeof(rules_path), "%s/formatting.xml", resources);
        }
        pdf = browsered_create_pdf(body, length, rules_path);
        if (pdf) {
            bytes = CFDataGetBytePtr(pdf);
            output_length = (size_t)CFDataGetLength(pdf);
            content_type = "application/pdf";
        }
#else
        free(body);
        send_error(client, "501 Not Implemented");
        return;
#endif
    }
    free(body);
    if (!bytes) { send_error(client, "502 Bad Gateway"); return; }
    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", content_type, output_length);
    if (send_all(client, header, (size_t)header_length)) send_all(client, (const char *)bytes, output_length);
    free(answer);
#ifdef __APPLE__
    if (pdf) CFRelease(pdf);
#endif
}

static void serve_request(int client, const char *resources)
{
    struct timeval timeout = {.tv_sec = 3};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* Read headers, including fragmented requests, with a strict size limit. */
    char request[8192];
    size_t used = 0;
    while (used < sizeof(request) - 1) {
        ssize_t received = recv(client, request + used, sizeof(request) - used - 1, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return;
        used += (size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    if (!strstr(request, "\r\n\r\n")) {
        send_error(client, "431 Request Header Fields Too Large");
        return;
    }

    char method[16], url[1024];
    if (sscanf(request, "%15s %1023s", method, url) != 2) {
        send_error(client, "400 Bad Request");
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        (strcmp(url, "/assistant") == 0 || strcmp(url, "/claude") == 0 ||
         strcmp(url, "/pdf") == 0 || strcmp(url, "/naia") == 0 ||
         strcmp(url, "/autoclave") == 0)) {
        serve_action(client, url, request, used, resources);
        return;
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(client, "405 Method Not Allowed");
        return;
    }
    char *query = strchr(url, '?');
    if (query) *query = '\0';

    const struct PublicFile *selected = NULL;
    for (size_t index = 0; index < sizeof(public_files) / sizeof(public_files[0]); ++index) {
        if (strcmp(url, public_files[index].url) == 0) {
            selected = &public_files[index];
            break;
        }
    }
    if (!selected) {
        send_error(client, "404 Not Found");
        return;
    }

    char filename[PATH_MAX];
    int path_length = -1;
    if (strcmp(selected->filename, "init.scm") == 0) {
        path_length = snprintf(filename, sizeof(filename), "%s/../../../init.scm", resources);
        if (path_length >= 0 && (size_t)path_length < sizeof(filename) &&
            access(filename, R_OK) != 0) path_length = -1;
    }
    if (path_length < 0)
        path_length = snprintf(filename, sizeof(filename), "%s/%s", resources, selected->filename);
    if (path_length < 0 || (size_t)path_length >= sizeof(filename)) {
        send_error(client, "500 Internal Server Error");
        return;
    }
    int file = open(filename, O_RDONLY);
    struct stat metadata;
    if (file < 0 || fstat(file, &metadata) < 0 || !S_ISREG(metadata.st_mode)) {
        if (file >= 0) close(file);
        send_error(client, "404 Not Found");
        return;
    }

    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n", selected->content_type, (long long)metadata.st_size);
    if (send_all(client, header, (size_t)header_length) && strcmp(method, "HEAD") != 0) {
        char buffer[16384];
        ssize_t count;
        while ((count = read(file, buffer, sizeof(buffer))) > 0) {
            if (!send_all(client, buffer, (size_t)count)) break;
        }
    }
    close(file);
}

static int create_listener(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    int reuse_address = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 16) < 0) {
        close(listener);
        return -1;
    }
    return listener;
}

static int serve_forever(int listener, const char *resources)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0 && errno == EINTR) continue;
        if (client < 0) return 1;
        /* A long assistant request must not block the editor. */
        pid_t worker = fork();
        if (worker == 0) {
            close(listener);
            signal(SIGCHLD, SIG_DFL);
            serve_request(client, resources);
            close(client);
            _exit(0);
        }
        close(client);
    }
}

/* The lock file records the helper that holds it: its process id, then the
 * modification time and path of the program it runs. */
static void record_helper(int lock, pid_t helper, const char *identity)
{
    char line[PATH_MAX + 64];
    int length = snprintf(line, sizeof(line), "%ld %s\n", (long)helper, identity);
    if (length > 0 && (size_t)length < sizeof(line) && ftruncate(lock, 0) == 0) {
        if (pwrite(lock, line, (size_t)length, 0) != (ssize_t)length) {
            int ignored = ftruncate(lock, 0);
            (void)ignored;
        }
    }
}

/* A helper started by another build of Browsered (an older one, or the copy
 * in another folder) would keep serving its own files. Stop it and take the
 * lock. Returns false when the running helper is this build, or cannot be
 * replaced; the caller then reuses it. */
static bool replace_other_helper(int lock, const char *identity)
{
    char line[PATH_MAX + 64];
    ssize_t length = pread(lock, line, sizeof(line) - 1, 0);
    if (length <= 0) return false;
    line[length] = '\0';
    char *newline = strchr(line, '\n');
    if (newline) *newline = '\0';
    char *space = strchr(line, ' ');
    if (!space || strcmp(space + 1, identity) == 0) return false;
    long helper = strtol(line, NULL, 10);
    if (helper <= 1 || kill((pid_t)helper, SIGTERM) != 0) return false;
    for (int attempt = 0; attempt < 50; ++attempt) {   /* up to 5 seconds */
        if (flock(lock, LOCK_EX | LOCK_NB) == 0) return true;
        usleep(100000);
    }
    return false;
}

static void open_editor(void)
{
#ifdef __APPLE__
    char *arguments[] = {"/usr/bin/open", "-a", "Google Chrome", EDITOR_URL, NULL};
#else
    char *arguments[] = {"/usr/bin/xdg-open", EDITOR_URL, NULL};
#endif
    pid_t process;
    if (posix_spawn(&process, arguments[0], NULL, NULL, arguments, environ) == 0) {
        waitpid(process, NULL, 0);
    }
}

int main(int argc, char **argv)
{
    /* Resolve assets relative to the app so the whole bundle can be moved. */
    char executable[PATH_MAX], resolved[PATH_MAX], resources[PATH_MAX];
#ifdef __APPLE__
    uint32_t capacity = sizeof(executable);
    if (_NSGetExecutablePath(executable, &capacity) != 0 || !realpath(executable, resolved)) return 1;
#else
    ssize_t link_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (link_length <= 0) return 1;
    executable[link_length] = '\0';
    if (!realpath(executable, resolved)) return 1;
#endif
    struct stat program;
    char identity[PATH_MAX + 32];
    if (stat(resolved, &program) != 0) return 1;
    snprintf(identity, sizeof(identity), "%lld %s", (long long)program.st_mtime, resolved);
    char *separator = strrchr(resolved, '/');
    if (!separator) return 1;
    *separator = '\0';
    int length = snprintf(resources, sizeof(resources), "%s/../Resources", resolved);
    if (length < 0 || (size_t)length >= sizeof(resources)) return 1;

    /* A per-user lock lets repeated app launches reuse our existing server. */
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "/tmp/browsered-%u.lock", (unsigned)getuid());
    int lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock < 0) return 1;
    if (flock(lock, LOCK_EX | LOCK_NB) < 0 && !replace_other_helper(lock, identity)) {
        close(lock);
        if (argc == 1) open_editor();
        return 0;
    }

    int listener = create_listener();
    if (listener < 0) {
        fprintf(stderr, "Browsered: cannot open local port %d: %s\n", PORT, strerror(errno));
        close(lock);
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "--serve") == 0) {
        record_helper(lock, getpid(), identity);
        return serve_forever(listener, resources);
    }

    /* The app starts its helper itself: no terminal or installed runtime. */
    pid_t helper = fork();
    if (helper < 0) return 1;
    if (helper == 0) {
        setsid();
        int null_device = open("/dev/null", O_RDWR);
        if (null_device >= 0) {
            dup2(null_device, STDIN_FILENO);
            dup2(null_device, STDOUT_FILENO);
            dup2(null_device, STDERR_FILENO);
            if (null_device > STDERR_FILENO) close(null_device);
        }
        return serve_forever(listener, resources);
    }
    record_helper(lock, helper, identity);
    close(listener);
    close(lock);
    open_editor();
    return 0;
}
