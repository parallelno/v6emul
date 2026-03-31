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
		A = 0, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		// Digits
		NUM_0, NUM_1, NUM_2, NUM_3, NUM_4, NUM_5, NUM_6, NUM_7, NUM_8, NUM_9,
		// Punctuation / symbols
		SPACE, MINUS, EQUALS, LEFTBRACKET, RIGHTBRACKET, BACKSLASH,
		SEMICOLON, APOSTROPHE, GRAVE, COMMA, PERIOD, SLASH,
		// Function keys
		F1, F2, F3, F4, F5, F6, F7, F8,
		// Special keys
		TAB, RETURN, BACKSPACE, ESCAPE,
		// Arrow keys
		UP, DOWN, LEFT, RIGHT,
		// Modifier keys
		LSHIFT, RSHIFT, LCTRL, LGUI, LALT, RALT,
		// System
		F11, F12,
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