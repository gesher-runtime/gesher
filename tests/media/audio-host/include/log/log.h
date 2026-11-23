// Explicit logging boundary for actual AOSP audio provider host tests.
#pragma once
#include <stdint.h>
#include <stdio.h>
#define ALOGE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ALOGV(...) ((void)0)
#define ALOGD(...) ((void)0)
