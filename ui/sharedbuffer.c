#include "qemu/osdep.h"

#include "lock.h"
#include "qapi/qapi-types-ui.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h" // 推荐使用 QEMU 原子宏
#ifdef _WIN32
// 模拟 __atomic_load_n(p, __ATOMIC_ACQUIRE)
static inline uint32_t atomic_load_acquire(const uint32_t *ptr) {
  return *(const volatile uint32_t *)ptr;
}

// 原子写 + 释放语义（使用 InterlockedExchange 自带全屏障）
static inline void atomic_store_release(uint32_t *ptr, uint32_t val) {
  InterlockedExchange((volatile LONG *)ptr, (LONG)val);
}
#else
// Linux 下继续使用 GCC 内建函数（原实现即可）
static inline uint32_t atomic_load_acquire(const uint32_t *ptr) {
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_release(uint32_t *ptr, uint32_t val) {
  __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}
#endif
#include "qemu/log-for-trace.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/surface.h"
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <string.h>

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
static void *hSem;
static bool graphics_ready = false;

static QEMUBH *sb_input_bh;

/* * 这是 BH 处理函数，由 QEMU 主线程调用。
 * 注意：在这里执行时，BQL 已经被主线程持有，无需手动加锁！
 */
static void sb_input_bh_handler(void *opaque) {
  struct BufferStruct *bs = global_buffer_header;
  if (!bs)
    return;

  uint32_t writer = atomic_load_acquire(&bs->input_write_idx);
  uint32_t reader = atomic_load_acquire(&bs->input_read_idx);
  uint32_t pending = (writer - reader) & (INPUT_RING_SIZE - 1);
  if (pending > 10) {
    qemu_log("Warning: Input lag detected, pending events: %u\n", pending);
  }
  while (reader != writer) {
    GodotInputEvent *ev = &bs->input_events[reader & (INPUT_RING_SIZE - 1)];
    QemuConsole *con = qemu_console_lookup_by_index(ev->console_index);

    switch (ev->type) {

    case INPUT_EVENT_KEY:
      qemu_input_event_send_key_qcode(NULL, (QKeyCode)ev->keycode, ev->pressed);
      qemu_input_event_sync();
      break;

    case INPUT_EVENT_MOUSE_BUTTON_STATE: {
      if (con) {
        static uint32_t bmap[INPUT_BUTTON__MAX] = {
            [INPUT_BUTTON_LEFT] = 1 << INPUT_BUTTON_LEFT,
            [INPUT_BUTTON_MIDDLE] = 1 << INPUT_BUTTON_MIDDLE,
            [INPUT_BUTTON_RIGHT] = 1 << INPUT_BUTTON_RIGHT,
        };
        static uint32_t prev_state = 0;
        if (prev_state != ev->button_state) {
          qemu_input_update_buttons(con, bmap, prev_state, ev->button_state);
          prev_state = ev->button_state;
          qemu_input_event_sync();

        }
      }
      break;
    }

    case INPUT_EVENT_MOUSE_MOVE: {
      if (ev->godot_w > 0 && ev->godot_h > 0) {
        int x = (int64_t)ev->mouse_x * 0x7FFF / ev->godot_w;
        int y = (int64_t)ev->mouse_y * 0x7FFF / ev->godot_h;
        x = CLAMP(x, 0, 0x7FFF);
        y = CLAMP(y, 0, 0x7FFF);
        qemu_input_queue_abs(con, INPUT_AXIS_X, x, 0, 0x7FFF);
        qemu_input_queue_abs(con, INPUT_AXIS_Y, y, 0, 0x7FFF);
        qemu_input_event_sync();
      }
      break;
    }

    case INPUT_EVENT_MOUSE_WHEEL: {
      int idx = ev->console_index;
      if (con && ev->wheel_delta != 0) {
        InputButton btn = (ev->wheel_delta > 0) ? INPUT_BUTTON_WHEEL_UP
                                                : INPUT_BUTTON_WHEEL_DOWN;
        qemu_input_queue_btn(con, btn, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(con, btn, false);
        qemu_input_event_sync();
      }
      break;
    }
    }

    reader++;
  }
  atomic_store_release(&bs->input_read_idx, reader);
}

#ifdef _WIN32
static DWORD WINAPI sb_input_thread(LPVOID lpParam)
#else
static void *sb_input_thread(void *arg)
#endif
{
  if (!global_buffer_header) {
#ifndef _WIN32
    return NULL;
#else
    return 1;
#endif
  }

#ifdef _WIN32
  HANDLE sem_handle = (HANDLE)hSem;
#else
  sem_t *sem_handle = (sem_t *)hSem;
#endif

  qemu_log("Input thread launched. Waiting for events...\n");

  while (1) {
    // 【核心修改】：真正的阻塞等待，不再消耗 CPU
#ifdef _WIN32
    WaitForSingleObject(sem_handle, INFINITE);
#else
    // 处理可能被信号中断的情况
    while (sem_wait(sem_handle) == -1 && errno == EINTR) {
      continue;
    }
#endif
    // 走到这里，说明外部程序（如 Godot）写入了输入事件并释放了信号量
    // 直接调度 BH，将任务转交给 QEMU 主线程处理
    if (sb_input_bh) {
      qemu_bh_schedule(sb_input_bh);
    }
  }

#ifndef _WIN32
  return NULL;
#else
  return 0;
#endif
}
static void MemCpyScn(sb_console *sbc, DisplaySurface *surf) {
  uint8_t *base_addr = (uint8_t *)sbc->mmap_addr_data;

  if (!base_addr)
    return;

  int cur_w = surface_width(surf);
  int cur_h = surface_height(surf);

  /* 安全检查：防止 Guest 渲染尺寸瞬间超过我们刚分配的内存 */
  SingleScreen *ss = &global_buffer_header->screens[sbc->idx];
  if (cur_w > ss->w || cur_h > ss->h)
    return;
  uint8_t *src = surface_data(surf);
  int copy_linesize = cur_w * 4;
  int src_stride = surface_stride(surf);
  if (LockScreen(ss, true, 5)) {
    bool next_write_to_b = !ss->next_write_to_b;

    // 【关键修正】：检查客户端是否正在读我们要写的那个缓冲区
    if (next_write_to_b && ss->metaB.ClientReading) {
      // 客户端在读 B，我们不能写 B。强制写 A（即覆盖旧帧）
      next_write_to_b = false;
    } else if (!next_write_to_b && ss->metaA.ClientReading) {
      // 客户端在读 A，我们不能写 A。强制写 B
      next_write_to_b = true;
    }

    // 标记服务端正在写入
    if (next_write_to_b)
      ss->metaB.ServerWriting = true;
    else
      ss->metaA.ServerWriting = true;
    uint32_t active_offset =
        next_write_to_b ? ss->metaB.data_offset : ss->metaA.data_offset;
    UnlockScreen(ss, true);
    uint8_t *dst = base_addr + active_offset;
    ss->stride = src_stride;
    ss->ImageFormat = surface_format(surf);
    if (src_stride == copy_linesize) {
      memcpy(dst, src, cur_h * copy_linesize); // 一次性大块拷贝
    } else {
      for (int i = 0; i < cur_h; i++) {
        memcpy(dst + (i * copy_linesize), src + (i * src_stride),
               copy_linesize);
      }
    }
    if (LockScreen(ss, true, 100)) {
      ss->next_write_to_b = next_write_to_b; // 只有写完了才翻转可用标记
      ss->metaB.ServerWriting = false;
      ss->metaA.ServerWriting = false;
      if (next_write_to_b) {
        ss->metaB.isNewFrame = true; // 告诉客户端：B区现在有新数据了
      } else {
        ss->metaA.isNewFrame = true; // 告诉客户端：A区现在有新数据了
      }
      UnlockScreen(ss, true);
    }
  }
}
static void MemCpyScn_Area(sb_console *sbc, DisplaySurface *surf, int x, int y,
                           int w, int h) {
  uint8_t *base_addr = (uint8_t *)sbc->mmap_addr_data;
  SingleScreen *ss = &global_buffer_header->screens[sbc->idx];

  if (!base_addr || !surf)
    return;

  // 1. 快速决策写哪个块
  if (!LockScreen(ss, true, 2))
    return;
  bool to_b = !ss->next_write_to_b;
  // 检查 Godot 是否正在读我们要写的那个区，如果是，就强制写另一个
  if (to_b && ss->metaB.ClientReading)
    to_b = false;
  else if (!to_b && ss->metaA.ClientReading)
    to_b = true;

  if (to_b)
    ss->metaB.ServerWriting = true;
  else
    ss->metaA.ServerWriting = true;
  uint32_t offset = to_b ? ss->metaB.data_offset : ss->metaA.data_offset;
  UnlockScreen(ss, true);

  // 2. 【核心优化】只拷贝变动区域 (Dirty Rect)
  uint8_t *src_data = surface_data(surf);
  uint8_t *dst_data = base_addr + offset;
  int stride = surface_stride(surf); // 每行字节数
  int bpp = 4;                       // RGBA

  for (int i = 0; i < h; i++) {
    // 计算每一行的起始偏移： (y + 当前行) * 一行长度 + (x * 4字节)
    int line_offset = (y + i) * stride + (x * bpp);
    memcpy(dst_data + line_offset, src_data + line_offset, w * bpp);
  }

  if (LockScreen(ss, true, 5)) {
    ss->next_write_to_b = to_b; // 只有这里改了，客户端才会看到“新”缓冲区
    ss->metaB.ServerWriting = false;
    ss->metaA.ServerWriting = false;
    if (to_b) {
      ss->metaB.isNewFrame = true; // 告诉客户端：B区现在有新数据了
    } else {
      ss->metaA.isNewFrame = true; // 告诉客户端：A区现在有新数据了
    }
    UnlockScreen(ss, true);
  }
}

// 修改 update 回调
static void sb_2d_update(DisplayChangeListener *dcl, int x, int y, int w,
                         int h) {
  if (!qemu_console_is_graphic(dcl->con)) {
    return;
  }
  sb_console *sbc = container_of(dcl, sb_console, dcl);

  DisplaySurface *surf = qemu_console_surface(dcl->con);
  // 只拷贝变动的 x,y,w,h 区域
  MemCpyScn_Area(sbc, surf, x, y, w, h);
}
static void sb_2d_refresh(DisplayChangeListener *dcl) {
  graphic_hw_update(dcl->con);
  if (!qemu_console_is_graphic(dcl->con)) {
    return;
  }
  DisplaySurface *surf = qemu_console_surface(dcl->con);
  if (!surf)
    return;
  // 3) 创建信号量（初始计数为0，无事件时阻塞）
  if (!graphics_ready) {
    graphics_ready = true;
    qemu_log(
        "sharedbuffer: Graphics ready, input events will now be processed.\n");
    sb_input_bh = qemu_bh_new(sb_input_bh_handler, NULL);
#ifdef _WIN32
    HANDLE hThread = CreateThread(NULL, 0, sb_input_thread, NULL, 0, NULL);
    if (hThread)
      CloseHandle(hThread);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, sb_input_thread, NULL);
    pthread_detach(tid);
#endif
  }
  MemCpyScn(container_of(dcl, sb_console, dcl), surf);
}
static void sb_realloc_data_shm(sb_console *sbc, int width, int height,
                                int pixman_format) {
  SingleScreen *ss = &global_buffer_header->screens[sbc->idx];
  size_t frame_size = (size_t)width * height * 4;
  size_t new_total_size = frame_size * 2;
  uint32_t next_version = ss->data_version + 1; // 预准备下一个版本号

  /* 1. 准备新的共享内存名字 (使用局部变量，不要直接改 sbc->shm_name_data) */
  char new_name[128];
  snprintf(new_name, sizeof(new_name), "/QemuData_%s_%d_V%u", sbc->id_str,
           sbc->idx, next_version);
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
    pNewAddr = mmap(NULL, new_total_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    hNewfd, 0);
  }
#endif

  if (!pNewAddr) {
    error_report("sharedbuffer: Failed to create new SHM '%s'", new_name);
#ifdef _WIN32
    if (hNewMap)
      CloseHandle(hNewMap);
#else
    if (hNewfd >= 0)
      close(hNewfd);
    shm_unlink(new_name);
#endif
    return;
  }

  ss->w = width;
  ss->h = height;
  ss->ImageFormat = pixman_format;
  ss->metaA.data_size = (uint32_t)frame_size;
  ss->metaB.data_size = (uint32_t)frame_size;
  ss->metaA.data_offset = 0;
  ss->metaB.data_offset = (uint32_t)frame_size;
  ss->total_data_size = new_total_size;
  pstrcpy(ss->shmName, sizeof(ss->shmName), new_name);
  // InitLockServer(ss, new_name);
  MemsBar();

  /* 4. 销毁旧的句柄/映射，切换到新的 */
#ifdef _WIN32
  if (sbc->mmap_addr_data)
    UnmapViewOfFile(sbc->mmap_addr_data);
  if (sbc->hMapFile_data)
    CloseHandle(sbc->hMapFile_data);
  sbc->hMapFile_data = hNewMap;
#else
  if (sbc->mmap_addr_data) {
    // munmap 需要提供长度，这里使用旧的 total_data_size
    munmap(sbc->mmap_addr_data, ss->total_data_size);
  }
  if (sbc->shm_fd_data >= 0)
    close(sbc->shm_fd_data);
  // 只有在确定不再需要旧名字时才 unlink
  shm_unlink(sbc->shm_name_data);
  sbc->shm_fd_data = hNewfd;
#endif

  sbc->mmap_addr_data = pNewAddr;
  // 将新名字保存到控制块供下次 unlink 使用
  pstrcpy(sbc->shm_name_data, sizeof(sbc->shm_name_data), new_name);

  qemu_log("sharedbuffer: Switched to %dx%d (Version %u)\n", width, height,
           ss->data_version);
  ss->data_version = next_version;
  ss->ServerSwitched = true;
  MemsBar();
}
static void sb_2d_switch(DisplayChangeListener *dcl,
                         DisplaySurface *new_surface) {
  if (!new_surface || !global_buffer_header)
    return;

  sb_console *sbc = container_of(dcl, sb_console, dcl);
  SingleScreen *ss = &global_buffer_header->screens[sbc->idx];

  int new_w = surface_width(new_surface);
  int new_h = surface_height(new_surface);
  ss->ImageFormat = surface_format(new_surface);

  /* 检查分辨率是否真的变了，防止冗余分配 */
  if (ss->w != new_w || ss->h != new_h) {
    sb_realloc_data_shm(sbc, new_w, new_h, ss->ImageFormat);
  }
  MemsBar();
  sb_2d_refresh(dcl);
}
static const DisplayChangeListenerOps sb_2d_ops = {
    .dpy_name = "sb-2d",
    .dpy_gfx_switch = sb_2d_switch,
    .dpy_refresh = sb_2d_refresh,
    .dpy_gfx_update = sb_2d_update,
};

