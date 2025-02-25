/*************************************************************************
Title:    Yet another (small) modbus (server) implementation for the avr.
Author:   Max Brueggemann
Hardware: any AVR with hardware UART, tested on Atmega 88/168 at 20Mhz
License:  BSD-3-Clause
          
DESCRIPTION:
    Refer to the header file yaMBSiavr.h.
    
USAGE:
    Refer to the header file yaMBSiavr.h.
                    
LICENSE:

Copyright 2017 Max Brueggemann, www.maxbrueggemann.de

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
                        
*************************************************************************/

#include <avr/io.h>
#include "yaMBSiavr.h"
#include <avr/interrupt.h>

volatile unsigned char BusState = 0;
volatile uint16_t modbusTimer = 0;
volatile unsigned char rxbuffer[MaxFrameIndex+1];
volatile uint16_t DataPos = 0;
volatile unsigned char PacketTopIndex = 7;
volatile unsigned char modBusStaMaStates = 0;
volatile uint16_t modbusDataAmount = 0;
volatile uint16_t modbusDataLocation = 0;

/* @brief: save address and amount
*
*/
void modbusSaveLocation(void)
{
	modbusDataLocation=(rxbuffer[3]|(rxbuffer[2]<<8));
	if (rxbuffer[1]==fcPresetSingleRegister || rxbuffer[1]==fcForceSingleCoil) modbusDataAmount=1;
	else modbusDataAmount=(rxbuffer[5]|(rxbuffer[4]<<8));
}

/* @brief: returns 1 if data location adr is touched by current command
*
*         Arguments: - adr: address of the data object
*
*/
uint8_t modbusIsInRange(uint16_t adr)
{
        if((modbusDataLocation <= adr) && (adr<(modbusDataLocation+modbusDataAmount)))
                return 1;
        return 0;
}

/* @brief: returns 1 if range of data locations is touched by current command
*
*         Arguments: - startAdr: address of first data object in range
*                    - lastAdr: address of last data object in range
*
*/
uint8_t modbusIsRangeInRange(uint16_t startAdr, uint16_t lastAdr)
{
        if(modbusIsInRange(startAdr) && modbusIsInRange(lastAdr))
                return 1;
        return 0;
}

uint8_t modbusGetBusState(void)
{
	return BusState;
}

#if ADDRESS_MODE == SINGLE_ADR
volatile unsigned char Address = 0x00;
uint8_t modbusGetAddress(void)
{
	return Address;
}

void modbusSetAddress(unsigned char newadr)
{
	Address = newadr;
}
#endif

#if PHYSICAL_TYPE == 485
void transceiver_txen(void)
{
	TRANSCEIVER_ENABLE_PORT|=(1<<TRANSCEIVER_ENABLE_PIN);
}

 void transceiver_rxen(void)
{
	TRANSCEIVER_ENABLE_PORT&=~(1<<TRANSCEIVER_ENABLE_PIN);
}
#endif

/* @brief: A fairly simple Modbus compliant 16 Bit CRC algorithm.
*
*  	Returns 1 if the crc check is positive, returns 0 and saves the calculated CRC bytes
*	at the end of the data array if it fails.
*  	
*/
uint8_t crc16(volatile uint8_t *ptrToArray,uint8_t inputSize) //A standard CRC algorithm
{
	uint16_t out=0xffff;
	uint16_t carry;
	unsigned char n;
	inputSize++;
	for (int l=0; l<inputSize; l++) {
		out ^= ptrToArray[l];
		for (n = 0; n < 8; n++) {
			carry = out & 1;
			out >>= 1;
			if (carry) out ^= 0xA001;
		}
	}
	//out=0x1234;
	if ((ptrToArray[inputSize]==out%256) && (ptrToArray[inputSize+1]==out/256)) //check
	{
		return 1;
	} else { 
		ptrToArray[inputSize]=out%256; //append Lo
		ptrToArray[inputSize+1]=out/256; //append Hi
		return 0;	
	}
}

