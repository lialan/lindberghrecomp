#pragma once
enum { CAB_P1_START = 1u << 0, CAB_P2_START = 1u << 1,
       CAB_TEST     = 1u << 2, CAB_SERVICE  = 1u << 3,
       CAB_P1_TRIGGER = 1u << 4, CAB_P1_RELOAD = 1u << 5,
       CAB_P1_ACTION  = 1u << 6, CAB_P1_FIREMODE = 1u << 7,
       /* the second gun: P2's the same four, shifted by CAB_P2_SHIFT */
       CAB_P2_TRIGGER = 1u << 8, CAB_P2_RELOAD = 1u << 9,
       CAB_P2_ACTION  = 1u << 10, CAB_P2_FIREMODE = 1u << 11 };
#define CAB_P2_SHIFT 4