static void sb_display_init(DisplayState *ds, DisplayOptions *o) {
  int i;
  for (i = 0;; i++) {
    QemuConsole *con = qemu_console_lookup_by_index(i);
    if (!con)
      break;
  }
  sb_num_outputs = i;
  if (sb_num_outputs == 0)
    return;

  sb_console_ptr = g_new0(sb_console, sb_num_outputs);

  /* 1. 创建控制块 (Ctrl) */
  size_t ctrl_shm_size =
      sizeof(struct BufferStruct) + (sizeof(SingleScreen) * sb_num_outputs);
  char name_ctrl[128];
  snprintf(name_ctrl, sizeof(name_ctrl), "/QemuCtrl_%s", o->u.sharedbuffer.id);
  qemu_log("sharedbuffer: ctrl_shm_size:%zu\n", ctrl_shm_size);

#ifdef _WIN32
  ULARGE_INTEGER liSizeCtrl;
  liSizeCtrl.QuadPart = ctrl_shm_size;
  HANDLE hMapFileCtrl =
      CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                         liSizeCtrl.HighPart, liSizeCtrl.LowPart, name_ctrl);
  if (hMapFileCtrl == NULL) {
    error_report("sharedbuffer: Failed to create Ctrl SHM at "
                 "CreateFileMappingA errno:%lu",
                 GetLastError());
    return;
  }
  global_buffer_header = (struct BufferStruct *)MapViewOfFile(
      hMapFileCtrl, FILE_MAP_ALL_ACCESS, 0, 0, 0);
