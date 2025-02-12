/**

MIT-like-non-ai-license

Copyright (c) 2024 Charles Lohr "CNLohr"

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the two following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

In addition the following restrictions apply:

1. The Software and any modifications made to it may not be used for the
purpose of training or improving machine learning algorithms, including but not
limited to artificial intelligence, natural language processing, or data
mining. This condition applies to any derivatives, modifications, or updates
based on the Software code. Any usage of the Software in an AI-training dataset
is considered a breach of this License.

2. The Software may not be included in any dataset used for training or
improving machine learning algorithms, including but not limited to artificial
intelligence, natural language processing, or data mining.


3. Any person or organization found to be in violation of these restrictions
will be subject to legal action and may be held liable for any damages
resulting from such use.

If any term is unenforcable, other terms remain in-force.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

**/


// NOT LORA!!!

// Transmit a sweep on 97.7MHz
#define FM_TRANSMITTER_SWEEP

// Nothing = Transmit a 315MHz signal.

// XXX WARNING: Something is wrong with this - 
//  The output isn't perfectly time aligned
//  And as such there are extra images in weird places.
//  TODO: Investigate the DMA+SPI Port jankyness.
#include "ch32v003fun.h"
#include <stdio.h>
#include <string.h>

#include "LoRa-SDR-Code.h"

#ifdef LORAWAN
#include "lorawan_simple.h"
#endif

#define DMA_SIZE_WORDS 128

#define SENDBUFF_WORDS (DMA_SIZE_WORDS*2)
uint8_t sendbuff[SENDBUFF_WORDS];

// Bits are shifted out MSBit first, then to LSBit


#define MAX_BYTES 25
#define MAX_SYMBOLS (MAX_BYTES*2+16)

// Our table is bespoke for the specific SF.
#define CHIPSSPREAD CHIRPLENGTH_WORDS// QUARTER_CHIRP_LENGTH_WORDS (TODO: Use the quater value elsewhere in the code)
#define MARK_FROM_SF0 (1<<SF_NUMBER) // SF7

#define PREAMBLE_CHIRPS 10
#define CODEWORD_LENGTH 2

uint32_t quadsetcount;
int16_t quadsets[MAX_SYMBOLS*4+PREAMBLE_CHIRPS*4+9+CODEWORD_LENGTH*4];
int runningcount_bits = 0;
volatile int fxcycle;
volatile int quadsetplace = -1;
volatile uint32_t temp;

#if 0

int16_t * AddChirp( int16_t * qso, int offset, int verneer )
{
	offset = offset * CHIPSSPREAD / (MARK_FROM_SF0);
	offset += verneer;
	*(qso++) = (CHIPSSPREAD * 0 / 4 + offset + CHIPSSPREAD ) % CHIPSSPREAD;
	*(qso++) = (CHIPSSPREAD * 1 / 4 + offset + CHIPSSPREAD ) % CHIPSSPREAD;
	*(qso++) = (CHIPSSPREAD * 2 / 4 + offset + CHIPSSPREAD ) % CHIPSSPREAD;
	*(qso++) = (CHIPSSPREAD * 3 / 4 + offset + CHIPSSPREAD ) % CHIPSSPREAD;
	return qso;
}

