#ifndef __PS2_INPUT_H__
#define __PS2_INPUT_H__

#include <Arduino.h>

// ------------------------------------------------------------
// PS/2 keyboard -> USB HID keyboard report
//
// PS/2 DATA  = GPIO32
// PS/2 CLOCK = GPIO33
//
// output:
// A1 01 MOD 00 KEY KEY KEY KEY KEY KEY
// ------------------------------------------------------------

#define PS2_DATA_PIN 32
#define PS2_CLOCK_PIN 33

static volatile uint8_t ps2_bit = 0;
static volatile uint8_t ps2_data = 0;
static volatile uint8_t ps2_parity = 0;

static volatile uint8_t ps2_fifo[32];
static volatile uint8_t ps2_fifo_r = 0;
static volatile uint8_t ps2_fifo_w = 0;

static uint8_t ps2_keys[6] = {0};
static uint8_t ps2_modifiers = 0;

static bool ps2_dirty = false;
static bool ps2_break = false;
static bool ps2_extended = false;
static bool ps2_initialized = false;

// Software NumLock state; LED control intentionally disabled for stability.
static bool ps2_numlock = true;
static bool ps2_numlock_pressed = false;
static bool ps2_capslock = false;
static bool ps2_scrolllock = false;
static bool ps2_capslock_pressed = false;
static bool ps2_scrolllock_pressed = false;
static bool ps2_led_initial_sync = false;

// Interrupt-driven host->keyboard TX state.
// This follows the proven PS2KeyAdvanced design: host requests-to-send,
// then the keyboard clocks every transmitted bit and the ISR advances TX.
static volatile bool ps2_tx_mode = false;
static volatile uint8_t ps2_tx_bit = 0;
static volatile uint8_t ps2_tx_shift = 0;
static volatile uint8_t ps2_tx_parity = 0;
static volatile bool ps2_tx_done = false;


static void IRAM_ATTR ps2_tx_isr_bit();

// ============================================================
// PS/2 CLOCK interrupt
// data is sampled on falling edge
// ============================================================

static void IRAM_ATTR ps2_clock_isr()
{
    // During host->keyboard transmission, the keyboard still owns CLOCK.
    // Each falling edge advances one TX bit.
    if (ps2_tx_mode)
    {
        ps2_tx_isr_bit();
        return;
    }

    uint8_t d = digitalRead(PS2_DATA_PIN);

    // bit 0 = start
    if (ps2_bit == 0)
    {
        if (d == 0)
        {
            ps2_bit = 1;
            ps2_data = 0;
            ps2_parity = 0;
        }
        return;
    }

    // bits 1..8 = data, LSB first
    if (ps2_bit >= 1 && ps2_bit <= 8)
    {

        uint8_t n = ps2_bit - 1;

        if (d)
        {
            ps2_data |= (1 << n);
            ps2_parity ^= 1;
        }

        ps2_bit++;
        return;
    }

    // bit 9 = odd parity
    if (ps2_bit == 9)
    {

        // total parity including parity bit must be odd
        ps2_parity ^= d;

        ps2_bit++;
        return;
    }

    // bit 10 = stop
    if (ps2_bit == 10)
    {

        if (d == 1 && ps2_parity == 1)
        {

            uint8_t next = (ps2_fifo_w + 1) & 31;

            if (next != ps2_fifo_r)
            {
                ps2_fifo[ps2_fifo_w] = ps2_data;
                ps2_fifo_w = next;
            }
        }

        ps2_bit = 0;
    }
}

// ============================================================
// Host -> keyboard TX, adapted to the interrupt-driven strategy used
// by PS2KeyAdvanced on ESP32.
// ============================================================

static void ps2_data_low()
{
    pinMode(PS2_DATA_PIN, OUTPUT);
    digitalWrite(PS2_DATA_PIN, LOW);
}

static void ps2_data_high()
{
    // PS/2 is open-collector: logical HIGH means release the line.
    pinMode(PS2_DATA_PIN, INPUT_PULLUP);
}

