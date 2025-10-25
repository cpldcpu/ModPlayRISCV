#include "modplay.h"
#include <string.h>

#ifdef TEST
#include <stdio.h>
#include <stdlib.h>

char testbuffer[512];

#define assert(cond, ...) if(!(cond)) { \
		snprintf(testbuffer, 512, __VA_ARGS__); \
		_assert(cond, #cond, &mp, __LINE__); \
	}

void _assert(int cond, const char *condstr, const ModPlayerStatus_t *mp, int line) {
	if(!cond) {
		fprintf(stderr, "TEST FAILED ON LINE %d: %s\n", line, condstr);
		fprintf(stderr, "order: %d, row: %d, tick: %d\n", mp->order + 1, mp->row, mp->tick);
		fprintf(stderr, "%s\n", testbuffer);
		exit(1);
	}
}
#else
#define assert(cond, ...)
#endif

// Comment out to turn off sample interpolation - will sound crunchy, but will run faster
// Can also be controlled via -DUSE_LINEAR_INTERPOLATION=0 compile flag
#ifndef USE_LINEAR_INTERPOLATION
#define USE_LINEAR_INTERPOLATION 1
#endif

// Set to 1 for mono output (saves memory bandwidth and code size)
// Can also be controlled via -DUSE_MONO_OUTPUT=1 compile flag
#ifndef USE_MONO_OUTPUT
#define USE_MONO_OUTPUT 0
#endif

ModPlayerStatus_t mp;

static const int32_t finetune_table[16] = {
	65536, 65065, 64596, 64132,
	63670, 63212, 62757, 62306,
	69433, 68933, 68438, 67945,
	67456, 66971, 66489, 66011
};

static const uint8_t sine_table[32] = {
	0, 24, 49, 74, 97, 120, 141, 161,
	180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197,
	180, 161, 141, 120, 97, 74, 49, 24
};

static const int32_t arpeggio_table[16] = {
	65536, 61858, 58386, 55109,
	52016, 49096, 46341, 43740,
	41285, 38968, 36781, 34716,
	32768, 30929, 29193, 27554
};

void _RecalculateWaveform(Oscillator_t *oscillator) {
	int32_t result = 0;

	// The following generators _might_ have been inspired by micromod's code:
	// https://github.com/martincameron/micromod/blob/master/micromod-c/micromod.c

	switch(oscillator->waveform) {
		case 0:
			// Sine
			result = sine_table[oscillator->phase & 0x1F];
			if((oscillator->phase & 0x20) > 0) result *= (-1);
			break;

		case 1:
			// Sawtooth
			result = 255 - (((oscillator->phase + 0x20) & 0x3F) << 3);
			break;

		case 2:
			// Square
			result = 255 - ((oscillator->phase & 0x20) << 4);
			break;

		case 3:
			// Random
			result = (mp.random >> 20) - 255;
			mp.random = (mp.random * 65 + 17) & 0x1FFFFFFF;
			break;
	}

	oscillator->val = result * oscillator->depth;
}

