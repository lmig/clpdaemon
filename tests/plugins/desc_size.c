/* Prints sizeof(clpd_plugin) of this build, for tests/cli/cli_test.sh. */
#include "clpd_plugin.h"

#include <stdio.h>

int main(void) {
  printf("%zu\n", sizeof(clpd_plugin));
  return 0;
}
