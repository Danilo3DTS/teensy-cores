/* Teensyduino Core Library
 * http://www.pjrc.com/teensy/
 * Copyright (c) 2017 PJRC.COM, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * 1. The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * 2. If the Software is incorporated into a build system that allows
 * selection among a list of target devices, then similar target
 * devices manufactured by PJRC.COM must be included in the list of
 * target devices and selectable in the same manner.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Dan.The main idea of this is to have two buffers: receive (rx_buffer) and transmit (tx_buffer).
* In order to transmit, the user must write data to tx_buffer, via methods like serial_write() and serial_print(). 
New data is written to the head of the buffer (usually to the end of the array), 
while data to be sent is read from the tail into the TDR, and the tail keeps moving towards the head.
* In order to receive, the user must read from the rx_buffer. 
New data received is read from the RDR and written to the head of the buffer. 
The user reads this data from the tail of the buffer, and moves towards the head.
*/
#include "kinetis.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include <stddef.h>

////////////////////////////////////////////////////////////////
// Tunable parameters (relatively safe to edit these numbers)
////////////////////////////////////////////////////////////////

#ifndef SERIAL1_TX_BUFFER_SIZE
#define SERIAL1_TX_BUFFER_SIZE     64 // number of outgoing bytes to buffer
#endif
#ifndef SERIAL1_RX_BUFFER_SIZE
#define SERIAL1_RX_BUFFER_SIZE     64 // number of incoming bytes to buffer
#endif
#define RTS_HIGH_WATERMARK (SERIAL1_RX_BUFFER_SIZE-24) // RTS requests sender to pause
#define RTS_LOW_WATERMARK  (SERIAL1_RX_BUFFER_SIZE-38) // RTS allows sender to resume
#define IRQ_PRIORITY  64  // 0 = highest priority, 255 = lowest


////////////////////////////////////////////////////////////////
// changes not recommended below this point....
////////////////////////////////////////////////////////////////

#ifdef SERIAL_9BIT_SUPPORT // Dan. if using 9 bits, then the minimum data size is 16bit
static uint8_t use9Bits = 0;  
#define BUFTYPE uint16_t
#else
#define BUFTYPE uint8_t
#define use9Bits 0
#endif

static volatile BUFTYPE tx_buffer[SERIAL1_TX_BUFFER_SIZE];  // the bytes to be sent will be stored here
static volatile BUFTYPE rx_buffer[SERIAL1_RX_BUFFER_SIZE];  // the bytes received will be stored here
static volatile BUFTYPE	*rx_buffer_storage_ = NULL;
static volatile BUFTYPE	*tx_buffer_storage_ = NULL;

static size_t tx_buffer_total_size_ = SERIAL1_TX_BUFFER_SIZE;
static size_t rx_buffer_total_size_ = SERIAL1_RX_BUFFER_SIZE;
static size_t rts_low_watermark_ = RTS_LOW_WATERMARK;
static size_t rts_high_watermark_ = RTS_HIGH_WATERMARK;

static volatile uint8_t transmitting = 0;
#if defined(KINETISK)
  static volatile uint8_t *transmit_pin=NULL;
  #define transmit_assert()   *transmit_pin = 1
  #define transmit_deassert() *transmit_pin = 0
  static volatile uint8_t *rts_pin=NULL;
  #define rts_assert()        *rts_pin = 0
  #define rts_deassert()      *rts_pin = 1
#elif defined(KINETISL)
  static volatile uint8_t *transmit_pin=NULL;
  static uint8_t transmit_mask=0;
  #define transmit_assert()   *(transmit_pin+4) = transmit_mask;
  #define transmit_deassert() *(transmit_pin+8) = transmit_mask;
  static volatile uint8_t *rts_pin=NULL;
  static uint8_t rts_mask=0;
  #define rts_assert()        *(rts_pin+8) = rts_mask;
  #define rts_deassert()      *(rts_pin+4) = rts_mask;
