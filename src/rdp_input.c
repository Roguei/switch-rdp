/*
 * rdp_input - FastPath input event encoding and HID/ASCII scancode maps.
 *
 * Payloads follow MS-RDPBCGR 2.2.8.1.2 (Client Fast-Path Input Event PDU).
 * They are complete transport payloads: the session writes them to the
 * socket/TLS directly, they are not MCS Send Data PDUs and carry no TPKT or
 * X.224 header.
 *
 *   keyboard event (5 bytes):
 *     [0]      0x04  fpInputHeader: action FASTPATH (bits 0..1 = 0),
 *                    numberOfEvents = 1 (bits 2..5)
 *     [1..2]   0x80 0x05  packet length, 2-byte big-endian form: the 0x80
 *                    bit of the first byte selects the 16-bit length and the
 *                    value counts the whole PDU including these 3 bytes
 *     [3]            eventHeader: event flags in bits 0..4
 *                    (RELEASE 0x01, EXTENDED 0x02), eventCode 0 (keyboard)
 *                    in bits 5..7
 *     [4]            keyCode (PC set-1 scancode)
 *
 *   mouse event (10 bytes):
 *     [0]      0x04  fpInputHeader (action FASTPATH, numberOfEvents = 1)
 *     [1..2]   0x80 0x0A  packet length, 2-byte big-endian form
 *     [3]      0x20  eventHeader: eventCode 1 (mouse) in bits 5..7
 *     [4..5]         pointerFlags (little endian)
 *     [6..7]         xPos (little endian)
 *     [8..9]         yPos (little endian)
 *
 * Scancodes are PC set-1.  Extended keys carry the 0xE0 prefix implicitly
 * through the extended flag, so the scancode itself stays an 8-bit value.
 */

#include <stdint.h>
#include <stddef.h>

#include "rdp_input.h"

/* ------------------------------------------------------------------ */
/* packet constants                                                   */
/* ------------------------------------------------------------------ */

/* fpInputHeader for one unencrypted event: action = 0, numEvents = 1. */
#define RDP_FP_INPUT_HDR    0x04u

/* eventHeader = eventFlags | (eventCode << 5).  Captured from a working
 * FreeRDP 3.5.1 session against this server:
 *   keyboard: 04 80 05 02 5b   (eventCode 0, flags 0x02 = extended)
 *   mouse   : 04 80 0a 20 00 08 80 02 90 01  (eventCode 1, flags 0x0800)
 * A wrong eventCode makes the server either drop the PDU or reset the
 * connection, so both values are pinned to the capture. */
#define RDP_FP_EVENT_KBD     (0u << 5)  /* 0x00 */
#define RDP_FP_EVENT_MOUSE   (1u << 5)  /* 0x20 */
#define RDP_FP_EVENT_UNICODE (4u << 5)  /* 0x80, SC_UNICODE in fastpath.h */

/* Fast-Path keyboard flags live in eventHeader bits 0..2. */
#define RDP_FP_KBD_RELEASE  0x01u
#define RDP_FP_KBD_EXTENDED 0x02u

/* First length byte flag selecting the 16-bit length form. */
#define RDP_FP_LEN16        0x80u

#define RDP_PTRFLAGS_BUTTON1 0x1000u
#define RDP_PTRFLAGS_BUTTON2 0x2000u
#define RDP_PTRFLAGS_BUTTON3 0x4000u
#define RDP_PTRFLAGS_DOWN    0x8000u
#define RDP_PTRFLAGS_MOVE    0x0800u
/* Wheel: PTRFLAGS_WHEEL (0x0200) | rotation (0x01FF), the negative direction
 * is signalled with PTRFLAGS_WHEEL_NEGATIVE (0x0100).  One notch = 120. */
#define RDP_PTRFLAGS_WHEEL          0x0200u
#define RDP_PTRFLAGS_WHEEL_NEGATIVE 0x0100u
#define RDP_WHEEL_MASK              0x01FFu
#define RDP_WHEEL_NOTCH             120

