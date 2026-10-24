/* Sources and the autoclave's quote check.
 *
 * A request may name its sources: https links, or files on this Mac (~/... or
 * /Users/...).  Browsered copies them into the request's private workspace as
 * source-1.pdf, source-2.html, ... and, for a PDF, extracts its text into
 * source-1.txt with the system's PDFKit.  The assistant may read only those
 * files.
 *
 * The quote check is the mechanical part of the autoclave: every quotation in
 * a test's KEY and NAIA answers is searched for in the source text, letter by
 * letter.  No language model takes part, so the check cannot hallucinate.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif
extern char **environ;

#define MAX_SOURCES 4
#define MAX_SOURCE_BYTES (60L * 1024 * 1024)

int browsered_fetch_sources(const char *text, size_t length, const char *directory,
                            char *notes, size_t notes_size);
char *browsered_autoclave(const char *buffer, size_t length);

/* Growing strings ------------------------------------------------------- */

struct Text {
    char *bytes;
    size_t length, capacity;
};

static bool text_add(struct Text *text, const char *bytes, size_t length)
{
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 1024;
        while (text->length + length + 1 > capacity) capacity *= 2;
        char *grown = realloc(text->bytes, capacity);
        if (!grown) return false;
        text->bytes = grown;
        text->capacity = capacity;
    }
    memcpy(text->bytes + text->length, bytes, length);
    text->length += length;
    text->bytes[text->length] = '\0';
    return true;
}

static bool text_puts(struct Text *text, const char *string)
{
    return text_add(text, string, strlen(string));
}

static bool text_printf(struct Text *text, const char *format, const char *a, const char *b)
{
    char line[2048];
    int length = snprintf(line, sizeof(line), format, a, b);
    if (length < 0) return false;
    if ((size_t)length >= sizeof(line)) length = (int)sizeof(line) - 1;
    return text_add(text, line, (size_t)length);
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *bytes = NULL;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) size = ftell(file);
    if (size >= 0 && size <= MAX_SOURCE_BYTES && fseek(file, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)size + 1);
        if (bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size) {
            bytes[size] = '\0';
            if (length) *length = (size_t)size;
        } else {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(file);
    return bytes;
}

