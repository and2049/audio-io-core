#ifndef AUDIO_ENGINE_UNITTESTS_CHECK_H
#define AUDIO_ENGINE_UNITTESTS_CHECK_H

#include <cstdio>
#include <cstdlib>

#define AE_CHECK(condition)                                                                        \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);           \
      std::exit(1);                                                                                \
    }                                                                                              \
  } while (false)

#endif