static void IRAM_ATTR ps2_tx_isr_bit()
{
    // ESP32 path starts with tx_bit=1 because the request-to-send start
    // bit is already present when the interrupt is reattached.
    ps2_tx_bit++;

    switch (ps2_tx_bit)
    {
        // 2..9 = eight data bits, LSB first.
        case 2: case 3: case 4: case 5:
        case 6: case 7: case 8: case 9:
        {
            uint8_t bit = ps2_tx_shift & 1;
            if (bit)
            {
                pinMode(PS2_DATA_PIN, INPUT_PULLUP);
                ps2_tx_parity++;
            }
            else
            {
                pinMode(PS2_DATA_PIN, OUTPUT);
                digitalWrite(PS2_DATA_PIN, LOW);
            }
            ps2_tx_shift >>= 1;
            break;
        }

        // Odd parity.
        case 10:
            if ((~ps2_tx_parity) & 1)
                pinMode(PS2_DATA_PIN, INPUT_PULLUP);
            else
            {
                pinMode(PS2_DATA_PIN, OUTPUT);
                digitalWrite(PS2_DATA_PIN, LOW);
            }
            break;

        // Stop bit = release DATA.
        case 11:
            pinMode(PS2_DATA_PIN, INPUT_PULLUP);
            break;

        // Keyboard ACK bit clock. Return immediately to RX mode.
        case 12:
            ps2_tx_mode = false;
            ps2_tx_done = true;
            ps2_tx_bit = 0;
            // RX ISR will handle the following 0xFA response byte.
            break;

        default:
            ps2_tx_bit = 0;
            ps2_tx_mode = false;
            ps2_tx_done = true;
            break;
    }
}

static bool ps2_wait_bus_idle(uint32_t timeout_us)
{
    uint32_t start = micros();
    while (!digitalRead(PS2_CLOCK_PIN) || !digitalRead(PS2_DATA_PIN))
    {
        if ((uint32_t)(micros() - start) >= timeout_us)
            return false;
    }
    return true;
}

static bool ps2_send_byte_irq(uint8_t value)
{
    if (!ps2_wait_bus_idle(10000))
        return false;

    // Match PS2KeyAdvanced's ESP32 send startup sequence.
    detachInterrupt(digitalPinToInterrupt(PS2_CLOCK_PIN));

    ps2_tx_shift = value;
    ps2_tx_parity = 0;
    ps2_tx_bit = 1;       // ESP32 workaround: start bit already established
    ps2_tx_done = false;
    ps2_tx_mode = true;

    // Put both lines high/released first.
    pinMode(PS2_DATA_PIN, OUTPUT);
    digitalWrite(PS2_DATA_PIN, HIGH);
    pinMode(PS2_CLOCK_PIN, OUTPUT);
    digitalWrite(PS2_CLOCK_PIN, HIGH);

    delayMicroseconds(10);

    // Host inhibits keyboard by holding CLOCK low.
    digitalWrite(PS2_CLOCK_PIN, LOW);
    delayMicroseconds(60);

    // Request-to-send start bit.
    digitalWrite(PS2_DATA_PIN, LOW);

    // Release CLOCK; keyboard now clocks the transmission.
    pinMode(PS2_CLOCK_PIN, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PS2_CLOCK_PIN),
                    ps2_clock_isr, FALLING);

    uint32_t start = millis();
    while (!ps2_tx_done)
    {
        if ((uint32_t)(millis() - start) > 100)
        {
            ps2_tx_mode = false;
            ps2_tx_done = false;
            ps2_tx_bit = 0;
            pinMode(PS2_DATA_PIN, INPUT_PULLUP);
            pinMode(PS2_CLOCK_PIN, INPUT_PULLUP);
            return false;
        }
        delay(1);
    }

    ps2_tx_done = false;
    pinMode(PS2_DATA_PIN, INPUT_PULLUP);
    pinMode(PS2_CLOCK_PIN, INPUT_PULLUP);
    return true;
}


// ============================================================
// read byte from FIFO
// ============================================================

static bool ps2_read_byte(uint8_t *b)
{
    if (ps2_fifo_r == ps2_fifo_w)
        return false;

    noInterrupts();

    *b = ps2_fifo[ps2_fifo_r];
    ps2_fifo_r = (ps2_fifo_r + 1) & 31;

    interrupts();

    return true;
}