#endif
#if SERIAL1_TX_BUFFER_SIZE > 65535
static volatile uint32_t tx_buffer_head = 0;
static volatile uint32_t tx_buffer_tail = 0;
#elif SERIAL1_TX_BUFFER_SIZE > 255
static volatile uint16_t tx_buffer_head = 0;
static volatile uint16_t tx_buffer_tail = 0;
#else
static volatile uint8_t tx_buffer_head = 0;
static volatile uint8_t tx_buffer_tail = 0;
#endif
#if SERIAL1_RX_BUFFER_SIZE > 65535
static volatile uint32_t rx_buffer_head = 0;
static volatile uint32_t rx_buffer_tail = 0;
#elif SERIAL1_RX_BUFFER_SIZE > 255
static volatile uint16_t rx_buffer_head = 0;
static volatile uint16_t rx_buffer_tail = 0;
#else
static volatile uint8_t rx_buffer_head = 0;
static volatile uint8_t rx_buffer_tail = 0;
#endif
static uint8_t rx_pin_num = 0;
static uint8_t tx_pin_num = 1;
#if defined(KINETISL)
static uint8_t half_duplex_mode = 0;
#endif

// UART0 and UART1 are clocked by F_CPU, UART2 is clocked by F_BUS
// UART0 has 8 byte fifo, UART1 and UART2 have 1 byte buffer

#ifdef HAS_KINETISK_UART0_FIFO
#define C2_ENABLE		UART_C2_TE | UART_C2_RE | UART_C2_RIE | UART_C2_ILIE
#else
#define C2_ENABLE		UART_C2_TE | UART_C2_RE | UART_C2_RIE
#endif
#define C2_TX_ACTIVE		C2_ENABLE | UART_C2_TIE
#define C2_TX_COMPLETING	C2_ENABLE | UART_C2_TCIE
#define C2_TX_INACTIVE		C2_ENABLE  // Dan. Active but doing nothing

// BITBAND Support
#define GPIO_BITBAND_ADDR(reg, bit) (((uint32_t)&(reg) - 0x40000000) * 32 + (bit) * 4 + 0x42000000)
#define GPIO_BITBAND_PTR(reg, bit) ((uint32_t *)GPIO_BITBAND_ADDR((reg), (bit)))
#define C3_TXDIR_BIT 5


void serial_begin(uint32_t divisor)
{
	SIM_SCGC4 |= SIM_SCGC4_UART0;	// turn on clock, TODO: use bitband
	rx_buffer_head = 0;
	rx_buffer_tail = 0;
	tx_buffer_head = 0;
	tx_buffer_tail = 0;
	transmitting = 0;
	switch (rx_pin_num) {
		case 0:  CORE_PIN0_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
		case 21: CORE_PIN21_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
		#if defined(KINETISL)
		case 3:  CORE_PIN3_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(2); break;
		case 25: CORE_PIN25_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(4); break;
		#endif
		#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
		case 27: CORE_PIN27_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
		#endif
	}
	switch (tx_pin_num) {
		case 1:  CORE_PIN1_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3); break;
		case 5:  CORE_PIN5_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3); break;
		#if defined(KINETISL)
		case 4:  CORE_PIN4_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(2); break;
		case 24: CORE_PIN24_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(4); break;
		#endif
		#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
		case 26: CORE_PIN26_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3); break;
		#endif
	}
#if defined(HAS_KINETISK_UART0)
	if (divisor < 32) divisor = 32;
	UART0_BDH = (divisor >> 13) & 0x1F;
	UART0_BDL = (divisor >> 5) & 0xFF;
	UART0_C4 = divisor & 0x1F;
#ifdef HAS_KINETISK_UART0_FIFO
	UART0_C1 = UART_C1_ILT;
	UART0_TWFIFO = 2; // tx watermark, causes S1_TDRE to set
	UART0_RWFIFO = 4; // rx watermark, causes S1_RDRF to set
	UART0_PFIFO = UART_PFIFO_TXFE | UART_PFIFO_RXFE;
#else
	UART0_C1 = 0;
	UART0_PFIFO = 0;
