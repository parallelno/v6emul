#include "core/keyboard.h"
#include "utils/utils.h"

dev::Keyboard::Keyboard()
{
	memset(m_encodingMatrix, 0, sizeof(m_encodingMatrix));
	InitMapping();
}

// Hardware thread
auto dev::Keyboard::KeyHandling(int _keyCode, int _action)
-> Operation
{
	int row, column;

	switch (static_cast<KeyCode>(_keyCode))
	{
	// BLK + VVOD functionality
	case KeyCode::F11:
		if (_action == static_cast<int>(KeyAction::KEY_UP)) {
			return Operation::RESET;
		}
		break;

		// BLK + SBR functionality
	case KeyCode::F12:
		if (_action == static_cast<int>(KeyAction::KEY_UP)) {
			return Operation::RESTART;
		}
		break;

		// SS (shift) key
	case KeyCode::LSHIFT: [[fallthrough]];
	case KeyCode::RSHIFT:
		m_keySS = _action == static_cast<int>(KeyAction::KEY_DOWN);
		break;

		// US (ctrl) key
	case KeyCode::LCTRL:
		m_keyUS = _action == static_cast<int>(KeyAction::KEY_DOWN);
		break;

		// RUS/LAT (cmd) key
	case KeyCode::LGUI: [[fallthrough]];
	case KeyCode::LALT: [[fallthrough]];
	case KeyCode::F6:
		m_keyRus = _action == static_cast<int>(KeyAction::KEY_DOWN);
		break;

		// Matrix keys
	default:

		auto it = m_keymap.find(_keyCode);
		if (it != m_keymap.end())
		{
			auto rowColumn = it->second;
			row = rowColumn >> 8;
			column = rowColumn & 0xFF;

			if (_action == static_cast<int>(KeyAction::KEY_UP)) {
				m_encodingMatrix[row] &= ~column;
			}
			else {
				m_encodingMatrix[row] |= column;
			}
		}
		break;
	}

	return Operation::NONE;
};

auto dev::Keyboard::Read(int _rows)
-> uint8_t
{
	uint8_t result = 0;
	for (auto row = 0; row < 8; ++row)
	{
		auto rowBit = 1 << row;
		result |= (_rows & rowBit) == 0 ? m_encodingMatrix[row] : 0;
	}
	return ~result;
}

