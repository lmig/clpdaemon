/* Minimal assertion helper shared by the C unit tests. */
#ifndef CLPD_TEST_EXPECT_H
#define CLPD_TEST_EXPECT_H

#include <stdio.h>

static int failures;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                      \
  } while (0)

#endif
