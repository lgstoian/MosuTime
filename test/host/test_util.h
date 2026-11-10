/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Shared check counters for the host test files.  Exactly one translation
 * unit (test_core.c) defines them; the test suites live in several files.
 */
#pragma once

#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_failures;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

#define EXPECT_STR(a, b)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (strcmp((a), (b)) != 0) {                                          \
            printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), \
                   (b));                                                      \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

void test_session(void);
void test_config_extra(void);