// This IRQ is called periodically to fill the output buffer that is shifted to SPI
void DMA1_Channel3_IRQHandler( void ) __attribute__((interrupt)) __attribute__((section(".srodata")));
void DMA1_Channel3_IRQHandler( void ) 
{
	//GPIOD->BSHR = 1;	 // Turn on GPIOD0 for profiling

	// Backup flags.
	volatile int intfr = DMA1->INTFR;
	do
	{
		// Clear all possible flags.
		DMA1->INTFCR = DMA1_IT_GL3;


		//int place = DMA1_Channel3->CNTR;
		// CNTR says that there are THIS MANY bytes left in the transfer.
		// So a high CNTR value indicates a very early place in the buffer.

		uint16_t * sb = 0;

		temp++;

		if( intfr & DMA1_IT_HT3 )
		{
			sb = sendbuff;
		}
		else if( intfr & DMA1_IT_TC3 )
		{
			sb = sendbuff + SENDBUFF_WORDS/2;
		}

		if( sb )
		{
			if( quadsetplace < 0 )
			{
				// Abort Send
				memset( sb, 0, SENDBUFF_WORDS*2/2 );
				DMA1_Channel3->CFGR &= ~DMA_CFGR1_EN;
				goto complete;
			}

			fxcycle += DMA_SIZE_WORDS;
			if( fxcycle == NUM_DMAS_PER_QUARTER_CHIRP*DMA_SIZE_WORDS )
			{
				fxcycle = 0;
				// Advance to next quarter word.
				quadsetplace++;

				if( quadsetplace > quadsetcount )
				{
#ifdef TEST_TONE
					quadsetplace = 0;
#else
					quadsetplace = -1;
#endif
					memset( sb, 0, SENDBUFF_WORDS*2/2 );
					goto complete;
				}
			}

			int symbol = quadsets[quadsetplace]; // Actually 0...CHIRPLENGTHWORDS

			const uint16_t * tsb = 0;

			// Select down- or up-chirp.
			if( symbol < 0 )
			{
				int word = fxcycle - symbol - 1;
				if( word >= CHIRPLENGTH_WORDS ) word -= CHIRPLENGTH_WORDS;
				word++;
				tsb = (&chirpbuff[word+REVERSE_START_OFFSET_BYTES/4]);
			}
			else
			{
				int word = fxcycle + symbol;
				if( word >= CHIRPLENGTH_WORDS ) word -= CHIRPLENGTH_WORDS;
				tsb = (&chirpbuff[word]);
			}

			// I tried using the DMA to do the copy - but that didn't work for some reason.
			//while( DMA1_Channel5->CFGR & 1 );
//			DMA1_Channel5->CNTR = DMA_SIZE_WORDS/2;
//			DMA1_Channel5->MADDR = (uint32_t)tsb;
//			DMA1_Channel5->PADDR = (uint32_t)sb;
//			DMA1_Channel5->CFGR |= DMA_CFGR1_EN;

			int cpy = DMA_SIZE_WORDS/2;
#ifdef TEST_TONE
	// Test tone
	memset( sb, 0xaa, cpy*2 );
#else
#if DMA_SIZE_WORDS_DIVISIBLE_BY_FOUR == 0
			#error need divisibiltiy by 2.
#else
			//while( cpy-- ) { *(sb++) = wordo; }
			// Guarantee aligned access.
			uint32_t * sbw = (uint32_t*)(((uint32_t)sb)&~3);
			uint32_t * tsbw = (uint32_t*)(((uint32_t)tsb)&~3);
			//while( cpy-- ) { *(sbw++) = *(tsbw++); }

			// Align the data copy if needed
			if( cpy & 1 )
			{
				*(sbw++) = *(tsbw++);
			}
			cpy /= 2; // Doubled up per loop
			asm volatile("\
1:\
c.lw a3, 0(%[from])\n\
c.lw a4, 4(%[from])\n\
c.addi %[from], 8\n\
c.addi %[cpy], -1\n\
c.sw a3, 0(%[to])\n\
c.sw a4, 4(%[to])\n\
c.addi %[to], 8\n\
c.bnez %[cpy], 1b\n\
" : [cpy]"+r"(cpy), [from]"+r"(tsbw), [to]"+r"(sbw) : : "a3", "a4", "memory");
#endif
#endif
		}

complete:
		intfr = DMA1->INTFR;
	} while( intfr );

	//GPIOD->BSHR = 1<<16; // Turn off GPIOD0 for profiling
}
#endif



void LoopFunction()  __attribute__((section(".srodata")));
void LoopFunction()
{
	uint8_t * start = (uint8_t*)DMA1_Channel5->MADDR;
	uint8_t * end = (uint8_t*)((uint32_t)DMA1_Channel5->MADDR + SENDBUFF_WORDS);
	uint8_t * here = start+ 8;
	uint32_t targ = 2000;
	uint32_t running = 0;
	uint8_t * tail = end - DMA1_Channel5->CNTR;
	uint32_t * cntr = DMA1_Channel5->CNTR;
	uint32_t temp = 0;
	uint32_t temp2 = 0;

	asm volatile("\n\
	li %[targ], 2000\n\
genloop:\n\
	lw %[temp], 0(%[cntr])\n\
	sub %[tail], %[end], %[temp]\n\
	beq %[here], %[tail], genloop\n\
innerloop:\
	li %[temp], 17\n\
	blt %[running], %[targ], noskip\n\
	li %[temp], 18\n\
	sub %[running], %[running], %[targ]\n\
noskip:\n\
	sb %[temp], 0(%[here])\n\
	addi %[here], %[here], 1\n\
	bne %[here], %[end], skipreset\n\
	add %[here], x0, %[start]\n\
skipreset:\n\
	bne %[here], %[tail], innerloop\n\
	j genloop\n\
" : [here]"+r"(here) :
	[start]"r"(start),
	[end]"r"(end),
	[targ]"r"(targ),
	[running]"r"(running),
	[tail]"r"(tail),
	[cntr]"r"(cntr),
	[temp]"r"(temp),
	[temp2]"r"(temp2) );

/*
	while(1)
	{
		int targ_f = 2000; //(frameno & 511)*9 + 1700;
		int run_f = 0;
		uint8_t * tail = end - DMA1_Channel5->CNTR;

		while( here != tail )
		{
			int setf = 17;
			if( run_f > targ_f )
			{
				setf = 18;
				run_f -= targ_f;
			}
			run_f += setf*32;

			*here = setf;

			here++;
		}
*/
/*
		for( j = 0; j < sizeof( sendbuff ); j++ )
		{
			int setf = 10;
			if( run_f > targ_f )
			{
				setf = 9;
				run_f -= targ_f;
			}
			run_f += setf*32;
			
			sendbuff[j] = setf;
		}
*/
}
void LoopFunction2() __attribute__((aligned(256))) __attribute__((section(".srodata"))) __attribute__ ((noinline));