#define RDP_KBD_EVENT_SIZE   5u
#define RDP_MOUSE_EVENT_SIZE 10u
/* fpInputHeader(1) + length(2) + eventHeader(1) + unicodeCode(2) */
#define RDP_UNICODE_EVENT_SIZE 6u

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void put_u16le(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* Writes fpInputHeader + 2-byte big-endian length. */
static void put_fp_header(uint8_t* out, uint16_t total_len)
{
    out[0] = (uint8_t)RDP_FP_INPUT_HDR;
    out[1] = (uint8_t)(RDP_FP_LEN16 | (uint8_t)(total_len >> 8));
    out[2] = (uint8_t)(total_len & 0xFFu);
}

/* PC set-1 scancodes for USB HID usages 4..29 (A..Z). */
static const uint8_t k_letter_scancode[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
    0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
    0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
};

static uint16_t mouse_button_flag(int button)
{
    switch (button)
    {
        case 1: return RDP_PTRFLAGS_BUTTON1;
        case 2: return RDP_PTRFLAGS_BUTTON2;
        case 3: return RDP_PTRFLAGS_BUTTON3;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* FastPath input builders                                            */
/* ------------------------------------------------------------------ */

int rdp_input_keyboard(uint8_t* out, size_t outcap, uint16_t scancode,
                       int down, int extended)
{
    uint8_t flags = 0;

    if (!out || outcap < (size_t)RDP_KBD_EVENT_SIZE)
        return -1;
    if (scancode > 0xFFu)
        return -1;                  /* set-1 scancodes are 8-bit */

    if (!down)
        flags |= RDP_KBD_FLAGS_RELEASE;
    if (extended)
        flags |= RDP_KBD_FLAGS_EXTENDED;

    put_fp_header(out, (uint16_t)RDP_KBD_EVENT_SIZE);
    out[3] = (uint8_t)(RDP_FP_EVENT_KBD | flags);
    out[4] = (uint8_t)scancode;
    return (int)RDP_KBD_EVENT_SIZE;
}

/* SC_UNICODE: one UTF-16 code unit, eventCode 4 -> eventHeader 0x80.
 *   [0] 0x04  [1..2] 0x80 0x06  [3] 0x80 | flags  [4..5] unicodeCode (LE)
 * Windows turns each press into a WM_CHAR, so the text is typed into whatever
 * control has focus regardless of the local keyboard layout - this is what the
 * on-screen keyboard feeds. */
int rdp_input_unicode(uint8_t* out, size_t outcap, uint16_t codeunit, int down)
{
    uint8_t flags = 0;

    if (!out || outcap < (size_t)RDP_UNICODE_EVENT_SIZE)
        return -1;
    if (!down)
        flags |= RDP_KBD_FLAGS_RELEASE;

    put_fp_header(out, (uint16_t)RDP_UNICODE_EVENT_SIZE);
    out[3] = (uint8_t)(RDP_FP_EVENT_UNICODE | flags);
    put_u16le(out + 4, codeunit);
    return (int)RDP_UNICODE_EVENT_SIZE;
}

int rdp_input_mouse_move(uint8_t* out, size_t outcap,
                         uint16_t x, uint16_t y, uint16_t width, uint16_t height)

{
    if (!out || outcap < (size_t)RDP_MOUSE_EVENT_SIZE)
        return -1;

    /* Keep the absolute position inside the announced surface. */
    if (width && x >= width)
        x = (uint16_t)(width - 1u);
    if (height && y >= height)
        y = (uint16_t)(height - 1u);

    put_fp_header(out, (uint16_t)RDP_MOUSE_EVENT_SIZE);
    out[3] = (uint8_t)RDP_FP_EVENT_MOUSE;
    put_u16le(out + 4, (uint16_t)RDP_PTRFLAGS_MOVE);
    put_u16le(out + 6, x);
    put_u16le(out + 8, y);
    return (int)RDP_MOUSE_EVENT_SIZE;
}

int rdp_input_mouse_button(uint8_t* out, size_t outcap, int button, int down)
{
    uint16_t flags = mouse_button_flag(button);

    if (!out || outcap < (size_t)RDP_MOUSE_EVENT_SIZE)
        return -1;
    if (!flags)
        return -1;
    if (down)
        flags |= RDP_PTRFLAGS_DOWN;

    put_fp_header(out, (uint16_t)RDP_MOUSE_EVENT_SIZE);
    out[3] = (uint8_t)RDP_FP_EVENT_MOUSE;
    put_u16le(out + 4, flags);
    put_u16le(out + 6, 0);
    put_u16le(out + 8, 0);
    return (int)RDP_MOUSE_EVENT_SIZE;
}

int rdp_input_mouse_button_at(uint8_t* out, size_t outcap, int button, int down,
                              uint16_t x, uint16_t y)
{
    uint16_t flags = mouse_button_flag(button);

    if (!out || outcap < (size_t)RDP_MOUSE_EVENT_SIZE)
        return -1;
    if (!flags)
        return -1;
    if (down)
        flags |= RDP_PTRFLAGS_DOWN;

    put_fp_header(out, (uint16_t)RDP_MOUSE_EVENT_SIZE);
    out[3] = (uint8_t)RDP_FP_EVENT_MOUSE;
    put_u16le(out + 4, flags);
    put_u16le(out + 6, x);
    put_u16le(out + 8, y);
    return (int)RDP_MOUSE_EVENT_SIZE;
}

int rdp_input_mouse_wheel_at(uint8_t* out, size_t outcap, int notches,
                             uint16_t x, uint16_t y)
{
    uint16_t flags = RDP_PTRFLAGS_WHEEL;
    int units;

    if (!out || outcap < (size_t)RDP_MOUSE_EVENT_SIZE)
        return -1;
    if (!notches)
        return -1;
    if (notches > 1) notches = 1;
    if (notches < -1) notches = -1;

    /* MS-RDPBCGR 2.2.8.1.1.3.1.1.3: WheelRotationMask (0x01FF) holds the number
     * of rotation units as a 9-bit two's complement value which the server
     * sign-extends; PTRFLAGS_WHEEL_NEGATIVE is bit 8 of that value, so -120
     * becomes 0x188 (and the flag comes for free). */
    units = notches * RDP_WHEEL_NOTCH;
    if (units < 0)
        flags |= (uint16_t)((0x200 - (-units)) & RDP_WHEEL_MASK);
    else
        flags |= (uint16_t)(units & RDP_WHEEL_MASK);

    put_fp_header(out, (uint16_t)RDP_MOUSE_EVENT_SIZE);
    out[3] = (uint8_t)RDP_FP_EVENT_MOUSE;
    put_u16le(out + 4, flags);
    put_u16le(out + 6, x);
    put_u16le(out + 8, y);
    return (int)RDP_MOUSE_EVENT_SIZE;
}

/* ------------------------------------------------------------------ */
/* USB HID usage -> PC set-1 scancode                                 */
/* ------------------------------------------------------------------ */

uint16_t rdp_scancode_from_hid(uint8_t hid_usage, int* extended)
{
    uint16_t sc = 0;
    int ext = 0;

    if (hid_usage >= 4u && hid_usage <= 29u)
    {
        sc = k_letter_scancode[(size_t)(hid_usage - 4u)];
    }
    else if (hid_usage >= 30u && hid_usage <= 39u)
    {
        /* 1..9 then 0 */
        sc = (uint16_t)(0x02u + (uint16_t)(hid_usage - 30u));
    }
    else
    {
        switch (hid_usage)
        {
            case 40: sc = 0x1C; break;          /* Enter                    */
            case 41: sc = 0x01; break;          /* Escape                   */
            case 42: sc = 0x0E; break;          /* Backspace                */
            case 43: sc = 0x0F; break;          /* Tab                      */
            case 44: sc = 0x39; break;          /* Space                    */
            case 45: sc = 0x0C; break;          /* -                        */
            case 46: sc = 0x0D; break;          /* =                        */
            case 47: sc = 0x1A; break;          /* [                        */
            case 48: sc = 0x1B; break;          /* ]                        */
            case 49: sc = 0x2B; break;          /* \                        */
            case 50: sc = 0x56; break;          /* non-US # / ~ (ISO key)   */
            case 51: sc = 0x27; break;          /* ;                        */
            case 52: sc = 0x28; break;          /* '                        */
            case 53: sc = 0x29; break;          /* `                        */
            case 54: sc = 0x33; break;          /* ,                        */
            case 55: sc = 0x34; break;          /* .                        */
            case 56: sc = 0x35; break;          /* /                        */
            case 57: sc = 0x3A; break;          /* Caps Lock                */

            case 58: sc = 0x3B; break;          /* F1                       */
            case 59: sc = 0x3C; break;          /* F2                       */
            case 60: sc = 0x3D; break;          /* F3                       */
            case 61: sc = 0x3E; break;          /* F4                       */
            case 62: sc = 0x3F; break;          /* F5                       */
            case 63: sc = 0x40; break;          /* F6                       */
            case 64: sc = 0x41; break;          /* F7                       */
            case 65: sc = 0x42; break;          /* F8                       */
            case 66: sc = 0x43; break;          /* F9                       */
            case 67: sc = 0x44; break;          /* F10                      */
            case 68: sc = 0x57; break;          /* F11                      */
            case 69: sc = 0x58; break;          /* F12                      */

            case 70: sc = 0x37; ext = 1; break; /* Print Screen             */
            case 71: sc = 0x46; break;          /* Scroll Lock              */
            /* 72 (Pause) needs the E1 prefix and is left unsupported. */

            case 73: sc = 0x52; ext = 1; break; /* Insert                   */
            case 74: sc = 0x47; ext = 1; break; /* Home                     */
            case 75: sc = 0x49; ext = 1; break; /* Page Up                  */
            case 76: sc = 0x53; ext = 1; break; /* Delete                   */
            case 77: sc = 0x4F; ext = 1; break; /* End                      */
            case 78: sc = 0x51; ext = 1; break; /* Page Down                */
            case 79: sc = 0x4D; ext = 1; break; /* Right Arrow              */
            case 80: sc = 0x4B; ext = 1; break; /* Left Arrow               */
            case 81: sc = 0x50; ext = 1; break; /* Down Arrow               */
            case 82: sc = 0x48; ext = 1; break; /* Up Arrow                 */

            case 83: sc = 0x45; break;          /* Num Lock                 */
            case 84: sc = 0x35; ext = 1; break; /* Keypad /                 */
            case 85: sc = 0x37; break;          /* Keypad *                 */
            case 86: sc = 0x4A; break;          /* Keypad -                 */
            case 87: sc = 0x4E; break;          /* Keypad +                 */
            case 88: sc = 0x1C; ext = 1; break; /* Keypad Enter             */
            case 89: sc = 0x4F; break;          /* Keypad 1                 */
            case 90: sc = 0x50; break;          /* Keypad 2                 */
            case 91: sc = 0x51; break;          /* Keypad 3                 */
            case 92: sc = 0x4B; break;          /* Keypad 4                 */
            case 93: sc = 0x4C; break;          /* Keypad 5                 */
            case 94: sc = 0x4D; break;          /* Keypad 6                 */
            case 95: sc = 0x47; break;          /* Keypad 7                 */
            case 96: sc = 0x48; break;          /* Keypad 8                 */
            case 97: sc = 0x49; break;          /* Keypad 9                 */
            case 98: sc = 0x52; break;          /* Keypad 0                 */
            case 99: sc = 0x53; break;          /* Keypad .                 */

            case 100: sc = 0x56; break;         /* non-US \ / | (ISO key)   */
            case 101: sc = 0x5D; ext = 1; break;/* Application              */

            /* Modifier usages (libnx HidKeyboardKey). */
            case 224: sc = 0x1D; break;         /* Left Ctrl                */
            case 225: sc = 0x2A; break;         /* Left Shift               */
            case 226: sc = 0x38; break;         /* Left Alt                 */
            case 227: sc = 0x5B; ext = 1; break;/* Left GUI                 */
            case 228: sc = 0x1D; ext = 1; break;/* Right Ctrl               */
            case 229: sc = 0x36; break;         /* Right Shift              */
            case 230: sc = 0x38; ext = 1; break;/* Right Alt                */
            case 231: sc = 0x5C; ext = 1; break;/* Right GUI                */

            default: break;
        }
    }

    if (extended)
        *extended = ext;
    return sc;
}

/* ------------------------------------------------------------------ */
/* printable ASCII -> PC set-1 scancode                               */
/* ------------------------------------------------------------------ */

uint16_t rdp_scancode_from_ascii(char c, int shift, int* extended)
{
    unsigned char u = (unsigned char)c;
    uint16_t sc = 0;

    /* PC set-1 scancodes do not change with shift; the caller sends the
       shift key itself.  The parameter is kept for API symmetry. */
    (void)shift;

    if (extended)
        *extended = 0;

    if (u >= (unsigned char)'a' && u <= (unsigned char)'z')
    {
        sc = k_letter_scancode[(size_t)(u - (unsigned char)'a')];
    }
    else if (u >= (unsigned char)'A' && u <= (unsigned char)'Z')
    {
        sc = k_letter_scancode[(size_t)(u - (unsigned char)'A')];
    }
    else if (u == (unsigned char)'0')
    {
        sc = 0x0B;                      /* '0' sits after '9' on the row */
    }
    else if (u >= (unsigned char)'1' && u <= (unsigned char)'9')
    {
        sc = (uint16_t)(0x02u + (uint16_t)(u - (unsigned char)'1'));
    }
    else
    {
        switch (u)
        {
            case ' ':  sc = 0x39; break;

            /* unshifted punctuation */
            case '-':  sc = 0x0C; break;
            case '=':  sc = 0x0D; break;
            case '[':  sc = 0x1A; break;
            case ']':  sc = 0x1B; break;
            case '\\': sc = 0x2B; break;
            case ';':  sc = 0x27; break;
            case '\'': sc = 0x28; break;
            case '`':  sc = 0x29; break;
            case ',':  sc = 0x33; break;
            case '.':  sc = 0x34; break;
            case '/':  sc = 0x35; break;

            /* shifted plane of the same keys */
            case '_':  sc = 0x0C; break;
            case '+':  sc = 0x0D; break;
            case '{':  sc = 0x1A; break;
            case '}':  sc = 0x1B; break;
            case '|':  sc = 0x2B; break;
            case ':':  sc = 0x27; break;
            case '"':  sc = 0x28; break;
            case '~':  sc = 0x29; break;
            case '<':  sc = 0x33; break;
            case '>':  sc = 0x34; break;
            case '?':  sc = 0x35; break;
            case '!':  sc = 0x02; break;
            case '@':  sc = 0x03; break;
            case '#':  sc = 0x04; break;
            case '$':  sc = 0x05; break;
            case '%':  sc = 0x06; break;
            case '^':  sc = 0x07; break;
            case '&':  sc = 0x08; break;
            case '*':  sc = 0x09; break;
            case '(':  sc = 0x0A; break;
            case ')':  sc = 0x0B; break;

            default: break;
        }
    }

    return sc;
}

/* ------------------------------------------------------------------ */
/* slow-path input events                                             */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

size_t rdp_input_event_keyboard(uint8_t* out, size_t outcap, uint16_t scancode,
                                int down, int extended)
{
    uint16_t kflags = 0;

    if (!out || outcap < 12u || scancode > 0xFFu)
        return 0;

    if (!down)       kflags |= 0x8000u;   /* KBDFLAGS_RELEASE  */
    if (extended)    kflags |= 0x0100u;   /* KBDFLAGS_EXTENDED */

    put_u32le(out + 0, 0);                /* eventTime            */
    put_u16le(out + 4, 0x0004u);          /* INPUT_EVENT_SCANCODE */
    put_u16le(out + 6, kflags);           /* keyboardFlags        */
    put_u16le(out + 8, scancode);         /* keyCode              */
    put_u16le(out + 10, 0);               /* pad2Octets           */
    return 12u;
}

size_t rdp_input_event_mouse(uint8_t* out, size_t outcap, uint16_t pointer_flags,
                             uint16_t x, uint16_t y)
{
    if (!out || outcap < 12u)
        return 0;

    put_u32le(out + 0, 0);                /* eventTime            */
    put_u16le(out + 4, 0x8001u);          /* INPUT_EVENT_MOUSE    */
    put_u16le(out + 6, pointer_flags);
    put_u16le(out + 8, x);
    put_u16le(out + 10, y);
    return 12u;
}
