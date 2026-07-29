#pragma once

#include <cstdint>
#include <unordered_map>
#include <functional>

#include "utils/types.h"

namespace dev
{
	// Abstract key codes matching the Vector-06C keyboard layout.
	// The frontend maps its platform-specific scancodes to these values.
	enum class KeyCode : int {
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
		COUNT
	};

	enum class KeyAction : int {
		KEY_UP = 0,
		KEY_DOWN = 1
	};

	class Keyboard
	{
	private:
		uint8_t m_encodingMatrix[8];
		using RowColumnCode = int;
		std::unordered_map<int, RowColumnCode> m_keymap;

	public:
		enum class Operation {
			NONE = 0,
			RESET,
			RESTART
		};
		bool m_keySS = false;
		bool m_keyUS = false;
		bool m_keyRus = false;
		Operation m_rebootType = Operation::NONE;

		Keyboard();

		auto KeyHandling(int _keyCode, int _action) -> Operation;
		auto Read(int _rows) -> uint8_t;

	private:
		void InitMapping();
	};
}