#else
  int fd_ctrl = shm_open(name_ctrl, O_CREAT | O_RDWR, 0666);
  if (fd_ctrl == -1) {
    error_report("sharedbuffer: Failed to create Ctrl SHM at shm_open errno:%d",
                 errno);
    return;
  }
  ftruncate(fd_ctrl, ctrl_shm_size);
  global_buffer_header =
      mmap(NULL, ctrl_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_ctrl, 0);
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
    global_buffer_header->screens[i].isNotGraphic = !qemu_console_is_graphic(con);
    /* 初始化元数据 */
    global_buffer_header->screens[i].id = i;
    if(global_buffer_header->screens[i].isNotGraphic){
        continue;
    }
    global_buffer_header->screens[i].ServerSwitched = false;
    global_buffer_header->screens[i].next_write_to_b = false;
    global_buffer_header->screens[i].metaA.Write_Available = false;
    global_buffer_header->screens[i].metaB.Write_Available = false;
    global_buffer_header->screens[i].metaA.ServerWriting = false;
    global_buffer_header->screens[i].metaB.ServerWriting = false;
    global_buffer_header->screens[i].data_version = 0;
    InitLockServer(&global_buffer_header->screens[i], o->u.sharedbuffer.id);

    /* 分配初始数据内存 (例如 640x480) */
    sb_realloc_data_shm(sbc, INIT_WIDTH, INIT_HEIGHT, PIXMAN_x8r8g8b8);

    if (qemu_console_is_graphic(con)) {
      sbc->dcl.con = con;
      sbc->dcl.ops = &sb_2d_ops;
      register_displaychangelistener(&sbc->dcl);
    }
  }
  char sem_name[128];

#ifdef _WIN32
  snprintf(sem_name, sizeof(sem_name), "Global\\Sem_%s", o->u.sharedbuffer.id);
  hSem = CreateSemaphoreA(NULL, 0, INPUT_RING_SIZE, sem_name);
#else
  // Linux 命名信号量必须以 '/' 开头
  snprintf(sem_name, sizeof(sem_name), "/Sem_%s", o->u.sharedbuffer.id);

  // O_CREAT: 如果不存在则创建
  // 0666: 权限设置
  // 0: 初始计数
  hSem = sem_open(sem_name, O_CREAT, 0666, 0);

  if (hSem == SEM_FAILED) {
    error_report("sharedbuffer: sem_open failed, errno:%d", errno);
    hSem = NULL;
  }
#endif
  pstrcpy(global_buffer_header->input_sem_name,
          sizeof(global_buffer_header->input_sem_name), sem_name);
  global_buffer_header->input_write_idx = 0;
  global_buffer_header->input_read_idx = 0;
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