#endif
#elif defined(HAS_KINETISL_UART0)
	if (divisor < 1) divisor = 1;
	UART0_BDH = (divisor >> 8) & 0x1F;
	UART0_BDL = divisor & 0xFF;
	UART0_C1 = 0;
#endif
	UART0_C2 = C2_TX_INACTIVE;
	NVIC_SET_PRIORITY(IRQ_UART0_STATUS, IRQ_PRIORITY);
	NVIC_ENABLE_IRQ(IRQ_UART0_STATUS);
}

void serial_format(uint32_t format)
{
	uint8_t c;

	c = UART0_C1;
	c = (c & ~0x13) | (format & 0x03);	// configure parity
	if (format & 0x04) c |= 0x10;		// 9 bits (might include parity)
	UART0_C1 = c;
	if ((format & 0x0F) == 0x04) UART0_C3 |= 0x40; // 8N2 is 9 bit with 9th bit always 1
	c = UART0_S2 & ~0x10;
	if (format & 0x10) c |= 0x10;		// rx invert
	UART0_S2 = c;
	c = UART0_C3 & ~0x10;
	if (format & 0x20) c |= 0x10;		// tx invert
	UART0_C3 = c;
#ifdef SERIAL_9BIT_SUPPORT
	c = UART0_C4 & 0x1F;
	if (format & 0x08) c |= 0x20;		// 9 bit mode with parity (requires 10 bits)
	UART0_C4 = c;
	use9Bits = format & 0x80;
#endif
#if defined(__MK64FX512__) || defined(__MK66FX1M0__) || defined(KINETISL)
	// For T3.5/T3.6/TLC See about turning on 2 stop bit mode
	if ( format & 0x100) {
		uint8_t bdl = UART0_BDL;
		UART0_BDH |= UART_BDH_SBNS;		// Turn on 2 stop bits - was turned off by set baud
		UART0_BDL = bdl;		// Says BDH not acted on until BDL is written
	}
#endif
	// process request for half duplex.
	if ((format & SERIAL_HALF_DUPLEX) != 0) {
		c = UART0_C1;
		c |= UART_C1_LOOPS | UART_C1_RSRC;
		UART0_C1 = c;

		// Lets try to make use of bitband address to set the direction for ue...
		#if defined(KINETISL)
		switch (tx_pin_num) {
			case 1:  CORE_PIN1_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3) | PORT_PCR_PE | PORT_PCR_PS ; break;
			case 5:  CORE_PIN5_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3) | PORT_PCR_PE | PORT_PCR_PS; break;
			case 4:  CORE_PIN4_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(2) | PORT_PCR_PE | PORT_PCR_PS; break;
			case 24: CORE_PIN24_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(4) | PORT_PCR_PE | PORT_PCR_PS; break;
		}
		half_duplex_mode = 1; 
		#else
		volatile uint32_t *reg = portConfigRegister(tx_pin_num);
		*reg = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3) | PORT_PCR_PE | PORT_PCR_PS; // pullup on output pin;
		transmit_pin = (uint8_t*)GPIO_BITBAND_PTR(UART0_C3, C3_TXDIR_BIT);
		#endif

	} else {
		#if defined(KINETISL)
		half_duplex_mode = 0; 
		#else
		if (transmit_pin == (uint8_t*)GPIO_BITBAND_PTR(UART0_C3, C3_TXDIR_BIT)) transmit_pin = NULL;
		#endif
	}
}

void serial_end(void)
{
	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return;
	while (transmitting) yield();  // wait for buffered data to send
	NVIC_DISABLE_IRQ(IRQ_UART0_STATUS);
	UART0_C2 = 0;
	switch (rx_pin_num) {
		case 0:  CORE_PIN0_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		case 21: CORE_PIN21_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#if defined(KINETISL)
		case 3:  CORE_PIN3_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		case 25: CORE_PIN25_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#endif
		#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
		case 27: CORE_PIN27_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#endif
	}
	switch (tx_pin_num & 127) {
		case 1:  CORE_PIN1_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		case 5:  CORE_PIN5_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#if defined(KINETISL)
		case 4:  CORE_PIN4_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		case 24: CORE_PIN24_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#endif
		#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
		case 26: CORE_PIN26_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1); break;
		#endif
	}
	UART0_S1;
	UART0_D; // clear leftover error status
	rx_buffer_head = 0;
	rx_buffer_tail = 0;
	if (rts_pin) rts_deassert();
}

