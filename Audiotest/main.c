/*
 * Fixed-frequency sine generator using Advanced Control Timer (TIM1) for PWM generation with DMA
 * Produces a 500 Hz tone centred around the DAC output operating in complementary PWM mode.
 */

#include "ch32fun.h"
#include <stdio.h>
#include <stdint.h>

// Audio configuration
#define DMASIZE		      8
#define OSR			      8
#define FRACTIONALDSM     1			 // 1 = active
#define NOISE_MODE        1          // 0 = sine wave, 1 = white noise
#define SAMPLE_RATE       187500/8U      // PWM update/sample rate
#define BUF_SAMPLES       512U        // DMA transfer length in samples
#define SINE_FREQ         500U        // Generated tone frequency in Hz
#define PWM_PERIOD        255U       //  256
// #define PWM_PERIOD        127U       //  256
#define PWM_CENTER        128		//
#define PWM_AMPLITUDE     64       // Peak deviation from centre value

#define MAGIC_SHIFT       30
// #define MAGIC_GAIN        ((int32_t)76620800)  // tan(pi * SINE_FREQ / SAMPLE_RATE) in Q30
#define MAGIC_GAIN        ((int32_t)289579257)  // tan(pi * SINE_FREQ / SAMPLE_RATE)*2^30 in Q30


// Ring buffer for CH1 PWM compare values (0..255)
#if DMASIZE == 8
static volatile uint8_t  g_rb_ch1[BUF_SAMPLES];
#else
static volatile uint16_t  g_rb_ch1[BUF_SAMPLES];
#endif

static volatile size_t   g_buffer_offset = 0;  // Tracks which half of buffer DMA just finished
static volatile uint32_t residual=0;

static uint32_t g_dither_lfsr = 0xA5A5A5A5u;

static inline uint32_t dither_lfsr_next(void)
{
	uint32_t lfsr = g_dither_lfsr;
	uint32_t feedback = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 21) ^ (lfsr >> 31)) & 1u;
	lfsr = (lfsr >> 1) | (feedback << 31);
	g_dither_lfsr = lfsr;
	return lfsr;
}

typedef struct {
	int32_t x;
	int32_t y;
} MagicCircleOscillator_t;

static MagicCircleOscillator_t g_oscillator = {
	.x = ((int32_t)1) << MAGIC_SHIFT,
	.y = 0
};

static int32_t magic_circle_step(MagicCircleOscillator_t *osc)
{
	int32_t x = osc->x;
	int32_t y = osc->y;
	int32_t xn = x - (int32_t)(((int64_t)MAGIC_GAIN * y) >> MAGIC_SHIFT);
	int32_t yn = y + (int32_t)(((int64_t)MAGIC_GAIN * xn) >> MAGIC_SHIFT);
	osc->x = xn;
	osc->y = yn;
	return yn;
}

static uint32_t magic_circle_next_pwm(MagicCircleOscillator_t *osc)
{
	int32_t sine = magic_circle_step(osc);
	int32_t sample = (int32_t)(((int64_t)sine * PWM_AMPLITUDE ) >> (MAGIC_SHIFT-16));
	int32_t pwm_value = (int32_t)(PWM_CENTER*65536) + sample;
	// if (pwm_value < 0) {
	// 	pwm_value = 0;
	// } else if ((uint32_t)pwm_value > PWM_PERIOD) {
	// 	pwm_value = PWM_PERIOD;
	// }
	return pwm_value;
}

