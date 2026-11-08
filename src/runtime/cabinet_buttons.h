#pragma once
enum { CAB_P1_START = 1u << 0, CAB_P2_START = 1u << 1,
       CAB_TEST     = 1u << 2, CAB_SERVICE  = 1u << 3,
       CAB_P1_TRIGGER = 1u << 4, CAB_P1_RELOAD = 1u << 5,
       CAB_P1_ACTION  = 1u << 6, CAB_P1_FIREMODE = 1u << 7,
       /* the second gun: P2's the same four, shifted by CAB_P2_SHIFT */
       CAB_P2_TRIGGER = 1u << 8, CAB_P2_RELOAD = 1u << 9,
       CAB_P2_ACTION  = 1u << 10, CAB_P2_FIREMODE = 1u << 11,
       /* A lever cabinet (Virtua Tennis 3): the 8-way stick and the two
        * buttons past the first (the first is CAB_P1_TRIGGER, switch 0x02).
        * P2's are the same six, shifted by CAB_P2_JOY_SHIFT. */
       CAB_P1_UP   = 1u << 12, CAB_P1_DOWN  = 1u << 13,
       CAB_P1_LEFT = 1u << 14, CAB_P1_RIGHT = 1u << 15,
       CAB_P1_BUTTON2 = 1u << 16,      /* byte 0, 0x01 (VT3: lob) */
       CAB_P1_BUTTON3 = 1u << 17,      /* byte 1, 0x80 (VT3: slice) */
       CAB_P2_UP   = 1u << 20, CAB_P2_DOWN  = 1u << 21,
       CAB_P2_LEFT = 1u << 22, CAB_P2_RIGHT = 1u << 23,
       CAB_P2_BUTTON2 = 1u << 24, CAB_P2_BUTTON3 = 1u << 25 };
#define CAB_P2_SHIFT 4
#define CAB_P2_JOY_SHIFT 8