ModPlayerStatus_t *ProcessMOD() {
	if(mp.tick == 0) {
		mp.skiporderrequest = -1;

		for(int i = 0; i < 4; i++) {  // Hardcoded 4 channels
			mp.ch[i].vibrato.val = mp.ch[i].tremolo.val = 0;

			const uint8_t *cell = mp.patterndata + 4 * (i + 4 * (mp.row + 64 * mp.ordertable[mp.order]));  // 4 channels

			int note_tmp = ((cell[0] << 8) | cell[1]) & 0xFFF;
			int sample_tmp = (cell[0] & 0xF0) | (cell[2] >> 4);
			int eff_tmp = cell[2] & 0x0F;
			int effval_tmp = cell[3];

			if(mp.ch[i].eff == 0 && mp.ch[i].effval != 0) {
				mp.ch[i].period = mp.ch[i].note;
			}

			if(sample_tmp) {
				if(sample_tmp > 31) sample_tmp = 1;

				mp.ch[i].sample = sample_tmp - 1;
				
				mp.ch[i].samplegen.length = mp.samples[sample_tmp - 1].actuallength << 1;
				mp.ch[i].samplegen.looplength = mp.samples[sample_tmp - 1].looplength << 1;
				mp.ch[i].volume = mp.sampleheaders[sample_tmp - 1].volume;
				mp.ch[i].samplegen.sample = mp.samples[sample_tmp - 1].data;
			}

			if(note_tmp) {
				int finetune;

				if(eff_tmp == 0xE && (effval_tmp & 0xF0) == 0x50)
					finetune = effval_tmp & 0xF;
				else
					finetune = mp.sampleheaders[mp.ch[i].sample].finetune;

				note_tmp = note_tmp * finetune_table[finetune & 0xF] >> 16;

				mp.ch[i].note = note_tmp;

				if(eff_tmp != 0x3 && eff_tmp != 0x5 && (eff_tmp != 0xE || (effval_tmp & 0xF0) != 0xD0)) {
					mp.ch[i].samplegen.age = mp.ch[i].samplegen.currentptr = 0;
					mp.ch[i].period = mp.ch[i].note;

					if(mp.ch[i].vibrato.waveform < 4) mp.ch[i].vibrato.phase = 0;
					if(mp.ch[i].tremolo.waveform < 4) mp.ch[i].tremolo.phase = 0;
				}
			}

			if(eff_tmp || effval_tmp) switch(eff_tmp) {
				case 0x3:
					if(effval_tmp) mp.ch[i].slideamount = effval_tmp;

				case 0x5:
					mp.ch[i].slidenote = mp.ch[i].note;
					break;

				case 0x4:
					if(effval_tmp & 0xF0) mp.ch[i].vibrato.speed = effval_tmp >> 4;
					if(effval_tmp & 0x0F) mp.ch[i].vibrato.depth = effval_tmp & 0x0F;

					// break intentionally left out here
	
				case 0x6:
					_RecalculateWaveform(&mp.ch[i].vibrato);
					break;

				case 0x7:
					if(effval_tmp & 0xF0) mp.ch[i].tremolo.speed = effval_tmp >> 4;
					if(effval_tmp & 0x0F) mp.ch[i].tremolo.depth = effval_tmp & 0x0F;
					_RecalculateWaveform(&mp.ch[i].tremolo);
					break;

				case 0xC:
					mp.ch[i].volume = (effval_tmp > 0x40) ? 0x40 : effval_tmp;
					break;

				case 0x9:
					if(effval_tmp) {
						mp.ch[i].samplegen.currentptr = effval_tmp << 8;
						mp.ch[i].sampleoffset = effval_tmp;
					} else {
						mp.ch[i].samplegen.currentptr = mp.ch[i].sampleoffset << 8;
					}

					mp.ch[i].samplegen.age = 0;
					break;

				case 0xB:
					if(effval_tmp >= mp.orders) effval_tmp = 0;

					mp.skiporderrequest = effval_tmp;
					break;

				case 0xD:
					if(mp.skiporderrequest < 0) {
						if(mp.order + 1 < mp.orders)
							mp.skiporderrequest = mp.order + 1;
						else
							mp.skiporderrequest = 0;
					}

					if(effval_tmp > 0x63) effval_tmp = 0;

					mp.skiporderdestrow = (effval_tmp >> 4) * 10 + (effval_tmp & 0xF); // What were the ProTracker guys smoking?!
					break;

				case 0xE:
					switch(effval_tmp >> 4) {
						case 0x1:
							mp.ch[i].period -= effval_tmp & 0xF;
							break;

						case 0x2:
							mp.ch[i].period += effval_tmp & 0xF;
							break;
						
						case 0x4:
							mp.ch[i].vibrato.waveform = effval_tmp & 0x7;
							break;

						case 0x6:
							if(effval_tmp & 0xF) {
								if(!mp.patloopcycle)
									mp.patloopcycle = (effval_tmp & 0xF) + 1;

								if(mp.patloopcycle > 1) {
									mp.skiporderrequest = mp.order;
									mp.skiporderdestrow = mp.patlooprow;
								}

								mp.patloopcycle--;
							} else {
								mp.patlooprow = mp.row;
							}

						case 0x7:
							mp.ch[i].tremolo.waveform = effval_tmp & 0x7;
							break;

						case 0xA:
							mp.ch[i].volume += effval_tmp & 0xF;
							if(mp.ch[i].volume > 0x40) mp.ch[i].volume = 0x40;
							break;

						case 0xB:
							mp.ch[i].volume -= effval_tmp & 0xF;
							if(mp.ch[i].volume < 0x00) mp.ch[i].volume = 0x00;
							break;

						case 0xE:
							mp.maxtick *= ((effval_tmp & 0xF) + 1);
							break;
					}
					break;

				case 0xF:
					if(effval_tmp) {
						if(effval_tmp < 0x20) {
							mp.maxtick = (mp.maxtick / mp.speed) * effval_tmp;
							mp.speed = effval_tmp;
						} else {
							mp.audiospeed = mp.samplerate * 125 / effval_tmp / 50;
						}
					}

					break;
			}

			mp.ch[i].eff = eff_tmp;
			mp.ch[i].effval = effval_tmp;
		}
	}

	for(int i = 0; i < 4; i++) {  // Hardcoded 4 channels
		int eff_tmp = mp.ch[i].eff;
		int effval_tmp = mp.ch[i].effval;

		if(eff_tmp || effval_tmp) switch(eff_tmp) {
			case 0x0:
				switch(mp.tick % 3) {
					case 0:
						mp.ch[i].period = mp.ch[i].note;
						break;

					case 1:
						mp.ch[i].period = (mp.ch[i].note * arpeggio_table[effval_tmp >> 4]) >> 16;
						break;

					case 2:
						mp.ch[i].period = (mp.ch[i].note * arpeggio_table[effval_tmp & 0xF]) >> 16;
						break;
				}
				break;

			case 0x1:
				if(mp.tick) mp.ch[i].period -= effval_tmp;
				break;

			case 0x2:
				if(mp.tick) mp.ch[i].period += effval_tmp;
				break;

			case 0x5:
				if(mp.tick) {
					if(effval_tmp > 0xF) {
						mp.ch[i].volume += (effval_tmp >> 4);
						if(mp.ch[i].volume > 0x40) mp.ch[i].volume = 0x40;
					} else {
						mp.ch[i].volume -= (effval_tmp & 0xF);
						if(mp.ch[i].volume < 0x00) mp.ch[i].volume = 0x00;
					}
				}
				
				effval_tmp = 0;
				// break intentionally left out here

			case 0x3:
				if(mp.tick) {
					if(!effval_tmp) effval_tmp = mp.ch[i].slideamount;

					if(mp.ch[i].slidenote > mp.ch[i].period) {
						mp.ch[i].period += effval_tmp;

						if(mp.ch[i].slidenote < mp.ch[i].period)
							mp.ch[i].period = mp.ch[i].slidenote;
					} else if(mp.ch[i].slidenote < mp.ch[i].period) {
						mp.ch[i].period -= effval_tmp;

						if(mp.ch[i].slidenote > mp.ch[i].period)
							mp.ch[i].period = mp.ch[i].slidenote;
					} 
				}

				break;

			case 0x4:
				if(mp.tick) {
					mp.ch[i].vibrato.phase += mp.ch[i].vibrato.speed;
					_RecalculateWaveform(&mp.ch[i].vibrato);
				}
				break;

			case 0x6:
				if(mp.tick) {
					mp.ch[i].vibrato.phase += mp.ch[i].vibrato.speed;
					_RecalculateWaveform(&mp.ch[i].vibrato);
				}
				// break intentionally left out here

			case 0xA:
				if(mp.tick) {
					if(effval_tmp > 0xF) {
						mp.ch[i].volume += (effval_tmp >> 4);
						if(mp.ch[i].volume > 0x40) mp.ch[i].volume = 0x40;
					} else {
						mp.ch[i].volume -= (effval_tmp & 0xF);
						if(mp.ch[i].volume < 0x00) mp.ch[i].volume = 0x00;
					}
				}

				break;

			case 0x7:
				if(mp.tick) {
					mp.ch[i].tremolo.phase += mp.ch[i].tremolo.speed;
					_RecalculateWaveform(&mp.ch[i].tremolo);
				}
				break;

			case 0xE:
				switch(effval_tmp >> 4) {
					case 0x9:
						if(mp.tick && !(mp.tick % (effval_tmp & 0xF)))
							mp.ch[i].samplegen.age = mp.ch[i].samplegen.currentptr = mp.ch[i].samplegen.currentsubptr = 0;
						break;

					case 0xC:
						if(mp.tick >= (effval_tmp & 0xF)) mp.ch[i].volume = 0;
						break;

					case 0xD:
						if(mp.tick == (effval_tmp & 0xF)) {
							mp.ch[i].samplegen.age = mp.ch[i].samplegen.currentptr = mp.ch[i].samplegen.currentsubptr = 0;
							mp.ch[i].period = mp.ch[i].note;
						}
						break;
				}

				break;
		}

		if(mp.ch[i].period < 0 && mp.ch[i].period != 0) {
			mp.ch[i].period = 0;
		}

		// Pre-calculate sampler period & volume

		if(mp.ch[i].period)
			mp.ch[i].samplegen.period = mp.paularate / (mp.ch[i].period + (mp.ch[i].vibrato.val >> 7));
		else
			mp.ch[i].samplegen.period = 0;
		
		int32_t vol = mp.ch[i].volume + (mp.ch[i].tremolo.val >> 6);

		if(vol < 0) vol = 0;
		if(vol > 64) vol = 64;

		mp.ch[i].samplegen.volume = vol;
	}

	mp.tick++;
	if(mp.tick >= mp.maxtick) {
		mp.tick = 0;
		mp.maxtick = mp.speed;

		if(mp.skiporderrequest >= 0) {
			mp.row = mp.skiporderdestrow;
			mp.order = mp.skiporderrequest;

			mp.skiporderdestrow = 0;
			mp.skiporderrequest = -1;
		} else {
			mp.row++;
			if(mp.row >= 0x40) {
				mp.row = 0;
				mp.order++;

				if(mp.order >= mp.orders) mp.order = 0;
			}
		}
	}

	return &mp;
}

