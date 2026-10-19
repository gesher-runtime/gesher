// Minimal stand-in for googletest's gtest_prod.h, which AOSP headers include
// only for FRIEND_TEST. Gesher's production build does not link googletest.
#pragma once

#define FRIEND_TEST(test_case_name, test_name) \
  friend class test_case_name##_##test_name##_Test