void serial_set_transmit_pin(uint8_t pin)
{
	while (transmitting) ;
	pinMode(pin, OUTPUT);
	digitalWrite(pin, LOW);
	transmit_pin = portOutputRegister(pin);
	#if defined(KINETISL)
	transmit_mask = digitalPinToBitMask(pin);
	#endif
}

void serial_set_tx(uint8_t pin, uint8_t opendrain)
{
	uint32_t cfg;

	if (opendrain) pin |= 128;
	if (pin == tx_pin_num) return;
	if ((SIM_SCGC4 & SIM_SCGC4_UART0)) {
		switch (tx_pin_num & 127) {
			case 1:  CORE_PIN1_CONFIG = 0; break; // PTB17
			case 5:  CORE_PIN5_CONFIG = 0; break; // PTD7
			#if defined(KINETISL)
			case 4:  CORE_PIN4_CONFIG = 0; break; // PTA2
			case 24: CORE_PIN24_CONFIG = 0; break; // PTE20
			#endif
			#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
			case 26: CORE_PIN26_CONFIG = 0; break; //PTA14
			#endif
		}
		if (opendrain) {
			cfg = PORT_PCR_DSE | PORT_PCR_ODE;
		} else {
			cfg = PORT_PCR_DSE | PORT_PCR_SRE;
		}
		switch (pin & 127) {
			case 1:  CORE_PIN1_CONFIG = cfg | PORT_PCR_MUX(3); break;
			case 5:  CORE_PIN5_CONFIG = cfg | PORT_PCR_MUX(3); break;
			#if defined(KINETISL)
			case 4:  CORE_PIN4_CONFIG = cfg | PORT_PCR_MUX(2); break;
			case 24: CORE_PIN24_CONFIG = cfg | PORT_PCR_MUX(4); break;
			#endif
			#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
			case 26: CORE_PIN26_CONFIG = cfg | PORT_PCR_MUX(3); break;
			#endif
		}
	}
	tx_pin_num = pin;
}

void serial_set_rx(uint8_t pin)
{
	if (pin == rx_pin_num) return;
	if ((SIM_SCGC4 & SIM_SCGC4_UART0)) {
		switch (rx_pin_num) {
			case 0:  CORE_PIN0_CONFIG = 0; break; // PTB16
			case 21: CORE_PIN21_CONFIG = 0; break; // PTD6
			#if defined(KINETISL)
			case 3:  CORE_PIN3_CONFIG = 0; break; // PTA1
			case 25: CORE_PIN25_CONFIG = 0; break; // PTE21
			#endif
			#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
			case 27: CORE_PIN27_CONFIG = 0; break; // PTA15
			#endif
		}
		switch (pin) {
			case 0:  CORE_PIN0_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
			case 21: CORE_PIN21_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
			#if defined(KINETISL)
			case 3:  CORE_PIN3_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(2); break;
			case 25: CORE_PIN25_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(4); break;
			#endif
			#if defined(__MK64FX512__) || defined(__MK66FX1M0__)
			case 27: CORE_PIN27_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); break;
			#endif
		}
	}
	rx_pin_num = pin;
}



int serial_set_rts(uint8_t pin)
{
	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return 0;
	if (pin < CORE_NUM_DIGITAL) {
		rts_pin = portOutputRegister(pin);
		#if defined(KINETISL)
		rts_mask = digitalPinToBitMask(pin);
		#endif
		pinMode(pin, OUTPUT);
		rts_assert();
	} else {
		rts_pin = NULL;
		return 0;
	}
/*
	if (pin == 6) {
		CORE_PIN6_CONFIG = PORT_PCR_MUX(3);
	} else if (pin == 19) {
		CORE_PIN19_CONFIG = PORT_PCR_MUX(3);
	} else {
		UART0_MODEM &= ~UART_MODEM_RXRTSE;
		return 0;
	}
	UART0_MODEM |= UART_MODEM_RXRTSE;
*/
	return 1;
}

