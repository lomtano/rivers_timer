#ifndef RIVERS_TIMER_H
#define RIVERS_TIMER_H

/*
 * rivers_timer.h
 *
 * rivers_timer 是一个小型 standalone SysTick timer module。
 *
 * 默认模型：
 * - 默认使用 Cortex-M SysTick 作为系统时基。
 * - rivers_timer_init() 会自动配置 SysTick。
 * - 用户需要在 SysTick_Handler() 中调用 rivers_timer_tick_handler() 推进系统时间。
 * - SysTick 中断只更新时间，不执行软件定时器回调。
 * - 软件定时器采用 rivers_timer_poll() 模型，回调运行在 rivers_timer_poll() 的调用上下文中。
 * - 软件定时器 API 统一使用 ms 单位，适合普通周期任务、状态检查、超时管理、LED 闪烁、通信超时等。
 * - us 级短延时和时间测量由 rivers_timer_get_uptime_us() / rivers_timer_delay_us() 提供。
 *
 * 重要边界：
 * - 本模块不依赖 HAL、CMSIS，也不依赖原 rivers_osal 的 task / queue / mem / irq / cortexm。
 * - 本模块不使用 malloc/free，软件定时器使用静态数组。
 * - 如果只使用基础 tick / uptime / delay，可以不调用 rivers_timer_poll()。
 * - 如果使用软件定时器，必须在 while(1) 或任务态中周期性调用 rivers_timer_poll()。
 * - 软件定时器不提供 us 级周期实时保证；ADC 精确定时采样、PWM、脉冲输出、控制环等 us 级实时任务应使用硬件 TIM / DMA / 外设触发。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CPU 主频，单位 Hz。
 * @note 默认值 168000000UL，对应常见 STM32F407 168 MHz 主频。
 * @note 当移植到其他芯片、修改系统主频，或需要让 SysTick 时间换算匹配真实 CPU 时钟时，需要修改。
 */
#ifndef RIVERS_TIMER_CPU_CLOCK_HZ
#define RIVERS_TIMER_CPU_CLOCK_HZ 168000000UL
#endif

/**
 * @brief SysTick 计数器输入时钟，单位 Hz。
 * @note 默认等于 RIVERS_TIMER_CPU_CLOCK_HZ，表示 SysTick 使用 Cortex-M 内核时钟。
 * @note 当前模块固定在 SysTick CTRL 中打开 CLKSOURCE，因此通常应保持它等于 CPU/HCLK 时钟。
 * @note 移植到不同主频芯片时，需要把该宏改成真实的 SysTick 输入时钟。
 */
#ifndef RIVERS_TIMER_SYSTICK_CLOCK_HZ
#define RIVERS_TIMER_SYSTICK_CLOCK_HZ RIVERS_TIMER_CPU_CLOCK_HZ
#endif

/**
 * @brief SysTick 中断频率，单位 Hz。
 * @note 默认 1000UL，表示每 1 ms 产生一次 SysTick 中断。
 * @note 软件定时器是 ms 级 poll 定时器；通常建议保持 1000Hz，便于 ms tick 累计。
 * @note 如果降低 tick 频率，delay_ms 和软件定时器粒度会随之变粗。
 */
#ifndef RIVERS_TIMER_TICK_RATE_HZ
#define RIVERS_TIMER_TICK_RATE_HZ 1000UL
#endif

/**
 * @brief 静态软件定时器数组容量。
 * @note 默认 16，表示最多同时创建 16 个软件定时器。
 * @note 数值越大，占用 RAM 越多；移植到 RAM 较小芯片时可按实际需求调小。
 */
#ifndef RIVERS_TIMER_MAX_SW_TIMERS
#define RIVERS_TIMER_MAX_SW_TIMERS 16
#endif

/**
 * @brief 是否启用 debug hook 上报。
 * @note 默认 0，不输出任何诊断信息，保持模块轻量。
 * @note 调试移植问题、非法参数或软件定时器资源耗尽时，可改成 1 并提供 RIVERS_TIMER_DEBUG_HOOK。
 */
