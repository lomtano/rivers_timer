# rivers_timer
`rivers_timer` 是一个独立的 Cortex-M SysTick 时间模块，不依赖 HAL/CMSIS/OSAL。支持自动初始化 SysTick、毫秒 tick、微秒 uptime、忙等 delay，以及基于主循环 `rivers_timer_poll()` 的毫秒级软件定时器。回调不在中断中执行，适合小型裸机工程快速移植。