static bool ps2_wait_reply(uint8_t wanted, uint32_t timeout_ms)
{
    uint32_t start = millis();

    while ((uint32_t)(millis() - start) < timeout_ms)
    {
        uint8_t b;
        if (ps2_read_byte(&b))
        {
            if (b == wanted)
                return true;

            if (b == 0xFE) // RESEND
                return false;

            // Ignore unrelated protocol bytes here.
        }
        delay(1);
    }

    return false;
}

static bool ps2_command(uint8_t value)
{
    // Clear stale protocol bytes before a command.
    uint8_t junk;
    while (ps2_read_byte(&junk)) {}

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!ps2_send_byte_irq(value))
            continue;

        if (ps2_wait_reply(0xFA, 100))
            return true;
    }

    return false;
}

static bool ps2_update_leds()
{
    uint8_t mask = 0;
    if (ps2_scrolllock) mask |= 0x01;
    if (ps2_numlock)    mask |= 0x02;
    if (ps2_capslock)   mask |= 0x04;

    if (!ps2_command(0xED))
        return false;

    if (!ps2_command(mask))
        return false;

    return true;
}


// ============================================================
// PS/2 Set 2 -> USB HID
// ============================================================

static uint8_t ps2_to_hid(uint8_t s, bool ext)
{
    if (ext)
    {
        switch (s)
        {

        case 0x75:
            return 0x52; // Up
        case 0x72:
            return 0x51; // Down
        case 0x6B:
            return 0x50; // Left
        case 0x74:
            return 0x4F; // Right

        case 0x70:
            return 0x49; // Insert
        case 0x71:
            return 0x4C; // Delete
        case 0x6C:
            return 0x4A; // Home
        case 0x69:
            return 0x4D; // End
        case 0x7D:
            return 0x4B; // Page Up
        case 0x7A:
            return 0x4E; // Page Down

        case 0x5A:
            return 0x28; // keypad Enter -> Enter
        case 0x4A:
            return 0x38; // keypad / -> /

        default:
            return 0;
        }
    }

    switch (s)
    {

    // letters
    case 0x1C:
        return 0x04; // A
    case 0x32:
        return 0x05; // B
    case 0x21:
        return 0x06; // C
    case 0x23:
        return 0x07; // D
    case 0x24:
        return 0x08; // E
    case 0x2B:
        return 0x09; // F
    case 0x34:
        return 0x0A; // G
    case 0x33:
        return 0x0B; // H
    case 0x43:
        return 0x0C; // I
    case 0x3B:
        return 0x0D; // J
    case 0x42:
        return 0x0E; // K
    case 0x4B:
        return 0x0F; // L
    case 0x3A:
        return 0x10; // M
    case 0x31:
        return 0x11; // N
    case 0x44:
        return 0x12; // O
    case 0x4D:
        return 0x13; // P
    case 0x15:
        return 0x14; // Q
    case 0x2D:
        return 0x15; // R
    case 0x1B:
        return 0x16; // S
    case 0x2C:
        return 0x17; // T
    case 0x3C:
        return 0x18; // U
    case 0x2A:
        return 0x19; // V
    case 0x1D:
        return 0x1A; // W
    case 0x22:
        return 0x1B; // X
    case 0x35:
        return 0x1C; // Y
    case 0x1A:
        return 0x1D; // Z

    // numbers
    case 0x16:
        return 0x1E; // 1
    case 0x1E:
        return 0x1F; // 2
    case 0x26:
        return 0x20; // 3
    case 0x25:
        return 0x21; // 4
    case 0x2E:
        return 0x22; // 5
    case 0x36:
        return 0x23; // 6
    case 0x3D:
        return 0x24; // 7
    case 0x3E:
        return 0x25; // 8
    case 0x46:
        return 0x26; // 9
    case 0x45:
        return 0x27; // 0

    // basic keys
    case 0x5A:
        return 0x28; // Enter
    case 0x76:
        return 0x29; // Escape
    case 0x66:
        return 0x2A; // Backspace
    case 0x0D:
        return 0x2B; // Tab
    case 0x29:
        return 0x2C; // Space

    case 0x4E:
        return 0x2D; // -
    case 0x55:
        return 0x2E; // =
    case 0x54:
        return 0x2F; // [
    case 0x5B:
        return 0x30; // ]
    case 0x5D:
        return 0x31; // backslash
    case 0x4C:
        return 0x33; // ;
    case 0x52:
        return 0x34; // '
    case 0x0E:
        return 0x35; // `
    case 0x41:
        return 0x36; // ,
    case 0x49:
        return 0x37; // .
    case 0x4A:
        return 0x38; // /

    case 0x58:
        return 0x39; // CapsLock

    // F keys
    case 0x05:
        return 0x3A; // F1
    case 0x06:
        return 0x3B; // F2
    case 0x04:
        return 0x3C; // F3
    case 0x0C:
        return 0x3D; // F4
    case 0x03:
        return 0x3E; // F5
    case 0x0B:
        return 0x3F; // F6
    case 0x83:
        return 0x40; // F7
    case 0x0A:
        return 0x41; // F8
    case 0x01:
        return 0x42; // F9
    case 0x09:
        return 0x43; // F10
    case 0x78:
        return 0x44; // F11
    case 0x07:
        return 0x45; // F12

    // Numeric keypad.
    // NumLock ON: map to normal number-row HID codes so Atari understands them.
    // NumLock OFF: map to navigation HID codes.
    case 0x70: return ps2_numlock ? 0x27 : 0x49; // KP0 -> 0 / Insert
    case 0x69: return ps2_numlock ? 0x1E : 0x4D; // KP1 -> 1 / End
    case 0x72: return ps2_numlock ? 0x1F : 0x51; // KP2 -> 2 / Down
    case 0x7A: return ps2_numlock ? 0x20 : 0x4E; // KP3 -> 3 / PgDn
    case 0x6B: return ps2_numlock ? 0x21 : 0x50; // KP4 -> 4 / Left
    case 0x73: return ps2_numlock ? 0x22 : 0x00; // KP5 -> 5 / no nav
    case 0x74: return ps2_numlock ? 0x23 : 0x4F; // KP6 -> 6 / Right
    case 0x6C: return ps2_numlock ? 0x24 : 0x4A; // KP7 -> 7 / Home
    case 0x75: return ps2_numlock ? 0x25 : 0x52; // KP8 -> 8 / Up
    case 0x7D: return ps2_numlock ? 0x26 : 0x4B; // KP9 -> 9 / PgUp
    case 0x71: return ps2_numlock ? 0x37 : 0x4C; // KP. -> . / Delete

    case 0x7C: return 0x25; // KP* -> 8 (usable Atari mapping)
    case 0x7B: return 0x2D; // KP- -> -
    case 0x79: return 0x2E; // KP+ -> =/+ key

    case 0x77: return 0x53; // NumLock (normally consumed in ps2_process)
    case 0x7E: return 0x47; // ScrollLock
    }

    return 0;
}

