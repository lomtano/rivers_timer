/*
 * rivers_timer.c
 *
 * rivers_timer 是一个小型 standalone SysTick timer module。
 *
 * 默认行为：
 * - rivers_timer_init() 自动配置 Cortex-M SysTick。
 * - SysTick 固定使用内核时钟源，CTRL 中固定打开 ENABLE、TICKINT、CLKSOURCE。
 * - 用户需要在 SysTick_Handler() 中调用 rivers_timer_tick_handler() 更新时间。
 * - rivers_timer_tick_handler() 只推进 uptime，不执行软件定时器回调，也不调用 rivers_timer_poll()。
 * - 软件定时器采用 ms 级 poll 模型，由 rivers_timer_poll() 在主循环或任务态执行到期回调。
 * - 如果只使用基础 tick / delay / uptime，可以不调用 rivers_timer_poll()。
 * - 如果使用软件定时器，必须周期性调用 rivers_timer_poll()。
 *
 * 本文件不依赖 HAL、CMSIS 或原 rivers_osal；不使用 malloc/free。us 级精确周期任务
 * 不应使用 poll 软件定时器，应使用硬件 TIM / DMA / 外设触发。
 */

#include "rivers_timer.h"

/* Cortex-M SysTick 寄存器地址。这里直接定义寄存器地址，不依赖 CMSIS 或 HAL。 */
#define RIVERS_TIMER_SYSTICK_CTRL_REG  (*((volatile uint32_t *)0xE000E010UL))
#define RIVERS_TIMER_SYSTICK_LOAD_REG  (*((volatile uint32_t *)0xE000E014UL))
#define RIVERS_TIMER_SYSTICK_VAL_REG   (*((volatile uint32_t *)0xE000E018UL))

/* SysTick CTRL 寄存器常用位。 */
#define RIVERS_TIMER_SYSTICK_ENABLE_BIT    (1UL << 0U)
#define RIVERS_TIMER_SYSTICK_TICKINT_BIT   (1UL << 1U)
#define RIVERS_TIMER_SYSTICK_CLKSOURCE_BIT (1UL << 2U)
#define RIVERS_TIMER_SYSTICK_COUNTFLAG_BIT (1UL << 16U)
#define RIVERS_TIMER_SYSTICK_MAX_RELOAD    (0x01000000UL)

#if RIVERS_TIMER_ENABLE_DEBUG_HOOK
#define RIVERS_TIMER_REPORT(module, message) RIVERS_TIMER_DEBUG_HOOK((module), (message))
#else
#define RIVERS_TIMER_REPORT(module, message) ((void)0)
#endif

/*
 * 极简 PRIMASK 临界区。
 *
 * 默认实现面向 Cortex-M：进入临界区时读取 PRIMASK 并关闭可屏蔽中断，退出时只在
 * 进入前原本开中断的情况下重新开中断。这样可以保持嵌套调用或外部提前关中断时的
 * 原始中断状态。
 *
 * 如果移植到非 Cortex-M 平台，可以通过编译选项预定义 RIVERS_TIMER_CRITICAL_ENTER()
 * 和 RIVERS_TIMER_CRITICAL_EXIT(state)，或直接替换本文件这段默认实现。
 */
#ifndef RIVERS_TIMER_CRITICAL_ENTER
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief 进入默认 PRIMASK 临界区。
 * @return 进入临界区前的 PRIMASK 快照。
 * @note 该函数会关闭 Cortex-M 可屏蔽中断，用于保护 uptime 计数和软件定时器静态数组。
 * @note 可在普通上下文或中断上下文调用；它本身不会被 SysTick 自动调用，只由本模块内部使用。
 */
static uint32_t rivers_timer_default_critical_enter(void) {
    uint32_t primask;

    __asm volatile (
        "mrs %0, primask\n"
        "cpsid i\n"
        : "=r" (primask)
        :
        : "memory");
    return primask;
}

/**
 * @brief 退出默认 PRIMASK 临界区。
 * @param primask 进入临界区前保存的 PRIMASK 快照。
 * @return 无。
 * @note 只有进入前 PRIMASK 为 0，即原本允许中断时，退出才重新打开中断。
 */
