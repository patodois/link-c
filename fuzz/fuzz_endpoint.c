/* libFuzzer harness: the endpoint file reader and writer, rbl_endpoint_read / rbl_endpoint_write (CONTRACT.md 6.2).
 *
 * Input: the first byte picks the file's permission bits (from a table), the rest is the file's content, written exactly
 * as given. Oracle:
 *   - a file that group or others can access (mode & 077) must be refused, whatever it holds; one that only the owner can
 *     access may be accepted;
 *   - an accepted file gives a port 1..65535 and a token of 1..256 bytes of valid UTF-8 without NUL, and was at most
 *     4096 bytes with no '\n' except the last byte (and a '\r' just before it);
 *   - the accepted values written back with rbl_endpoint_write() read back the same (and the written file is at most 4096
 *     bytes, mode 0600).
 * The decision on the content itself is compared with an independent reference by check_corpus.py.
 */
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include "fuzz_common.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const mode_t MODES[] = { 0600, 0400, 0644, 0640, 0602, 0660, 0700 & ~0077, 0604, 0620, 0666, 0200, 0610 };

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static char path[128], path2[128];
    mode_t mode;
    int fd;
    uint16_t port = 0;
    char token[257];
    int rc;
    if (size < 1) return 0;
    if (!path[0]) {
        snprintf(path, sizeof path, "/dev/shm/rbl-fuzz-ep-%d", (int)getpid());
        snprintf(path2, sizeof path2, "/dev/shm/rbl-fuzz-ep2-%d", (int)getpid());
    }
    mode = MODES[data[0] % (sizeof MODES / sizeof MODES[0])];
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) abort();
    if (size > 1 && write(fd, data + 1, size - 1) != (ssize_t)(size - 1)) abort();
    if (fchmod(fd, mode) != 0) abort();
    close(fd);

    rc = rbl_endpoint_read(path, &port, token);
    if (mode & 077) {
        FUZZ_CHECK(rc != RBL_OK, "a file with mode %o was accepted", (unsigned)mode);
    }
    if (rc == RBL_OK) {
        size_t tl = strlen(token), n = size - 1;
        struct stat st;
        char back[257];
        uint16_t bport = 0;
        FUZZ_CHECK(port >= 1, "port 0 accepted");
        FUZZ_CHECK(tl >= 1 && tl <= 256, "token length %zu", tl);
        FUZZ_CHECK(fz_utf8_valid((const uint8_t*)token, tl), "token is not UTF-8");
        FUZZ_CHECK(n <= 4096, "a file of %zu bytes was accepted", n);
        {   /* no newline but the last byte */
            size_t i;
            for (i = 0; i + 1 < n; i++) FUZZ_CHECK(data[1 + i] != '\n', "a newline at %zu was accepted", i);
        }
        /* write it back and read it again */
        unlink(path2);
        FUZZ_CHECK(rbl_endpoint_write(path2, port, token) == RBL_OK, "cannot write back an accepted endpoint");
        FUZZ_CHECK(stat(path2, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size <= 4096, "written file: mode %o size %lld",
                   (unsigned)(st.st_mode & 0777), (long long)st.st_size);
        FUZZ_CHECK(rbl_endpoint_read(path2, &bport, back) == RBL_OK && bport == port && strcmp(back, token) == 0, "round trip differs");
        unlink(path2);
    }
    unlink(path);
    return 0;
}
