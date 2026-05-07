# rivers_timer

`rivers_timer` 是一个从 `rivers_osal` 的 timer 模块思路中独立出来的 **standalone SysTick 时间模块**。

它适合用于小型裸机工程，提供统一的系统时间基准、忙等延时和软件定时器能力。模块只包含两个文件：

```text
rivers_timer.h
rivers_timer.c
```

模块不依赖 `rivers_osal` 原有的 `task / queue / mem / irq / cortexm` 等模块，也不依赖 HAL、CMSIS 或芯片厂商 SDK 头文件。SysTick 寄存器地址、SysTick 位定义和极简临界区实现都在模块内部完成。

---

## 1. 功能特性

`rivers_timer` 当前提供三类能力：

```text
rivers_timer
│
├── 系统时基
│   ├── rivers_timer_init()
│   ├── rivers_timer_tick_handler()
│   ├── rivers_timer_get_tick()
│   ├── rivers_timer_get_uptime_ms()
│   └── rivers_timer_get_uptime_us()
│
├── 忙等延时
│   ├── rivers_timer_delay_ms()
│   └── rivers_timer_delay_us()
│
└── 软件定时器
    ├── rivers_sw_timer_create()
    ├── rivers_sw_timer_start()
    ├── rivers_sw_timer_stop()
    ├── rivers_sw_timer_delete()
    ├── rivers_sw_timer_set_period()
    ├── rivers_sw_timer_set_remaining()
    └── rivers_timer_poll()
```

其中：

- `SysTick` 负责提供系统时间基准。
- `rivers_timer_tick_handler()` 只更新时间计数，不执行软件定时器回调。
- 软件定时器采用 `poll` 模型，由 `rivers_timer_poll()` 在主循环或任务态执行回调。
- 软件定时器使用 **ms 作为时间单位**。
- `rivers_timer_get_uptime_us()` 和 `rivers_timer_delay_us()` 保留微秒级基础时间能力。

---

## 2. 设计定位

这个模块的定位不是完整 RTOS，也不是任务调度器，而是一个轻量级时间基础设施。

它适合：

- 裸机工程中的统一毫秒 tick
- 替代简单的 `HAL_GetTick()` / `HAL_Delay()` 使用场景
- 获取系统运行时间
- 做短时间忙等延时
- 实现 ms 级软件定时器
- 小型工程中的 LED 闪烁、通信超时、状态周期检查等

它不适合：

- us 级精确周期任务
- 高实时性控制环
- ADC 精确定时采样
- PWM / 脉冲精确输出
- 在软件定时器回调中执行长时间阻塞任务

如果需要真正的 us 级周期行为，应优先使用硬件 TIM、输出比较、DMA、ADC 外部触发等硬件机制，而不是 poll 模型的软件定时器。

---

## 3. 基本使用方法

### 3.1 添加文件

把下面两个文件加入工程：

```text
rivers_timer.h
rivers_timer.c
```

在需要使用时间接口的文件中包含头文件：

```c
#include "rivers_timer.h"
```

### 3.2 初始化

在系统时钟配置完成后调用：

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    rivers_timer_init();

    while (1)
    {
        rivers_timer_poll();
    }
}
```

`rivers_timer_init()` 会完成：

- 清零内部时间计数
- 清空软件定时器表
- 自动配置 Cortex-M SysTick
- 同步内部 tick 参数

### 3.3 SysTick 中断接入

需要在 `SysTick_Handler()` 中调用：

```c
void SysTick_Handler(void)
{
    rivers_timer_tick_handler();

    /*
     * 如果工程仍然依赖 HAL_Delay() / HAL_GetTick()，
     * 可以继续保留 HAL_IncTick()。
     */
    // HAL_IncTick();
}
```

`rivers_timer_tick_handler()` 只负责推进系统时间，不执行软件定时器回调。

软件定时器回调由 `rivers_timer_poll()` 在主循环或任务态执行。

---

## 4. 配置宏

可以在包含 `rivers_timer.h` 之前，或者在编译选项中覆盖以下宏。

### 4.1 CPU 主频

```c
#define RIVERS_TIMER_CPU_CLOCK_HZ 168000000UL
```

默认值为 `168 MHz`，适合常见 STM32F407 工程。  
如果芯片主频不是 168 MHz，应改成实际 CPU 主频。

### 4.2 SysTick 输入时钟

```c
#define RIVERS_TIMER_SYSTICK_CLOCK_HZ RIVERS_TIMER_CPU_CLOCK_HZ
```

默认等于 CPU 主频，表示 SysTick 使用内核时钟。

### 4.3 SysTick 频率

```c
#define RIVERS_TIMER_TICK_RATE_HZ 1000UL
```

默认 `1000 Hz`，即每 1 ms 产生一次 SysTick 中断。

### 4.4 软件定时器数量

```c
#define RIVERS_TIMER_MAX_SW_TIMERS 16
```

默认最多支持 16 个软件定时器。

软件定时器使用静态数组管理，不使用 `malloc/free`。  
数值越大，占用 RAM 越多。

### 4.5 Debug Hook

```c
#define RIVERS_TIMER_ENABLE_DEBUG_HOOK 0
#define RIVERS_TIMER_DEBUG_HOOK(module, message) ((void)0)
```

默认关闭调试输出。

如果需要调试参数错误、定时器表耗尽等问题，可以启用 debug hook，并把它接到串口、RTT 或日志系统。

---

## 5. API 说明

### 5.1 初始化与 Tick

#### `rivers_timer_init`

```c
void rivers_timer_init(void);
```

初始化 timer 模块，并自动配置 SysTick。

调用位置：系统时钟配置完成后。

#### `rivers_timer_tick_handler`

```c
void rivers_timer_tick_handler(void);
```

推进系统时间。

调用位置：`SysTick_Handler()`。

注意：该函数只更新时间，不执行软件定时器回调。

#### `rivers_timer_get_tick`

```c
uint32_t rivers_timer_get_tick(void);
```

获取 32 位毫秒 tick。

返回值允许自然回绕，适合用无符号差值判断时间间隔：

```c
uint32_t start = rivers_timer_get_tick();