ModPlayerStatus_t *RenderMOD(short *buf, int len, int osr) {
#if USE_MONO_OUTPUT
	memset(buf, 0, len * 2);  // Mono: 1 channel * 2 bytes
	const int32_t chmul = 32768;  // 131072 / 2 channels
#else
	memset(buf, 0, len * 4);  // Stereo: 2 channels * 2 bytes
	const int32_t majorchmul = 65536;  // 131072 / 2
	const int32_t minorchmul = 21845;  // 131072 / 6
#endif

	for(int s = 0; s < len; s+=osr) {
		// Process the tick, if necessary

		if(mp.audiotick <= 0) {
			ProcessMOD();
			mp.audiotick = mp.audiospeed;
		}

		mp.audiotick-=osr;

		// Render the audio

#if USE_MONO_OUTPUT
		int32_t mono = 0;
#else
		int32_t l = 0, r = 0;
#endif

		for(int ch = 0; ch < 4; ch++) {  // Hardcoded 4 channels
			PaulaChannel_t *pch = &mp.ch[ch].samplegen;

			if(pch->sample) {
				// If the single-shot sample has finished playing, skip this channel

				if((pch->looplength == 0) && (pch->currentptr >= pch->length))
					continue;

				// If it is a looping sample, wrap around to the loop point

				while(pch->currentptr >= pch->length)
					pch->currentptr -= pch->looplength;

				// Render the current sample

				if(!pch->muted) {
#if USE_LINEAR_INTERPOLATION
					uint32_t nextptr = pch->currentptr + 1;

					while(nextptr >= pch->length) {
						if(pch->looplength != 0)
							nextptr -= pch->looplength;
						else
							nextptr = pch->currentptr;
					}

					assert(pch->currentptr < pch->length, "channel: %d, test %u < %u", ch, pch->currentptr, pch->length);
					assert(nextptr < pch->length, "channel: %d, test %u < %u", ch, nextptr, pch->length);

					int32_t sample1 = pch->sample[pch->currentptr];
					int32_t sample2 = pch->sample[nextptr];

					assert(pch->currentsubptr < 0x10000, "channel: %d, test %u < 0x10000", ch, pch->currentsubptr);

					int32_t sample = (sample1 * (0x10000 - pch->currentsubptr) +
						sample2 * pch->currentsubptr) * pch->volume / 65536;
#else
					int32_t sample = pch->sample[pch->currentptr] * pch->volume;
#endif

#if USE_MONO_OUTPUT
					// Mix all channels equally to mono
					mono += sample * chmul;
#else
					// Distribute the rendered sample across both output channels (stereo panning)
					if((ch & 3) == 1 || (ch & 3) == 2) {
						l += sample * minorchmul;
						r += sample * majorchmul;
					} else {
						l += sample * majorchmul;
						r += sample * minorchmul;
					}
#endif
				}

				// Advance to the next required sample

				pch->currentsubptr += pch->period*osr;

				if(pch->currentsubptr >= 0x10000) {
					pch->currentptr += pch->currentsubptr >> 16;
					pch->currentsubptr &= 0xFFFF;
				}

				if(pch->age < INT32_MAX)
					pch->age++;
			}
		}

#if USE_MONO_OUTPUT
		// buf[s] = mono / 65536;
		for (int i = 0; i < osr; i++) {
			buf[s + i] = ((mono / 65536)+32768)>>pwm_shift;  // scale to 11 bit output
			
		}
#else
		buf[s * 2] = l / 65536;
		buf[s * 2 + 1] = r / 65536;
#endif
	}

	return &mp;
}