#ifndef RIVERS_TIMER_ENABLE_DEBUG_HOOK
#define RIVERS_TIMER_ENABLE_DEBUG_HOOK 0
#endif

/**
 * @brief 调试诊断钩子。
 * @param module 模块名字符串，当前通常为 "timer"。
 * @param message 诊断消息字符串。
 * @note 默认空实现。移植时可改成串口打印、RTT 输出或日志系统上报。
 */
#ifndef RIVERS_TIMER_DEBUG_HOOK
#define RIVERS_TIMER_DEBUG_HOOK(module, message) ((void)0)
#endif

/**
 * @brief rivers_timer 模块统一返回状态码。
 */
typedef enum {
    RIVERS_TIMER_OK = 0,             /**< 操作成功。 */
    RIVERS_TIMER_ERR_PARAM = -1,     /**< 输入参数非法，例如 timer_id 越界或回调函数为空。 */
    RIVERS_TIMER_ERR_FULL = -2,      /**< 资源已满，预留给静态表耗尽等场景；create 接口按约定返回 -1。 */
    RIVERS_TIMER_ERR_NOT_FOUND = -3, /**< 指定 timer_id 当前没有对应的已分配软件定时器。 */
    RIVERS_TIMER_ERR_NOT_READY = -4  /**< 目标对象或状态尚未就绪，例如对未 active 的定时器设置 remaining。 */
} rivers_timer_status_t;

/**
 * @brief 软件定时器到期回调函数类型。
 * @param arg 创建软件定时器时传入的用户参数。
 * @note 回调运行在 rivers_timer_poll() 的调用上下文中，不运行在 SysTick 中断中。
 * @note 回调内可以做少量业务，但仍建议不要长时间阻塞，否则会影响后续软件定时器 poll 的及时性。
 */
typedef void (*rivers_timer_callback_t)(void *arg);

/**
 * @brief 初始化 standalone SysTick timer 模块。
 * @return 无。
 * @note 会清零内部 uptime 计数和软件定时器静态数组。
 * @note 会自动配置 Cortex-M SysTick：检查时钟、计算 reload、写 LOAD=reload-1、最后打开 ENABLE/TICKINT/CLKSOURCE。
 * @note SysTick 是递减计数器，一个完整周期包含 LOAD + 1 个硬件计数。
 * @note 初始化后用户必须在 SysTick_Handler() 中调用 rivers_timer_tick_handler()。
 * @note 本函数不执行软件定时器回调，也不依赖 HAL 或 CMSIS。
 */
void rivers_timer_init(void);

/**
 * @brief 推进 rivers_timer 的系统时间基准。
 * @return 无。
 * @note 应放在 SysTick_Handler() 中调用。
 * @note 该函数只累加 us/ms uptime，不执行用户回调，不调用 rivers_timer_poll()，不做复杂业务。
 * @note 如果工程还依赖 HAL_Delay/HAL_GetTick，可以在 SysTick_Handler() 中额外保留 HAL_IncTick()。
 */
void rivers_timer_tick_handler(void);

/**
 * @brief 获取毫秒 tick 计数。
 * @return 32 位毫秒 tick，允许自然回绕。
 * @note 依赖 rivers_timer_tick_handler() 持续推进时间。
 * @note 可用于普通上下文和中断上下文；不会忙等，不执行软件定时器回调。
 */
uint32_t rivers_timer_get_tick(void);

/**
 * @brief 获取系统运行毫秒数。
 * @return 32 位毫秒 uptime，允许自然回绕。
 * @note 与 rivers_timer_get_tick() 语义一致。
 * @note 不会忙等，不执行软件定时器回调。
 */
uint32_t rivers_timer_get_uptime_ms(void);

