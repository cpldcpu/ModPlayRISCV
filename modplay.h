#ifndef MODPLAY_H_INCLUDED
#define MODPLAY_H_INCLUDED
#include <stdint.h>

typedef struct {
	const int8_t *sample;
	uint32_t age;
	uint32_t currentptr;
	uint32_t length;
	uint32_t looplength;
	uint32_t period;
	int32_t volume;
	int32_t currentsubptr; // only lower 16 bits are used in generation
	int8_t muted;
} PaulaChannel_t;

typedef struct {
	const int8_t *data;
	uint16_t actuallength;
	uint16_t looplength;
} Sample_t;

typedef struct {
	int32_t val;
	uint8_t waveform;
	uint8_t phase;
	uint8_t speed;
	uint8_t depth;
} Oscillator_t;

typedef struct {
	uint32_t note;
	uint8_t sample, eff, effval;

	uint8_t slideamount, sampleoffset;
	short volume;
	int32_t slidenote;

	int32_t period;

	Oscillator_t vibrato, tremolo;
	PaulaChannel_t samplegen;
} TrackerChannel_t;

typedef struct /*__attribute__((packed))*/ {
	char name[22];
	uint8_t lengthhi;
	uint8_t lengthlo;
	uint8_t finetune;
	uint8_t volume;
	uint8_t looppointhi;
	uint8_t looppointlo;
	uint8_t looplengthhi;
	uint8_t looplengthlo;
} SampleHeader_t;

#ifndef CHANNELS
#define CHANNELS 32
#endif

typedef struct {
	int channels, orders, maxpattern, order, row, tick, maxtick, speed,
		skiporderrequest, skiporderdestrow,
		patlooprow, patloopcycle;

	uint32_t samplerate, paularate, audiospeed, audiotick, random;

	TrackerChannel_t ch[CHANNELS];

	const uint8_t *patterndata, *ordertable;
	const SampleHeader_t *sampleheaders;
	Sample_t samples[31];
} ModPlayerStatus_t;

/*
 * ModPlayerStatus_t *InitMOD(const uint8_t *mod, uint32_t samplerate);
 * 
 * Initializes the MOD player with the given mod file and samplerate.
 */

ModPlayerStatus_t *InitMOD(const uint8_t *mod, uint32_t samplerate);

#ifndef USING_EXTERNAL_RENDERING

/*
 * ModPlayerStatus_t *RenderMOD(short *buf, int len);
 *
 * Renders a buffer from the MOD given to InitMOD() to `*buf`.
 *
 * NOTE: `len` specifies the number of samples, NOT BYTES.
 *
 * STEREO MODE (default, USE_MONO_OUTPUT=0):
 * Renders in 16-bit stereo, so the `*buf` array is expected
 * to have `len` * 4 allocated bytes of memory (2 channels * 2 bytes).
 *
 * The samples are interleaved as follows:
 * Index | Value
 * ------+------
 * 0     | Sample 0, left channel
 * 1     | Sample 0, right channel
 * 2     | Sample 1, left channel
 * 3     | Sample 1, right channel
 * ...   | ...
 *
 * MONO MODE (USE_MONO_OUTPUT=1):
 * Renders in 16-bit mono, so the `*buf` array is expected
 * to have `len` * 2 allocated bytes of memory (1 channel * 2 bytes).
 * All channels are mixed equally without panning.
 *
 * This makes it compatible with many popular audio output
 * libraries without any extra conversion (SDL, PortAudio etc).
 */

ModPlayerStatus_t *RenderMOD(short *buf, int len,int osr);

#endif

#ifdef USING_EXTERNAL_RENDERING

/*
 * ModPlayerStatus_t *ProcessMOD();
 * 
 * Advances to the next tick of the MOD file.
 * 
 * Handles the decoding of all pattern data (notes, samples, effects)
 * and generates pitch/volume/sample offset commands for an external sampler
 * (this info is accessible in the returned object->paula[0..3]).
 * 
 * NOTE: THIS FUNCTION IS CALLED INTERNALLY BY RenderMOD()!
 * Please do not call this function if you are building a desktop
 * application, use RenderMOD() instead, which will give you
 * raw sample data that you can pass to your audio subsystem.
 * 
 * This function is only intended to be called by your application
 * if you are not interested in MODPlay's sample renderer
 * and want to create your own, or you want to use this library
 * on system with actual hardware samplers (such as the Commodore Amiga
 * or the Sony PlayStation).
 */

ModPlayerStatus_t *ProcessMOD();

#endif

/*
 * ModPlayerStatus_t *JumpMOD(int order);
 * 
 * Jumps to a given order in the MOD file.
 * 
 * `order` is a number from 0 to numpatterns-1 in case you want
 * to jump to a particular pattern.
 * 
 * If you are, for example, implementing a UI with back/forward skip,
 * you can call this function with -2 to go back one pattern, or
 * -1 to go forward one pattern.
 * 
 * If the desired pattern cannot be reached with regular playback
 * (i.e. it is skipped over with Bxx in previous patterns),
 * JumpMOD will be unable to reach it and will try to play
 * pattern data after the skip.
 */

ModPlayerStatus_t *JumpMOD(int order);

#endif
