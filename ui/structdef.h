#pragma pack(push, 1)
#pragma once
struct SingleScreen {
    int id;
    int ImageFormat;
    int w;
    int h;
    char shmName[128];
#ifndef _WIN32
    pthread_mutex_t lock;      /* Linux: 互斥锁对象直接存在这里 */
#else
    HANDLE _internal_DONT_SET_mutexHandle_client;
    HANDLE _internal_DONT_SET_mutexHandle_server;
#endif
    char mutexName[300];

    bool ServerWritingA;
    bool ServerWritingB;
    bool ServerSwitched;
    bool Server_B_Available;
    bool ClientReadingA;
    bool ClientReadingB;
    uint32_t stride;
    uint32_t data_version;    /* 内存重建计数器 */
    uint64_t total_data_size; /* 当前 Data SHM 的总字节数 */
    uint32_t dataA_offset;    /* 始终为 0 */
    uint32_t dataB_offset;    /* 始终为 dataA_size */
    uint32_t dataA_size;
    uint32_t dataB_size;
};

typedef struct SingleScreen SingleScreen;

struct BufferStruct {
    int screen_count;
    SingleScreen screens[];
};
#pragma pack(pop)