ModPlayerStatus_t *InitMOD(const uint8_t *mod, uint32_t samplerate) {
	// Hardcoded for 4-channel ProTracker MODs only
	// Verify signature (M.K. or M!K!)
	uint32_t signature = mod[1083] | (mod[1082] << 8) | (mod[1081] << 16) | (mod[1080] << 24);
	if(signature != 0x4D2E4B2E && signature != 0x4D214B21) {
		return NULL;  // Only accept 4-channel ProTracker MODs
	}

	memset(&mp, 0, sizeof(mp));

	mp.channels = 4;  // Hardcoded to 4 channels

	mp.samplerate = samplerate;
	mp.paularate = (3546895 / samplerate) << 16;

	mp.orders = mod[950];
	mp.ordertable = mod + 952;

	mp.maxpattern = 0;

	for(int i = 0; i < 128; i++) {
		if(mp.ordertable[i] >= mp.maxpattern) mp.maxpattern = mp.ordertable[i];
	}
	mp.maxpattern++;

	const int8_t *samplemem = ((const int8_t *) mod) + 1084 + 64 * 4 * 4 * mp.maxpattern;  // 4 channels hardcoded
	mp.patterndata = mod + 1084;

	mp.sampleheaders = (SampleHeader_t *) (mod + 20);

	for(int i = 0; i < 31; i++) {
		const SampleHeader_t *sample = mp.sampleheaders + i;

		uint16_t length = (sample->lengthhi << 8) | sample->lengthlo;
		uint16_t looppoint = (sample->looppointhi << 8) | sample->looppointlo;
		mp.samples[i].actuallength = (sample->looplengthhi << 8) | sample->looplengthlo;

		mp.samples[i].data = samplemem;
		samplemem += length * 2;

		mp.samples[i].actuallength += looppoint;

		if(mp.samples[i].actuallength < 0x2) {
			mp.samples[i].actuallength = length;
			looppoint = 0xFFFF;
			mp.samples[i].looplength = 0;
		} else if(mp.samples[i].actuallength > length) {
			looppoint /= 2;
			mp.samples[i].actuallength -= looppoint;
			mp.samples[i].looplength = mp.samples[i].actuallength - looppoint;
		} else {
			mp.samples[i].looplength = mp.samples[i].actuallength - looppoint;
		}
	}

	mp.maxtick = mp.speed = 6; mp.audiospeed = mp.samplerate / 50;

	for(int i = 0; i < 4; i++) {  // Hardcoded 4 channels
		mp.ch[i].samplegen.age = INT32_MAX;
	}

	return &mp;
}

