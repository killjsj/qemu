// lock.h
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include "structdef.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <errno.h>
#endif

// --- Server 端初始化 ---
static void InitLockServer(struct SingleScreen *ss, const char* name_prefix) {
#ifdef _WIN32
    // 生成唯一名称并存入 SHM
    snprintf(ss->mutexName, sizeof(ss->mutexName), "Global\\Mtx_%s_%d", name_prefix, ss->id);
    // 创建锁，存入 Server 专用句柄位
    ss->_internal_DONT_SET_mutexHandle_server = CreateMutexA(NULL, FALSE, ss->mutexName);
#else
    // Linux: 直接在 SHM 中初始化 pthread mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&ss->lock, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

// --- Client 端初始化 ---
__attribute__((unused)) static void InitLockClient(struct SingleScreen *ss) {
#ifdef _WIN32
    // 根据 SHM 里的名称打开锁，存入 Client 专用句柄位
    ss->_internal_DONT_SET_mutexHandle_client = CreateMutexA(NULL, FALSE, ss->mutexName);
#else
    // Linux 不需要额外操作，直接使用 ss->lock
#endif
}

// --- 加锁 (Server 用 server 句柄，Client 用 client 句柄) ---
static bool LockScreen(struct SingleScreen *ss, bool is_server, int timeout_ms) {
#ifdef _WIN32
    HANDLE h = is_server ? ss->_internal_DONT_SET_mutexHandle_server : ss->_internal_DONT_SET_mutexHandle_client;
    if (!h) return false;
    DWORD res = WaitForSingleObject(h, timeout_ms);
    return (res == WAIT_OBJECT_0 || res == WAIT_ABANDONED);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    int ret = pthread_mutex_timedlock(&ss->lock, &ts);
    if (ret == EOWNERDEAD) {
        pthread_mutex_consistent(&ss->lock);
        return true;
    }
    return ret == 0;
#endif
}

// --- 释放锁 ---
static void UnlockScreen(struct SingleScreen *ss, bool is_server) {
#ifdef _WIN32
    HANDLE h = is_server ? ss->_internal_DONT_SET_mutexHandle_server : ss->_internal_DONT_SET_mutexHandle_client;
    if (h) ReleaseMutex(h);
#else
    pthread_mutex_unlock(&ss->lock);
#endif
}
