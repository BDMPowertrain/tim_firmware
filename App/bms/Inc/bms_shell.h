#ifndef APP_BMS_INC_BMS_SHELL_H
#define APP_BMS_INC_BMS_SHELL_H

/* Hands the BMS image's own shell commands (cells, analog, vbat) to the
   shell. Call once from tasks_init(), before shellTask is created. */
void bms_shell_register(void);

#endif /* APP_BMS_INC_BMS_SHELL_H */