__attribute__((section(".sdata"))) __attribute__((aligned(256))) const uint32_t tablef[] = {
		0x06060606,
		0x06060607,
		0x06070607,
		0x07070706,
		0x07070707,
		0x07070708,
		0x07080708,
		0x08080807,
		0x08080808, 
		0x08080809, 
		0x08090809, 
		0x09090908, 
		0x09090909, 
		0x0909090a, 
		0x090a090a, 
		0x0a0a0a09, 
		0x0a0a0a0a,  // Below this line is unstable - i.e. sometimes there are missing DMA transfers.
		0x0a0a0a0b, 
		0x0a0b0a0b, 
		0x0b0b0b0a, 
		0x0b0b0b0b, 
		0x0b0b0b0c, 
		0x0b0c0b0c, 
		0x0c0c0c0b, 
		0x0c0c0c0c,
		0x0c0c0c0d, 
		0x0c0d0c0d, 
		0x0d0d0d0c, 
		0x0d0d0d0d,
		0x0d0d0d0e, 
		0x0d0e0d0e, 
		0x0e0e0e0d,
		0x0e0e0e0e, 
		0x0e0e0e0f, 
		0x0e0f0e0f, 
		0x0f0f0f0e,
		0x0f0f0f0f, 
		0x0f0f0f10, 
		0x0f100f10, 
		0x1010100f,
		0x10101010, 
		0x10101011, 
		0x10111011, 
		0x11111110,
	};

void LoopFunction2()
{

	uint32_t * start = (uint8_t*)DMA1_Channel5->MADDR;
	uint32_t * end = (uint8_t*)((uint32_t)DMA1_Channel5->MADDR + SENDBUFF_WORDS);
	uint32_t * here = start;

	int run_f = 0;

	volatile uint32_t * cntrptr = &DMA1_Channel5->CNTR;

	while(1)
	{
		//uint32_t * tail = 0xfffffffc & (uintptr_t)(((uint8_t*)end) - *cntrptr);
		//if( tail == end ) tail--;
		uint32_t * tail = ((SENDBUFF_WORDS-1) & (0xfffffffc)) & (uintptr_t)(((uint8_t*)start) + SENDBUFF_WORDS - *cntrptr);

		while( here != tail )
		{
#ifdef FM_TRANSMITTER_SWEEP
			// 97.7MHz FM Station
			int notein = (SysTick->CNT>>3)&0x1fff;
			if( notein > 0xfff ) notein = 0x2000-notein;
			uint32_t cp = (notein)+0x20000;
#else
			// 315MHz
			uint32_t cp = 0x1bfc3;
#endif
			*(here++) = tablef[run_f>>12];
			run_f &= (1<<12)-1;
			run_f += cp;
			if( here == end )
				here = start;
		}
	}

}