static void rivers_timer_default_critical_exit(uint32_t primask) {
    if (primask == 0U) {
        __asm volatile ("cpsie i" ::: "memory");
    }
}
#define RIVERS_TIMER_CRITICAL_ENTER() rivers_timer_default_critical_enter()
#define RIVERS_TIMER_CRITICAL_EXIT(state) rivers_timer_default_critical_exit((state))
#elif defined(__ICCARM__)
/**
 * @brief IAR 编译器下进入默认 PRIMASK 临界区。
 * @return 进入临界区前的 PRIMASK 快照。
 * @note 作用与 GCC/Clang 版本一致，用于保护本模块内部共享状态。
 */
static uint32_t rivers_timer_default_critical_enter(void) {
    uint32_t primask;

    __asm volatile ("MRS %0, PRIMASK" : "=r" (primask));
    __asm volatile ("CPSID i");
    return primask;
}

/**
 * @brief IAR 编译器下退出默认 PRIMASK 临界区。
 * @param primask 进入临界区前保存的 PRIMASK 快照。
 * @return 无。
 */
static void rivers_timer_default_critical_exit(uint32_t primask) {
    if (primask == 0U) {
        __asm volatile ("CPSIE i");
    }
}
#define RIVERS_TIMER_CRITICAL_ENTER() rivers_timer_default_critical_enter()
#define RIVERS_TIMER_CRITICAL_EXIT(state) rivers_timer_default_critical_exit((state))
#else
/**
 * @brief 无内联汇编支持时的空临界区占位实现。
 * @return 固定返回 0。
 * @note 该分支只保证代码能编译；真正移植到有并发/中断的平台时，应提供有效临界区实现。
 */
static uint32_t rivers_timer_default_critical_enter(void) {
    return 0U;
}

/**
 * @brief 无内联汇编支持时的空临界区退出占位实现。
 * @param primask 占位参数，不使用。
 * @return 无。
 */
static void rivers_timer_default_critical_exit(uint32_t primask) {
    (void)primask;
}
#define RIVERS_TIMER_CRITICAL_ENTER() rivers_timer_default_critical_enter()
#define RIVERS_TIMER_CRITICAL_EXIT(state) rivers_timer_default_critical_exit((state))
#endif
#endif

/**
 * @brief 软件定时器控制块。
 * @note 所有软件定时器都来自 s_sw_timers 静态数组，不使用动态内存分配。
 * @note 软件定时器统一使用 ms 单位；us 级短延时由基础时间接口负责。
 */
typedef struct {
    bool allocated;                 /* 该槽位是否已被 create 分配。 */
    bool active;                    /* 该定时器当前是否处于启动状态。 */
    bool periodic;                  /* true 表示周期定时器，false 表示单次定时器。 */
    uint64_t expiry_ms;             /* 下一次绝对到期时间，单位 ms。 */
    uint32_t period_ms;             /* 周期长度或单次超时时长，单位 ms。 */
    rivers_timer_callback_t cb;     /* 到期回调函数。 */
    void *arg;                      /* 传递给回调函数的用户参数。 */
} rivers_sw_timer_entry_t;

/* 32 位 uptime 对外使用，允许自然回绕；64 位 uptime 给长时间运行和软件定时器内部比较使用。 */
static volatile uint32_t s_uptime_us32 = 0U;
static volatile uint32_t s_uptime_ms32 = 0U;
static volatile uint64_t s_uptime_us64 = 0U;
static volatile uint64_t s_uptime_ms64 = 0U;
/* us 累计转换成 ms 时保留余数，避免长期运行时丢弃不足 1ms 的误差。 */
static volatile uint32_t s_ms_remainder_us = 0U;

/* 当前 SysTick tick source 的缓存参数，用于 tick_handler 和子节拍估算。 */
static bool s_tick_ready = false;
static uint32_t s_tick_counter_hz = 0U;
static uint32_t s_tick_reload_value = 0U;
static uint32_t s_tick_period_ticks = 0U;
static uint32_t s_tick_period_us = 1000U;

/* 软件定时器静态表。容量由 RIVERS_TIMER_MAX_SW_TIMERS 控制。 */
static rivers_sw_timer_entry_t s_sw_timers[RIVERS_TIMER_MAX_SW_TIMERS];

