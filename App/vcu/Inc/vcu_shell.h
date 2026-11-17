#ifndef APP_VCU_INC_VCU_SHELL_H
#define APP_VCU_INC_VCU_SHELL_H

/* Hands the VCU image's own shell commands (imu, imustream, log, can, vbat,
   inputs) to the shell, and its refusal hook for `hang` and `crash`
   (vcu_step_bench_refusal()). Call once from tasks_init(), before shellTask
   is created. */
void vcu_shell_register(void);

#endif /* APP_VCU_INC_VCU_SHELL_H */
