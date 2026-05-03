#pragma once
#pragma pack(push, 1)
enum InputEventType {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY,
    INPUT_EVENT_MOUSE_BUTTON_STATE,   // 按钮状态整体更新
    INPUT_EVENT_MOUSE_MOVE,           // 绝对移动（含 Godot 视口尺寸）
    INPUT_EVENT_MOUSE_WHEEL
};

typedef struct GodotInputEvent {
    uint32_t type;          // InputEventType
    int32_t  console_index; // 关联的屏幕索引（支持多头显示）

    // 键盘
    uint32_t keycode;       // QKeyCode 枚举
    bool     pressed;

    // 鼠标按钮状态
    uint32_t button_state;  // 位掩码，每位对应 INPUT_BUTTON_*
    int32_t  delta_z;


    // 鼠标绝对坐标（Godot 视口内的像素位置）
    int32_t  mouse_x;
    int32_t  mouse_y;

    // Godot 视口的实际宽高（用于映射到 guest 显示区域）
    int32_t  godot_w;
    int32_t  godot_h;
    // 滚轮增量（保留，可后续扩展）
    int32_t  wheel_delta;
} GodotInputEvent;

struct screen_buffer_meta{
    bool Write_Available;
    bool ClientReading;
	bool ServerWriting;
	bool isNewFrame;
    bool isPartialUpdate;
    int lastChangedX;
    int lastChangedY;
    int lastChangedW;
    int lastChangedH;
    uint32_t data_offset;    /* 始终为 dataA_size */
    uint32_t data_size;
};
typedef struct screen_buffer_meta screen_buffer_meta;
struct SingleScreen {
    bool isNotGraphic;
    bool ServerSwitched;
    bool isDirty;
    bool next_write_to_b;
    int id;
    int ImageFormat;
    int w;
    int h;
    char shmName[128];
    char mutexName[128];
#ifndef _WIN32
    pthread_mutex_t lock;      /* Linux: 互斥锁对象直接存在这里 */
#else
    HANDLE _internal_DONT_SET_mutexHandle_client;
    HANDLE _internal_DONT_SET_mutexHandle_server;
#endif
    uint32_t stride;
    uint32_t data_version;    /* 内存重建计数器 */
    screen_buffer_meta metaA;
    screen_buffer_meta metaB;
    uint64_t total_data_size; /* 当前 Data SHM 的总字节数 */
};

typedef struct SingleScreen SingleScreen;
#define INPUT_RING_SIZE 2048
struct BufferStruct {
    int screen_count;
    char input_sem_name[128];
    uint32_t input_write_idx;
    uint32_t input_read_idx;
    struct GodotInputEvent input_events[INPUT_RING_SIZE];
    SingleScreen screens[];

};
#pragma pack(pop)