/**
 * @brief 累加已经流逝的微秒时间，并同步维护 32/64 位毫秒时间。
 * @param delta_us 本次 tick 对应的时间增量，单位 us。
 * @return 无。
 * @note 该函数由 rivers_timer_tick_handler() 调用，通常运行在 SysTick 中断上下文中。
 * @note 它只更新时间账本，不执行软件定时器 poll，也不执行用户回调。
 * @note us 转 ms 时会保留 s_ms_remainder_us，避免每个 tick 都丢弃不足 1ms 的余数。
 */
static void rivers_timer_accumulate_us(uint32_t delta_us) {
    uint32_t total_us;
    uint32_t elapsed_ms;

    s_uptime_us32 += delta_us;
    s_uptime_us64 += (uint64_t)delta_us;

    total_us = s_ms_remainder_us + delta_us;
    elapsed_ms = total_us / 1000U;
    s_uptime_ms32 += elapsed_ms;
    s_uptime_ms64 += (uint64_t)elapsed_ms;
    s_ms_remainder_us = (total_us % 1000U);
}

/**
 * @brief 根据硬件计数周期换算一个 SysTick tick 的微秒长度。
 * @param period_ticks 一个 SysTick 周期包含的硬件计数个数，通常为 LOAD + 1。
 * @param counter_hz SysTick 计数器输入时钟，单位 Hz。
 * @return 一个 SysTick 周期约等于多少微秒；参数非法时返回 1000us 作为保底值。
 * @note 不需要在临界区中调用；它只做纯计算。
 * @note tick_handler 后续会按这个值累加 uptime。
 */
static uint32_t rivers_timer_calc_tick_period_us(uint32_t period_ticks, uint32_t counter_hz) {
    uint32_t period_us;

    if ((period_ticks == 0U) || (counter_hz == 0U)) {
        return 1000U;
    }

    period_us = (uint32_t)((((uint64_t)period_ticks * 1000000ULL) +
                            ((uint64_t)counter_hz / 2ULL)) /
                           (uint64_t)counter_hz);
    return (period_us == 0U) ? 1U : period_us;
}

/**
 * @brief 从当前 SysTick 寄存器同步 tick source 参数。
 * @return 无。
 * @note 读取 LOAD 并计算 LOAD + 1 对应的完整硬件计数周期。
 * @note rivers_timer_init() 和 tick_handler 在 tick 参数尚未就绪时会调用它。
 * @note 它不执行软件定时器逻辑，也不执行 delay；通常不要求外部直接调用。
 */
static void rivers_timer_sync_systick_source(void) {
    uint32_t reload_value = RIVERS_TIMER_SYSTICK_LOAD_REG;
    uint32_t period_ticks = reload_value + 1U;

    s_tick_counter_hz = (uint32_t)RIVERS_TIMER_SYSTICK_CLOCK_HZ;
    s_tick_reload_value = reload_value;
    s_tick_period_ticks = period_ticks;
    s_tick_period_us = rivers_timer_calc_tick_period_us(period_ticks, s_tick_counter_hz);
    s_tick_ready = ((s_tick_counter_hz != 0U) && (period_ticks != 0U));
}

/**
 * @brief 自动配置 Cortex-M SysTick。
 * @return 无。
 * @note 配置流程固定为：检查时钟和 tick 频率、计算 reload、检查 24 位范围、关闭 CTRL、清空 VAL、写 LOAD=reload-1、最后写 CTRL 启动 SysTick。
 * @note SysTick 是递减计数器，LOAD 写 reload - 1 才能得到 reload 个硬件计数的完整周期。
 * @note CTRL 固定打开 ENABLE、TICKINT、CLKSOURCE，即默认使用 Cortex-M 内核时钟源。
 * @note 本函数不执行软件定时器回调；SysTick 中断触发后应由 SysTick_Handler 调用 rivers_timer_tick_handler()。
 */
