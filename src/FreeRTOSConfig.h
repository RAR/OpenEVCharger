/* FreeRTOSConfig.h for OpenBHZD on GD32F205VG (Cortex-M3 @ 120 MHz).
 *
 * Tuned for safety-core scope: 4 tasks (idle + safety + io + comms + persist),
 * heap_4, mutexes for state-snapshot guarding, no software timers in M1
 * (added later if needed), preemptive scheduling, stack-overflow checking
 * level 2 enabled (most thorough), no idle hook in M1.
 *
 * The kernel ISR handler names are aliased here to the Cortex-M default
 * names that the vendor startup vector table uses, so we don't have to
 * patch startup_gd32f20x_cl.S.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0

#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)   /* words = 512 B */
#define configTOTAL_HEAP_SIZE                   ((size_t)16384)
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               4
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        0
#define configENABLE_BACKWARD_COMPATIBILITY     0

/* Hooks */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

/* Run-time stats / trace — off in M1, can enable per-milestone for debug */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routine support — not used */
#define configUSE_CO_ROUTINES                   0

/* API inclusions */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTimerPendFunctionCall          0

/* Cortex-M3 NVIC priority bits — GD32F205 uses 4 bits (16 priority levels) */
#define configPRIO_BITS                         4

/* The lowest interrupt priority that can be used in a call to a "set priority"
 * function. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15

/* The highest interrupt priority that can be used by any interrupt service
 * routine that makes calls to interrupt-safe FreeRTOS API functions. DO NOT
 * CALL FREERTOS API FUNCTIONS FROM ANY INTERRUPT THAT HAS A HIGHER PRIORITY
 * THAN THIS! (lower numbers = higher priority on Cortex-M) */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* assert macro — fall through to a simple infinite loop on failure;
 * a debugger can break in here and inspect the stack. */
#define configASSERT(x) \
    do { if (!(x)) { __asm volatile("cpsid i"); for (;;) {} } } while (0)

/* Map FreeRTOS handler names to Cortex-M vector-table names so the vendor
 * startup file (which references SVC_Handler/PendSV_Handler/SysTick_Handler)
 * picks up the FreeRTOS implementations without modifying startup_gd32f20x_cl.S */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
