# UART Command Shell with DMA

An interactive command shell over serial on the NUCLEO-F401RE. Commands typed into a
terminal control LED brightness and blink rate through hardware PWM.

Receive runs on circular DMA.
The USART's IDLE line interrupt signals the end of a burst.

Commands drive hardware PWM on TIM2_CH1 (LD2, PA5) for brightness, while TIM3 sets the 
blink interval in milliseconds. Both the PWM duty cylce and the blink interval are 
configurable at runtime from the shell and are controlled independently.

## Commands

| Command | Effect |
|---|---|
| `help` | List available commands |
| `pwm <0-100>` | LED brightness, as duty cycle percent |
| `blink <ms>` | LED on/off interval |
| `status` | Report current brightness and blink rate |
| `uptime` | Seconds since reset |

Invalid commands, out-of-range values, and missing arguments each return a
distinct error message.

## How receive works

1. Circular DMA writes each incoming byte into a ring buffer. `NDTR` counts down and
   reloads automatically at the end, so the stream does not stop.
2. When the line goes quiet for one frame period, the USART raises `IDLE` — a single
   interrupt meaning "that burst is over". 
3. `RX_BUFFER_SIZE - NDTR` gives the write position. Everything between the previous
   read position and there is a new command.

## Hardware

NUCLEO-F401RE (STM32F401RE, Cortex-M4 @ 84 MHz). LED on PA5, serial on PA2/PA3 via the
ST-Link virtual COM port. No external components.

## Connecting

Open a serial terminal on the ST-Link's COM port at **115200 8N1**. On Windows, find the
port under Device Manager → Ports. CubeIDE has a built-in terminal.

## Build

Open in STM32CubeIDE, build, and flash with the Run button. Peripheral configuration is
in the `.ioc` file — open with standalone STM32CubeMX to regenerate if needed.

## Configuration

| | |
|---|---|
| USART2 | 115200 8N1, 16× oversampling, no flow control |
| DMA1 Stream 5 | RX, channel 4, circular, peripheral-to-memory |
| DMA1 Stream 6 | TX, channel 4, normal, memory-to-peripheral |
| PA5 | LD2, alternate function AF1 → TIM2_CH1 |
| TIM2 | PSC 839, ARR 99 → 1 kHz PWM on CH1; compare value maps to percent |
| TIM3 | PSC 8399, ARR 4999 → 500 ms per phase; gates the PWM output on and off at the blink interval |

## References

- **RM0368** — STM32F401 reference manual
- **UM1724** — NUCLEO-64 board manual
- **UM1725** — STM32F4 HAL and LL driver reference