static void rivers_timer_configure_systick(void) {
    uint32_t reload;
    uint32_t ctrl_value;

    /* 1. 检查 SysTick 输入时钟和目标 tick 频率，避免除 0 或无效时基。 */
    if ((RIVERS_TIMER_SYSTICK_CLOCK_HZ == 0UL) || (RIVERS_TIMER_TICK_RATE_HZ == 0UL)) {
        RIVERS_TIMER_REPORT("timer", "invalid SysTick clock or tick rate");
        s_tick_ready = false;
        return;
    }

    /* 2. 计算一个 tick 需要多少个 SysTick 输入时钟周期。 */
    reload = (uint32_t)(RIVERS_TIMER_SYSTICK_CLOCK_HZ / RIVERS_TIMER_TICK_RATE_HZ);
    /* 3. SysTick LOAD 只有 24 位，reload 必须落在 1..0x01000000 范围内。 */
    if ((reload == 0U) || (reload > RIVERS_TIMER_SYSTICK_MAX_RELOAD)) {
        RIVERS_TIMER_REPORT("timer", "SysTick reload is outside 24-bit range");
        s_tick_ready = false;
        return;
    }

    ctrl_value = RIVERS_TIMER_SYSTICK_ENABLE_BIT |
                 RIVERS_TIMER_SYSTICK_TICKINT_BIT |
                 RIVERS_TIMER_SYSTICK_CLKSOURCE_BIT;

    /* 4. 先关闭 CTRL，防止配置过程中触发不完整的 SysTick 中断。 */
    RIVERS_TIMER_SYSTICK_CTRL_REG = 0UL;
    /* 5. 清空 VAL，让新周期从确定状态开始。 */
    RIVERS_TIMER_SYSTICK_VAL_REG = 0UL;
    /* 6. 写 LOAD = reload - 1；SysTick 递减计数，一个周期包含 LOAD + 1 个计数。 */
    RIVERS_TIMER_SYSTICK_LOAD_REG = reload - 1UL;
    /* 7. 最后写 CTRL，固定打开 ENABLE、TICKINT、CLKSOURCE。 */
    RIVERS_TIMER_SYSTICK_CTRL_REG = ctrl_value;
}

/**
 * @brief 判断 SysTick 计数器是否已经启用。
 * @return true 表示 CTRL.ENABLE 已置位，false 表示未启用。
 * @note 被 uptime_us 子节拍估算逻辑调用。
 * @note 不需要在临界区中调用；调用方如果需要一致性，会在外层关中断。
 */
static bool rivers_timer_systick_enabled(void) {
    return ((RIVERS_TIMER_SYSTICK_CTRL_REG & RIVERS_TIMER_SYSTICK_ENABLE_BIT) != 0U);
}

/**
 * @brief 在临界区内估算当前 SysTick 周期内已经流逝的微秒数。
 * @return 当前 tick 内的子节拍偏移，单位 us；如果 SysTick 未就绪则返回 0。
 * @note 调用者必须已经进入临界区，避免读取期间与 SysTick 中断更新时间账本发生竞争。
 * @note SysTick 是递减计数器，VAL 越小表示越接近下一次中断。
 * @note 读取流程为：读 VAL、读 CTRL 中 COUNTFLAG、再读 VAL，用于缩小跨回卷点时的竞态窗口。
 * @note 该函数不执行软件定时器回调，也不会忙等。
 */
static uint32_t rivers_timer_get_subtick_us_locked(void) {
    uint32_t current_before;
    uint32_t current_after;
    uint64_t elapsed_ticks;
    bool elapsed_flag;

    if ((!s_tick_ready) || (!rivers_timer_systick_enabled()) || (s_tick_counter_hz == 0U)) {
        return 0U;
    }

    current_before = RIVERS_TIMER_SYSTICK_VAL_REG;
    elapsed_flag = ((RIVERS_TIMER_SYSTICK_CTRL_REG & RIVERS_TIMER_SYSTICK_COUNTFLAG_BIT) != 0U);
    current_after = RIVERS_TIMER_SYSTICK_VAL_REG;

    if (elapsed_flag) {
        elapsed_ticks = (uint64_t)s_tick_period_ticks +
                        (uint64_t)s_tick_reload_value -
                        (uint64_t)current_after;
    } else {
        elapsed_ticks = (uint64_t)s_tick_reload_value - (uint64_t)current_before;
    }

    return (uint32_t)((elapsed_ticks * 1000000ULL) / (uint64_t)s_tick_counter_hz);
}

