#include "titan_log.h"
#include "titan_sys.h"

static size_t titan_strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

void titan_buf_init(struct titan_buf *b, char *storage, size_t cap) {
  b->data = storage;
  b->cap = cap;
  b->len = 0;
  b->truncated = 0;
  if (cap)
    storage[0] = '\0';
}

static void titan_buf_putc(struct titan_buf *b, char c) {
  if (b->len + 1 >= b->cap) {
    b->truncated = 1;
    return;
  }
  b->data[b->len++] = c;
  b->data[b->len] = '\0';
}

void titan_buf_str(struct titan_buf *b, const char *s) {
  while (*s)
    titan_buf_putc(b, *s++);
}

void titan_buf_u64(struct titan_buf *b, uint64_t v) {
  char tmp[24];
  size_t n = 0;
  if (v == 0) {
    titan_buf_putc(b, '0');
    return;
  }
  while (v) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n)
    titan_buf_putc(b, tmp[--n]);
}

void titan_buf_i64(struct titan_buf *b, int64_t v) {
  if (v < 0) {
    titan_buf_putc(b, '-');
    titan_buf_u64(b, (uint64_t)(-v));
  } else {
    titan_buf_u64(b, (uint64_t)v);
  }
}

void titan_buf_hex(struct titan_buf *b, uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  char tmp[16];
  size_t n = 0;
  titan_buf_str(b, "0x");
  if (v == 0) {
    titan_buf_putc(b, '0');
    return;
  }
  while (v) {
    tmp[n++] = digits[v & 0xf];
    v >>= 4;
  }
  while (n)
    titan_buf_putc(b, tmp[--n]);
}

int titan_buf_emit(struct titan_buf *b) {
  if (b->truncated)
    return -1;
  titan_buf_putc(b, '\n');
  if (b->truncated)
    return -1;
  long n = titan_write(1, b->data, b->len);
  return n == (long)b->len ? 0 : -1;
}

void titan_write_str(int fd, const char *s) {
  titan_write(fd, s, titan_strlen(s));
}

void titan_write_u64(int fd, uint64_t v) {
  struct titan_buf b;
  char storage[32];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_u64(&b, v);
  titan_buf_putc(&b, '\n');
  titan_write(fd, b.data, b.len);
}

void titan_fatal(const char *msg) {
  titan_write_str(2, "titan_fatal: ");
  titan_write_str(2, msg);
  titan_write_str(2, "\n");
  titan_exit_group(4);
  for (;;)
    ;
}