int serial_set_cts(uint8_t pin)
{
#if defined(KINETISK)
	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return 0;
	if (pin == 18) {
		CORE_PIN18_CONFIG = PORT_PCR_MUX(3) | PORT_PCR_PE; // weak pulldown
	} else if (pin == 20) {
		CORE_PIN20_CONFIG = PORT_PCR_MUX(3) | PORT_PCR_PE; // weak pulldown
	} else {
		UART0_MODEM &= ~UART_MODEM_TXCTSE;
		return 0;
	}
	UART0_MODEM |= UART_MODEM_TXCTSE;
	return 1;
#else
	return 0;
#endif
}

// Dan. appends the designated character to the tx_buffer. If the buffer is full, it clears it by sending the bytes.
// Dan. These bytes in the tx_buffer are normally sent in the isr.
void serial_putchar(uint32_t c)
{
	uint32_t head, n;

	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return;  // Dan. I guess: If this UART is not enabled
	if (transmit_pin) transmit_assert();  // Dan. The pin goes HIGH before transmission
	#if defined(KINETISL)
	if (half_duplex_mode) {
		__disable_irq();
		volatile uint32_t reg = UART0_C3;
		reg |= UART_C3_TXDIR;
		UART0_C3 = reg;
		__enable_irq();
	}
	#endif 
	head = tx_buffer_head;
	if (++head >= tx_buffer_total_size_) head = 0;  // Dan. If the head has exceeded the total storage, then write to the beginning of the array
	// Dan. Clearing the tx_buffer. Head is chasing tail. Tail incremented in isr. Only then can a new byte be put in the buffer.
	while (tx_buffer_tail == head) {  // Dan. This blocks the CPU 
		int priority = nvic_execution_priority();
		if (priority <= IRQ_PRIORITY) {
			if ((UART0_S1 & UART_S1_TDRE)) {  // Dan. If the transmit data register is empty (something can be placed in it to be sent)
				uint32_t tail = tx_buffer_tail;
				if (++tail >= tx_buffer_total_size_) tail = 0;  // Dan. Increments tail
				if (tail < SERIAL1_TX_BUFFER_SIZE) {  // Dan. If tail is within the range of tx_buffer size
					n = tx_buffer[tail];  // Dan. Read a byte from the first array (tx_buffer), and store it in n. Buffer is read from the tail and written from the head.
				} else {
					n = tx_buffer_storage_[tail-SERIAL1_TX_BUFFER_SIZE];  // Dan. This is probably after extending the tx buffer storage. Read from the second, extended array (tx_buffer_storage_)
				}
				if (use9Bits) UART0_C3 = (UART0_C3 & ~0x40) | ((n & 0x100) >> 2);
				UART0_D = n;  // Dan. Write n to the data register.
				tx_buffer_tail = tail;
			}
		} else if (priority >= 256) {
			yield();
		}
	}
	if (head < SERIAL1_TX_BUFFER_SIZE) {
		tx_buffer[head] = c;  // Dan. The character is written to the head of the array
	} else {
		tx_buffer_storage_[head - SERIAL1_TX_BUFFER_SIZE] = c;  // Dan. If additional storage has been requested, write the new character to the head of the extra array.
	}
	transmitting = 1;
	tx_buffer_head = head;
	UART0_C2 = C2_TX_ACTIVE;  // Dan. Enable TE, RE, RIE, and TIE
}

