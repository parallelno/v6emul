#include "core/audio.h"
#include <algorithm>
#include "utils/utils.h"

dev::Audio::Audio(TimerI8253& _timer, AYWrapper& _aywrapper) :
	m_timer(_timer), m_aywrapper(_aywrapper)
{
	Init();
}

void dev::Audio::Pause(bool _pause)
{
	// No-op: audio playback is handled externally via ReadSamples()
}

void dev::Audio::Mute(const bool _mute) { m_muteMul = _mute ? 0.0f : 1.0f; }

void dev::Audio::Reset()
{
	m_aywrapper.Reset();
	m_timer.Reset();
	m_buffer.fill(0);
	m_readBuffIdx = m_writeBuffIdx = 0;
	m_muteMul = 1.0f;
}

void dev::Audio::Init()
{
	m_buffer.fill(0);
	m_inited = true;
}

// _cycles are ticks of the 1.5 Mhz timer.
// Hardware thread
void dev::Audio::Clock(int _cycles, const float _beeper)
{
	if (!m_inited) return;

	for (int tick = 0; tick < _cycles; ++tick)
	{
		float sample = (m_timer.Clock(1) + m_aywrapper.Clock(2) + _beeper) * m_muteMul;

		if (Downsample(sample))
		{
			m_buffer[(m_writeBuffIdx++) % BUFFER_SIZE] = sample;
		}
	}
}

// resamples to a lower rate using a linear interpolation.
// returns true if the output sample is ready, false otherwise
bool dev::Audio::Downsample(float& _sample)
{
	static int sampleCounter = 0;
	static float accumulator = 0;
	accumulator += _sample;

	if (++sampleCounter > m_downsampleRate)
	{
		_sample = accumulator / m_downsampleRate;
		sampleCounter = 0;
		accumulator = 0;
		return true;
	}

	return false;
}

int dev::Audio::ReadSamples(float* _out, int _maxSamples)
{
	int available = static_cast<int>(m_writeBuffIdx - m_readBuffIdx);
	int toRead = std::min(available, _maxSamples);
	for (int i = 0; i < toRead; ++i)
	{
		_out[i] = m_buffer[(m_readBuffIdx++) % BUFFER_SIZE];
	}
	return toRead;
}
