#include <PrismaUI/PrismaUI.h>

namespace PrismaUI::SkyrimKeyHandler {
    using namespace ultralight::KeyCodes;

    int ConvertSkyrimDXScanCodeToUltralight(uint32_t skyrimDXScanCode) {
        switch (skyrimDXScanCode) {
        case 0x01: return GK_ESCAPE;
        case 0x02: return GK_1;
        case 0x03: return GK_2;
        case 0x04: return GK_3;
        case 0x05: return GK_4;
        case 0x06: return GK_5;
        case 0x07: return GK_6;
        case 0x08: return GK_7;
        case 0x09: return GK_8;
        case 0x0A: return GK_9;
        case 0x0B: return GK_0;
        case 0x0C: return GK_OEM_MINUS;
        case 0x0D: return GK_OEM_PLUS;
        case 0x0E: return GK_BACK;
        case 0x0F: return GK_TAB;
        case 0x10: return GK_Q;
        case 0x11: return GK_W;
        case 0x12: return GK_E;
        case 0x13: return GK_R;
        case 0x14: return GK_T;
        case 0x15: return GK_Y;
        case 0x16: return GK_U;
        case 0x17: return GK_I;
        case 0x18: return GK_O;
        case 0x19: return GK_P;
        case 0x1A: return GK_OEM_4;
        case 0x1B: return GK_OEM_6;
        case 0x1C: return GK_RETURN;
        case 0x1D: return GK_LCONTROL;
        case 0x1E: return GK_A;
        case 0x1F: return GK_S;
        case 0x20: return GK_D;
        case 0x21: return GK_F;
        case 0x22: return GK_G;
        case 0x23: return GK_H;
        case 0x24: return GK_J;
        case 0x25: return GK_K;
        case 0x26: return GK_L;
        case 0x27: return GK_OEM_1;
        case 0x28: return GK_OEM_7;
        case 0x29: return GK_OEM_3;
        case 0x2A: return GK_LSHIFT;
        case 0x2B: return GK_OEM_5;
        case 0x2C: return GK_Z;
        case 0x2D: return GK_X;
        case 0x2E: return GK_C;
        case 0x2F: return GK_V;
        case 0x30: return GK_B;
        case 0x31: return GK_N;
        case 0x32: return GK_M;
        case 0x33: return GK_OEM_COMMA;
        case 0x34: return GK_OEM_PERIOD;
        case 0x35: return GK_OEM_2;
        case 0x36: return GK_RSHIFT;
        case 0x37: return GK_MULTIPLY;
        case 0x38: return GK_LMENU;
        case 0x39: return GK_SPACE;
        case 0x3A: return GK_CAPITAL;
        case 0x3B: return GK_F1;
        case 0x3C: return GK_F2;
        case 0x3D: return GK_F3;
        case 0x3E: return GK_F4;
        case 0x3F: return GK_F5;
        case 0x40: return GK_F6;
        case 0x41: return GK_F7;
        case 0x42: return GK_F8;
        case 0x43: return GK_F9;
        case 0x44: return GK_F10;
        case 0x45: return GK_NUMLOCK;
        case 0x46: return GK_SCROLL;
        case 0x47: return GK_NUMPAD7;
        case 0x48: return GK_NUMPAD8;
        case 0x49: return GK_NUMPAD9;
        case 0x4A: return GK_SUBTRACT;
        case 0x4B: return GK_NUMPAD4;
        case 0x4C: return GK_NUMPAD5;
        case 0x4D: return GK_NUMPAD6;
        case 0x4E: return GK_ADD;
        case 0x4F: return GK_NUMPAD1;
        case 0x50: return GK_NUMPAD2;
        case 0x51: return GK_NUMPAD3;
        case 0x52: return GK_NUMPAD0;
        case 0x53: return GK_DECIMAL;
        case 0x57: return GK_F11;
        case 0x58: return GK_F12;

        case 0xB7: return GK_SNAPSHOT;

        case 0xC5: return GK_PAUSE;

        case 0xC7: return GK_HOME;
        case 0xC8: return GK_UP;
        case 0xC9: return GK_PRIOR;
        case 0xCB: return GK_LEFT;
        case 0xCD: return GK_RIGHT;
        case 0xCF: return GK_END;
        case 0xD0: return GK_DOWN;
        case 0xD1: return GK_NEXT;
        case 0xD2: return GK_INSERT;
        case 0xD3: return GK_DELETE;

        default:
            return 0;
        }
    }
}