/* @brief: copies a single bit from one char to another char (or arrays thereof)
*
*
*/
void listBitCopy(volatile uint8_t *source, uint16_t sourceNr,volatile uint8_t *target, uint16_t targetNr)
{
	if(*(source+(sourceNr/8))&(1<<(sourceNr-((sourceNr/8)*8))))
	{
		*(target+(targetNr/8))|=(1<<(targetNr-((targetNr/8)*8)));
	} else *(target+(targetNr/8))&=~(1<<(targetNr-((targetNr/8)*8)));
}

/* @brief: Back to receiving state.
*
*/
void modbusReset(void)
{
	BusState=(1<<TimerActive); //stop receiving (error)
	modbusTimer=0;
}

void modbusTickTimer(void)
{
	if (BusState&(1<<TimerActive)) 
	{
		modbusTimer++;
		if (BusState&(1<<Receiving)) //we are in receiving mode
		{
			if ((modbusTimer==modbusInterCharTimeout)) {
				BusState|=(1<<GapDetected);
			} else if ((modbusTimer==modbusInterFrameDelayReceiveEnd)) { //end of message
				#if ADDRESS_MODE == MULTIPLE_ADR
               		 if (crc16(rxbuffer,DataPos-3)) { //perform crc check only. This is for multiple/all address mode.
				modbusSaveLocation();
				BusState=(1<<ReceiveCompleted);
			 } else modbusReset();
				#endif
				#if ADDRESS_MODE == SINGLE_ADR
				if (rxbuffer[0]==Address && crc16(rxbuffer,DataPos-3)) { //is the message for us? => perform crc check
					modbusSaveLocation();
					BusState=(1<<ReceiveCompleted);
				} else modbusReset();
				#endif
			}
		} else if (modbusTimer==modbusInterFrameDelayReceiveStart) BusState|=(1<<BusTimedOut);
	}
}

ISR(UART_RECEIVE_INTERRUPT)
{
	unsigned char data;
#if defined(attiny3226_init)
	data = UART_N.RXDATAL;
#else
	data = UART_DATA;
#endif
	modbusTimer=0; //reset timer
	if (!(BusState & (1<<ReceiveCompleted)) && !(BusState & (1<<TransmitRequested)) && !(BusState & (1<<Transmitting)) && (BusState & (1<<Receiving)) && !(BusState & (1<<BusTimedOut)))
	{
		if (DataPos>MaxFrameIndex) 
		{
			modbusReset();
		}
	    else
		{
			rxbuffer[DataPos]=data;
			DataPos++; //TODO: maybe prevent this from exceeding 255?
		}	    
    } 
	else if (!(BusState & (1<<ReceiveCompleted)) && !(BusState & (1<<TransmitRequested)) && !(BusState & (1<<Transmitting)) && !(BusState & (1<<Receiving)) && (BusState & (1<<BusTimedOut))) 
	{ 
		 rxbuffer[0]=data;
		 BusState=((1<<Receiving)|(1<<TimerActive));
		 DataPos=1;
    }
}

ISR(UART_TRANSMIT_INTERRUPT)
{
	BusState&=~(1<<TransmitRequested);
	BusState|=(1<<Transmitting);
#if defined(attiny3226_init)
	UART_N.TXDATAL=rxbuffer[DataPos];
#else
	UART_DATA=rxbuffer[DataPos];
#endif
	DataPos++;
	if (DataPos==(PacketTopIndex+1)) 
	{
#if defined(attiny3226_init)
		UART_N.CTRLA &= ~(USART_DREIE_bm);
#else
		UART_CONTROL&=~(1<<UART_UDRIE);
#endif
	}
}

ISR(UART_TRANSMIT_COMPLETE_INTERRUPT)
{
	#if PHYSICAL_TYPE == 485
	transceiver_rxen();
	#endif
	modbusReset();
#if defined(attiny3226_init)
	UART_N.STATUS |= USART_TXCIF_bm;
#endif
}