void dev::Keyboard::InitMapping()
{
	// Keyboard encoding matrix:
	//              columns
	//     │ 7   6   5   4   3   2   1   0
	// ────┼───────────────────────────────
	//   7 │SPC  ^   ]   \   [   Z   Y   X
	//   6 │ W   V   U   T   S   R   Q   P
	// r 5 │ O   N   M   L   K   J   I   H
	// o 4 │ G   F   E   D   C   B   A   @
	// w 3 │ /   .   =   ,   ;   :   9   8
	// s 2 │ 7   6   5   4   3   2   1   0
	//   1 │F5  F4  F3  F2  F1  AR2 STR LDA,
	//   0 │DN  RT  UP  LFT ZB  VK  PS  TAB
	//
	// LDA - left diagonal arrow

	m_keymap = {
		// KeyCode				RowColumnCode = row<<8 | 1<<column
		{ static_cast<int>(KeyCode::SPACE),			0x780 },
		{ static_cast<int>(KeyCode::GRAVE),			0x701 },
		{ static_cast<int>(KeyCode::RIGHTBRACKET),	0x720 },
		{ static_cast<int>(KeyCode::BACKSLASH),		0x710 },
		{ static_cast<int>(KeyCode::LEFTBRACKET),	0x708 },
		{ static_cast<int>(KeyCode::Z),				0x704 },
		{ static_cast<int>(KeyCode::Y),				0x702 },
		{ static_cast<int>(KeyCode::X),				0x701 },

		{ static_cast<int>(KeyCode::W),				0x680 },
		{ static_cast<int>(KeyCode::V),				0x640 },
		{ static_cast<int>(KeyCode::U),				0x620 },
		{ static_cast<int>(KeyCode::T),				0x610 },
		{ static_cast<int>(KeyCode::S),				0x608 },
		{ static_cast<int>(KeyCode::R),				0x604 },
		{ static_cast<int>(KeyCode::Q),				0x602 },
		{ static_cast<int>(KeyCode::P),				0x601 },

		{ static_cast<int>(KeyCode::O),				0x580 },
		{ static_cast<int>(KeyCode::N),				0x540 },
		{ static_cast<int>(KeyCode::M),				0x520 },
		{ static_cast<int>(KeyCode::L),				0x510 },
		{ static_cast<int>(KeyCode::K),				0x508 },
		{ static_cast<int>(KeyCode::J),				0x504 },
		{ static_cast<int>(KeyCode::I),				0x502 },
		{ static_cast<int>(KeyCode::H),				0x501 },

		{ static_cast<int>(KeyCode::G),				0x480 },
		{ static_cast<int>(KeyCode::F),				0x440 },
		{ static_cast<int>(KeyCode::E),				0x420 },
		{ static_cast<int>(KeyCode::D),				0x410 },
		{ static_cast<int>(KeyCode::C),				0x408 },
		{ static_cast<int>(KeyCode::B),				0x404 },
		{ static_cast<int>(KeyCode::A),				0x402 },
		{ static_cast<int>(KeyCode::MINUS),			0x401 }, // '@'

		{ static_cast<int>(KeyCode::SLASH),			0x380 },
		{ static_cast<int>(KeyCode::PERIOD),		0x340 },
		{ static_cast<int>(KeyCode::EQUALS),		0x320 },
		{ static_cast<int>(KeyCode::COMMA),			0x310 },
		{ static_cast<int>(KeyCode::SEMICOLON),		0x308 },
		{ static_cast<int>(KeyCode::APOSTROPHE),	0x304 },
		{ static_cast<int>(KeyCode::NUM_9),			0x302 },
		{ static_cast<int>(KeyCode::NUM_8),			0x301 },

		{ static_cast<int>(KeyCode::NUM_7),			0x280 },
		{ static_cast<int>(KeyCode::NUM_6),			0x240 },
		{ static_cast<int>(KeyCode::NUM_5),			0x220 },
		{ static_cast<int>(KeyCode::NUM_4),			0x210 },
		{ static_cast<int>(KeyCode::NUM_3),			0x208 },
		{ static_cast<int>(KeyCode::NUM_2),			0x204 },
		{ static_cast<int>(KeyCode::NUM_1),			0x202 },
		{ static_cast<int>(KeyCode::NUM_0),			0x201 },

		{ static_cast<int>(KeyCode::F5),			0x180 },
		{ static_cast<int>(KeyCode::F4),			0x140 },
		{ static_cast<int>(KeyCode::F3),			0x120 },
		{ static_cast<int>(KeyCode::F2),			0x110 },
		{ static_cast<int>(KeyCode::F1),			0x108 },
		{ static_cast<int>(KeyCode::ESCAPE),		0x104 }, // AR2
		{ static_cast<int>(KeyCode::F8),			0x102 }, // STR
		{ static_cast<int>(KeyCode::F7),			0x101 }, // LDA - left diagonal arrow

		{ static_cast<int>(KeyCode::DOWN),			0x080 },
		{ static_cast<int>(KeyCode::RIGHT),			0x040 },
		{ static_cast<int>(KeyCode::UP),			0x020 },
		{ static_cast<int>(KeyCode::LEFT),			0x010 },
		{ static_cast<int>(KeyCode::BACKSPACE),		0x008 }, // ZB
		{ static_cast<int>(KeyCode::RETURN),		0x004 }, // VK
		{ static_cast<int>(KeyCode::RALT),			0x002 }, // PS
		{ static_cast<int>(KeyCode::TAB),			0x001 }, // TAB
	};
}
