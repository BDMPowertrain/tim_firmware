#include "cpu_share.h"

/* The task's rise since the earlier sample, or since zero if it is not
   there. Unsigned, so a counter that wrapped once still counts. */
static uint32_t rise(const cpu_sample_t *before, size_t before_count,
                     const cpu_sample_t *task)
{
    for (size_t i = 0u; i < before_count; i++) {
        if (before[i].task_number == task->task_number) {
            return task->run_time - before[i].run_time;
        }
    }
    return task->run_time;
}

void cpu_share_compute(const cpu_sample_t *before, size_t before_count,
                       const cpu_sample_t *now, size_t now_count,
                       uint32_t *share_permille)
{
    uint64_t total = 0u;

    for (size_t i = 0u; i < now_count; i++) {
        total += rise(before, before_count, &now[i]);
    }

    for (size_t i = 0u; i < now_count; i++) {
        if (total == 0u) {
            share_permille[i] = 0u;
            continue;
        }
        const uint64_t part = rise(before, before_count, &now[i]);
        share_permille[i] = (uint32_t)((part * 1000u + total / 2u) / total);
    }
}
