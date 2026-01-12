#ifndef TTY_H
#define TTY_H

#include <stdint.h>

#define NR_TTYS 4
#define TTY_BUFFER_SIZE 128
#define TTY_OUTPUT_BUFFER_SIZE 4096

struct tty {
    int id;
    char input_buffer[TTY_BUFFER_SIZE];
    int read_idx;
    int write_idx;
    
    // Output Buffer (Ring Buffer)
    char output_buffer[TTY_OUTPUT_BUFFER_SIZE];
    int out_head; // Write index
    int out_tail; // Read index (start of valid data)
    int out_count; // Number of valid chars
};

void tty_init(void);
void tty_input(char c);
char tty_read_char(int core_id);
void tty_write(int core_id, char c);
void tty_switch(int tty_id);
int tty_get_active(void);

#endif
