// main.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <algorithm>
#include <stdlib.h>

#if !defined(_WINDOWS_)
#   define WIN32_LEAN_AND_MEAN
#   include <Windows.h>
#   undef WIN32_LEAN_AND_MEAN
#endif

#include "cit.h"
#include "nadir.h"

void DummyWait(CIT* cit, WaitCallback* callback) {}
void DummyWake(CIT* cit, WakeCallback* callback) {}

int TestSinglePeekPoke() {
    WaitCallback waitPop;
    waitPop.wait = DummyWait;
    WaitCallback waitPush;
    waitPush.wait = DummyWait;
    WakeCallback wakePop;
    wakePop.wake = DummyWake;
    WakeCallback wakePush;
    wakePush.wake = DummyWake;

    CIT* cit = cit_create(10, sizeof(uint32_t), &waitPop, &wakePop, &waitPush, &wakePush);
    uint32_t out_value = 0;
    if (cit_trypop(cit, &out_value)) {
        return 1;
    }
    uint32_t in_value = 0x4711;
    if (!cit_trypush(cit, &in_value)) {
        exit(1);
    }
    if (!cit_trypop(cit, &out_value)) {
        exit(1);
    }
    if (out_value != 0x4711) {
        exit(1);
    }
    if (cit_trypop(cit, &out_value)) {
        exit(1);
    }
    cit_close(cit);
    return 0;
}

static HANDLE popEvent = INVALID_HANDLE_VALUE;
static HANDLE pushEvent = INVALID_HANDLE_VALUE;

void PopEventWait(CIT* cit, WaitCallback* callback) { ::WaitForSingleObject(popEvent, INFINITE); }
void PopEventWake(CIT* cit, WakeCallback* callback) { ::SetEvent(popEvent); }

void PushEventWait(CIT* cit, WaitCallback* callback) { ::WaitForSingleObject(pushEvent, INFINITE); }
void PushEventWake(CIT* cit, WakeCallback* callback) { ::SetEvent(pushEvent); }

int TestMultiPokeNoWait() {

    WaitCallback waitPop;
    waitPop.wait = PopEventWait;
    WaitCallback waitPush;
    waitPush.wait = PushEventWait;
    WakeCallback wakePop;
    wakePop.wake = PopEventWake;
    WakeCallback wakePush;
    wakePush.wake = PushEventWake;

    CIT* cit = cit_create(10, sizeof(uint32_t), &waitPop, &wakePop, &waitPush, &wakePush);
    for (uint32_t i = 100; i < 110; ++i) {
        cit_push(cit, &i);
    }
    uint32_t popped_values[10];
    for (uint32_t i = 0; i < 10; ++i) {
        cit_pop(cit, &popped_values[i]);
    }
    std::sort(&popped_values[0], &popped_values[10]);
    for (uint32_t i = 100; i < 110; ++i) {
        if (popped_values[i - 100] != i) {
            cit_close(cit);
            exit(1);
        }
    }
    cit_close(cit);
    return 0;
}

int32_t TestPokeBlockedPushThread(void* context_data) {
    CIT* cit = (CIT*)context_data;

    uint32_t noWait = 0x8123;
    cit_push(cit, &noWait);

    // Should block:
    uint32_t doWait = 0xadfda;
    cit_push(cit, &doWait);

    // Should release cit_pop(cit, &pop[2]);
    uint32_t blocked = 0x981ac;
    cit_push(cit, &blocked);

    return 0;
}

int32_t TestPokeBlockedPopThread(void* context_data) {
    CIT* cit = (CIT*)context_data;

    // Should release cit_push(cit, &doWait);
    uint32_t pop[3];
    cit_pop(cit, &pop[0]);

    cit_pop(cit, &pop[1]);

    // Should block:
    cit_pop(cit, &pop[2]);

    return 0;
}

int TestPokeBlocked() {

    WaitCallback waitPop;
    waitPop.wait = PopEventWait;
    WaitCallback waitPush;
    waitPush.wait = PushEventWait;
    WakeCallback wakePop;
    wakePop.wake = PopEventWake;
    WakeCallback wakePush;
    wakePush.wake = PushEventWake;

    CIT* cit = cit_create(1, sizeof(uint32_t), &waitPop, &wakePop, &waitPush, &wakePush);

    nadir::HThread thread_1 = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestPokeBlockedPushThread, 0, cit);
    nadir::HThread thread_2 = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestPokeBlockedPopThread, 0, cit);

    nadir::JoinThread(thread_1, nadir::TIMEOUT_INFINITE);
    nadir::JoinThread(thread_2, nadir::TIMEOUT_INFINITE);

    nadir::DeleteThread(thread_2);
    nadir::DeleteThread(thread_1);

    cit_close(cit);
    return 0;
}

static const uint32_t TestLotsOfPopThreadValuesCount = 100000;

int32_t TestLotsOfPushThread(void* context_data) {
    CIT* cit = (CIT*)context_data;

    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount; i++) {
        cit_push(cit, &i);
    }
    return 0;
}

static uint32_t TestLotsOfPopThreadValues[TestLotsOfPopThreadValuesCount];

int32_t TestLotsOfPopThread(void* context_data) {
    CIT* cit = (CIT*)context_data;

    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount; i++) {
        cit_pop(cit, &TestLotsOfPopThreadValues[i]);
    }

    return 0;
}