/**
 * @brief 获取系统运行微秒数。
 * @return 32 位微秒 uptime，允许自然回绕。
 * @note 返回值约等于“已累计整 tick 时间 + 当前 SysTick 周期内子节拍偏移”。
 * @note 依赖 SysTick LOAD / VAL 估算当前 tick 内已经过去的微秒数。
 * @note 读取过程使用临界区保护，避免与 SysTick 中断同时修改累计变量。
 * @note 不会忙等，不执行软件定时器回调。
 */
uint32_t rivers_timer_get_uptime_us(void);

/**
 * @brief 忙等延时指定毫秒数。
 * @param ms 延时时长，单位 ms。
 * @return 无。
 * @note 依赖 rivers_timer_tick_handler() 持续推进毫秒 tick。
 * @note 这是忙等延时，不让出 CPU，不调用 HAL_Delay，不执行软件定时器回调。
 * @note 使用无符号差值判断，允许 32 位 tick 自然回绕。
 * @note 不建议在中断中调用 delay，尤其是不应在中断中长时间忙等。
 */
void rivers_timer_delay_ms(uint32_t ms);

/**
 * @brief 忙等延时指定微秒数。
 * @param us 延时时长，单位 us。
 * @return 无。
 * @note 依赖 rivers_timer_get_uptime_us()，通常需要 SysTick 已启用才能获得 tick 内子节拍精度。
 * @note 这是忙等延时，不让出 CPU，不调用 HAL_Delay，不执行软件定时器回调。
 * @note 使用无符号差值判断，允许 32 位微秒计数自然回绕。
 * @note 适合较短的硬件等待；真正 us 级周期性任务应使用硬件定时器。
 */
void rivers_timer_delay_us(uint32_t us);

/**
 * @brief 轮询软件定时器并执行已到期回调。
 * @return 无。
 * @note 应在 while(1) 主循环或任务态周期性调用。
 * @note 本函数负责扫描软件定时器静态数组，检查 allocated 且 active 的定时器是否到期。
 * @note 到期回调在临界区外执行，不在 SysTick 中断中执行。
 * @note 调用频率越高，软件定时器响应越及时；如果不调用 rivers_timer_poll()，软件定时器不会执行回调。
 * @note 软件定时器是 ms 级 poll 定时器，不适合表达 us 级实时周期任务。
 */
void rivers_timer_poll(void);

/**
 * @brief 创建一个 ms 级软件定时器。
 * @param timeout_ms 定时器周期或单次超时时长，单位 ms。
 * @param periodic true 表示周期定时器，false 表示单次定时器。
 * @param cb 定时器到期后的回调函数，不能为 NULL。
 * @param arg 传递给回调函数的用户参数。
 * @return 成功返回 timer_id，失败返回 -1。
 * @note 软件定时器使用静态数组，不使用 malloc/free。
 * @note 创建后不会自动启动，需要调用 rivers_sw_timer_start()。
 * @note 回调不会在 SysTick 中断中执行，只会在 rivers_timer_poll() 中执行。
 * @note 这是 ms 级 poll 定时器；真正需要 us 级周期行为的场景应使用硬件 TIM / DMA / 外设触发。
 */
int rivers_sw_timer_create(uint32_t timeout_ms, bool periodic, rivers_timer_callback_t cb, void *arg);

/**
 * @brief 启动指定软件定时器。
 * @param timer_id rivers_sw_timer_create() 返回的定时器 ID。
 * @return RIVERS_TIMER_OK 表示启动成功；RIVERS_TIMER_ERR_PARAM 表示 ID 越界；RIVERS_TIMER_ERR_NOT_FOUND 表示该 ID 未分配。
 * @note 启动后 expiry_ms 会被设置为当前 64 位毫秒时间 + period_ms。
 * @note 不会忙等，不执行软件定时器回调；真正回调由后续 rivers_timer_poll() 触发。
 * @note 应在主循环或任务态调用。
 */
rivers_timer_status_t rivers_sw_timer_start(int timer_id);

