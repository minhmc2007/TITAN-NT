/* Validates scripts/dso_x18_scan.mjs: built with SCS, it must show x18 prologues. */
__attribute__((noinline)) int leaf(int x) { return x + 1; }

__attribute__((noinline)) int caller(int x) { return leaf(x) + 1; }

volatile int sink;

int main(void) {
  sink = caller(1);
  return sink == 3 ? 0 : 1;
}