static bool write_file(const char *path, const char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool written = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

/* Finding the sources a text names ---------------------------------------- */

/* A source ends at white space, or at ) ] > " ' when they close the link. */
static size_t token_length(const char *start, const char *end)
{
    const char *cursor = start;
    while (cursor < end && !isspace((unsigned char)*cursor) &&
           !strchr("()<>\"'[]", *cursor)) ++cursor;
    while (cursor > start && strchr(".,;:", cursor[-1])) --cursor;
    return (size_t)(cursor - start);
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t length = strlen(name), suffix_length = strlen(suffix);
    if (length < suffix_length) return false;
    for (size_t index = 0; index < suffix_length; ++index) {
        if (tolower((unsigned char)name[length - suffix_length + index]) != suffix[index]) return false;
    }
    return true;
}

/* The sources named in text: https links and files under the home folder. */
static int find_sources(const char *text, size_t length, char found[][PATH_MAX], int limit)
{
    const char *end = text + length;
    int count = 0;
    for (const char *cursor = text; cursor < end && count < limit; ++cursor) {
        bool link = (size_t)(end - cursor) > 8 && strncmp(cursor, "https://", 8) == 0;
        bool home = (size_t)(end - cursor) > 2 && strncmp(cursor, "~/", 2) == 0;
        bool user = (size_t)(end - cursor) > 7 && strncmp(cursor, "/Users/", 7) == 0;
        if (!link && !home && !user) continue;
        if (cursor > text && !isspace((unsigned char)cursor[-1]) && !strchr("(<\"'[", cursor[-1])) continue;
        size_t size = token_length(cursor, end);
        if (size == 0 || size >= PATH_MAX) continue;
        char candidate[PATH_MAX];
        memcpy(candidate, cursor, size);
        candidate[size] = '\0';
        if (!link && !has_suffix(candidate, ".pdf") && !has_suffix(candidate, ".txt") &&
            !has_suffix(candidate, ".md") && !has_suffix(candidate, ".html")) continue;
        bool repeated = false;
        for (int index = 0; index < count; ++index) repeated |= strcmp(found[index], candidate) == 0;
        if (!repeated) strcpy(found[count++], candidate);
        cursor += size - 1;
    }
    return count;
}

/* Downloading, copying and reading ---------------------------------------- */

static bool run_quietly(char *const arguments[])
{
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t process;
    int spawned = posix_spawn(&process, arguments[0], &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return false;
    int status = 0;
    while (waitpid(process, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* The system's curl, presented as a browser: some journals refuse other programs. */
static bool download(const char *link, const char *path)
{
    char *arguments[] = {"/usr/bin/curl", "--silent", "--location", "--fail",
        "--max-time", "90", "--max-filesize", "60000000", "--user-agent",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/141.0 Safari/537.36",
        "--output", (char *)path, (char *)link, NULL};
    return run_quietly(arguments);
}

static bool copy_home_file(const char *name, const char *path)
{
    char full[PATH_MAX];
    const char *home = getenv("HOME");
    if (strncmp(name, "~/", 2) == 0) {
        if (!home || snprintf(full, sizeof(full), "%s/%s", home, name + 2) >= (int)sizeof(full)) return false;
    } else {
        snprintf(full, sizeof(full), "%s", name);
    }
    size_t length = 0;
    char *bytes = read_file(full, &length);
    if (!bytes) return false;
    bool copied = write_file(path, bytes, length);
    free(bytes);
    return copied;
}

/* PDF text through PDFKit, called from C through the Objective-C runtime. */
static bool pdf_to_text(const char *pdf_path, const char *text_path)
{
#ifdef __APPLE__
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)pdf_path, (CFIndex)strlen(pdf_path), false);
    if (!url) return false;
    Class pdf_class = objc_getClass("PDFDocument");
    bool written = false;
    if (pdf_class) {
        id document = ((id (*)(id, SEL))objc_msgSend)((id)pdf_class, sel_registerName("alloc"));
        document = ((id (*)(id, SEL, id))objc_msgSend)(document, sel_registerName("initWithURL:"), (id)url);
        if (document) {
            id string = ((id (*)(id, SEL))objc_msgSend)(document, sel_registerName("string"));
            const char *utf8 = string
                ? ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String"))
                : NULL;
            if (utf8 && *utf8) written = write_file(text_path, utf8, strlen(utf8));
            ((void (*)(id, SEL))objc_msgSend)(document, sel_registerName("release"));
        }
    }
    CFRelease(url);
    return written;
#else
    (void)pdf_path; (void)text_path;
    return false;
#endif
}

static bool starts_with_bytes(const char *path, const char *magic)
{
    char head[8] = {0};
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(head, 1, sizeof(head) - 1, file);
    fclose(file);
    return count >= strlen(magic) && memcmp(head, magic, strlen(magic)) == 0;
}

/* Fetch one source into directory as source-N.*; its text goes to source-N.txt.
 * Returns the text file's name, or NULL with the reason in why. */
/* The most recent source is remembered, so that a later request (scoring the
 * answers of a test, say) can use it without naming it again.  It is kept in
 * ~/Library/Application Support/Browsered/: last-source.txt and its name. */
static bool memory_path(char *path, size_t size, const char *file)
{
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return false;
#ifdef __APPLE__
    const char *const levels[] = {"Library", "Library/Application Support",
                                  "Library/Application Support/Browsered"};
    for (size_t index = 0; index < 3; ++index) {
        snprintf(path, size, "%s/%s", home, levels[index]);
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
    }
    int length = snprintf(path, size, "%s/Library/Application Support/Browsered/%s", home, file);
    return length > 0 && (size_t)length < size;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char config_root[PATH_MAX], browsered_dir[PATH_MAX];
    if (xdg && xdg[0] == '/') snprintf(config_root, sizeof(config_root), "%s", xdg);
    else snprintf(config_root, sizeof(config_root), "%s/.config", home);
    if (mkdir(config_root, 0700) != 0 && errno != EEXIST) return false;
    snprintf(browsered_dir, sizeof(browsered_dir), "%s/Browsered", config_root);
    if (mkdir(browsered_dir, 0700) != 0 && errno != EEXIST) return false;
    int length = snprintf(path, size, "%s/%s", browsered_dir, file);
    return length > 0 && (size_t)length < size;
#endif
}

static void remember_source(const char *name, const char *text_path)
{
    char path[PATH_MAX];
    size_t length = 0;
    char *bytes = read_file(text_path, &length);
    if (bytes && memory_path(path, sizeof(path), "last-source.txt") && write_file(path, bytes, length) &&
        memory_path(path, sizeof(path), "last-source.name")) {
        write_file(path, name, strlen(name));
    }
    free(bytes);
}

/* Copy the remembered source into directory as source-N.txt; its name goes to name. */
static bool recall_source(const char *directory, int number, char *text_name, size_t text_size,
                          char *name, size_t name_size)
{
    char path[PATH_MAX], target[PATH_MAX];
    size_t length = 0;
    if (!memory_path(path, sizeof(path), "last-source.name")) return false;
    char *remembered = read_file(path, &length);
    if (!remembered) return false;
    snprintf(name, name_size, "%s", remembered);
    free(remembered);
    if (!memory_path(path, sizeof(path), "last-source.txt")) return false;
    char *bytes = read_file(path, &length);
    if (!bytes) return false;
    snprintf(text_name, text_size, "source-%d.txt", number);
    snprintf(target, sizeof(target), "%s/%s", directory, text_name);
    bool copied = write_file(target, bytes, length);
    free(bytes);
    return copied;
}

static bool fetch_source(const char *name, int number, const char *directory,
                         char *text_name, size_t text_size, const char **why)
{
    char raw[PATH_MAX], text_path[PATH_MAX];
    snprintf(raw, sizeof(raw), "%s/source-%d.raw", directory, number);
    snprintf(text_name, text_size, "source-%d.txt", number);
    snprintf(text_path, sizeof(text_path), "%s/%s", directory, text_name);
    bool fetched = strncmp(name, "https://", 8) == 0 ? download(name, raw) : copy_home_file(name, raw);
    if (!fetched) {
        *why = strncmp(name, "https://", 8) == 0
            ? "the download failed (the site may refuse programs; download it in Chrome and give its path)"
            : "the file could not be read";
        return false;
    }
    if (starts_with_bytes(raw, "%PDF")) {
        char pdf[PATH_MAX];
        snprintf(pdf, sizeof(pdf), "%s/source-%d.pdf", directory, number);
        rename(raw, pdf);
        if (!pdf_to_text(pdf, text_path)) {
            *why = "the PDF has no extractable text";
            return false;
        }
        remember_source(name, text_path);
        return true;
    }
    if (strncmp(name, "https://", 8) == 0 && has_suffix(name, ".pdf")) {
        unlink(raw);
        *why = "the site sent a web page instead of the PDF (download it in Chrome and give its path)";
        return false;
    }
    if (rename(raw, text_path) != 0) {
        *why = "the file could not be stored";
        return false;
    }
    remember_source(name, text_path);
    return true;
}

/* The part of a request that may name sources: the user's request, and the
 * ";; source:" lines of an autoclave test in the buffer.  Other links in the
 * buffer are content, never fetched. */
static size_t request_part(const char *text, size_t length)
{
    const char *marker = strstr(text, "\n\nCurrent buffer:\n");
    return marker ? (size_t)(marker - text) : length;
}

static int source_lines(const char *text, size_t length, char found[][PATH_MAX], int count, int limit)
{
    const char *end = text + length;
    for (const char *line = text; line < end && count < limit;) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (!next) next = end;
        const char *field = line;
        while (field < next && (*field == ' ' || *field == '\t')) ++field;
        if ((size_t)(next - field) > 11 && strncmp(field, ";; source:", 10) == 0) {
            count += find_sources(field + 10, (size_t)(next - field - 10), found + count, limit - count);
        }
        line = next + 1;
    }
    return count;
}

/* Fetch the sources a request names into directory and describe them in
 * notes for the assistant.  Returns the number of sources whose text is ready. */
int browsered_fetch_sources(const char *text, size_t length, const char *directory,
                            char *notes, size_t notes_size)
{
    char found[MAX_SOURCES][PATH_MAX];
    int count = find_sources(text, request_part(text, length), found, MAX_SOURCES);
    count = source_lines(text, length, found, count, MAX_SOURCES);
    notes[0] = '\0';
    if (count == 0) {
        char text_name[64], name[PATH_MAX];
        if (!recall_source(directory, 1, text_name, sizeof(text_name), name, sizeof(name))) return 0;
        snprintf(notes, notes_size,
            "This request names no source. The most recent source used in Browsered is %s; "
            "its text is %s in your working directory (read it with the Read tool; it is the "
            "cached source copy, not a file-access restriction). Use it when relevant.\n\n",
            name, text_name);
        return 1;
    }
    size_t used = (size_t)snprintf(notes, notes_size,
        "Sources Browsered fetched for this request, in your working directory "
        "(read with available file tools; these copies do not restrict other authorized reads):\n");
    int ready = 0;
    for (int index = 0; index < count && used < notes_size; ++index) {
        char text_name[64];
        const char *why = NULL;
        int written;
        if (fetch_source(found[index], index + 1, directory, text_name, sizeof(text_name), &why)) {
            ++ready;
            written = snprintf(notes + used, notes_size - used, "- %s: text of %s\n", text_name, found[index]);
        } else {
            written = snprintf(notes + used, notes_size - used, "- %s: not available, %s\n", found[index], why);
        }
        if (written > 0) used += (size_t)written;
    }
    if (used < notes_size) snprintf(notes + used, notes_size - used, "\n");
    return ready;
}

/* The quote check -------------------------------------------------------- */

/* Letters and digits in lower case, every other run of characters as one
 * space, so that line breaks, hyphenation, curly quotes and dashes from a PDF
 * do not hide a quotation.  Letters outside ASCII (é, ã) are kept as they are;
 * the ligatures fi, fl, ff, ffi and ffl are spelt out. */
static char *normalize(const char *text, size_t length)
{
    char *out = malloc(length * 3 + 1);
    if (!out) return NULL;
    size_t used = 0;
    bool space = true;
    for (size_t index = 0; index < length;) {
        unsigned char c = (unsigned char)text[index];
        if (c == '-' && index + 1 < length) {
            /* A word broken at the end of a line: "transcrip-\ntion". */
            size_t after = index + 1;
            while (after < length && (text[after] == ' ' || text[after] == '\r')) ++after;
            if (after < length && text[after] == '\n' && used > 0 && !space) {
                index = after + 1;
                while (index < length && isspace((unsigned char)text[index])) ++index;
                continue;
            }
        }
        if (c < 0x80) {
            if (isalnum(c)) {
                out[used++] = (char)tolower(c);
                space = false;
            } else if (!space) {
                out[used++] = ' ';
                space = true;
            }
            ++index;
            continue;
        }
        size_t width = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (index + width > length) width = length - index;
        const char *spelt = NULL;
        if (width == 3 && (unsigned char)text[index] == 0xEF && (unsigned char)text[index + 1] == 0xAC) {
            switch ((unsigned char)text[index + 2]) {
            case 0x80: spelt = "ff"; break;
            case 0x81: spelt = "fi"; break;
            case 0x82: spelt = "fl"; break;
            case 0x83: spelt = "ffi"; break;
            case 0x84: spelt = "ffl"; break;
            default: break;
            }
        }
        /* U+2000..U+206F: spaces, dashes, quotation marks, bullets. */
        bool punctuation = width == 3 && c == 0xE2 && (unsigned char)text[index + 1] >= 0x80 &&
                           (unsigned char)text[index + 1] <= 0x81;
        bool no_break = width == 2 && c == 0xC2 && (unsigned char)text[index + 1] == 0xA0;
        if (spelt) {
            size_t size = strlen(spelt);
            memcpy(out + used, spelt, size);
            used += size;
            space = false;
        } else if (punctuation || no_break) {
            if (!space) { out[used++] = ' '; space = true; }
        } else {
            memcpy(out + used, text + index, width);
            used += width;
            space = false;
        }
        index += width;
    }
    while (used > 0 && out[used - 1] == ' ') --used;
    out[used] = '\0';
    return out;
}

static int word_count(const char *normalized)
{
    int words = 0;
    for (const char *cursor = normalized; *cursor; ++cursor) {
        if (*cursor != ' ' && (cursor == normalized || cursor[-1] == ' ')) ++words;
    }
    return words;
}

/* Opening and closing marks: "..." “...” «...» */
static const char *closing_mark(const char *cursor, size_t *open_width)
{
    if (*cursor == '"') { *open_width = 1; return "\""; }
    if (strncmp(cursor, "\xE2\x80\x9C", 3) == 0) { *open_width = 3; return "\xE2\x80\x9D"; }
    if (strncmp(cursor, "\xC2\xAB", 2) == 0) { *open_width = 2; return "\xC2\xBB"; }
    return NULL;
}

struct Check {
    int quotes, missing;
};

/* Check every quotation of four or more words in one answer. */
static void check_answer(struct Text *report, struct Check *check, const char *tag,
                         const char *answer, size_t length, const char *source)
{
    const char *end = answer + length;
    int found_quotes = 0;
    for (const char *cursor = answer; cursor < end; ++cursor) {
        size_t open_width = 0;
        const char *close = closing_mark(cursor, &open_width);
        if (!close) continue;
        const char *start = cursor + open_width;
        const char *stop = NULL;
        for (const char *scan = start; scan + strlen(close) <= end; ++scan) {
            if (strncmp(scan, close, strlen(close)) == 0) { stop = scan; break; }
        }
        if (!stop) break;
        cursor = stop + strlen(close) - 1;
        char *quote = normalize(start, (size_t)(stop - start));
        if (!quote) continue;
        if (word_count(quote) >= 4) {
            ++found_quotes;
            ++check->quotes;
            char shown[240];
            size_t size = (size_t)(stop - start) < sizeof(shown) - 4 ? (size_t)(stop - start) : sizeof(shown) - 4;
            memcpy(shown, start, size);
            strcpy(shown + size, size < (size_t)(stop - start) ? "..." : "");
            for (char *c = shown; *c; ++c) if (*c == '\n') *c = ' ';
            bool present = strstr(source, quote) != NULL;
            if (!present) ++check->missing;
            char line[400];
            int size_written = present
                ? snprintf(line, sizeof(line), "%s quote %d: found in the source\n", tag, found_quotes)
                : snprintf(line, sizeof(line), "%s quote %d: NOT FOUND in the source: \"%s\"\n",
                           tag, found_quotes, shown);
            if (size_written > 0) text_add(report, line, strlen(line));
        }
        free(quote);
    }
    if (found_quotes == 0) text_printf(report, "%s: no quotation to check\n", tag, "");
}

/* Tags of an autoclave test: Q1:, KEY1:, NAIA1: at the start of a line. */
static size_t line_tag(const char *line, const char *end, char *tag, size_t tag_size)
{
    const char *cursor = line;
    while (cursor < end && isupper((unsigned char)*cursor)) ++cursor;
    size_t letters = (size_t)(cursor - line);
    const char *digits = cursor;
    while (cursor < end && isdigit((unsigned char)*cursor)) ++cursor;
    if (letters == 0 || cursor == digits || cursor >= end || *cursor != ':') return 0;
    size_t size = (size_t)(cursor - line);
    if (size + 1 > tag_size) return 0;
    memcpy(tag, line, size);
    tag[size] = '\0';
    return size + 1;
}

/* Run the quote check over an autoclave test and return the report, to be
 * appended to the buffer. */
char *browsered_autoclave(const char *buffer, size_t length)
{
    struct Text report = {0};
    char found[MAX_SOURCES][PATH_MAX];
    int count = source_lines(buffer, length, found, 0, MAX_SOURCES);
    text_puts(&report, "\n;; autoclave report: mechanical quote check, no AI involved\n");
    char directory[] = "/tmp/browsered-autoclave-XXXXXX";
    if (!mkdtemp(directory)) return report.bytes;
    struct Text source = {0};
    if (count == 0) {
        /* No ";; source:" line: use the most recent source, and say so. */
        char text_name[64], name[PATH_MAX], path[PATH_MAX];
        if (recall_source(directory, 1, text_name, sizeof(text_name), name, sizeof(name))) {
            snprintf(path, sizeof(path), "%s/%s", directory, text_name);
            size_t size = 0;
            char *bytes = read_file(path, &size);
            if (bytes) {
                text_add(&source, bytes, size);
                text_printf(&report, ";; source: %s (the most recent source used in Browsered; %s)\n",
                            name, "the buffer names none");
                free(bytes);
            }
            unlink(path);
        } else {
            text_puts(&report, ";; no source: name one in a \";; source:\" line\n");
        }
    }
    for (int index = 0; index < count; ++index) {
        char text_name[64], path[PATH_MAX];
        const char *why = NULL;
        if (fetch_source(found[index], index + 1, directory, text_name, sizeof(text_name), &why)) {
            snprintf(path, sizeof(path), "%s/%s", directory, text_name);
            size_t size = 0;
            char *bytes = read_file(path, &size);
            if (bytes) {
                text_add(&source, bytes, size);
                text_add(&source, "\n", 1);
                char characters[32];
                snprintf(characters, sizeof(characters), "%zu", size);
                text_printf(&report, ";; source: %s (%s bytes of text)\n", found[index], characters);
                free(bytes);
            }
            unlink(path);
        } else {
            text_printf(&report, ";; source: %s NOT AVAILABLE: %s\n", found[index], why);
        }
        snprintf(path, sizeof(path), "%s/source-%d.pdf", directory, index + 1);
        unlink(path);
        snprintf(path, sizeof(path), "%s/source-%d.raw", directory, index + 1);
        unlink(path);
    }
    rmdir(directory);
    if (source.length == 0) {
        free(source.bytes);
        return report.bytes;
    }
    char *normalized_source = normalize(source.bytes, source.length);
    free(source.bytes);
    if (!normalized_source) return report.bytes;

    struct Check check = {0};
    const char *end = buffer + length;
    char tag[32] = "";
    const char *answer = NULL;
    bool tagged = false;
    for (const char *line = buffer; line <= end;) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (!next) next = end;
        char new_tag[32];
        size_t tag_size = line_tag(line, next, new_tag, sizeof(new_tag));
        bool comment = next - line >= 2 && line[0] == ';' && line[1] == ';';
        if (tag_size || comment || next == end) {
            const char *answer_end = (tag_size || comment) ? line : end;
            if (answer && (strncmp(tag, "KEY", 3) == 0 || strncmp(tag, "NAIA", 4) == 0)) {
                check_answer(&report, &check, tag, answer, (size_t)(answer_end - answer), normalized_source);
                tagged = true;
            }
            answer = NULL;
            tag[0] = '\0';
            if (tag_size) {
                strcpy(tag, new_tag);
                answer = line + tag_size;
            }
        }
        if (next == end) break;
        line = next + 1;
    }
    if (!tagged) {
        /* Answers pasted in another form: check every quotation in the buffer. */
        text_puts(&report, ";; no KEY or NAIA lines: checking every quotation in the buffer\n");
        check_answer(&report, &check, "Buffer", buffer, length, normalized_source);
    }
    free(normalized_source);
    char numbers[64];
    snprintf(numbers, sizeof(numbers), "%d quotations checked, %d", check.quotes, check.missing);
    text_printf(&report, ";; summary: %s not found in the source%s\n", numbers,
                check.missing ? " (red bag: suspect answers)" : "");
    return report.bytes;
}