#ifdef HAS_KINETISK_UART0_FIFO
void serial_write(const void *buf, unsigned int count)
{
	const uint8_t *p = (const uint8_t *)buf;
	const uint8_t *end = p + count;
	uint32_t head, n;

	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return;
	if (transmit_pin) transmit_assert();
	while (p < end) {
		head = tx_buffer_head;
		if (++head >= tx_buffer_total_size_) head = 0;
		if (tx_buffer_tail == head) {
			UART0_C2 = C2_TX_ACTIVE;
			do {
				int priority = nvic_execution_priority();
				if (priority <= IRQ_PRIORITY) {
					if ((UART0_S1 & UART_S1_TDRE)) {
						uint32_t tail = tx_buffer_tail;
						if (++tail >= tx_buffer_total_size_) tail = 0;
						if (tail < SERIAL1_TX_BUFFER_SIZE) {
							n = tx_buffer[tail];
						} else {
							n = tx_buffer_storage_[tail-SERIAL1_TX_BUFFER_SIZE];
						}
						if (use9Bits) UART0_C3 = (UART0_C3 & ~0x40) | ((n & 0x100) >> 2);
						UART0_D = n;
						tx_buffer_tail = tail;
					}
				} else if (priority >= 256) {
					yield();
				}
			} while (tx_buffer_tail == head);
		}
		if (head < SERIAL1_TX_BUFFER_SIZE) {
			tx_buffer[head] = *p++;
		} else {
			tx_buffer_storage_[head - SERIAL1_TX_BUFFER_SIZE] = *p++;
		}
		transmitting = 1;
		tx_buffer_head = head;
	}
	UART0_C2 = C2_TX_ACTIVE;
}
#else
// Dan. Given a buffer and the number of items in it, put each one in the tx_buffer
void serial_write(const void *buf, unsigned int count)
{
	const uint8_t *p = (const uint8_t *)buf;
	while (count-- > 0) serial_putchar(*p++);  // Dan. I think this puts byte by byte in the tx_buffer
}
#endif

void serial_flush(void)
{
	while (transmitting) yield(); // wait
}

int serial_write_buffer_free(void)
{
	uint32_t head, tail;

	head = tx_buffer_head;
	tail = tx_buffer_tail;
	if (head >= tail) return tx_buffer_total_size_ - 1 - head + tail;
	return tail - head - 1;
}

// Dan. Returns the number of bytes received not yet read
int serial_available(void)
{
	uint32_t head, tail;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head >= tail) return head - tail;  // Dan. The characters to be read are between head and tail.
	return rx_buffer_total_size_ + head - tail;  // Dan. This will never return a negative number
}

// Dan. Gets the oldest character from the rx_buffer and returns it.
int serial_getchar(void)
{
	uint32_t head, tail;
	int c;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head == tail) return -1;
	if (++tail >= rx_buffer_total_size_) tail = 0;
	if (tail < SERIAL1_RX_BUFFER_SIZE) {
		c = rx_buffer[tail];  // Dan. The value is read from the tail, but written to the head. A value is written to rx_buffer in the isr.
	} else {
		c = rx_buffer_storage_[tail-SERIAL1_RX_BUFFER_SIZE];
	}
	rx_buffer_tail = tail;  // Dan. Next read from the next tail
	if (rts_pin) {
		int avail;
		if (head >= tail) avail = head - tail;
		else avail = rx_buffer_total_size_ + head - tail;
		if (avail <= rts_low_watermark_) rts_assert();
	}
	return c;
}

int serial_peek(void)
{
	uint32_t head, tail;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head == tail) return -1;
	if (++tail >= rx_buffer_total_size_) tail = 0;
	if (tail < SERIAL1_RX_BUFFER_SIZE) {
		return rx_buffer[tail];
	}
	return rx_buffer_storage_[tail-SERIAL1_RX_BUFFER_SIZE];
}

void serial_clear(void)
{
#ifdef HAS_KINETISK_UART0_FIFO
	if (!(SIM_SCGC4 & SIM_SCGC4_UART0)) return;
	UART0_C2 &= ~(UART_C2_RE | UART_C2_RIE | UART_C2_ILIE);
	UART0_CFIFO = UART_CFIFO_RXFLUSH;
	UART0_C2 |= (UART_C2_RE | UART_C2_RIE | UART_C2_ILIE);
#endif
	rx_buffer_head = rx_buffer_tail;
	if (rts_pin) rts_assert();
}

