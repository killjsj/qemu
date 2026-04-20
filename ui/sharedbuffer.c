#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log-for-trace.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/surface.h"
#include <stdbool.h>
#include <string.h>
#include "lock.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif

/* 初始默认分辨率，后续随 Guest OS 动态调整 */
#define INIT_WIDTH 640
#define INIT_HEIGHT 480

#include "structdef.h"

struct sb_console {
    DisplayChangeListener dcl;
    QemuConsole *con;
    int idx;
    char id_str[64]; /* 保存命令行传入的 ID */
#ifdef _WIN32
    HANDLE hMapFile_ctrl;
    HANDLE hMapFile_data;
#else
    int shm_fd_ctrl;
    int shm_fd_data;
#endif
    void *mmap_addr_ctrl;
    void *mmap_addr_data;
    char shm_name_ctrl[128];
    char shm_name_data[128];
};

typedef struct sb_console sb_console;
static sb_console *sb_console_ptr;
static struct BufferStruct *global_buffer_header;

static int sb_num_outputs = 0;

static inline void MemsBar(void) {
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}
static void sb_2d_refresh(DisplayChangeListener *dcl) {
    graphic_hw_update(dcl->con);
    DisplaySurface *surf = qemu_console_surface(dcl->con);
    if (!surf) return;

    sb_console *sbc = container_of(dcl, sb_console, dcl);
    uint8_t *base_addr = (uint8_t *)sbc->mmap_addr_data;
    

    if (!base_addr) return;

    int cur_w = surface_width(surf);
    int cur_h = surface_height(surf);

    /* 安全检查：防止 Guest 渲染尺寸瞬间超过我们刚分配的内存 */
    SingleScreen *ss = &global_buffer_header->screens[sbc->idx];
    if (cur_w > ss->w || cur_h > ss->h) return;
    uint8_t *src = surface_data(surf);
    int copy_linesize = cur_w * 4;
    int src_stride = surface_stride(surf);
    if (LockScreen(ss, true, 5)) {
        bool writetob = !ss->Server_B_Available;
        if(writetob && !ss->ClientReadingB){
            ss->ServerWritingB = true;
            writetob = true;
        } else if(!writetob && !ss->ClientReadingA) {
            ss->ServerWritingA = true;
            writetob = false;
        } else{
            if(ss->ClientReadingA){
                ss->ServerWritingB = true;
                writetob = true;
            }else if(ss->ClientReadingB){
                ss->ServerWritingA = true;
                writetob = false;
            }
        }
        uint32_t offset = (writetob) ? ss->dataB_offset : ss->dataA_offset;
        uint8_t *dst = base_addr + offset;
        ss->stride = src_stride;
        ss->ImageFormat = surface_format(surf);
        for (int i = 0; i < cur_h; i++) {
            memcpy(dst + (i * (cur_w * 4)), src + (i * src_stride), copy_linesize);
        }

        if(writetob){
            ss->Server_B_Available = true;
                ss->ServerWritingB = false;
        } else {
            ss->Server_B_Available = false;
                ss->ServerWritingA = false;
        }
        ss->isNewFrame = true;
        UnlockScreen(ss, true);
    }
}

static void sb_realloc_data_shm(sb_console *sbc, int width, int height, int pixman_format) {
    SingleScreen *ss = &global_buffer_header->screens[sbc->idx];
    size_t frame_size = (size_t)width * height * 4;
    size_t new_total_size = frame_size * 2;
    uint32_t next_version = ss->data_version + 1; // 预准备下一个版本号

    /* 1. 准备新的共享内存名字 (使用局部变量，不要直接改 sbc->shm_name_data) */
    char new_name[128];
    snprintf(new_name, sizeof(new_name), 
             "/QemuData_%s_%d_V%u", sbc->id_str, sbc->idx, next_version);
    qemu_log("sharedbuffer: creating new shm: %s\n", new_name);
    void *pNewAddr = NULL;

#ifdef _WIN32

    HANDLE hNewMap = NULL;
    ULARGE_INTEGER liSize;
    liSize.QuadPart = new_total_size;
    // 使用 new_name 而不是未初始化的变量
    hNewMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 
                                 liSize.HighPart, liSize.LowPart, new_name);
    if (hNewMap) {
        pNewAddr = MapViewOfFile(hNewMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    }
#else    
    int hNewfd = -1;
    hNewfd = shm_open(new_name, O_CREAT | O_RDWR, 0666);
    if (hNewfd >= 0) {
        ftruncate(hNewfd, new_total_size);
        // 【修正】这里必须用 hNewfd 而不是旧的 sbc->shm_fd_data
        pNewAddr = mmap(NULL, new_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, hNewfd, 0);
    }
#endif

    if (!pNewAddr) {
        error_report("sharedbuffer: Failed to create new SHM '%s'", new_name);
#ifdef _WIN32
        if (hNewMap) CloseHandle(hNewMap);
#else    
        if (hNewfd >= 0) close(hNewfd);
        shm_unlink(new_name); 
#endif
        return;
    }

    ss->w = width;
    ss->h = height;
    ss->ImageFormat = pixman_format;
    ss->dataA_size = (uint32_t)frame_size;
    ss->dataB_size = (uint32_t)frame_size;
    ss->dataA_offset = 0;
    ss->dataB_offset = (uint32_t)frame_size;
    ss->total_data_size = new_total_size;
    pstrcpy(ss->shmName,sizeof(ss->shmName), new_name);
    // InitLockServer(ss, new_name);
    MemsBar();
    

    /* 4. 销毁旧的句柄/映射，切换到新的 */
#ifdef _WIN32
    if (sbc->mmap_addr_data) UnmapViewOfFile(sbc->mmap_addr_data);
    if (sbc->hMapFile_data) CloseHandle(sbc->hMapFile_data);
    sbc->hMapFile_data = hNewMap;
#else
    if (sbc->mmap_addr_data) {
        // munmap 需要提供长度，这里使用旧的 total_data_size
        munmap(sbc->mmap_addr_data, ss->total_data_size);
    }
    if (sbc->shm_fd_data >= 0) close(sbc->shm_fd_data);
    // 只有在确定不再需要旧名字时才 unlink
    shm_unlink(sbc->shm_name_data); 
    sbc->shm_fd_data = hNewfd;
#endif

    sbc->mmap_addr_data = pNewAddr;
    // 将新名字保存到控制块供下次 unlink 使用
    pstrcpy(sbc->shm_name_data, sizeof(sbc->shm_name_data), new_name);

    qemu_log("sharedbuffer: Switched to %dx%d (Version %u)\n", width, height, ss->data_version);
    ss->data_version = next_version; 
    ss->ServerSwitched = true;
    MemsBar();
}
static void sb_2d_switch(DisplayChangeListener *dcl, DisplaySurface *new_surface) {
    if (!new_surface || !global_buffer_header) return;

    sb_console *sbc = container_of(dcl, sb_console, dcl);
    SingleScreen *ss = &global_buffer_header->screens[sbc->idx];

    int new_w = surface_width(new_surface);
    int new_h = surface_height(new_surface);
    ss->ImageFormat = surface_format(new_surface);

    /* 检查分辨率是否真的变了，防止冗余分配 */
    if (ss->w != new_w || ss->h != new_h) {
        sb_realloc_data_shm(sbc, new_w, new_h,ss->ImageFormat);
    }
    MemsBar();
    sb_2d_refresh(dcl);
}

