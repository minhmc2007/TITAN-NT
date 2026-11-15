/* Validates scripts/dso_x18_scan.mjs: built with SCS, it must show x18 prologues. */
int leaf(int x) { return x + 1; }

int caller(int x) { return leaf(x) + 1; }

int main(void) { return caller(1) == 3 ? 0 : 1; }