int TestSyncronization() {

    WaitCallback waitPop;
    waitPop.wait = PopEventWait;
    WaitCallback waitPush;
    waitPush.wait = PushEventWait;
    WakeCallback wakePop;
    wakePop.wake = PopEventWake;
    WakeCallback wakePush;
    wakePush.wake = PushEventWake;

    CIT* cit = cit_create(10, sizeof(uint32_t), &waitPop, &wakePop, &waitPush, &wakePush);

    nadir::HThread thread_1 = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPushThread, 0, cit);
    nadir::HThread thread_2 = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPopThread, 0, cit);

    nadir::JoinThread(thread_1, nadir::TIMEOUT_INFINITE);
    nadir::JoinThread(thread_2, nadir::TIMEOUT_INFINITE);

    nadir::DeleteThread(thread_2);
    nadir::DeleteThread(thread_1);

    std::sort(&TestLotsOfPopThreadValues[0], &TestLotsOfPopThreadValues[TestLotsOfPopThreadValuesCount]);
    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount; i++) {
        if (TestLotsOfPopThreadValues[i] != i) {
            exit(1);
        }
    }

    cit_close(cit);
    return 0;
}

int32_t TestLotsOfPushThreadA(void* context_data) {
    CIT* cit = (CIT*)context_data;

    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount / 2; i++) {
        cit_push(cit, &i);
    }
    return 0;
}

int32_t TestLotsOfPushThreadB(void* context_data) {
    CIT* cit = (CIT*)context_data;

    for (uint32_t i = TestLotsOfPopThreadValuesCount / 2; i < TestLotsOfPopThreadValuesCount; i++) {
        cit_push(cit, &i);
    }
    return 0;
}


int32_t TestLotsOfPopThreadHalf(void* context_data) {
    CIT* cit = (CIT*)context_data;

    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount / 2; i++) {
        uint32_t popped;
        cit_pop(cit, &popped);
        TestLotsOfPopThreadValues[popped]++;
    }

    return 0;
}

int TestMultiProducerMultiConsumer() {

    WaitCallback waitPop;
    waitPop.wait = PopEventWait;
    WaitCallback waitPush;
    waitPush.wait = PushEventWait;
    WakeCallback wakePop;
    wakePop.wake = PopEventWake;
    WakeCallback wakePush;
    wakePush.wake = PushEventWake;

    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount; i++) {
        TestLotsOfPopThreadValues[i] = 0;
    }

    CIT* cit = cit_create(10, sizeof(uint32_t), &waitPop, &wakePop, &waitPush, &wakePush);

    nadir::HThread thread_1_a = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPushThreadA, 0, cit);
    nadir::HThread thread_1_b = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPushThreadB, 0, cit);
    nadir::HThread thread_2_a = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPopThreadHalf, 0, cit);
    nadir::HThread thread_2_b = nadir::CreateThread(malloc(nadir::GetThreadSize()), TestLotsOfPopThreadHalf, 0, cit);

    nadir::JoinThread(thread_1_a, nadir::TIMEOUT_INFINITE);
    nadir::JoinThread(thread_1_b, nadir::TIMEOUT_INFINITE);
    nadir::JoinThread(thread_2_a, nadir::TIMEOUT_INFINITE);
    nadir::JoinThread(thread_2_b, nadir::TIMEOUT_INFINITE);

    nadir::DeleteThread(thread_2_a);
    nadir::DeleteThread(thread_2_b);
    nadir::DeleteThread(thread_1_a);
    nadir::DeleteThread(thread_1_b);

    std::sort(&TestLotsOfPopThreadValues[0], &TestLotsOfPopThreadValues[TestLotsOfPopThreadValuesCount]);
    for (uint32_t i = 0; i < TestLotsOfPopThreadValuesCount; i++) {
        if (TestLotsOfPopThreadValues[i] != 1) {
            exit(1);
        }
    }

    cit_close(cit);
    return 0;
}


int main() {
    popEvent = ::CreateEvent(0, FALSE, FALSE, 0);
    pushEvent = ::CreateEvent(0, FALSE, FALSE, 0);

    ::WaitForSingleObject(popEvent, 0);
    ::WaitForSingleObject(pushEvent, 0);
    if (0 != TestSinglePeekPoke()) {
        exit(1);
    }

    ::WaitForSingleObject(popEvent, 0);
    ::WaitForSingleObject(pushEvent, 0);
    if (0 != TestMultiPokeNoWait()) {
        exit(1);
    }

    ::WaitForSingleObject(popEvent, 0);
    ::WaitForSingleObject(pushEvent, 0);
    if (0 != TestPokeBlocked()) {
        exit(1);
    }

    ::WaitForSingleObject(popEvent, 0);
    ::WaitForSingleObject(pushEvent, 0);
    if (0 != TestSyncronization()) {
        exit(1);
    }

    ::WaitForSingleObject(popEvent, 0);
    ::WaitForSingleObject(pushEvent, 0);
    if (0 != TestMultiProducerMultiConsumer()) {
        exit(1);
    }

    ::CloseHandle(pushEvent);
    ::CloseHandle(popEvent);
    return 0;
}