// status interrupt combines
//   Transmit data below watermark  UART_S1_TDRE
//   Transmit complete		    UART_S1_TC
//   Idle line			    UART_S1_IDLE
//   Receive data above watermark   UART_S1_RDRF
//   LIN break detect		    UART_S2_LBKDIF
//   RxD pin active edge	    UART_S2_RXEDGIF

// UART Interrupt handler
void uart0_status_isr(void)
{
	uint32_t head, tail, n;
	uint8_t c;
#ifdef HAS_KINETISK_UART0_FIFO
	uint32_t newhead;
	uint8_t avail;

	if (UART0_S1 & (UART_S1_RDRF | UART_S1_IDLE)) {
		__disable_irq();
		avail = UART0_RCFIFO;
		if (avail == 0) {
			// The only way to clear the IDLE interrupt flag is
			// to read the data register.  But reading with no
			// data causes a FIFO underrun, which causes the
			// FIFO to return corrupted data.  If anyone from
			// Freescale reads this, what a poor design!  There
			// write should be a write-1-to-clear for IDLE.
			c = UART0_D;
			// flushing the fifo recovers from the underrun,
			// but there's a possible race condition where a
			// new character could be received between reading
			// RCFIFO == 0 and flushing the FIFO.  To minimize
			// the chance, interrupts are disabled so a higher
			// priority interrupt (hopefully) doesn't delay.
			// TODO: change this to disabling the IDLE interrupt
			// which won't be simple, since we already manage
			// which transmit interrupts are enabled.
			UART0_CFIFO = UART_CFIFO_RXFLUSH;
			__enable_irq();
		} else {
			__enable_irq();
			head = rx_buffer_head;
			tail = rx_buffer_tail;
			do {
				if (use9Bits && (UART0_C3 & 0x80)) {
					n = UART0_D | 0x100;
				} else {
					n = UART0_D;
				}
				newhead = head + 1;
				if (newhead >= rx_buffer_total_size_) newhead = 0;
				if (newhead != tail) {
					head = newhead;
					if (newhead < SERIAL1_RX_BUFFER_SIZE) {
						rx_buffer[head] = n;
					} else {
						rx_buffer_storage_[head-SERIAL1_RX_BUFFER_SIZE] = n;
					}
				}
			} while (--avail > 0);
			rx_buffer_head = head;
			if (rts_pin) {
				int avail;
				if (head >= tail) avail = head - tail;
				else avail = rx_buffer_total_size_ + head - tail;
				if (avail >= rts_high_watermark_) rts_deassert();
			}
		}
	}
	c = UART0_C2;
	if ((c & UART_C2_TIE) && (UART0_S1 & UART_S1_TDRE)) {
		head = tx_buffer_head;
		tail = tx_buffer_tail;
		do {
			if (tail == head) break;
			if (++tail >= tx_buffer_total_size_) tail = 0;
			avail = UART0_S1;
			if (tail < SERIAL1_TX_BUFFER_SIZE) {
				n = tx_buffer[tail];
			} else {
				n = tx_buffer_storage_[tail-SERIAL1_TX_BUFFER_SIZE];
			}
			if (use9Bits) UART0_C3 = (UART0_C3 & ~0x40) | ((n & 0x100) >> 2);
			UART0_D = n;
		} while (UART0_TCFIFO < 8);
		tx_buffer_tail = tail;
		if (UART0_S1 & UART_S1_TDRE) UART0_C2 = C2_TX_COMPLETING;
	}
#else
	if (UART0_S1 & UART_S1_RDRF) {  // Dan. If a receive event happened (Receive Data Register Full)
		if (use9Bits && (UART0_C3 & 0x80)) {  // Dan. If the UART is configured for 9 bits, and the 9th bit is a 1 (0x80)
			n = UART0_D | 0x100;
		} else {
			n = UART0_D;  // Dan. store the value in the read data register
		}
		head = rx_buffer_head + 1;
		if (head >= rx_buffer_total_size_) head = 0;
		if (head != rx_buffer_tail) {
			if (head < SERIAL1_RX_BUFFER_SIZE) {
				rx_buffer[head] = n;  // Dan. Write the value in the Read Data Register to the head (end) of rx_buffer. Buffer is read from the tail and written from the head.
			} else {
				rx_buffer_storage_[head-SERIAL1_RX_BUFFER_SIZE] = n;
			}

			rx_buffer_head = head;
		}
	}
	c = UART0_C2;
	if ((c & UART_C2_TIE) && (UART0_S1 & UART_S1_TDRE)) {  // Dan. If a transmit event happened (done transitting a byte) (Transmit Data Register Empty)
		head = tx_buffer_head;
		tail = tx_buffer_tail;
		if (head == tail) {
			UART0_C2 = C2_TX_COMPLETING;  // Dan. When this happens, the loop in serial_putchar() is executed
		} else {
			if (++tail >= tx_buffer_total_size_) tail = 0;
			if (tail < SERIAL1_TX_BUFFER_SIZE) {
				n = tx_buffer[tail];  // Dan. Read the oldest value (tail) in the tx_buffer. Buffer is read from the tail and written from the head.
			} else {
				n = tx_buffer_storage_[tail-SERIAL1_TX_BUFFER_SIZE];
			}
			if (use9Bits) UART0_C3 = (UART0_C3 & ~0x40) | ((n & 0x100) >> 2);
			UART0_D = n;  // Dan. When transmission of a byte is complete, put the oldest value (tail) in the Transmit Data Register 
			tx_buffer_tail = tail;
		}
	}
#endif
	if ((c & UART_C2_TCIE) && (UART0_S1 & UART_S1_TC)) {  // Dan. If a transmit complete event happened
		transmitting = 0;
		if (transmit_pin) transmit_deassert();  // Dan. Lower the transmit pin, to signal end of Transmission Complete
		#if defined(KINETISL)
		if (half_duplex_mode) {
			__disable_irq();
			volatile uint32_t reg = UART0_C3;
			reg &= ~UART_C3_TXDIR;
			UART0_C3 = reg;
			__enable_irq();
		}
		#endif
		UART0_C2 = C2_TX_INACTIVE;
	}
}


