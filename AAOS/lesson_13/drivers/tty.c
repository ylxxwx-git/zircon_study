#include "tty.h"
#include "uart.h"
#include "print.h"
#include "string.h"

struct tty ttys[NR_TTYS];
volatile int active_tty_id = 0;

// Escape Sequence State Machine
// 0: Normal
// 1: Esc received (0x1B)
static int input_state = 0;

void tty_init(void) {
    for (int i = 0; i < NR_TTYS; i++) {
        ttys[i].id = i;
        ttys[i].read_idx = 0;
        ttys[i].write_idx = 0;
        memset(ttys[i].input_buffer, 0, TTY_BUFFER_SIZE);
        
        ttys[i].out_head = 0;
        ttys[i].out_tail = 0;
        ttys[i].out_count = 0;
        memset(ttys[i].output_buffer, 0, TTY_OUTPUT_BUFFER_SIZE);
    }
    active_tty_id = 0;
    printk("TTY Driver Initialized. Active: 0\n");
}

void tty_switch(int tty_id) {
    if (tty_id >= 0 && tty_id < NR_TTYS) {
        active_tty_id = tty_id;
        
        // Clear screen and move cursor home
        uart_write_char('\033');
        uart_write_char('[');
        uart_write_char('2');
        uart_write_char('J');
        uart_write_char('\033');
        uart_write_char('[');
        uart_write_char('H');
        
        printk("Switched to TTY %d\n", tty_id);
        
        // Replay Output Buffer
        struct tty *t = &ttys[tty_id];
        int idx = t->out_tail;
        for (int i = 0; i < t->out_count; i++) {
            uart_write_char(t->output_buffer[idx]);
            idx = (idx + 1) % TTY_OUTPUT_BUFFER_SIZE;
        }
    }
}

int tty_get_active(void) {
    return active_tty_id;
}

void tty_input(char c) {
    // Handle Escape Sequences for Switching
    if (input_state == 0) {
        if (c == 0x1B) { // Esc
            input_state = 1;
            return;
        }
    } else if (input_state == 1) {
        input_state = 0; // Reset state
        if (c >= '1' && c <= '4') {
            tty_switch(c - '1');
            return;
        }
        // If not 1-4, it was a real Esc sequence (or just Esc).
        // For simplicity, we drop the Esc if it wasn't a switch, 
        // or we could inject it back. Let's just inject the current char.
        // (Real VT100 handling is complex, we just want switching).
    }

    // Normal Input -> Active TTY
    struct tty *t = &ttys[active_tty_id];
    int next_write = (t->write_idx + 1) % TTY_BUFFER_SIZE;
    
    if (next_write != t->read_idx) {
        t->input_buffer[t->write_idx] = c;
        t->write_idx = next_write;
    }
}

char tty_read_char(int core_id) {
    if (core_id < 0 || core_id >= NR_TTYS) return 0;
    
    struct tty *t = &ttys[core_id];
    
    // Wait for input
    while (t->read_idx == t->write_idx) {
        // Sleep or WFE?
        // Since we are in a syscall, we can schedule() or WFE.
        // For now, simple WFE loop (assuming IRQ will wake us).
        asm volatile("wfe");
    }
    
    char c = t->input_buffer[t->read_idx];
    t->read_idx = (t->read_idx + 1) % TTY_BUFFER_SIZE;
    return c;
}

void tty_write(int core_id, char c) {
    if (core_id < 0 || core_id >= NR_TTYS) return;
    
    struct tty *t = &ttys[core_id];
    
    // Store in output buffer
    t->output_buffer[t->out_head] = c;
    t->out_head = (t->out_head + 1) % TTY_OUTPUT_BUFFER_SIZE;
    
    if (t->out_count < TTY_OUTPUT_BUFFER_SIZE) {
        t->out_count++;
    } else {
        // Buffer full, overwrite oldest (tail advances)
        t->out_tail = (t->out_tail + 1) % TTY_OUTPUT_BUFFER_SIZE;
    }

    // Only write to UART if this core owns the active TTY
    if (core_id == active_tty_id) {
        uart_write_char(c);
    }
}