// ============================================================
// modifiers
// ============================================================

static uint8_t ps2_modifier(uint8_t s, bool ext)
{
    if (!ext)
    {
        switch (s)
        {
        case 0x14:
            return 0x01; // Left Ctrl
        case 0x12:
            return 0x02; // Left Shift
        case 0x11:
            return 0x04; // Left Alt
        case 0x59:
            return 0x20; // Right Shift
        }
    }
    else
    {
        switch (s)
        {
        case 0x14:
            return 0x10; // Right Ctrl
        case 0x11:
            return 0x40; // Right Alt
        case 0x1F:
            return 0x08; // Left GUI
        case 0x27:
            return 0x80; // Right GUI
        }
    }

    return 0;
}

// ============================================================
// add/remove HID key
// ============================================================

static void ps2_key_down(uint8_t key)
{
    if (!key)
        return;

    // already held
    for (int i = 0; i < 6; i++)
        if (ps2_keys[i] == key)
            return;

    for (int i = 0; i < 6; i++)
    {
        if (!ps2_keys[i])
        {
            ps2_keys[i] = key;
            ps2_dirty = true;
            return;
        }
    }
}

static void ps2_key_up(uint8_t key)
{
    if (!key)
        return;

    for (int i = 0; i < 6; i++)
    {
        if (ps2_keys[i] == key)
        {
            ps2_keys[i] = 0;
            ps2_dirty = true;
        }
    }
}

