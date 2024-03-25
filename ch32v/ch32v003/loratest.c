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

// XXX WARNING: Something is wrong with this - 
//  The output isn't perfectly time aligned
//  And as such there are extra images in weird places.
//  TODO: Investigate the DMA+SPI Port jankyness.
#include "ch32v003fun.h"
#include <stdio.h>
#include <string.h>
#include "chirpbuff.h"
#include "chirpbuffinfo.h"

#include "LoRa-SDR-Code.h"

//#define LORAWAN
//#define TEST_TONE

#ifdef LORAWAN
#include "lorawan_simple.h"
#endif

#define SENDBUFF_WORDS (DMA_SIZE_WORDS*2)
uint16_t sendbuff[SENDBUFF_WORDS];

#if !defined( FOUND_PERFECT_DIVISOR )
#error Code only written for perfect division right now.
#endif

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

static uint16_t lora_symbols[MAX_SYMBOLS];
int lora_symbols_count;


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
			//while( DMA1_Channel2->CFGR & 1 );
//			DMA1_Channel2->CNTR = DMA_SIZE_WORDS/2;
//			DMA1_Channel2->MADDR = (uint32_t)tsb;
//			DMA1_Channel2->PADDR = (uint32_t)sb;
//			DMA1_Channel2->CFGR |= DMA_CFGR1_EN;

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


