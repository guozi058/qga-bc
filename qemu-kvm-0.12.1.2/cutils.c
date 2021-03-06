/*
 * Simple C functions to supplement the C library
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "host-utils.h"
#include <math.h>

void pstrcpy(char *buf, int buf_size, const char *str)
{
    int c;
    char *q = buf;

    if (buf_size <= 0)
        return;

    for(;;) {
        c = *str++;
        if (c == 0 || q >= buf + buf_size - 1)
            break;
        *q++ = c;
    }
    *q = '\0';
}

/* strcat and truncate. */
char *pstrcat(char *buf, int buf_size, const char *s)
{
    int len;
    len = strlen(buf);
    if (len < buf_size)
        pstrcpy(buf + len, buf_size - len, s);
    return buf;
}

int strstart(const char *str, const char *val, const char **ptr)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (*p != *q)
            return 0;
        p++;
        q++;
    }
    if (ptr)
        *ptr = p;
    return 1;
}

int stristart(const char *str, const char *val, const char **ptr)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (qemu_toupper(*p) != qemu_toupper(*q))
            return 0;
        p++;
        q++;
    }
    if (ptr)
        *ptr = p;
    return 1;
}

/* XXX: use host strnlen if available ? */
int qemu_strnlen(const char *s, int max_len)
{
    int i;

    for(i = 0; i < max_len; i++) {
        if (s[i] == '\0') {
            break;
        }
    }
    return i;
}

time_t mktimegm(struct tm *tm)
{
    time_t t;
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    if (m < 3) {
        m += 12;
        y--;
    }
    t = 86400 * (d + (153 * m - 457) / 5 + 365 * y + y / 4 - y / 100 + 
                 y / 400 - 719469);
    t += 3600 * tm->tm_hour + 60 * tm->tm_min + tm->tm_sec;
    return t;
}

int qemu_fls(int i)
{
    return 32 - clz32(i);
}

/*
 * Make sure data goes on disk, but if possible do not bother to
 * write out the inode just for timestamp updates.
 *
 * Unfortunately even in 2009 many operating systems do not support
 * fdatasync and have to fall back to fsync.
 */
