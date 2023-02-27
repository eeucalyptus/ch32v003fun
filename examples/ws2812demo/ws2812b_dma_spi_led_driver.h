/* Single-File-Header for using asynchronous LEDs with the CH32V003 using DMA to the SPI port.
   I may write another version of this to use DMA to timer ports, but, the SPI port can be used
   to generate outputs very efficiently. So, for now, SPI Port.

   Copyright 2023 <>< Charles Lohr, under the MIT-x11 or NewBSD License, you choose!

   If you are including this in main, simply 
	#define WS2812DMA_IMPLEMENTATION

   You will need to implement the following two functions, as callbacks from the ISR.
	uint32_t CallbackWS2812BLED( int ledno );

   You willalso need to call
	InitWS2812DMA();

   Then, whenyou want to update the LEDs, call:
	WS2812BStart( int num_leds );
*/

#ifndef _WS2812_LED_DRIVER_H
#define _WS2812_LED_DRIVER_H

#include <stdint.h>

// Use DMA and SPI to stream out WS2812B LED Data via the MOSI pin.
void WS2812BDMAInit( );
void WS2812BDMAStart( int leds );

// Callbacks that you must implement.
uint32_t WS2812BLEDCallback( int ledno );

extern volatile int WS2812BLEDDone;


#ifdef WS2812DMA_IMPLEMENTATION

// Note first 2 LEDs of DMA Buffer are 0's as a "break"
// Need one extra LED at end to leave line high. 
// This must be greater than WS2812B_RESET_PERIOD.
//  1: Divisble by 2.
//  2: 
#define DMALEDS 16
#define WS2812B_RESET_PERIOD 6
#define DMA_BUFFER_LEN (((DMALEDS+1)/2)*6)

static uint16_t WS2812dmabuff[DMA_BUFFER_LEN];
static int WS2812LEDs;
static int WS2812LEDPlace;
volatile int WS2812BLEDDone;

// This is the code that updates a portion of the WS2812dmabuff with new data.
// This effectively creates the bitstream that outputs to the LEDs.
static void WS2812FillBuffSec( uint16_t * ptr, int numhalfwords, int ending )
{
	const static uint16_t bitquartets[16] = {
		0b1000100010001000, 0b1000100010001110, 0b1000100011101000, 0b1000100011101110,
		0b1000111010001000, 0b1000111010001110, 0b1000111011101000, 0b1000111011101110,
		0b1110100010001000, 0b1110100010001110, 0b1110100011101000, 0b1110100011101110,
		0b1110111010001000, 0b1110111010001110, 0b1110111011101000, 0b1110111011101110, };

	int i;
	uint16_t * end = ptr + numhalfwords;
	int ledcount = WS2812LEDs;
	int place = WS2812LEDPlace;

	while( place < 0 && ptr != end )
	{
		(*ptr++) = 0;
		(*ptr++) = 0;
		place++;
	}

	while( ptr != end )
	{
		if( place >= ledcount )
		{
			// Optionally, leave line high.
			while( ptr != end )
				(*ptr++) = 0;//0xffff;

			// If we're "totally finished" we can set the flag.
			if( place == ledcount+1 ) 
				WS2812BLEDDone = 1;

			// Only safe to do this when we're on the second leg.
			if( ending )
			{
				if( place == ledcount )
				{
					// Take the DMA out of circular mode and let it expire.
					DMA1_Channel3->CFGR &= ~DMA_Mode_Circular;
				}
				place++;
			}


			break;
		}

		uint32_t ledval24bit = WS2812BLEDCallback( place++ );
		ptr[0] = bitquartets[(ledval24bit>>20)&0xf];
		ptr[1] = bitquartets[(ledval24bit>>16)&0xf];
		ptr[2] = bitquartets[(ledval24bit>>12)&0xf];
		ptr[3] = bitquartets[(ledval24bit>>8)&0xf];
		ptr[4] = bitquartets[(ledval24bit>>4)&0xf];
		ptr[5] = bitquartets[(ledval24bit>>0)&0xf];
		ptr += 6;
		i += 6;
	}
	WS2812LEDPlace = place;
}

void DMA1_Channel3_IRQHandler( void ) __attribute__((interrupt));
void DMA1_Channel3_IRQHandler( void ) 
{
	GPIOD->BSHR = 1;	 // Turn on GPIOD0

	// Backup flags.
	int intfr = DMA1->INTFR;

	// Clear all possible flags.
	DMA1->INTFCR = DMA1_IT_GL3;

	if( intfr & DMA1_IT_HT3 )
	{
		// Complete (Fill in second part)
		WS2812FillBuffSec( WS2812dmabuff + DMA_BUFFER_LEN / 2, DMA_BUFFER_LEN / 2, 1 );
	}
	if( intfr & DMA1_IT_TC3 )
	{
		// Halfwaay (Fill in first part)
		WS2812FillBuffSec( WS2812dmabuff, DMA_BUFFER_LEN / 2, 0 );
	}

	GPIOD->BSHR = 1<<16; // Turn off GPIOD0
}

void WS2812BDMAStart( int leds )
{
	WS2812BLEDDone = 0;
	WS2812LEDs = leds;
	WS2812LEDPlace = -WS2812B_RESET_PERIOD;
	WS2812FillBuffSec( WS2812dmabuff, DMA_BUFFER_LEN, 0 );
	DMA1_Channel3->CFGR |= DMA_Mode_Circular;
	DMA1_Channel3->CNTR = DMA_BUFFER_LEN; // Number of unique uint16_t entries.
}

void WS2812BDMAInit( )
{
	WS2812BLEDDone = 1;

	// Enable DMA + Peripherals
	RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;

	// MOSI, Configure GPIO Pin
	GPIOC->CFGLR &= ~(0xf<<(4*6));
	GPIOC->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF)<<(4*6);

	// Configure SPI 
	SPI1->CTLR1 = 
		SPI_NSS_Soft | SPI_CPHA_1Edge | SPI_CPOL_Low | SPI_DataSize_16b |
		SPI_Mode_Master | SPI_Direction_1Line_Tx |
		3<<3; // Dvisior

	SPI1->CTLR2 = SPI_CTLR2_TXDMAEN;
	SPI1->HSCR = 1;

	SPI1->CTLR1 |= CTLR1_SPE_Set;

	SPI1->DATAR = 0; // Set LEDs Low.

	//memset( bufferset, 0xaa, sizeof( bufferset ) );

	//DMA1_Channel3 is for SPI1TX
	DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
	DMA1_Channel3->MADDR = (uint32_t)WS2812dmabuff;
	DMA1_Channel3->CNTR  = 0;// sizeof( bufferset )/2; // Number of unique copies.  (Don't start, yet!)
	DMA1_Channel3->CFGR  =
		DMA_M2M_Disable |		 
		DMA_Priority_VeryHigh |
		DMA_MemoryDataSize_HalfWord |
		DMA_PeripheralDataSize_HalfWord |
		DMA_MemoryInc_Enable |
		DMA_Mode_Normal | // OR DMA_Mode_Circular or DMA_Mode_Normal
		DMA_DIR_PeripheralDST |
		DMA_IT_TC | DMA_IT_HT; // Transmission Complete + Half Empty Interrupts. 

//	NVIC_SetPriority( DMA1_Channel3_IRQn, 0<<4 ); // Regular priority.
	NVIC_EnableIRQ( DMA1_Channel3_IRQn );
	DMA1_Channel3->CFGR |= DMA_CFGR1_EN;
}

#endif

#endif