/**
 * @brief 获取 64 位毫秒 uptime。
 * @return 64 位毫秒时间，用于软件定时器内部绝对到期时间比较。
 * @note 读取过程使用临界区保护，避免与 SysTick 中断同时更新 s_uptime_ms64。
 * @note 不暴露为公开 API；公开接口 rivers_timer_get_uptime_ms() 保持 32 位自然回绕语义。
 */
static uint64_t rivers_timer_get_uptime_ms64(void) {
    uint32_t state;
    uint64_t now_ms;

    state = RIVERS_TIMER_CRITICAL_ENTER();
    now_ms = s_uptime_ms64;
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return now_ms;
}

void rivers_timer_init(void) {
    uint32_t state;

    /* 1. 清零内部 uptime 计数。 */
    state = RIVERS_TIMER_CRITICAL_ENTER();
    s_uptime_us32 = 0U;
    s_uptime_ms32 = 0U;
    s_uptime_us64 = 0U;
    s_uptime_ms64 = 0U;
    s_ms_remainder_us = 0U;
    RIVERS_TIMER_CRITICAL_EXIT(state);

    /* 2. 清空软件定时器静态数组。 */
    state = RIVERS_TIMER_CRITICAL_ENTER();
    for (int i = 0; i < RIVERS_TIMER_MAX_SW_TIMERS; ++i) {
        s_sw_timers[i].allocated = false;
        s_sw_timers[i].active = false;
        s_sw_timers[i].periodic = false;
        s_sw_timers[i].expiry_ms = 0U;
        s_sw_timers[i].period_ms = 0U;
        s_sw_timers[i].cb = NULL;
        s_sw_timers[i].arg = NULL;
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    /* 3-9. 自动配置 SysTick；10. 同步内部 tick 参数。 */
    rivers_timer_configure_systick();
    rivers_timer_sync_systick_source();
}

void rivers_timer_tick_handler(void) {
    if (!s_tick_ready) {
        rivers_timer_sync_systick_source();
    }

    /*
     * tick_handler 只做时间推进：
     * - 根据 s_tick_period_us 累加 s_uptime_us32 / s_uptime_us64。
     * - 同步累加 s_uptime_ms32 / s_uptime_ms64。
     * - 保留 s_ms_remainder_us，避免 us 转 ms 时长期丢失余数。
     * - 不执行用户回调。
     * - 不调用 rivers_timer_poll()。
     * - 不做复杂业务。
     *
     * 推荐直接放在 SysTick_Handler() 中调用，保持中断内工作量最小。
     * 如果工程还依赖 HAL_Delay/HAL_GetTick，可以在 SysTick_Handler() 中额外保留 HAL_IncTick()。
     */
    rivers_timer_accumulate_us(s_tick_period_us);
}

uint32_t rivers_timer_get_uptime_us(void) {
    uint32_t state;
    uint32_t now_us;
    uint32_t extra_us;

    if (!s_tick_ready) {
        rivers_timer_sync_systick_source();
    }

    state = RIVERS_TIMER_CRITICAL_ENTER();
    /* 先读取已经由 tick_handler 累计的整 tick 微秒时间。 */
    now_us = s_uptime_us32;
    /* 再根据 SysTick LOAD / VAL 估算当前 tick 内已经流逝的子节拍时间。 */
    extra_us = rivers_timer_get_subtick_us_locked();
    RIVERS_TIMER_CRITICAL_EXIT(state);

    /* 返回整 tick + 子节拍；32 位加法允许自然回绕。 */
    return now_us + extra_us;
}

uint32_t rivers_timer_get_uptime_ms(void) {
    uint32_t state;
    uint32_t now_ms;

    state = RIVERS_TIMER_CRITICAL_ENTER();
    now_ms = s_uptime_ms32;
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return now_ms;
}

uint32_t rivers_timer_get_tick(void) {
    return rivers_timer_get_uptime_ms();
}

void rivers_timer_delay_us(uint32_t us) {
    uint32_t start = rivers_timer_get_uptime_us();

    while ((uint32_t)(rivers_timer_get_uptime_us() - start) < us) {
        /*
         * 忙等延时：
         * - 不让出 CPU。
         * - 不调用 HAL_Delay。
         * - 不执行软件定时器回调。
         * - 使用无符号差值判断，允许 32 位微秒计数自然回绕。
         */
    }
}

void rivers_timer_delay_ms(uint32_t ms) {
    uint32_t start = rivers_timer_get_tick();

    while ((uint32_t)(rivers_timer_get_tick() - start) < ms) {
        /*
         * 忙等延时：
         * - 不让出 CPU。
         * - 不调用 HAL_Delay。
         * - 不执行软件定时器回调。
         * - 使用无符号差值判断，允许 32 位 tick 自然回绕。
         */
    }
}

/**
 * @brief 检查软件定时器 ID 是否落在静态数组范围内。
 * @param timer_id 待检查的软件定时器 ID。
 * @return true 表示 ID 数值范围合法；false 表示小于 0 或大于等于 RIVERS_TIMER_MAX_SW_TIMERS。
 * @note 该函数只检查下标范围，不检查 allocated 状态。
 * @note 不需要临界区；调用方会在访问 s_sw_timers 时自行进入临界区。
 */
static bool rivers_sw_timer_id_valid(int timer_id) {
    return ((timer_id >= 0) && (timer_id < RIVERS_TIMER_MAX_SW_TIMERS));
}

int rivers_sw_timer_create(uint32_t timeout_ms, bool periodic, rivers_timer_callback_t cb, void *arg) {
    uint32_t state;
    int timer_id = -1;

    if (cb == NULL) {
        RIVERS_TIMER_REPORT("timer", "create called with NULL callback");
        return -1;
    }

    state = RIVERS_TIMER_CRITICAL_ENTER();
    for (int i = 0; i < RIVERS_TIMER_MAX_SW_TIMERS; ++i) {
        if (!s_sw_timers[i].allocated) {
            s_sw_timers[i].allocated = true;
            s_sw_timers[i].active = false;
            s_sw_timers[i].periodic = periodic;
            s_sw_timers[i].expiry_ms = 0U;
            s_sw_timers[i].period_ms = timeout_ms;
            s_sw_timers[i].cb = cb;
            s_sw_timers[i].arg = arg;
            timer_id = i;
            break;
        }
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    if (timer_id < 0) {
        RIVERS_TIMER_REPORT("timer", "software timer table is full");
    }
    return timer_id;
}

rivers_timer_status_t rivers_sw_timer_start(int timer_id) {
    uint32_t state;
    uint64_t now_ms;
    rivers_timer_status_t status = RIVERS_TIMER_OK;

    if (!rivers_sw_timer_id_valid(timer_id)) {
        return RIVERS_TIMER_ERR_PARAM;
    }

    now_ms = rivers_timer_get_uptime_ms64();

    state = RIVERS_TIMER_CRITICAL_ENTER();
    if (!s_sw_timers[timer_id].allocated) {
        status = RIVERS_TIMER_ERR_NOT_FOUND;
    } else {
        s_sw_timers[timer_id].expiry_ms = now_ms + (uint64_t)s_sw_timers[timer_id].period_ms;
        s_sw_timers[timer_id].active = true;
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return status;
}

rivers_timer_status_t rivers_sw_timer_stop(int timer_id) {
    uint32_t state;
    rivers_timer_status_t status = RIVERS_TIMER_OK;

    if (!rivers_sw_timer_id_valid(timer_id)) {
        return RIVERS_TIMER_ERR_PARAM;
    }

    state = RIVERS_TIMER_CRITICAL_ENTER();
    if (!s_sw_timers[timer_id].allocated) {
        status = RIVERS_TIMER_ERR_NOT_FOUND;
    } else {
        s_sw_timers[timer_id].active = false;
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return status;
}

rivers_timer_status_t rivers_sw_timer_delete(int timer_id) {
    uint32_t state;
    rivers_timer_status_t status = RIVERS_TIMER_OK;

    if (!rivers_sw_timer_id_valid(timer_id)) {
        return RIVERS_TIMER_ERR_PARAM;
    }

    state = RIVERS_TIMER_CRITICAL_ENTER();
    if (!s_sw_timers[timer_id].allocated) {
        status = RIVERS_TIMER_ERR_NOT_FOUND;
    } else {
        s_sw_timers[timer_id].allocated = false;
        s_sw_timers[timer_id].active = false;
        s_sw_timers[timer_id].periodic = false;
        s_sw_timers[timer_id].expiry_ms = 0U;
        s_sw_timers[timer_id].period_ms = 0U;
        s_sw_timers[timer_id].cb = NULL;
        s_sw_timers[timer_id].arg = NULL;
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return status;
}

rivers_timer_status_t rivers_sw_timer_set_period(int timer_id, uint32_t period_ms) {
    uint32_t state;
    uint64_t now_ms;
    rivers_timer_status_t status = RIVERS_TIMER_OK;

    if (!rivers_sw_timer_id_valid(timer_id)) {
        return RIVERS_TIMER_ERR_PARAM;
    }

    now_ms = rivers_timer_get_uptime_ms64();

    state = RIVERS_TIMER_CRITICAL_ENTER();
    if (!s_sw_timers[timer_id].allocated) {
        status = RIVERS_TIMER_ERR_NOT_FOUND;
    } else {
        s_sw_timers[timer_id].period_ms = period_ms;
        if (s_sw_timers[timer_id].active) {
            s_sw_timers[timer_id].expiry_ms = now_ms + (uint64_t)period_ms;
        }
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return status;
}

rivers_timer_status_t rivers_sw_timer_set_remaining(int timer_id, uint32_t remaining_ms) {
    uint32_t state;
    uint64_t now_ms;
    rivers_timer_status_t status = RIVERS_TIMER_OK;

    if (!rivers_sw_timer_id_valid(timer_id)) {
        return RIVERS_TIMER_ERR_PARAM;
    }

    now_ms = rivers_timer_get_uptime_ms64();

    state = RIVERS_TIMER_CRITICAL_ENTER();
    if (!s_sw_timers[timer_id].allocated) {
        status = RIVERS_TIMER_ERR_NOT_FOUND;
    } else if (!s_sw_timers[timer_id].active) {
        status = RIVERS_TIMER_ERR_NOT_READY;
    } else {
        s_sw_timers[timer_id].expiry_ms = now_ms + (uint64_t)remaining_ms;
    }
    RIVERS_TIMER_CRITICAL_EXIT(state);

    return status;
}

/**
 * @brief 轮询并处理已经到期的软件定时器。
 * @return 无。
 * @note 应在主循环或任务态调用，不应放在 SysTick 中断中调用。
 * @note 处理流程：获取当前 64 位毫秒时间，遍历静态数组，检查 allocated && active 的定时器是否到期。
 * @note 到期后先在临界区内取出 cb 和 arg，并更新 active / expiry_ms；退出临界区后再执行用户回调。
 * @note 周期定时器使用 do-while 推进 expiry_ms，直到 expiry_ms 落到 now_ms 之后，避免周期漂移。
 * @note 单次定时器到期后 active=false。
 * @note 如果不调用本函数，软件定时器不会触发回调。
 */
void rivers_timer_poll(void) {
    uint64_t now_ms;

    for (int i = 0; i < RIVERS_TIMER_MAX_SW_TIMERS; ++i) {
        uint32_t state;
        rivers_timer_callback_t cb = NULL;
        void *arg = NULL;

        now_ms = rivers_timer_get_uptime_ms64();

        state = RIVERS_TIMER_CRITICAL_ENTER();
        if (s_sw_timers[i].allocated && s_sw_timers[i].active &&
            (now_ms >= s_sw_timers[i].expiry_ms)) {
            cb = s_sw_timers[i].cb;
            arg = s_sw_timers[i].arg;

            if (s_sw_timers[i].periodic) {
                if (s_sw_timers[i].period_ms == 0U) {
                    s_sw_timers[i].active = false;
                } else {
                    do {
                        s_sw_timers[i].expiry_ms += (uint64_t)s_sw_timers[i].period_ms;
                    } while (now_ms >= s_sw_timers[i].expiry_ms);
                }
            } else {
                s_sw_timers[i].active = false;
            }
        }
        RIVERS_TIMER_CRITICAL_EXIT(state);

        if (cb != NULL) {
            cb(arg);
        }
    }
}
