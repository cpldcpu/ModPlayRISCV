/*
 * MOD Player using Advanced Control Timer (TIM1) for PWM generation with DMA
 * cpldcpu Oct 26, 2023
 * 
 * Audio output on PC3 (inverter) and P4 (non-inverted) using complementary PWM outputs
 *
 * Based on CH32fun PWM example 03-28-2023 E. Brombaugh
 * Modified to use DMA for PWM value loading - inspired from https://github.com/BogdanTheGeek/ch32fun-audio/blob/main/main.c
 * Integrated with MODPlay engine for MOD file playback - Original MODplay: https://github.com/prochazkaml/MODPlay 
 */

#include "ch32fun.h"
#include <stdio.h>
#include <stdint.h>

// Configure MODPlay for mono output and include implementation
#define USE_MONO_OUTPUT 1
#define USE_LINEAR_INTERPOLATION 1
#define CHANNELS 4
#define pwm_shift        5             // PWM shift to scale 16-bit to 11-bit


#include "modplay.c"
// Move criticial functions to sram to speed up processing. takes ~2kb sram
ModPlayerStatus_t *RenderMOD(short *buf, int len, int osr) __attribute__((section(".srodata"))) __attribute__((used));
ModPlayerStatus_t *ProcessMOD() __attribute__((section(".srodata"))) __attribute__((used));
void _RecalculateWaveform(Oscillator_t *oscillator) __attribute__((section(".srodata"))) __attribute__((used));


// Audio configuration
#define SAMPLE_RATE      22050*1         // MOD playback sample rate
#define BUF_SAMPLES      256           // Reduced buffer size to save RAM
#define osr              1             // oversampling ratio


// Include embedded MOD file
#include "test_mod.h"


// Ring buffer for CH1 PWM compare values (0..255)
static volatile uint16_t  g_rb_ch1[BUF_SAMPLES];
static volatile size_t   g_buffer_offset = 0;  // Tracks which half of buffer DMA just finished

// MOD player pointer
static ModPlayerStatus_t *mod_player = NULL;

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

		// Render MOD audio samples
		if (mod_player) {
			RenderMOD((short *)(void *)&g_rb_ch1[offset], BUF_SAMPLES/2, osr);
		}

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

	// PC3 is T1CH1N, 10MHz Output alt func, push-pull
	GPIOC->CFGLR &= ~(0xf<<(4*3));
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF)<<(4*3);

	// PC4 is T1CH1, 10MHz Output alt func, push-pull
	GPIOC->CFGLR &= ~(0xf<<(4*4));
	GPIOC->CFGLR |= (GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF)<<(4*4);

	// Reset TIM1 to init all regs
	RCC->APB2PRSTR |=  RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;
	
	// Prescaler to achieve sample/update rate
	TIM1->PSC = 0;  // 48MHz PWM clock

	// Auto Reload - determines PWM resolution 
	// 48 Mhz / 2177 = ~22050 Hz
	TIM1->ATRLR = 2176;  // 11-bit PWM, sample rate = 22.05 kHz 
	// TIM1->ATRLR = 273;  // 8-bit PWM, sample rate = 22.05*8 kHz 

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
						   DMA_CFGR1_MSIZE_0 | // 16-bit memory
					       DMA_CFGR1_PSIZE_1 | // 32-bit peripheral
	                       DMA_CFGR1_CIRC |          // Circular mode
						   DMA_CFGR1_PL |            // High priority
	                       DMA_CFGR1_MINC |          // Memory increment
	                       DMA_CFGR1_HTIE |          // Half-transfer interrupt enable
	                       DMA_CFGR1_TCIE;           // Transfer complete interrupt enable
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
}

/*
 * Stop PWM audio DMA
 */
void pwm_audio_stop(void)
{
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

	printf("\r\r\n\nMOD Player with PWM/DMA Audio\n\r");

	t1pwm_init();

	printf("Sample rate: %d Hz\n\r", SAMPLE_RATE);

	mod_player = InitMOD(test_mod, SAMPLE_RATE);

	printf("MOD file loaded: %u bytes\n\r", test_mod_len);
	printf("Channels: %d, Orders: %d, Patterns: %d\n\r",
	       mod_player->channels, mod_player->orders, mod_player->maxpattern);

	// Fill first half
	RenderMOD((short *)(void *)g_rb_ch1, BUF_SAMPLES,osr);

	// Reset counters
	g_buffer_offset = 0;

	// NOW start the DMA and timer
	pwm_audio_start();

	printf("MOD playback active!\n\r");

	while(1)
	{
		Delay_Ms(2000);

		// Print MOD playback status
		if (mod_player) {
			printf("Order: %d/%d, Row: %d/64, Tick: %d/%d\n\r",
			       mod_player->order + 1, mod_player->orders,
			       mod_player->row, mod_player->tick, mod_player->maxtick);
		}

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
			uint32_t int_rate_hz = (2 * SAMPLE_RATE) / BUF_SAMPLES;  // interrupts per second
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
