//
//  MtkLog.cpp
//  SimpleMtkWlan
//

#include "MtkLog.hpp"

#include <libkern/libkern.h>
#include <IOKit/IOLib.h>

static constexpr size_t kLogSize = 262144;  // start-up is ~100 KB of log at full verbosity
static char gLog[kLogSize];
static volatile uint64_t gLogHead; // total bytes ever written

volatile int mtk_verbose_scans = 8;

void mtk_vlog(const char *fmt, va_list ap)
{
    char line[320];
    int len = vsnprintf(line, sizeof(line), fmt, ap);
    if (len <= 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = sizeof(line) - 1;

    IOLog("%s", line);

    // Reserve space atomically; concurrent writers (interrupt and work loop paths)
    // get disjoint byte ranges, so lines never interleave mid-character.
    uint64_t start = __atomic_fetch_add(&gLogHead, (uint64_t)len, __ATOMIC_RELAXED);
    for (int i = 0; i < len; i++)
        gLog[(start + i) % kLogSize] = line[i];
}

void mtk_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mtk_vlog(fmt, ap);
    va_end(ap);
}

size_t mtk_log_snapshot(char *out, size_t max)
{
    if (!out || max == 0)
        return 0;
    uint64_t head = __atomic_load_n(&gLogHead, __ATOMIC_RELAXED);
    size_t avail = head < kLogSize ? (size_t)head : kLogSize;
    if (avail > max - 1)
        avail = max - 1;
    uint64_t start = head - avail;
    for (size_t i = 0; i < avail; i++)
        out[i] = gLog[(start + i) % kLogSize];
    out[avail] = 0;
    // Drop a partial first line after the ring wrapped.
    if (head > kLogSize) {
        size_t nl = 0;
        while (nl < avail && out[nl] != '\n')
            nl++;
        if (nl < avail) {
            size_t skip = nl + 1;
            memmove(out, out + skip, avail - skip + 1);
            avail -= skip;
        }
    }
    return avail;
}