void modbusInit(void)
{
#if defined(attiny3226_init) 
	TXPORT.DIR |=  (1 << TXPIN);
	RXPORT.DIR &= ~(1 << RXPIN);
	UART_PORTMUX &= UART_PORTMUX_AND_MASK;
	UART_PORTMUX |= UART_PORTMUX_OR_MASK;
	UART_N.BAUD = BAUD_PRESC;
	UART_N.CTRLA = USART_TXCIE_bm | USART_RXCIE_bm;
	UART_N.CTRLC = USART_CHSIZE_0_bm | USART_CHSIZE_1_bm;
	UART_N.CTRLB = USART_RXEN_bm | USART_TXEN_bm | USART_RXMODE_0_bm;
#else
	UBRRH = (unsigned char)((_UBRR) >> 8);
	UBRRL = (unsigned char) _UBRR;
	UART_STATUS = (1<<U2X); //double speed mode.
#ifdef URSEL   // if UBRRH and UCSRC share the same I/O location , e.g. ATmega8
	UCSRC = (1<<URSEL)|(3<<UCSZ0); //Frame Size
#else
   UCSRC = (3<<UCSZ0); //Frame Size
#endif
	UART_CONTROL = (1<<TXCIE)|(1<<RXCIE)|(1<<RXEN)|(1<<TXEN); // USART receiver and transmitter and receive complete interrupt
#endif
	#if PHYSICAL_TYPE == 485
	TRANSCEIVER_ENABLE_PORT_DDR|=(1<<TRANSCEIVER_ENABLE_PIN);
	transceiver_rxen();
	#endif
	BusState=(1<<TimerActive);
}

/* @brief: Sends a response.
*
*         Arguments: - packtop: Position of the last byte containing data.
*                               modbusSendException is a good usage example.
*/
void modbusSendMessage(unsigned char packtop)
{
	PacketTopIndex=packtop+2;
	crc16(rxbuffer,packtop);
	BusState|=(1<<TransmitRequested);
	DataPos=0;
	#if PHYSICAL_TYPE == 485
	transceiver_txen();
	#endif
#if defined(attiny3226_init)
	UART_N.CTRLA |= USART_DREIE_bm;
#else
	UART_CONTROL|=(1<<UART_UDRIE);
#endif
	BusState&=~(1<<ReceiveCompleted);
}

/* @brief: Sends an exception response.
*
*         Arguments: - exceptionCode
*                              
*/
void modbusSendException(unsigned char exceptionCode)
{
	rxbuffer[1]|=(1<<7); //setting MSB of the function code (the exception flag)
	rxbuffer[2]=exceptionCode; //Exceptioncode. Also the last byte containing data
	modbusSendMessage(2);
}


/* @brief:  Returns the amount of requested data objects (coils, discretes, registers)
*
*/
uint16_t modbusRequestedAmount(void)
{
	return modbusDataAmount;
}

/* @brief: Returns the address of the first requested data object (coils, discretes, registers)
*
*/
uint16_t modbusRequestedAddress(void)
{
	return modbusDataLocation;
}

/* @brief: copies a single or multiple bytes from one array of bytes to an array of 16-bit-words
*
*/
void intToModbusRegister(volatile uint16_t *inreg, volatile uint8_t *outreg, uint8_t amount)
{
	for (uint8_t c=0; c<amount; c++)
	{
			*(outreg+c*2) = (uint8_t)(*(inreg+c) >> 8);
			*(outreg+1+c*2) = (uint8_t)(*(inreg+c));
	}
}

/* @brief: copies a single or multiple 16-bit-words from one array of integers to an array of bytes
*
*/
void modbusRegisterToInt(volatile uint8_t *inreg, volatile uint16_t *outreg, uint8_t amount)
{
	for (uint8_t c=0; c<amount; c++)
	{
		*(outreg+c) = (*(inreg+c*2) << 8) + *(inreg+1+c*2);
	}
}