static void fill_pwm_buffer(int32_t start, int32_t count) __attribute__((section(".srodata"))) __attribute__((used));
static void fill_pwm_buffer(int32_t start, int32_t count)
{
	uint32_t accu=residual;
	#if DMASIZE ==8
	   uint8_t *buffer=&g_rb_ch1[start];
	#else
	   uint16_t *buffer=&g_rb_ch1[start];
	#endif

	for(int32_t i = 0; i < count; i+=OSR)
	{
#if NOISE_MODE == 1
		// White noise generation using LFSR
		uint32_t noise = dither_lfsr_next();
		// Treat noise as signed Q31 value (range [-1, 1])
		// Scale by PWM_AMPLITUDE and convert to 16.16 fixed point (matching sine processing)
		int32_t sample = (int32_t)(((int64_t)(int32_t)noise * PWM_AMPLITUDE) >> 15);
		// Center at PWM_CENTER in 16.16 fixed point format
		uint32_t val = (uint32_t)((PWM_CENTER << 16) + sample);
#else
		// Sine wave generation using magic circle oscillator
		// uint32_t dither=dither_lfsr_next();
		// int32_t val=magic_circle_next_pwm(&g_oscillator) + (dither & 0x007f);
		uint32_t val=magic_circle_next_pwm(&g_oscillator);
#endif
		uint32_t integer= val>>16;
		uint32_t fraction = val<<16;

#if  FRACTIONALDSM == 0
		fraction=0;
#endif	

#if OSR>=16
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
#endif
#if OSR>=8
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
#endif
#if OSR>=4
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
#endif
#if OSR>=2		
		accu+=fraction;
		*buffer++ = integer + (accu<fraction?1:0);
#endif		
		accu+=fraction;		
		*buffer++ = integer + (accu<fraction?1:0);
	}

	residual=accu;
}

// Profiling statistics
typedef struct {
	uint32_t count;
	uint32_t total_cycles;
	uint32_t min_cycles;
	uint32_t max_cycles;
} ProfileStats_t;

static volatile ProfileStats_t g_profile_stats = {0, 0, UINT32_MAX, 0};

/*
 * DMA1 Channel 5 interrupt handler
 * Called when DMA transfer is half-complete or fully complete
 * This allows us to update the buffer half that's not currently being read
 * Placed in SRAM for faster execution
 */

// void DMA1_Channel5_IRQHandler(void) __attribute__((interrupt)) __attribute__((section(".srodata"))) __attribute__((used));
void DMA1_Channel5_IRQHandler(void) __attribute__((interrupt));
void DMA1_Channel5_IRQHandler(void)
{
	// Start profiling - capture SysTick counter (counts up)
	uint32_t start_cycles = SysTick->CNT;

	volatile uint32_t intfr = DMA1->INTFR;

	do
	{
		// Clear all interrupt flags for Channel 5
		DMA1->INTFCR = DMA1_IT_GL5;

		// Determine which buffer half to update based on interrupt type
		size_t offset = 0;
		if (intfr & DMA1_IT_TC5) {
			// Transfer Complete: DMA reading first half, update second half
			offset = BUF_SAMPLES / 2;
		} else if (intfr & DMA1_IT_HT5) {
			// Half Transfer: DMA reading second half, update first half
			offset = 0;
		} else {
			// No relevant interrupt, skip processing
			break;
		}

		g_buffer_offset = offset;

		// Render sine samples
		fill_pwm_buffer(offset, BUF_SAMPLES / 2);

		// Re-check interrupt flags in case new interrupt occurred during handling
		intfr = DMA1->INTFR;
	} while (intfr & (DMA1_IT_TC5 | DMA1_IT_HT5));

	// End profiling - capture SysTick counter
	uint32_t end_cycles = SysTick->CNT;

	// Calculate elapsed cycles (SysTick counts up, handle wraparound)
	uint32_t elapsed;
	if (end_cycles >= start_cycles) {
		elapsed = end_cycles - start_cycles;
	} else {
		// Wrapped around
		elapsed = (SysTick->CMP - start_cycles) + end_cycles;
	}

	// Update statistics
	g_profile_stats.count++;
	g_profile_stats.total_cycles += elapsed;
	if (elapsed < g_profile_stats.min_cycles) {
		g_profile_stats.min_cycles = elapsed;
	}
	if (elapsed > g_profile_stats.max_cycles) {
		g_profile_stats.max_cycles = elapsed;
	}
}

/*
 * initialize TIM1 for PWM
 */