// ============================================================
// process PS/2 bytes
// ============================================================

static void ps2_process()
{
    uint8_t s;

    while (ps2_read_byte(&s))
    {

        if (s == 0xE0)
        {
            ps2_extended = true;
            continue;
        }

        if (s == 0xF0)
        {
            ps2_break = true;
            continue;
        }

        // ignore BAT / ACK etc.
        if (s == 0xAA || s == 0xFA)
        {
            ps2_break = false;
            ps2_extended = false;
            continue;
        }

        // NumLock: software state + keyboard LED.
        if (!ps2_extended && s == 0x77)
        {
            bool update_leds = false;

            if (!ps2_break)
            {
                if (!ps2_numlock_pressed)
                {
                    ps2_numlock = !ps2_numlock;
                    ps2_numlock_pressed = true;
                    update_leds = true;
                }
            }
            else
            {
                ps2_numlock_pressed = false;
            }

            ps2_break = false;
            ps2_extended = false;

            if (update_leds)
                (void)ps2_update_leds();

            continue;
        }

        // CapsLock: keep sending the normal HID CapsLock key, but mirror LED.
        if (!ps2_extended && s == 0x58)
        {
            if (!ps2_break && !ps2_capslock_pressed)
            {
                ps2_capslock = !ps2_capslock;
                ps2_capslock_pressed = true;
                (void)ps2_update_leds();
            }
            else if (ps2_break)
            {
                ps2_capslock_pressed = false;
            }
        }

        // ScrollLock LED state. The HID key is still passed through normally.
        if (!ps2_extended && s == 0x7E)
        {
            if (!ps2_break && !ps2_scrolllock_pressed)
            {
                ps2_scrolllock = !ps2_scrolllock;
                ps2_scrolllock_pressed = true;
                (void)ps2_update_leds();
            }
            else if (ps2_break)
            {
                ps2_scrolllock_pressed = false;
            }
        }

        uint8_t mod = ps2_modifier(s, ps2_extended);

        if (mod)
        {

            if (ps2_break)
            {
                ps2_modifiers &= ~mod;
            }
            else
            {
                ps2_modifiers |= mod;
            }

            // Dolezite:
            // aj samotny Shift/Ctrl/Alt musi vygenerovat HID report
            ps2_dirty = true;
        }
        else
        {

            uint8_t key = ps2_to_hid(s, ps2_extended);

            if (ps2_break)
                ps2_key_up(key);
            else
                ps2_key_down(key);
        }

        ps2_break = false;
        ps2_extended = false;
    }
}

// ============================================================
// init
// ============================================================

static void ps2_init()
{
    if (ps2_initialized)
        return;

    ps2_initialized = true;

    pinMode(PS2_DATA_PIN, INPUT_PULLUP);
    pinMode(PS2_CLOCK_PIN, INPUT_PULLUP);

    attachInterrupt(
        digitalPinToInterrupt(PS2_CLOCK_PIN),
        ps2_clock_isr,
        FALLING);

    memset(ps2_keys, 0, sizeof(ps2_keys));

    ps2_modifiers = 0;
    ps2_dirty = false;
    ps2_break = false;
    ps2_extended = false;
}

// ============================================================
// Generate exactly the HID packet esp_8_bit expects:
//
// A1 01 modifiers 00 key1 key2 key3 key4 key5 key6
// ============================================================

static int ps2_get_hid(uint8_t *dst)
{
    ps2_init();
    ps2_process();

    // Synchronize physical LEDs once after keyboard startup.
    if (!ps2_led_initial_sync && millis() > 1500)
    {
        ps2_led_initial_sync = true;
        (void)ps2_update_leds();
    }

    if (!ps2_dirty)
        return 0;

    ps2_dirty = false;

    dst[0] = 0xA1;
    dst[1] = 0x01;
    dst[2] = ps2_modifiers;
    dst[3] = 0;

    int j = 0;

    for (int i = 0; i < 6; i++)
        if (ps2_keys[i])
            dst[4 + j++] = ps2_keys[i];

    while (j < 6)
        dst[4 + j++] = 0;

    return 10;
}

#endif