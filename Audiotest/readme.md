# Audio Test

This is a small project to test different configurations of an audio output signal chain based on PWM plus noise shaper (DSM) and DMA on the CH32V00x. 

It will generate either a sine wave test signal or white noise. The output of the microcontroller can then be connected to a reconstruction filter and audio analyzer (DSO, computer audio card) to be analyzed in detail.

## Configuration Options

Edit defines in `main.c`:

### Signal Mode
```c
#define NOISE_MODE        0    // 0 = sine wave, 1 = white noise
```

### PWM Configuration
```c
#define PWM_PERIOD        255  // PWM resolution: 255=8-bit, 511=9-bit, etc.
#define PWM_CENTER        128  // Center level (DC offset, addjust according to period)
#define PWM_AMPLITUDE     64   // Output amplitude (peak deviation from center)
```

### Delta-Sigma Modulation
```c
#define OSR               8    // Oversampling ratio: 2/4/8/16
#define FRACTIONALDSM     1    // Fractional DSM: 0=off, 1=on (better SNR)
#define DMASIZE           8    // DMA transfer size: 8=8-bit, 16=16-bit
```

## Usage

1. Build and flash to CH32V003/CH32V006
2. Connect spectrum analyzer to PC3/PC4 (TIM1_CH1/CH1N)
3. Measure THD, noise floor, and frequency response