ModPlayerStatus_t *JumpMOD(int order) {
	int neworder = mp.order;

	ModPlayerStatus_t old_mp = mp;

	memset(&mp, 0, sizeof(mp));

	mp.orders = old_mp.orders;
	mp.maxpattern = old_mp.maxpattern;
	mp.samplerate = old_mp.samplerate;
	mp.paularate = old_mp.paularate;

	mp.channels = old_mp.channels;
	mp.sampleheaders = old_mp.sampleheaders;

	mp.patterndata = old_mp.patterndata;
	mp.ordertable = old_mp.ordertable;

	memcpy(mp.samples, old_mp.samples, sizeof(mp.samples));

	mp.maxtick = mp.speed = 6; mp.audiospeed = mp.samplerate / 50;

	for(int i = 0; i < 4; i++) {  // Hardcoded 4 channels
		mp.ch[i].samplegen.age = INT32_MAX;
	}

	switch(order) {
		case -2:
			if(neworder > 0) neworder--;
			break;

		case -1:
			if(neworder < mp.orders - 1) neworder++;
			break;

		default:
			if(order < 0) order = 0;
			if(order >= mp.orders) order = mp.orders - 1;

			neworder = order;
			break;
	}

	int oldorder = 0;

	while(mp.order < neworder) {
		ProcessMOD();

		if(oldorder > mp.order)
			break;
		else
			oldorder = mp.order;
	}

	return &mp;
}