int main()
{
	SystemInit();

	funGpioInitAll();

	// Set a wait state (1 = normal <= 48MHz)
	FLASH->ACTLR = 1;

// MCO for testing.
//	funPinMode( PA8, GPIO_CFGLR_OUT_50Mhz_AF_PP );	RCC->CFGR0 |= RCC_CFGR0_MCO_PLL;

	printf( "Switching to HSE\n" );
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
	printf( "HSE Switched\n" );
	Delay_Ms( 10 );
	RCC->CTLR &= ~RCC_HSION;
	Delay_Ms( 10 );
	//printf( "HSI Off [%08lx %08lx]\n", RCC->CTLR, RCC->CFGR0 ); HSI Off [03035180 0001000a]

	funPinMode( PC6, GPIO_CFGLR_OUT_50Mhz_AF_PP ); // MOSI

	RCC->APB2PCENR |= RCC_APB2Periph_SPI1;
	RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;

	// Configure SPI 
	SPI1->CTLR1 = 
		SPI_NSS_Soft | SPI_CPHA_1Edge | SPI_CPOL_Low | SPI_DataSize_16b |
		SPI_Mode_Master | SPI_Direction_1Line_Tx |
		0 |
		(SPI_DIV-1)<<3; // Divisior = 0


	// If using DMA may need this.
	SPI1->CTLR2 = SPI_CTLR2_TXDMAEN;

	SPI1->HSCR = 1; // High-speed enable.

	SPI1->CTLR1 |= CTLR1_SPE_Set;
	//SPI1->DATAR = 0x55aa; // Set SPI line Low.

#if 0
	// I tried using DMA Channel 4 to do a mem-mem copy.
	// Doesn't seem to work.
	DMA1_Channel2->PADDR = (uint32_t)sendbuff; // SOURCE
	DMA1_Channel2->MADDR = (uint32_t)sendbuff; // DEST
	DMA1_Channel2->CNTR = DMA_SIZE_WORDS;
	DMA1_Channel2->CFGR = 
		DMA_M2M_Enable |
		DMA_Priority_Low |
		DMA_MemoryDataSize_HalfWord |
		DMA_PeripheralDataSize_HalfWord |
		DMA_MemoryInc_Enable |
		DMA_PeripheralInc_Enable |
		DMA_Mode_Normal | // DMA_Mode_Circular or DMA_Mode_Normal
		DMA_DIR_PeripheralDST;
#endif

	//DMA1_Channel3 is for SPI1TX
	DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
	DMA1_Channel3->MADDR = (uint32_t)sendbuff;
	DMA1_Channel3->CNTR  = 0;// sizeof( bufferset )/2; // Number of unique copies.  (Don't start, yet!)
	DMA1_Channel3->CFGR  =
		DMA_M2M_Disable |		 
		DMA_Priority_VeryHigh |
		DMA_MemoryDataSize_HalfWord |
		DMA_PeripheralDataSize_HalfWord |
		DMA_MemoryInc_Enable |
		DMA_Mode_Circular | // OR DMA_Mode_Circular or DMA_Mode_Normal
		DMA_DIR_PeripheralDST |
		DMA_IT_TC | DMA_IT_HT; // Transmission Complete + Half Empty Interrupts. 

	NVIC_EnableIRQ( DMA1_Channel3_IRQn );

	memset( sendbuff, 0x00, sizeof( sendbuff ) );


	// Enter critical section.
	DMA1_Channel3->MADDR = (uint32_t)sendbuff;
	DMA1_Channel3->CNTR = SENDBUFF_WORDS; // Number of unique uint16_t entries.
	DMA1_Channel3->CFGR |= DMA_CFGR1_EN;

	uint32_t frame = 0;
	while(1)
	{
		Delay_Ms( 1000 );

#ifdef LORAWAN
		// Optionally generate a LoRaWAN Packet.
		const static uint8_t raw_pl[4] = { 0xaa, 0xaa, 0xaa, 0xaa, };
		// Just some random data.
		static uint8_t raw_payload_with_b0[MAX_BYTES+8] = { 0 }; 
		uint8_t * payload_in = raw_payload_with_b0 + 16;
		int payload_in_size;

		// Be sure to provide your appropriate keys and address here.
		static const uint8_t payload_key[AES_BLOCKLEN] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // AppSKey Big Endian
		static const uint8_t network_skey[AES_BLOCKLEN] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // NwkSKey Big Endian
		static const uint8_t devaddress[4] = { 0x00, 0x00, 0x00, 0x00 }; // Device address Little Endian LSB (Written backwards from Device address default view)

		payload_in_size = GenerateLoRaWANPacket( raw_payload_with_b0, raw_pl, sizeof(raw_pl), payload_key, network_skey, devaddress, frame );
#else
		uint8_t payload_in[15] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
		int payload_in_size = 15;

#endif

		lora_symbols_count = 0;
		int r = CreateMessageFromPayload( lora_symbols, &lora_symbols_count, MAX_SYMBOLS, SF_NUMBER, 4, payload_in, payload_in_size );

		if( r < 0 )
		{
			printf( "Failed to generate message (%d)\n", r );
			// Failed
			continue;
		}

		frame++;

		int j;

		quadsetcount = 0;
		int16_t * qso = quadsets;
		for( j = 0; j < PREAMBLE_CHIRPS; j++ )
		{
			qso = AddChirp( qso, 0, 0 );
		}

		uint8_t syncword = 0x43;

	#if SF_NUMBER == 6
		#define CODEWORD_SHIFT 2
	#else
		#define CODEWORD_SHIFT 3
	#endif

		if( CODEWORD_LENGTH > 0 )
			qso = AddChirp( qso,  ( ( syncword & 0xf ) << CODEWORD_SHIFT ), 0 );
		if( CODEWORD_LENGTH > 1 )
			qso = AddChirp( qso, ( ( ( syncword & 0xf0 ) >> 4 ) << CODEWORD_SHIFT ), 0);

		*(qso++) = -(CHIPSSPREAD * 0 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 1 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 2 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 3 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 0 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 1 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 2 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 3 / 4 )-1;
		*(qso++) = -(CHIPSSPREAD * 0 / 4 )-1;

		if( SF_NUMBER <= 6 )
		{
			// Two additional upchirps with SF6 https://github.com/tapparelj/gr-lora_sdr/issues/74#issuecomment-1891569580
			for( j = 0; j < 2; j++ )
				qso = AddChirp( qso, 0, 0 );
		}

		for( j = 0; j < lora_symbols_count; j++ )
		{
			int ofs = lora_symbols[j];
			qso = AddChirp( qso, ofs, 0 );
		}

		runningcount_bits = 0;

		// This tells the interrupt we have data.
		quadsetcount = qso - quadsets + 0;
		printf( "- %d -- %d [%d] %d; %d\n", (int)temp, (int)lora_symbols_count, (int)quadsetcount, CHIPSSPREAD/4, sendbuff[0] );
		quadsetplace = 0;

		DMA1_Channel3->CFGR |= DMA_CFGR1_EN;

		while( quadsetplace >= 0 );
	}
}