int qemu_fdatasync(int fd)
{
#ifdef CONFIG_FDATASYNC
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

/* io vectors */

void qemu_iovec_init(QEMUIOVector *qiov, int alloc_hint)
{
    qiov->iov = qemu_malloc(alloc_hint * sizeof(struct iovec));
    qiov->niov = 0;
    qiov->nalloc = alloc_hint;
    qiov->size = 0;
}

void qemu_iovec_init_external(QEMUIOVector *qiov, struct iovec *iov, int niov)
{
    int i;

    qiov->iov = iov;
    qiov->niov = niov;
    qiov->nalloc = -1;
    qiov->size = 0;
    for (i = 0; i < niov; i++)
        qiov->size += iov[i].iov_len;
}

void qemu_iovec_add(QEMUIOVector *qiov, void *base, size_t len)
{
    assert(qiov->nalloc != -1);

    if (qiov->niov == qiov->nalloc) {
        qiov->nalloc = 2 * qiov->nalloc + 1;
        qiov->iov = qemu_realloc(qiov->iov, qiov->nalloc * sizeof(struct iovec));
    }
    qiov->iov[qiov->niov].iov_base = base;
    qiov->iov[qiov->niov].iov_len = len;
    qiov->size += len;
    ++qiov->niov;
}

/*
 * Copies iovecs from src to the end of dst. It starts copying after skipping
 * the given number of bytes in src and copies until src is completely copied
 * or the total size of the copied iovec reaches size.The size of the last
 * copied iovec is changed in order to fit the specified total size if it isn't
 * a perfect fit already.
 */
void qemu_iovec_copy(QEMUIOVector *dst, QEMUIOVector *src, uint64_t skip,
    size_t size)
{
    int i;
    size_t done;
    void *iov_base;
    uint64_t iov_len;

    assert(dst->nalloc != -1);

    done = 0;
    for (i = 0; (i < src->niov) && (done != size); i++) {
        if (skip >= src->iov[i].iov_len) {
            /* Skip the whole iov */
            skip -= src->iov[i].iov_len;
            continue;
        } else {
            /* Skip only part (or nothing) of the iov */
            iov_base = (uint8_t*) src->iov[i].iov_base + skip;
            iov_len = src->iov[i].iov_len - skip;
            skip = 0;
        }

        if (done + iov_len > size) {
            qemu_iovec_add(dst, iov_base, size - done);
            break;
        } else {
            qemu_iovec_add(dst, iov_base, iov_len);
        }
        done += iov_len;
    }
}

void qemu_iovec_concat(QEMUIOVector *dst, QEMUIOVector *src, size_t size)
{
    qemu_iovec_copy(dst, src, 0, size);
}

/*
 * Concatenates (partial) iovecs from src_iov to the end of dst.
 * It starts copying after skipping `soffset' bytes at the
 * beginning of src and adds individual vectors from src to
 * dst copies up to `sbytes' bytes total, or up to the end
 * of src_iov if it comes first.  This way, it is okay to specify
 * very large value for `sbytes' to indicate "up to the end
 * of src".
 * Only vector pointers are processed, not the actual data buffers.
 */
void qemu_iovec_concat_iov(QEMUIOVector *dst,
                           struct iovec *src_iov, unsigned int src_cnt,
                           size_t soffset, size_t sbytes)
{
    int i;
    size_t done;
    assert(dst->nalloc != -1);
    for (i = 0, done = 0; done < sbytes && i < src_cnt; i++) {
        if (soffset < src_iov[i].iov_len) {
            size_t len = MIN(src_iov[i].iov_len - soffset, sbytes - done);
            qemu_iovec_add(dst, src_iov[i].iov_base + soffset, len);
            done += len;
            soffset = 0;
        } else {
            soffset -= src_iov[i].iov_len;
        }
    }
    assert(soffset == 0); /* offset beyond end of src */
}

void qemu_iovec_destroy(QEMUIOVector *qiov)
{
    assert(qiov->nalloc != -1);

    qemu_iovec_reset(qiov);
    qemu_free(qiov->iov);
    qiov->nalloc = 0;
    qiov->iov = NULL;
}

void qemu_iovec_reset(QEMUIOVector *qiov)
{
    assert(qiov->nalloc != -1);

    qiov->niov = 0;
    qiov->size = 0;
}

void qemu_iovec_to_buffer(QEMUIOVector *qiov, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    int i;

    for (i = 0; i < qiov->niov; ++i) {
        memcpy(p, qiov->iov[i].iov_base, qiov->iov[i].iov_len);
        p += qiov->iov[i].iov_len;
    }
}

/*
 * No dma flushing needed here, as the aio code will call dma_bdrv_cb()
 * on completion as well, which will result in a call to
 * dma_bdrv_unmap() which will do the flushing ....
 */
void qemu_iovec_from_buffer(QEMUIOVector *qiov, const void *buf, size_t count)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t copy;
    int i;

    for (i = 0; i < qiov->niov && count; ++i) {
        copy = count;
        if (copy > qiov->iov[i].iov_len)
            copy = qiov->iov[i].iov_len;
        memcpy(qiov->iov[i].iov_base, p, copy);
        p     += copy;
        count -= copy;
    }
}

void qemu_iovec_memset(QEMUIOVector *qiov, int c, size_t count)
{
    size_t n;
    int i;

    for (i = 0; i < qiov->niov && count; ++i) {
        n = MIN(count, qiov->iov[i].iov_len);
        memset(qiov->iov[i].iov_base, c, n);
        count -= n;
    }
}

void qemu_iovec_memset_skip(QEMUIOVector *qiov, int c, size_t count,
                            size_t skip)
{
    int i;
    size_t done;
    void *iov_base;
    uint64_t iov_len;

    done = 0;
    for (i = 0; (i < qiov->niov) && (done != count); i++) {
        if (skip >= qiov->iov[i].iov_len) {
            /* Skip the whole iov */
            skip -= qiov->iov[i].iov_len;
            continue;
        } else {
            /* Skip only part (or nothing) of the iov */
            iov_base = (uint8_t*) qiov->iov[i].iov_base + skip;
            iov_len = qiov->iov[i].iov_len - skip;
            skip = 0;
        }

        if (done + iov_len > count) {
            memset(iov_base, c, count - done);
            break;
        } else {
            memset(iov_base, c, iov_len);
        }
        done += iov_len;
    }
}