/**
 * @brief 停止指定软件定时器。
 * @param timer_id 软件定时器 ID。
 * @return RIVERS_TIMER_OK 表示停止成功；RIVERS_TIMER_ERR_PARAM 表示 ID 越界；RIVERS_TIMER_ERR_NOT_FOUND 表示该 ID 未分配。
 * @note 停止后 active=false，但定时器仍保持 allocated，可再次 start。
 * @note 不会忙等，不执行软件定时器回调。
 * @note 应在主循环或任务态调用。
 */
rivers_timer_status_t rivers_sw_timer_stop(int timer_id);

/**
 * @brief 删除指定软件定时器。
 * @param timer_id 软件定时器 ID。
 * @return RIVERS_TIMER_OK 表示删除成功；RIVERS_TIMER_ERR_PARAM 表示 ID 越界；RIVERS_TIMER_ERR_NOT_FOUND 表示该 ID 未分配。
 * @note delete 后 timer_id 立即失效，内部 allocated / active / cb / arg 等字段会被清空。
 * @note 不会忙等，不执行软件定时器回调。
 * @note 应在主循环或任务态调用。
 */
rivers_timer_status_t rivers_sw_timer_delete(int timer_id);

/**
 * @brief 修改软件定时器周期或单次超时时长。
 * @param timer_id 软件定时器 ID。
 * @param period_ms 新周期或新超时时长，单位 ms。
 * @return RIVERS_TIMER_OK 表示修改成功；RIVERS_TIMER_ERR_PARAM 表示 ID 越界；RIVERS_TIMER_ERR_NOT_FOUND 表示该 ID 未分配。
 * @note 如果定时器正在运行，下一次到期时间会从当前时刻重新计算为 now_ms + period_ms。
 * @note 如果定时器已停止，只保存新的 period_ms，不会自动启动。
 * @note 不会忙等，不执行软件定时器回调。
 */
rivers_timer_status_t rivers_sw_timer_set_period(int timer_id, uint32_t period_ms);

/**
 * @brief 修改正在运行的软件定时器剩余时间。
 * @param timer_id 软件定时器 ID。
 * @param remaining_ms 距离下一次到期还剩多少毫秒。
 * @return RIVERS_TIMER_OK 表示修改成功；RIVERS_TIMER_ERR_PARAM 表示 ID 越界；RIVERS_TIMER_ERR_NOT_FOUND 表示该 ID 未分配；RIVERS_TIMER_ERR_NOT_READY 表示定时器未处于 active 状态。
 * @note 该接口只对已启动的 active 定时器有效。
 * @note remaining_ms 为 0 时，下一次 rivers_timer_poll() 会尽快处理该定时器。
 * @note 不会忙等，不执行软件定时器回调。
 */
rivers_timer_status_t rivers_sw_timer_set_remaining(int timer_id, uint32_t remaining_ms);

/*
 * 完整接入示例：
 *
 * #include "rivers_timer.h"
 *
 * static volatile bool g_timer_flag = false;
 *
 * static void led_timer_cb(void *arg)
 * {
 *     (void)arg;
 *     g_timer_flag = true;
 * }
 *
 * int main(void)
 * {
 *     HAL_Init();
 *     SystemClock_Config();
 *
 *     rivers_timer_init();
 *
 *     int led_timer = rivers_sw_timer_create(1000U, true, led_timer_cb, NULL);
 *     if (led_timer >= 0) {
 *         (void)rivers_sw_timer_start(led_timer);
 *     }
 *
 *     while (1) {
 *         rivers_timer_poll();
 *
 *         if (g_timer_flag) {
 *             g_timer_flag = false;
 *             // 在主循环里执行真正业务，例如翻转 LED、发送日志等。
 *         }
 *
 *         uint32_t now = rivers_timer_get_tick();
 *         (void)now;
 *     }
 * }
 *
 * void SysTick_Handler(void)
 * {
 *     rivers_timer_tick_handler();
 *
 *     // 如果工程仍依赖 HAL_Delay/HAL_GetTick，可以保留：
 *     // HAL_IncTick();
 * }
 */

#ifdef __cplusplus
}
#endif

#endif /* RIVERS_TIMER_H */