void t1pwm_init( void )
{
	// Enable GPIOD and TIM1
	// Also enable AFIO so remapping writes take effect
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_TIM1 | RCC_APB2Periph_AFIO;
	RCC->AHBPCENR  |= RCC_AHBPeriph_DMA1;

	// Clear the TIM1_RM field and set the remap value (value 2 -> CH1N -> PC3)
	AFIO->PCFR1 &= ~AFIO_PCFR1_TIM1_RM;      // clear the 2-bit field
	AFIO->PCFR1 |=  AFIO_PCFR1_TIM1_RM_1 | AFIO_PCFR1_TIM1_RM_0;    // set remap = 2 (don't OR the same macro twice)

	// Set PC3 and PC4 low before configuring to ensure drivers are off
	GPIOC->OUTDR &= ~((1<<3) | (1<<4));

	// Clear GPIO config - keeps pins as inputs (drivers disabled)
	GPIOC->CFGLR &= ~(0xf<<(4*3));
	GPIOC->CFGLR &= ~(0xf<<(4*4));

	// Note: GPIO outputs will be enabled in pwm_audio_start() after timer stabilizes

	// Reset TIM1 to init all regs
	RCC->APB2PRSTR |=  RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;
	
	// Prescaler to achieve sample/update rate
	TIM1->PSC = 0;  // 48MHz PWM clock

	// Auto Reload - determines PWM resolution 
	// 48 Mhz / 2177 = ~22050 Hz
	TIM1->ATRLR = PWM_PERIOD;  // 11-bit PWM, sample rate = 22.05 kHz 
	// TIM1->ATRLR = 273;  // 8-bit PWM, sample rate = 22.05*8 kHz 

	// Set Center aligned PWM on Timer 1
	TIM1->CTLR1  &= ~TIM1_CTLR1_CMS;
	TIM1->CTLR1 |= TIM1_CTLR1_CMS_0;  // 01=Mode1, int on down

	// Reload immediately
	TIM1->SWEVGR |=  TIM1_SWEVGR_UG;

	// Enable CH1N output, positive pol
	TIM1->CCER |= TIM1_CCER_CC1NE | TIM1_CCER_CC1NP;

	// Enable CH1 output, positive pol
	TIM1->CCER |= TIM1_CCER_CC1E | TIM1_CCER_CC1P;

	// CH1 Mode is output, PWM1 (CC1S = 00, OC1M = 110)
	TIM1->CHCTLR1 |= TIM1_CHCTLR1_OC1M_2 | TIM1_CHCTLR1_OC1M_1;

	// Set the Capture Compare Register value to 50% initially
	TIM1->CH1CVR = 128;

	// Enable TIM1 outputs
	TIM1->BDTR |= TIM1_BDTR_MOE;

	// --- Configure DMA1 Channel 5 for TIM1 CH1 (triggered by TIM1 Update) ---
	DMA1_Channel5->CFGR  = 0;
	DMA1_Channel5->PADDR = (uint32_t)&TIM1->CH1CVR;  // Peripheral: TIM1 CH1 compare register
	DMA1_Channel5->MADDR = (uint32_t)g_rb_ch1;       // Memory: CH1 ring buffer
	DMA1_Channel5->CNTR  = BUF_SAMPLES;              // Number of transfers
	DMA1_Channel5->CFGR  = DMA_CFGR1_DIR |           // Memory to peripheral

					       DMA_CFGR1_PSIZE_1 | // 32-bit peripheral
	                       DMA_CFGR1_CIRC |          // Circular mode
						   DMA_CFGR1_PL |            // High priority
	                       DMA_CFGR1_MINC |          // Memory increment
	                       DMA_CFGR1_HTIE |          // Half-transfer interrupt enable
	                       DMA_CFGR1_TCIE;           // Transfer complete interrupt enable

	#if DMASIZE == 16
		DMA1_Channel5->CFGR  |= DMA_CFGR1_MSIZE_0;  // 16-bit memory
	#endif
}

