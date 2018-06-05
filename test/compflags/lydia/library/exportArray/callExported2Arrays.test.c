#include <stdio.h>

#include "exportFuncWith2Arrays.h"

extern void chpl_library_init(int argc, char* argv[]);
extern void chpl_library_finalize(void);

// Test of calling an exported function that takes an array
int main(int argc, char* argv[]) {
  // Initialize the Chapel runtime and standard modules
  chpl_library_init(argc, argv);

  // Call the function
  int64_t x[5] = {1, 2, 3, 4, 5};
  int64_t y[5] = {2, 3, 4, 5, 6};
  foo(x, 5, y, 5);
  for (int i = 0; i < 5; i++) {
    printf("Element[%d] = %lld\n", i, x[i]);
  }

  // Shutdown the Chapel runtime and standard modules
  chpl_library_finalize();

  return 0;
}
