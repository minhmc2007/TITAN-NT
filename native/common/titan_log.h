#ifndef TITAN_LOG_H
#define TITAN_LOG_H

#include <stddef.h>
#include <stdint.h>

struct titan_buf {
  char *data;
  size_t cap;
  size_t len;
  int truncated;
};

#ifdef __cplusplus
extern "C" {
#endif

void titan_buf_init(struct titan_buf *b, char *storage, size_t cap);
void titan_buf_str(struct titan_buf *b, const char *s);
void titan_buf_u64(struct titan_buf *b, uint64_t v);
void titan_buf_i64(struct titan_buf *b, int64_t v);
void titan_buf_hex(struct titan_buf *b, uint64_t v);
int titan_buf_emit(struct titan_buf *b);

void titan_fatal(const char *msg);
void titan_write_str(int fd, const char *s);
void titan_write_u64(int fd, uint64_t v);

#ifdef __cplusplus
}
#endif

#endif
