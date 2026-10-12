//
//  MtkLog.hpp
//  SimpleMtkWlan
//
//  Driver log ring buffer. Every line still goes to the kernel log, and the most
//  recent ones are kept so they can be published in the I/O Registry: the macOS
//  installer has no usable `log show`, and early boot messages get pushed out of
//  the kernel message buffer.
//

#ifndef MtkLog_hpp
#define MtkLog_hpp

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void mtk_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mtk_vlog(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));

/*
 * Number of upcoming scan cycles to log in full. Idle STA mode rescans every
 * few seconds forever, so per-scan lines are only logged while this is
 * positive: the first scans after boot and the scans after a join request.
 */
extern volatile int mtk_verbose_scans;

/* Copies the buffered log, oldest line first, NUL-terminated. Returns the length. */
size_t mtk_log_snapshot(char *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* MtkLog_hpp */
