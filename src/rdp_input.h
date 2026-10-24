#ifndef RDP_INPUT_H
#define RDP_INPUT_H

#include <stdint.h>
#include <stddef.h>

/* FastPath input event flags */
#define RDP_KBD_FLAGS_RELEASE  0x01
#define RDP_KBD_FLAGS_EXTENDED 0x02

/* Builds a complete FastPath input payload containing one keyboard event
   (MS-RDPBCGR 2.2.8.1.2): fpInputHeader, 2-byte length, eventHeader and
   keyCode.  The buffer is written to the transport directly; it is not an
   MCS Send Data PDU.
   scancode: PC set-1 scan code (e.g. 0x1E for 'A', 0x1C for Enter).
   down: 1 = key press, 0 = release. extended: 1 for extended keys (arrows,
   right ctrl, delete, etc).
   out/outcap must be >= 16 bytes. Returns bytes written (5) or -1. */
int rdp_input_keyboard(uint8_t* out, size_t outcap, uint16_t scancode,
                       int down, int extended);

/* Builds a complete FastPath input payload containing a UNICODE keyboard event
   (MS-RDPBCGR 2.2.8.1.2, eventCode 4): fpInputHeader, length, eventHeader,
   unicodeCode (6 bytes).  One UTF-16 code unit per event; send a press and a
   release per character.  out/outcap must be >= 8 bytes. */
int rdp_input_unicode(uint8_t* out, size_t outcap, uint16_t codeunit, int down);

/* Builds a complete FastPath input payload containing a mouse move (absolute
   position in surface pixels): fpInputHeader, length, eventHeader, flags, x
   and y (10 bytes). Returns bytes written or -1. */int rdp_input_mouse_move(uint8_t* out, size_t outcap,
                         uint16_t x, uint16_t y, uint16_t width, uint16_t height);

/* Builds a complete FastPath input payload containing a mouse button event
   (10 bytes). button: 1 = left, 2 = right, 3 = middle. down: 1 press,
   0 release. */
int rdp_input_mouse_button(uint8_t* out, size_t outcap, int button, int down);

/* Same, but repeats the current pointer position in the event. */
int rdp_input_mouse_button_at(uint8_t* out, size_t outcap, int button, int down,
                              uint16_t x, uint16_t y);

/* Builds a FastPath pointer event carrying a wheel rotation (10 bytes).
   notches > 0 scrolls away from the user (up), < 0 towards the user (down);
   one notch is the usual "three lines" step Windows expects. */
int rdp_input_mouse_wheel_at(uint8_t* out, size_t outcap, int notches,
                             uint16_t x, uint16_t y);

/* Translate a USB HID keyboard usage id (libnx HidKeyboardKey, 4..231) into a
   PC set-1 scancode. Returns 0 if unsupported, and sets *extended when the key
   is an extended scancode. */
uint16_t rdp_scancode_from_hid(uint8_t hid_usage, int* extended);

/* Translate a printable ASCII character plus shift state into a PC set-1
   scancode. Returns 0 if unsupported. */
uint16_t rdp_scancode_from_ascii(char c, int shift, int* extended);


/* Slow-path (MCS Send Data) input events, MS-RDPBCGR 2.2.8.1.1.
 * Each event is 12 bytes and is preceded by numberEvents(2) pad2Octets(2). */
size_t rdp_input_event_keyboard(uint8_t* out, size_t outcap, uint16_t scancode,
                                int down, int extended);
size_t rdp_input_event_mouse(uint8_t* out, size_t outcap, uint16_t pointer_flags,
                             uint16_t x, uint16_t y);

#endif