static const DisplayChangeListenerOps sb_2d_ops = {
    .dpy_name = "sb-2d",
    .dpy_gfx_switch = sb_2d_switch,
    .dpy_refresh = sb_2d_refresh,
};

static void sb_display_init(DisplayState *ds, DisplayOptions *o) {
    int i;
    for (i = 0; ; i++) {
        if (!qemu_console_lookup_by_index(i)) break;
    }
    sb_num_outputs = i;
    if (sb_num_outputs == 0) return;

    sb_console_ptr = g_new0(sb_console, sb_num_outputs);

    /* 1. 创建控制块 (Ctrl) */
    size_t ctrl_shm_size = sizeof(struct BufferStruct) + (sizeof(SingleScreen) * sb_num_outputs);
    char name_ctrl[128];
    snprintf(name_ctrl, sizeof(name_ctrl), "/QemuCtrl_%s", o->u.sharedbuffer.id);
    qemu_log("sharedbuffer: ctrl_shm_size:%zu\n", ctrl_shm_size);
    
#ifdef _WIN32
    ULARGE_INTEGER liSizeCtrl; liSizeCtrl.QuadPart = ctrl_shm_size;
    HANDLE hMapFileCtrl = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 
                                             liSizeCtrl.HighPart, liSizeCtrl.LowPart, name_ctrl);
    if(hMapFileCtrl == NULL){
      error_report("sharedbuffer: Failed to create Ctrl SHM at CreateFileMappingA errno:%lu",GetLastError());
      return;
    }
    global_buffer_header = (struct BufferStruct *)MapViewOfFile(hMapFileCtrl, FILE_MAP_ALL_ACCESS, 0, 0, 0);
#else
    int fd_ctrl = shm_open(name_ctrl, O_CREAT | O_RDWR, 0666);
    if(fd_ctrl == -1){
      error_report("sharedbuffer: Failed to create Ctrl SHM at shm_open errno:%d",errno);
      return;
    }
    ftruncate(fd_ctrl, ctrl_shm_size);
    global_buffer_header = mmap(NULL, ctrl_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_ctrl, 0);
#endif

    if (!global_buffer_header) {
        error_report("sharedbuffer: Failed to create Ctrl SHM");
        return;
    }

    global_buffer_header->screen_count = sb_num_outputs;
    /* 2. 为每个屏幕初始化 DCL 并分配初始 Data 块 */
    for (i = 0; i < sb_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        sb_console *sbc = &sb_console_ptr[i];

        sbc->con = con;
        sbc->idx = i;
        pstrcpy(sbc->id_str, sizeof(sbc->id_str), o->u.sharedbuffer.id);
        sbc->mmap_addr_ctrl = global_buffer_header;
        
        /* 初始化元数据 */
        global_buffer_header->screens[i].id = i;
        global_buffer_header->screens[i].ServerSwitched = false;
        global_buffer_header->screens[i].Server_B_Available = false;
        global_buffer_header->screens[i].ServerWritingA = false;
        global_buffer_header->screens[i].ServerWritingB = false;
        global_buffer_header->screens[i].data_version = 0;
        InitLockServer(&global_buffer_header->screens[i], o->u.sharedbuffer.id);

        /* 分配初始数据内存 (例如 640x480) */
        sb_realloc_data_shm(sbc, INIT_WIDTH, INIT_HEIGHT,PIXMAN_x8r8g8b8);

        if (qemu_console_is_graphic(con)) {
            sbc->dcl.con = con;
            sbc->dcl.ops = &sb_2d_ops;
            register_displaychangelistener(&sbc->dcl);
        }
    }
    qemu_log("sharedbuffer: Initialized with %d screens.\n", sb_num_outputs);
}

static QemuDisplay qemu_display_sharedbuffer = {
    .type = DISPLAY_TYPE_SHAREDBUFFER,
    .init = sb_display_init,
};

static void register_sharedbuffer(void) {
    qemu_display_register(&qemu_display_sharedbuffer);
}

type_init(register_sharedbuffer);