int main()
{
	SystemInit();

	funGpioInitAll();

	// Set a wait state (1 = normal <= 48MHz)
	FLASH->ACTLR = 1;

// MCO for testing.
//	funPinMode( PA8, GPIO_CFGLR_OUT_50Mhz_AF_PP );	RCC->CFGR0 |= RCC_CFGR0_MCO_PLL;

//	printf( "Switching to HSE\n" );
	Delay_Ms( 10 );

	// Disable clock security system.
	RCC->CTLR &= ~RCC_CSSON;

	// Enable external crystal
	RCC->CTLR |= RCC_HSEON;

	// XXX NOTE: This is only used if you have a clock, not an oscillator.
//	RCC->CTLR |= RCC_HSEBYP;

	// Set System Clock Source to be 0.
	RCC->CFGR0 = (RCC->CFGR0 & ~RCC_SW) | 0;

	// Disable PLL
	RCC->CTLR &= ~RCC_PLLON;

	// Switch to HSE
	RCC->CFGR0 |= RCC_PLLSRC;

	// Enable PLL
	RCC->CTLR |= RCC_PLLON;

	// Wait for HSE to become ready.
	while( !( RCC->CTLR & RCC_HSERDY) );
	while( !( RCC->CTLR & RCC_PLLRDY) );

	RCC->CFGR0 |= RCC_SW_1; //  Switch system clock to PLL
//	printf( "HSE Switched\n" );
	Delay_Ms( 10 );
	RCC->CTLR &= ~RCC_HSION;
	Delay_Ms( 10 );
	//printf( "HSI Off [%08lx %08lx]\n", RCC->CTLR, RCC->CFGR0 ); HSI Off [03035180 0001000a]

	RCC->APB2PCENR |= 	RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOC |
						RCC_APB2Periph_TIM1;

	RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;

	funPinMode( PC3, GPIO_CFGLR_OUT_50Mhz_AF_PP ); // T1C3 on PC3

	RCC->APB2PRSTR |= RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;


	// Prescaler 
	TIM1->PSC = 0x0000;
	
	// Auto Reload - sets period
	TIM1->ATRLR = 17;
	
	// Reload immediately
	TIM1->SWEVGR |= TIM_UG;
	
	// Enable CH1N output, positive pol
	TIM1->CCER |= TIM_CC3E;
	TIM1->CCER |= TIM_CC1E;
	
	// Compare 3 = for output
	// Modes:
	//  0, 1, 2: Nothing
	//  3: Flip
	//  4, 5: Nothing
	//  6: "Fast PWM mode 1"
	//  7: Flipping (Further out)
#ifdef FM_TRANSMITTER_SWEEP
	TIM1->CHCTLR2 =  TIM_OC3M_0 | TIM_OC3M_1 | TIM_OC3PE | TIM_OC3FE;
#else
	TIM1->CHCTLR2 =  TIM_OC3M_2 | TIM_OC3M_1 | TIM_OC3PE | TIM_OC3FE;
#endif

	// Compare 1 = for triggering
	TIM1->CHCTLR1 = TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1FE;
	
	// Set the Capture Compare Register value to 50% initially
	TIM1->CH3CVR = 4;  // ACTUALLY Ignored typically it seems.
	TIM1->CH1CVR = 0; // This triggers DMA.
	
	// Enable TIM1 outputs
	TIM1->BDTR |= TIM_MOE;
	
	// Enable TIM1
	TIM1->CTLR1 |= TIM_CEN;

	TIM1->DMAINTENR = TIM_TDE | TIM_UDE; // Outputs DMA1_Channel5

#if 0
	// Another try - use TIM2 to trigger DMA?
	RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
	RCC->APB1PRSTR |= RCC_APB1Periph_TIM2;
	RCC->APB1PRSTR &= ~RCC_APB1Periph_TIM2;

	TIM2->PSC = 0x0000;
	TIM2->ATRLR = 37;
	TIM2->SWEVGR |= TIM_UG;
	TIM2->BDTR |= TIM_MOE;
	TIM2->CHCTLR1 = TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1FE;
	TIM2->DMAINTENR |= TIM_UDE | TIM_TDE | TIM_TDE | TIM_CC1DE;
	TIM2->CTLR1 |= TIM_CEN;
	// TIM2_UP = DMA Channel 2.
#endif

	DMA1_Channel5->PADDR = (uint32_t)&TIM1->CH3CVR;
	DMA1_Channel5->MADDR = (uint32_t)sendbuff;
	DMA1_Channel5->CNTR  = 0;// sizeof( bufferset )/2; // Number of unique copies.  (Don't start, yet!)
	DMA1_Channel5->CFGR  =
		DMA_M2M_Disable |		 
		DMA_Priority_VeryHigh |
		DMA_PeripheralDataSize_HalfWord |
		DMA_MemoryDataSize_Byte |
		DMA_MemoryInc_Enable |
		DMA_Mode_Circular | // OR DMA_Mode_Circular or DMA_Mode_Normal
		DMA_DIR_PeripheralDST |
		0;
		//DMA_IT_TC | DMA_IT_HT; // Transmission Complete + Half Empty Interrupts. 

//	NVIC_EnableIRQ( DMA1_Channel3_IRQn );

		int j;
		for( j = 0; j < sizeof( sendbuff ); j++ )
		{
			sendbuff[j] = 12;
		}


	// Enter critical section.
	DMA1_Channel5->MADDR = (uint32_t)sendbuff;
	DMA1_Channel5->CNTR = SENDBUFF_WORDS; // Number of unique uint16_t entries.
	DMA1_Channel5->CFGR |= DMA_CFGR1_EN;


	LoopFunction2();
}