/* @brief: Handles single/multiple register reading and single/multiple register writing.
*
*         Arguments: - ptrToInArray: pointer to the user's data array containing registers
*                    - startAddress: address of the first register in the supplied array
*                    - size: input array size in the requested format (16bit-registers)
*
*/
uint8_t modbusExchangeRegisters(volatile uint16_t *ptrToInArray, uint16_t startAddress, uint16_t size)
{
	if ((modbusDataLocation>=startAddress) && ((startAddress+size)>=(modbusDataAmount+modbusDataLocation))) {
		
		if ((rxbuffer[1]==fcReadHoldingRegisters) || (rxbuffer[1]==fcReadInputRegisters) )
		{
			if ((modbusDataAmount*2)<=(MaxFrameIndex-4)) //message buffer big enough?
			{
				rxbuffer[2]=(unsigned char)(modbusDataAmount*2);
				intToModbusRegister(ptrToInArray+(modbusDataLocation-startAddress),rxbuffer+3,modbusDataAmount);
				modbusSendMessage(2+rxbuffer[2]);
				return 1;
			} else modbusSendException(ecIllegalDataValue);
		}
		else if (rxbuffer[1]==fcPresetMultipleRegisters)
		{
			if (((rxbuffer[6])>=modbusDataAmount*2) && ((DataPos-9)>=rxbuffer[6])) //enough data received?
			{
				modbusRegisterToInt(rxbuffer+7,ptrToInArray+(modbusDataLocation-startAddress),(unsigned char)(modbusDataAmount));
				modbusSendMessage(5);
				return 1;
			} else modbusSendException(ecIllegalDataValue);//too few data bytes received
		}
		else if (rxbuffer[1]==fcPresetSingleRegister)
		{
			modbusRegisterToInt(rxbuffer+4,ptrToInArray+(modbusDataLocation-startAddress),1);
			modbusSendMessage(5);
			return 1;
		} 
		//modbusSendException(ecSlaveDeviceFailure); //inapropriate call of modbusExchangeRegisters
		return 0;
		} else {
		modbusSendException(ecIllegalDataValue);
		return 0;
	}
}

/* @brief: Handles single/multiple input/coil reading and single/multiple coil writing.
*
*         Arguments: - ptrToInArray: pointer to the user's data array containing bits
*                    - startAddress: address of the first bit in the supplied array
*                    - size: input array size in the requested format (bits)
*
*/
uint8_t modbusExchangeBits(volatile uint8_t *ptrToInArray, uint16_t startAddress, uint16_t size)
{
	if ((modbusDataLocation>=startAddress) && ((startAddress+size)>=(modbusDataAmount+modbusDataLocation)))
	{
		if ((rxbuffer[1]==fcReadInputStatus) || (rxbuffer[1]==fcReadCoilStatus))
		{
			if (modbusDataAmount<=((MaxFrameIndex-4)*8)) //message buffer big enough?
			{
				rxbuffer[2]=(modbusDataAmount/8);
				if (modbusDataAmount%8>0)
				{
					rxbuffer[(uint8_t)(modbusDataAmount/8)+3]=0x00; //fill last data byte with zeros
					rxbuffer[2]++;
				}
				for (uint16_t c = 0; c<modbusDataAmount; c++)
				{
					listBitCopy(ptrToInArray,modbusDataLocation-startAddress+c,rxbuffer+3,c);
				}
				modbusSendMessage(rxbuffer[2]+2);
				return 1;
			} else modbusSendException(ecIllegalDataValue); //too many bits requested within single request
		}
		else if (rxbuffer[1]==fcForceMultipleCoils)
		{
			if (((rxbuffer[6]*8)>=modbusDataAmount) && ((DataPos-9)>=rxbuffer[6])) //enough data received?
			{
				for (uint16_t c = 0; c<modbusDataAmount; c++)
				{
					listBitCopy(rxbuffer+7,c,ptrToInArray,modbusDataLocation-startAddress+c);
				}
				modbusSendMessage(5);
				return 1;
			} else modbusSendException(ecIllegalDataValue);//exception too few data bytes received
		}
		else if (rxbuffer[1]==fcForceSingleCoil) {
			listBitCopy(rxbuffer+4,0,ptrToInArray,modbusDataLocation-startAddress);
			modbusSendMessage(5); 
			return 1;
		}
		//modbusSendException(ecSlaveDeviceFailure); //inanpropriate call of modbusExchangeBits
		return 0;
	} else
	{
		modbusSendException(ecIllegalDataValue);
		return 0;
	}
}
