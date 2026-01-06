#include "lib.h"

#define MAX_CMD_LEN 64

void main(void) {
    char cmd[MAX_CMD_LEN];
    memset(cmd, 0, MAX_CMD_LEN);
    int cmd_pos = 0;
    char c;

    sys_write("Welcome to AAOS Shell!\n", 23);
    
    while (1) {
        sys_write("> ", 2);
        cmd_pos = 0;
        
        while (1) {
            int n = sys_read(&c, 1);
            if (n > 0) {
                sys_write(&c, 1);
                if (c == '\r' || c == '\n') {
                    sys_write("\n", 1);
                    cmd[cmd_pos] = 0;
                    break;
                } else if (c == 127 || c == 8) {
                    if (cmd_pos > 0) {
                        cmd_pos--;
                        sys_write("\b \b", 3);
                    }
                } else {
                    if (cmd_pos < MAX_CMD_LEN - 1) {
                        cmd[cmd_pos++] = c;
                    }
                }
            }
        }
        if (strcmp(cmd, "help") == 0) {
            sys_write("Available commands:\n", 20);
            sys_write("  help - Show this help\n", 24);
            sys_write("  echo <text> - Print text\n", 27);
            sys_write("  exit - Exit shell\n", 20);
        } else if (strncmp(cmd, "echo ", 5) == 0) {
            sys_write(cmd + 5, strlen(cmd + 5));
            sys_write("\n", 1);
        } else if (strcmp(cmd, "exit") == 0) {
            sys_write("Exiting...\n", 11);
            sys_exit(0);
        } else if (strlen(cmd) > 0) {
            sys_write("Unknown command: ", 17);
            sys_write(cmd, strlen(cmd));
            sys_write("\n", 1);
        }
    }
}