/*
 * Start PWM audio DMA
 */
void pwm_audio_start(void)
{
	// Enable NVIC interrupt for DMA Channel 5 (for double-buffering)
	NVIC_EnableIRQ(DMA1_Channel5_IRQn);

	// Enable TIM1 DMA requests: UDE for CH1 (via DMA Ch5)
	TIM1->DMAINTENR |= TIM1_DMAINTENR_UDE;

	// Enable CH1 DMA channel
	DMA1_Channel5->CFGR |= DMA_CFGR1_EN;  // CH1 DMA (triggered by Update)

	// Start the timer - this begins the DMA transfers
	TIM1->CTLR1 |= TIM1_CTLR1_CEN;

	Delay_Us(100);
	// Ensure timer output is active before enabling pin drivers to prevent stuck speaker

	// PC3 is T1CH1N, 10MHz Output alt func, push-pull
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF)<<(4*3);
	// PC4 is T1CH1, 10MHz Output alt func, push-pull
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF)<<(4*4);
}

/*
 * Stop PWM audio DMA
 */
void pwm_audio_stop(void)
{
	GPIOC->CFGLR &= ~(0xf<<(4*3));
	GPIOC->CFGLR &= ~(0xf<<(4*4));

	TIM1->CTLR1 &= ~TIM1_CTLR1_CEN;
	TIM1->DMAINTENR &= ~TIM1_DMAINTENR_UDE;
	DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
	NVIC_DisableIRQ(DMA1_Channel5_IRQn);
}

/*
 * entry
 */
int main()
{
	SystemInit();

#if NOISE_MODE == 1
	printf("\r\r\n\nWhite Noise Generator with PWM/DMA Audio\n\r");
#else
	printf("\r\r\n\n500 Hz Sine Generator with PWM/DMA Audio\n\r");
#endif

	t1pwm_init();

	printf("Sample rate: %d Hz\n\r", SAMPLE_RATE);

	// Fill first half
	fill_pwm_buffer(0, BUF_SAMPLES);

	// Reset counters
	g_buffer_offset = 0;

	// NOW start the DMA and timer
	pwm_audio_start();

#if NOISE_MODE == 1
	printf("White noise playback active!\n\r");
#else
	printf("Sine playback active!\n\r");
#endif

	while(1)
	{
		Delay_Ms(2000);

		// Print profiling statistics
		if (g_profile_stats.count > 0) {
			uint32_t avg_cycles = g_profile_stats.total_cycles / g_profile_stats.count;

			// Convert cycles to microseconds
			uint32_t avg_us = (avg_cycles * 1000) / (FUNCONF_SYSTEM_CORE_CLOCK / 1000);
			uint32_t min_us = (g_profile_stats.min_cycles * 1000) / (FUNCONF_SYSTEM_CORE_CLOCK / 1000);
			uint32_t max_us = (g_profile_stats.max_cycles * 1000) / (FUNCONF_SYSTEM_CORE_CLOCK / 1000);

			// Calculate interrupt rate and CPU usage
			// Expected rate: 2 interrupts per buffer (HT + TC) * sample_rate / buffer_size
			// = 2 * 22050 / 64 ≈ 689 Hz
			uint32_t int_rate_hz = (2 * SAMPLE_RATE * OSR) / BUF_SAMPLES;  // interrupts per second
			uint32_t cpu_percent = (avg_cycles * int_rate_hz * 100) / FUNCONF_SYSTEM_CORE_CLOCK;

			printf("IRQ: avg=%lu us, min=%lu us, max=%lu us, rate=%lu Hz, CPU=%lu%%\n\r",
			       avg_us, min_us, max_us, int_rate_hz, cpu_percent);

			// Reset statistics for next interval
			g_profile_stats.count = 0;
			g_profile_stats.total_cycles = 0;
			g_profile_stats.min_cycles = UINT32_MAX;
			g_profile_stats.max_cycles = 0;
		}
	}
}
