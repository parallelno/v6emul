#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// ── KeyCode enum: must mirror dev::KeyCode from core/keyboard.h ──────
// Duplicated here to avoid linking v6core into the test client.
namespace KeyCode {
	enum : int {
		// Letters
		A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73, J = 74,
		K = 75, L = 76, M = 77, N = 78, O = 79, P = 80, Q = 81, R = 82, S = 83,
		T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,
		// Digits
		NUM_0 = 48, NUM_1 = 49, NUM_2 = 50, NUM_3 = 51, NUM_4 = 52, NUM_5 = 53,
		NUM_6 = 54, NUM_7 = 55, NUM_8 = 56, NUM_9 = 57,
		// Punctuation / symbols
		SPACE = 0x20, MINUS = 0xBD, PLUS = 0xBB, LBRACKET = 0xDB, RBRACKET = 0xDD,
		BACKSLASH = 0xDC, SEMICOLON = 0xBA, APOSTROPHE = 0xDE, GRAVE = 0xC0,
		COMMA = 0xBC, PERIOD = 0xBE, SLASH = 0xBF,
		// Function keys
		F1 = 0x70, F2 = 0x71, F3 = 0x72, F4 = 0x73,
		F5 = 0x74, F6 = 0x75, F7 = 0x76, F8 = 0x77,
		// Special keys
		TAB = 0x09, RETURN = 0x0D, BACKSPACE = 0x08, ESCAPE = 0x1B,
		// Arrow keys
		UP = 0x26, DOWN = 0x28, LEFT = 0x25, RIGHT = 0x27,
		// Modifier keys
		LSHIFT = 0xA0, RSHIFT = 0xA1, LCTRL = 0xA2, LGUI = 0x5B, LALT = 0xA4, RALT = 0xA5,
		// System
		F11 = 0x7A, F12 = 0x7B,
		INVALID = -1
	};
}

inline constexpr int KEY_ACTION_UP   = 0;
inline constexpr int KEY_ACTION_DOWN = 1;

inline int VkToKeyCode(WPARAM vk)
{
	switch (vk) {
	case 'A': return KeyCode::A;  case 'B': return KeyCode::B;
	case 'C': return KeyCode::C;  case 'D': return KeyCode::D;
	case 'E': return KeyCode::E;  case 'F': return KeyCode::F;
	case 'G': return KeyCode::G;  case 'H': return KeyCode::H;
	case 'I': return KeyCode::I;  case 'J': return KeyCode::J;
	case 'K': return KeyCode::K;  case 'L': return KeyCode::L;
	case 'M': return KeyCode::M;  case 'N': return KeyCode::N;
	case 'O': return KeyCode::O;  case 'P': return KeyCode::P;
	case 'Q': return KeyCode::Q;  case 'R': return KeyCode::R;
	case 'S': return KeyCode::S;  case 'T': return KeyCode::T;
	case 'U': return KeyCode::U;  case 'V': return KeyCode::V;
	case 'W': return KeyCode::W;  case 'X': return KeyCode::X;
	case 'Y': return KeyCode::Y;  case 'Z': return KeyCode::Z;
	case '0': return KeyCode::NUM_0;  case '1': return KeyCode::NUM_1;
	case '2': return KeyCode::NUM_2;  case '3': return KeyCode::NUM_3;
	case '4': return KeyCode::NUM_4;  case '5': return KeyCode::NUM_5;
	case '6': return KeyCode::NUM_6;  case '7': return KeyCode::NUM_7;
	case '8': return KeyCode::NUM_8;  case '9': return KeyCode::NUM_9;
	case VK_SPACE:      return KeyCode::SPACE;
	case VK_OEM_MINUS:  return KeyCode::MINUS;
	case VK_OEM_PLUS:   return KeyCode::PLUS;
	case VK_OEM_4:      return KeyCode::LBRACKET;
	case VK_OEM_6:      return KeyCode::RBRACKET;
	case VK_OEM_5:      return KeyCode::BACKSLASH;
	case VK_OEM_1:      return KeyCode::SEMICOLON;
	case VK_OEM_7:      return KeyCode::APOSTROPHE;
	case VK_OEM_3:      return KeyCode::GRAVE;
	case VK_OEM_COMMA:  return KeyCode::COMMA;
	case VK_OEM_PERIOD: return KeyCode::PERIOD;
	case VK_OEM_2:      return KeyCode::SLASH;
	case VK_F1:  return KeyCode::F1;  case VK_F2:  return KeyCode::F2;
	case VK_F3:  return KeyCode::F3;  case VK_F4:  return KeyCode::F4;
	case VK_F5:  return KeyCode::F5;  case VK_F6:  return KeyCode::F6;
	case VK_F7:  return KeyCode::F7;  case VK_F8:  return KeyCode::F8;
	case VK_F11: return KeyCode::F11;
	case VK_F12: return KeyCode::F12;
	case VK_TAB:      return KeyCode::TAB;
	case VK_RETURN:   return KeyCode::RETURN;
	case VK_BACK:     return KeyCode::BACKSPACE;
	case VK_ESCAPE:   return KeyCode::ESCAPE;
	case VK_UP:       return KeyCode::UP;
	case VK_DOWN:     return KeyCode::DOWN;
	case VK_LEFT:     return KeyCode::LEFT;
	case VK_RIGHT:    return KeyCode::RIGHT;
	case VK_LSHIFT:   return KeyCode::LSHIFT;
	case VK_RSHIFT:   return KeyCode::RSHIFT;
	case VK_LCONTROL: return KeyCode::LCTRL;
	case VK_LWIN:     return KeyCode::LGUI;
	case VK_LMENU:    return KeyCode::LALT;
	case VK_RMENU:    return KeyCode::RALT;
	default:          return KeyCode::INVALID;
	}
}

// Resolve left/right variants for shift, ctrl, alt
inline WPARAM MapExtendedKey(WPARAM vk, LPARAM lParam)
{
	switch (vk) {
	case VK_SHIFT:   return MapVirtualKeyW((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
	case VK_CONTROL: return (lParam & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
	case VK_MENU:    return (lParam & (1 << 24)) ? VK_RMENU    : VK_LMENU;
	default:         return vk;
	}
}
