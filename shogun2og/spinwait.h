#pragma once
#include <windows.h>

// A helper spins for its next job before sleeping. A count of pause rounds lasts as long as the CPU makes it, so a short
// spin is bounded in time instead; the clock is read every SPIN_CHECK_ROUNDS pauses.
#define SPIN_CHECK_ROUNDS 32

static LONGLONG spin_ticks_per_us;

static inline LONGLONG spin_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static inline LONGLONG spin_deadline(LONG microseconds) {
    if (!spin_ticks_per_us) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        spin_ticks_per_us = frequency.QuadPart >= 1000000 ? frequency.QuadPart / 1000000 : 1;
    }
    return spin_now() + microseconds * spin_ticks_per_us;
}

static inline BOOL spin_expired(int round, LONGLONG deadline) {
    return round % SPIN_CHECK_ROUNDS == SPIN_CHECK_ROUNDS - 1 && spin_now() >= deadline;
}
