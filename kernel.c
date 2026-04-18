#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ADDRESS 0xB8000

#define PORT_KEYBOARD_DATA 0x60
#define PORT_KEYBOARD_STATUS 0x64

#define COLOR(fg, bg) (uint8_t)(((bg) << 4) | ((fg) & 0x0F))

static volatile uint16_t* const vga = (uint16_t*)VGA_ADDRESS;

static size_t cursor_x = 10;
static size_t cursor_y = 8;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void draw_cell(size_t x, size_t y, char ch, uint8_t color) {
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    vga[y * VGA_WIDTH + x] = ((uint16_t)color << 8) | (uint8_t)ch;
}

static void draw_text(size_t x, size_t y, const char* text, uint8_t color) {
    size_t i = 0;
    while (text[i] && x + i < VGA_WIDTH) {
        draw_cell(x + i, y, text[i], color);
        ++i;
    }
}

static void draw_desktop(void) {
    const uint8_t desktop_color = COLOR(15, 4); // white on red

    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            draw_cell(x, y, ' ', desktop_color);
        }
    }

    // Blue test texture: filled square
    const uint8_t texture_color = COLOR(15, 1); // white on blue
    for (size_t y = 6; y < 14; ++y) {
        for (size_t x = 30; x < 46; ++x) {
            draw_cell(x, y, ' ', texture_color);
        }
    }

    draw_text(2, 1, "ZerDOS GUI shell test", COLOR(15, 4));
    draw_text(2, 3, "Move cursor: W/A/S/D", COLOR(14, 4));
}

static void draw_cursor(void) {
    // Cursor as bright block
    draw_cell(cursor_x, cursor_y, 'X', COLOR(0, 15));
}

static void render(void) {
    draw_desktop();
    draw_cursor();
}

static char scancode_to_ascii(uint8_t sc) {
    static const char map[128] = {
        [0x1E] = 'a', [0x11] = 'w', [0x1F] = 's', [0x20] = 'd',
        [0x10] = 'q'
    };

    if (sc >= 128) return 0;
    return map[sc];
}

static char wait_keypress(void) {
    while (1) {
        while ((inb(PORT_KEYBOARD_STATUS) & 1) == 0) {
        }

        uint8_t scancode = inb(PORT_KEYBOARD_DATA);
        if (scancode & 0x80) continue;

        char c = scancode_to_ascii(scancode);
        if (c) return c;
    }
}

void kmain(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    (void)multiboot_magic;
    (void)multiboot_info_addr;

    render();

    while (1) {
        char key = wait_keypress();

        if (key == 'w' && cursor_y > 0) {
            --cursor_y;
        } else if (key == 's' && cursor_y + 1 < VGA_HEIGHT) {
            ++cursor_y;
        } else if (key == 'a' && cursor_x > 0) {
            --cursor_x;
        } else if (key == 'd' && cursor_x + 1 < VGA_WIDTH) {
            ++cursor_x;
        } else if (key == 'q') {
            // Quick reset position for convenience
            cursor_x = 10;
            cursor_y = 8;
        }

        render();
    }
}