if ((uint32_t)(rivers_timer_get_tick() - start) >= 1000U)
{
    /* 1 秒到 */
}
```

#### `rivers_timer_get_uptime_ms`

```c
uint32_t rivers_timer_get_uptime_ms(void);
```

获取 32 位毫秒运行时间。

#### `rivers_timer_get_uptime_us`

```c
uint32_t rivers_timer_get_uptime_us(void);
```

获取 32 位微秒运行时间。

该接口会基于 SysTick 的 `LOAD / VAL` 估算当前 tick 内的子节拍时间，因此比单纯毫秒 tick 更细。

---

### 5.2 忙等延时

#### `rivers_timer_delay_ms`

```c
void rivers_timer_delay_ms(uint32_t ms);
```

忙等延时指定毫秒数。

注意：

- 不让出 CPU
- 不调用 HAL_Delay
- 不执行软件定时器回调
- 不建议在中断中调用长延时

#### `rivers_timer_delay_us`

```c
void rivers_timer_delay_us(uint32_t us);
```

忙等延时指定微秒数。

适合短时间硬件等待，不适合长时间阻塞。

---

### 5.3 软件定时器

软件定时器使用 ms 作为单位，回调由 `rivers_timer_poll()` 执行。

#### `rivers_sw_timer_create`

```c
int rivers_sw_timer_create(uint32_t timeout_ms,
                           bool periodic,
                           rivers_timer_callback_t cb,
                           void *arg);
```

创建软件定时器。

参数：

- `timeout_ms`：周期或单次超时时间，单位 ms
- `periodic`：`true` 表示周期定时器，`false` 表示单次定时器
- `cb`：到期回调函数
- `arg`：传递给回调函数的用户参数

返回值：

- 成功：返回 `timer_id`
- 失败：返回 `-1`

创建后不会自动启动，需要调用 `rivers_sw_timer_start()`。

#### `rivers_sw_timer_start`

```c
rivers_timer_status_t rivers_sw_timer_start(int timer_id);
```

启动软件定时器。

#### `rivers_sw_timer_stop`

```c
rivers_timer_status_t rivers_sw_timer_stop(int timer_id);
```

停止软件定时器。停止后定时器仍然保留，可以再次启动。

#### `rivers_sw_timer_delete`

```c
rivers_timer_status_t rivers_sw_timer_delete(int timer_id);
```

删除软件定时器。删除后 `timer_id` 失效。

#### `rivers_sw_timer_set_period`

```c
rivers_timer_status_t rivers_sw_timer_set_period(int timer_id,
                                                 uint32_t period_ms);
```

修改软件定时器周期。如果定时器正在运行，下一次到期时间会从当前时间重新计算。

#### `rivers_sw_timer_set_remaining`

```c
rivers_timer_status_t rivers_sw_timer_set_remaining(int timer_id,
                                                    uint32_t remaining_ms);