/*
 * Checks if a buffer is all zeroes
 *
 * Attention! The len must be a multiple of 4 * sizeof(long) due to
 * restriction of optimizations in this function.
 */
bool buffer_is_zero(const void *buf, size_t len)
{
    /*
     * Use long as the biggest available internal data type that fits into the
     * CPU register and unroll the loop to smooth out the effect of memory
     * latency.
     */

    size_t i;
    long d0, d1, d2, d3;
    const long * const data = buf;

    assert(len % (4 * sizeof(long)) == 0);
    len /= sizeof(long);

    for (i = 0; i < len; i += 4) {
        d0 = data[i + 0];
        d1 = data[i + 1];
        d2 = data[i + 2];
        d3 = data[i + 3];

        if (d0 || d1 || d2 || d3) {
            return false;
        }
    }

    return true;
}

#ifndef _WIN32
/* Sets a specific flag */
int fcntl_setfl(int fd, int flag)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags == -1)
        return -errno;

    if (fcntl(fd, F_SETFL, flags | flag) == -1)
        return -errno;

    return 0;
}
#endif

/*
 * Convert string to bytes, allowing either B/b for bytes, K/k for KB,
 * M/m for MB, G/g for GB or T/t for TB. End pointer will be returned
 * in *end, if not NULL. A valid value must be terminated by
 * whitespace, ',' or '\0'. Return -ERANGE on overflow, Return -EINVAL
 * on other error.
 */
int64_t strtosz_suffix(const char *nptr, char **end, const char default_suffix)
{
    int64_t retval = -EINVAL;
    char *endptr;
    unsigned char c, d;
    int mul_required = 0;
    double val, mul, integral, fraction;

    errno = 0;
    val = strtod(nptr, &endptr);
    if (isnan(val) || endptr == nptr || errno != 0) {
        goto fail;
    }
    fraction = modf(val, &integral);
    if (fraction != 0) {
        mul_required = 1;
    }
    /*
     * Any whitespace character is fine for terminating the number,
     * in addition we accept ',' to handle strings where the size is
     * part of a multi token argument.
     */
    c = *endptr;
    d = c;
    if (qemu_isspace(c) || c == '\0' || c == ',') {
        c = 0;
        d = default_suffix;
    }
    switch (qemu_toupper(d)) {
    case STRTOSZ_DEFSUFFIX_B:
        mul = 1;
        if (mul_required) {
            goto fail;
        }
        break;
    case STRTOSZ_DEFSUFFIX_KB:
        mul = 1 << 10;
        break;
    case STRTOSZ_DEFSUFFIX_MB:
        mul = 1ULL << 20;
        break;
    case STRTOSZ_DEFSUFFIX_GB:
        mul = 1ULL << 30;
        break;
    case STRTOSZ_DEFSUFFIX_TB:
        mul = 1ULL << 40;
        break;
    default:
        goto fail;
    }
    /*
     * If not terminated by whitespace, ',', or \0, increment endptr
     * to point to next character, then check that we are terminated
     * by an appropriate separating character, ie. whitespace, ',', or
     * \0. If not, we are seeing trailing garbage, thus fail.
     */
    if (c != 0) {
        endptr++;
        if (!qemu_isspace(*endptr) && *endptr != ',' && *endptr != 0) {
            goto fail;
        }
    }
    if ((val * mul >= INT64_MAX) || val < 0) {
        retval = -ERANGE;
        goto fail;
    }
    retval = val * mul;

fail:
    if (end) {
        *end = endptr;
    }

    return retval;
}

int64_t strtosz(const char *nptr, char **end)
{
    return strtosz_suffix(nptr, end, STRTOSZ_DEFSUFFIX_MB);
}

/*
 * helper to parse debug environment variables
 */
int parse_debug_env(const char *name, int max, int initial)
{
    char *debug_env = getenv(name);
    char *inv = NULL;
    int debug;

    if (!debug_env) {
        return initial;
    }
    debug = strtol(debug_env, &inv, 10);
    if (inv == debug_env) {
        return initial;
    }
    if (debug < 0 || debug > max) {
        fprintf(stderr, "warning: %s not in [0, %d]", name, max);
        return initial;
    }
    return debug;
}