// Dan. Also writes the characters in a string (char*) to the tx_buffer, until the character is '\0', making sure that every '\n' is preceded by a '\r'
void serial_print(const char *p)
{
	while (*p) {
		char c = *p++;
		if (c == '\n') serial_putchar('\r');
		serial_putchar(c);
	}
}

static void serial_phex1(uint32_t n)
{
	n &= 15;
	if (n < 10) {
		serial_putchar('0' + n);
	} else {
		serial_putchar('A' - 10 + n);
	}
}

void serial_phex(uint32_t n)
{
	serial_phex1(n >> 4);
	serial_phex1(n);
}

void serial_phex16(uint32_t n)
{
	serial_phex(n >> 8);
	serial_phex(n);
}

void serial_phex32(uint32_t n)
{
	serial_phex(n >> 24);
	serial_phex(n >> 16);
	serial_phex(n >> 8);
	serial_phex(n);
}

void serial_add_memory_for_read(void *buffer, size_t length)
{
	rx_buffer_storage_ = (BUFTYPE*)buffer;
	if (buffer) {
		rx_buffer_total_size_ = SERIAL1_RX_BUFFER_SIZE + length;
	} else {
		rx_buffer_total_size_ = SERIAL1_RX_BUFFER_SIZE;
	} 

	rts_low_watermark_ = RTS_LOW_WATERMARK + length;
	rts_high_watermark_ = RTS_HIGH_WATERMARK + length;
}

void serial_add_memory_for_write(void *buffer, size_t length)
{
	tx_buffer_storage_ = (BUFTYPE*)buffer;
	if (buffer) {
		tx_buffer_total_size_ = SERIAL1_TX_BUFFER_SIZE + length;
	} else {
		tx_buffer_total_size_ = SERIAL1_TX_BUFFER_SIZE;
	} 
}