```

修改正在运行的软件定时器剩余时间。仅对 active 状态的定时器有效。

#### `rivers_timer_poll`

```c
void rivers_timer_poll(void);
```

扫描软件定时器并执行到期回调。

如果使用软件定时器，必须在主循环或任务态周期性调用：

```c
while (1)
{
    rivers_timer_poll();

    /* 其他业务 */
}
```

注意：

- 不调用 `rivers_timer_poll()`，软件定时器不会执行回调。
- `rivers_timer_poll()` 调用越频繁，软件定时器响应越及时。
- 回调在 `rivers_timer_poll()` 的调用上下文中执行，不在 SysTick 中断中执行。
- 回调不会在临界区内执行。

---

## 6. 软件定时器示例

```c
#include "rivers_timer.h"

static volatile bool g_led_flag = false;

static void led_timer_cb(void *arg)
{
    (void)arg;

    /*
     * 回调运行在 rivers_timer_poll() 的调用上下文中。
     * 建议回调里只做轻量操作，复杂业务可以放到主循环中处理。
     */
    g_led_flag = true;
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    rivers_timer_init();

    int led_timer = rivers_sw_timer_create(1000U, true, led_timer_cb, NULL);
    if (led_timer >= 0)
    {
        rivers_sw_timer_start(led_timer);
    }

    while (1)
    {
        rivers_timer_poll();

        if (g_led_flag)
        {
            g_led_flag = false;

            /*
             * 在这里执行真正业务，例如翻转 LED。
             */
            // HAL_GPIO_TogglePin(GPIOx, GPIO_PIN_x);
        }

        uint32_t now = rivers_timer_get_tick();
        (void)now;
    }
}

void SysTick_Handler(void)
{
    rivers_timer_tick_handler();

    /*
     * 如果工程仍依赖 HAL_Delay() / HAL_GetTick()，
     * 可以继续保留 HAL_IncTick()。
     */
    // HAL_IncTick();
}
```

---

## 7. 状态码

```c
typedef enum {
    RIVERS_TIMER_OK = 0,
    RIVERS_TIMER_ERR_PARAM = -1,
    RIVERS_TIMER_ERR_FULL = -2,
    RIVERS_TIMER_ERR_NOT_FOUND = -3,
    RIVERS_TIMER_ERR_NOT_READY = -4
} rivers_timer_status_t;
```

| 状态码 | 含义 |
|---|---|
| `RIVERS_TIMER_OK` | 操作成功 |
| `RIVERS_TIMER_ERR_PARAM` | 参数非法 |
| `RIVERS_TIMER_ERR_FULL` | 静态资源已满 |
| `RIVERS_TIMER_ERR_NOT_FOUND` | 指定 timer_id 未分配 |
| `RIVERS_TIMER_ERR_NOT_READY` | 目标对象或状态尚未就绪 |

---

## 8. 设计取舍

### 8.1 为什么软件定时器使用 ms 单位

软件定时器采用 `poll` 模型，真实触发时间取决于：

- 主循环多久调用一次 `rivers_timer_poll()`
- 主循环是否有阻塞
- 回调函数执行多久
- 当前是否有其他代码长时间占用 CPU

因此，软件定时器不适合表达 us 级实时周期任务。

模块保留了 `rivers_timer_get_uptime_us()` 和 `rivers_timer_delay_us()`，用于短延时和时间测量；但软件定时器统一使用 ms 单位，避免误导用户认为 poll 软件定时器能提供稳定的 us 级实时性。

### 8.2 为什么软件定时器回调不放在 SysTick 中断中

如果软件定时器回调直接在 SysTick 中断中执行，使用会更“自动”，但回调函数必须严格遵守中断上下文限制，不能阻塞、不能做重活。

当前设计选择让 `SysTick` 只更新时间，软件定时器回调放在 `rivers_timer_poll()` 中执行：

```text
SysTick_Handler
    └── rivers_timer_tick_handler()
            └── 只累加时间

main while(1)
    └── rivers_timer_poll()
            └── 检查软件定时器并执行回调
```

这样安全性更好，也更符合裸机主循环模型。

---

## 9. 注意事项

1. `rivers_timer_init()` 应在系统时钟配置完成后调用。
2. `SysTick_Handler()` 中必须调用 `rivers_timer_tick_handler()`。
3. 如果使用软件定时器，主循环中必须周期性调用 `rivers_timer_poll()`。
4. 软件定时器回调不在 SysTick 中断中执行。
5. `delay_ms()` 和 `delay_us()` 都是忙等延时，不会执行软件定时器回调。
6. 软件定时器使用静态数组，不使用动态内存。
7. 软件定时器是 ms 级 poll 定时器，不适合 us 级实时周期任务。
8. 需要精确定时的任务应使用硬件 TIM / DMA / ADC 触发等硬件机制。
