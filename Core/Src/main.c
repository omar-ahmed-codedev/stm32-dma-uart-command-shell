/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : UART command shell using DMA, between STM32F401RE and St-Link virtual COM port.
  * 				  Button Input.
  * @author			: Omar Ahmed
  ******************************************************************************
  * @details
  * An interactive command shell giving users control over the on-board LED (LD2, PA5)
  * blink rate (ms) and brightness (PWM)) independently.
  * TIM2_CH1 drives the PWM for brightness, and TIM3 sets the blink interval in milliseconds.
  * In addition, it creates feedback responses and status messages.
  *
  * Communication is handled through DMA1 stream 6 for TX and stream 5 for RX,
  * which operates in circular mode, storing the received messages in a ring buffer.
  * The end of each message is detected by the USART IDLE interrupt.
  *
  * Commands: --
  *
  *
  * Target: NUCLEO-F401RE (STM32F401RE, ARM Cortex-M4, 84 MHz)
  *
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>		// Byte and string manipulation
#include <stdio.h>		// Declares vsnprintf, used in shell_printf
#include <stdarg.h>		// Standard arguments, machinery for variadic functions.
#include <stdlib.h>		// Standard library.
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RX_BUFFER_SIZE	64
#define TX_BUFFER_SIZE	64
#define MSG_LEN_MAX	64
#define BLINK_MS_MAX	6553		// TIM3 ARR is 16-bit --> 65535 // 1 ARR

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;

// LED settings


// UART receive
extern volatile uint8_t rx_ready;
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static char tx_buffer[TX_BUFFER_SIZE];

static volatile uint16_t rx_tail = 0;
static volatile uint16_t rx_head = 0;

static char rx_msg[MSG_LEN_MAX];
static uint16_t rx_msg_len = 0;

// LED state
static volatile uint8_t blink_on = 0;
static volatile uint16_t blink_ms = 0;	// Size needs to match resigters of TIM3
static volatile uint32_t duty_cycle = 0;




/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM3_Init(void);
static void UART_Recieve_Start(void);
static void shell_poll(void);
static void shell_execute(char *msg);
static void shell_printf(const char *msg, ...);		// ... is ellipsis. variadic function --> accepts any number of additional arguments
static int parse_int(char *str, uint32_t *val);
static void led_set_pwm(void);
static void led_set_blink(uint16_t ms);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  Intialize recieve on the UART through DMA1. Always running.
  */

static void UART_Recieve_Start(void){
	rx_tail = 0;
	rx_msg_len = 0;
	HAL_UART_Receive_DMA(&huart2, rx_buffer, RX_BUFFER_SIZE);
}

/**
  * @brief  Pol what is received in rx_buffer and hand it to shell_execute.
  */
static void shell_poll(void){

	// Buffer size - NDTR, to get where the recieve msg stopped.
	// NDTR is updated continously.
	rx_head = RX_BUFFER_SIZE - (uint16_t)__HAL_DMA_GET_COUNTER(huart2.hdmarx);

	while (rx_tail != rx_head){			// tail = head when a msg is complete

		char c = rx_buffer[rx_tail];
		rx_tail = (rx_tail+1) % RX_BUFFER_SIZE; 	// Increment tail. Wrap at buffer end.

		if (c == '\r' || c == '\n'){		// When a msg is done
			if (rx_msg_len>0){
				rx_msg[rx_msg_len]= '\0';
				shell_execute(rx_msg);
				rx_msg_len = 0;
			}
		}
		else if (rx_msg_len < (MSG_LEN_MAX - 1)){
			rx_msg[rx_msg_len] = c;
			rx_msg_len++;
		}
		else{
			rx_msg_len = 0;	// Clear the rest of the line in overflow msgs
		}
	}
}

/**
  * @brief  Chech content of received msg, act on it, and print a response.
  */
void static shell_execute(char *msg){

	uint32_t value = 0;		// stroul return unsigned long, which is 32 bits on ARM32 M4

	// Check msg
	// help
	if(strncmp(msg, "help",4)==0){
		shell_printf("\r\n"
					"pwm<0-100>		brightness %% duty cycle \r\n"
					"blink <ms>		blink half-period, 0 = steady on\r\n"
					"status			current settings\r\n"
					"help			this text\r\n");
	} // pmw <0-100>
	else if (strncmp(msg, "pmw", 3)==0){
		if(!parse_int(msg+3, &value) || value > 100){
			shell_printf("Error: invalid pwm value, expects <0-100>!");
		}
		else if(value<=100){
			duty_cycle = value;
			led_set_pwm();
			shell_printf("Brightness set to %u%%!\r\n", duty_cycle);
		}

	} // blink <0-100>
	else if (strncmp(msg, "blink",5)==0){
		if (!parse_int(msg+5, &value) || value>100){
			shell_printf("Error: invalid brightness, expects 0-%u\r\n!", BLINK_MS_MAX);
		}
		else if(value == 0){
			led_set_blink(0);
			shell_printf("Blinking off!\r\n");

		}
		else{
			led_set_blink((uint16_t) value);
			shell_printf("Blink half-period set to %u!\r\n", value);
		}
	} // status
	else if (strncmp(msg, "status", 6)){
		shell_printf("Brightness: %u%%\r\n", duty_cycle);
		shell_printf("Blink half-period: %u%%", blink_ms);

	}
	else{
		shell_printf("Error: unknown command!");
	}
}

/**
  * @brief  Parse a decimal value from a string and reject any trailing junk
  * @retval int, 1 on success, 0 on failure.
  */
static int parse_int(char *str, uint32_t *val){

	uint32_t v;		// uint32_t is same size as unsigned long on Cortex M4
	char *end;

	while (*str == ' '){
		str++;
	}
	if(*str<0 || *str>9){
		return 0;
	}

	v=strtoul(str, &end, 10);

	while (*end == ' '){
		end++;
	}

	if (*end!='\0'){
		return 0;

	}

	*val = v;
	return 1;
}


/**
  * @brief  Formats a message and hand it to the TX DMA.
  */

static void shell_printf(const char *str, ...){

    va_list args;   // A type for handling the arguments represented by ..., it iterates over the additional arguments
    int len;

    while (huart2.gState != HAL_UART_STATE_READY){
        // Wait until UART is ready for another operation after the latest HAL_UART_Transmit_DMA
        // Acceptable because it is only called in the main loop
    }

    // Intialize args --> args-> first agrument after str (the last named argument of the function)
    va_start(args, str);        // Gives vsnprint access to the arguments supplied through ...

    // Formulate the arguments into a string buffer with a max.
    len = vsnprintf(tx_buffer, TX_BUFFER_SIZE, str, args);        // Difference between snprintf is that it accepts vardiac (accepts additional arguments)
                                                               // Len recieves the bytes written or would have been written if the buffer is not long enough in the buffer. (minus 1(\0))
    va_end(args);      // Finished using va_start
    if (len > 0){
        if(len> (int) TX_BUFFER_SIZE){
            len = (int) TX_BUFFER_SIZE;
        }
        // Starts the transmission and returns before all the bytes have physically been transmitted.
        HAL_UART_Transmit_DMA(&huart2, (uint8_t*)tx_buffer, (uint16_t) len);
    }

}

/**
  * @brief  Updates the PWM duty cycle in the capture compare register
  */
static void led_set_pwm(void){
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim2);
    uint32_t crr = 0;

    if(blink_on){
        crr = ((arr+1)*(uint32_t)duty_cycle/100);
    }

    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, crr);

}


/**
  * @brief  Updates the blink half-period.
  */
static void led_set_blink(uint16_t ms){

    blink_ms = ms;  // Take a copy of current blink half-period
    if(ms==0){
        HAL_TIM_Base_Stop_IT(&htim3);    // Stop TIM3 interrupt
        blink_on = 1;           // Must be set to 1 in case TIM3 was stopped during the off phase, leaving crr 0 in led_set_pwm forever.
    }
    else{
        __HAL_TIM_SET_AUTORELOAD(&htim3, (uint32_t) ms*10 - 1);   // Update auto-reload. With 100KHz, ARR = ms*10 -10
        __HAL_TIM_SET_COUNTER(&htim3,0);    // Reset counter
        blink_on = 1;
        HAL_TIM_Base_Start_IT(&htim3);   // Enable interrupt and start counter
    }
    led_set_pwm();

}

// TIM3 toggles blink_on to control whether CCR get the latest value or a 0 through its interrupt. This approach was chosen over the also-valid alternative TIM3 directly controlling PA5(LED),
// because the pin would have to be reconfigured between alternate fucntion and normal output during runtime.


/**
  * @brief  TIM3 update:toggles blink_on to control whether CCR get the latest value or a 0 through its interrupt.
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim){

    if(htim->Instance == TIM3){
    	blink_on ^= 1;
    	led_set_pwm();
    }
}

/**
  * @brief  Recover from receive errors. Restart the UART
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart){

    if(huart->Instance == USART2){
        __HAL_UART_CLEAR_OREFLAG(huart);    // A new byte finished arriving in the shift register before the previous one was read out of DR
        __HAL_UART_CLEAR_NEFLAG(huart);     // If the three samples in the middle of sampling disagree, the bit was noisy
        __HAL_UART_CLEAR_FEFLAG(huart);     // Frame error: stop bit was where it should not be

        if (huart->RxState != HAL_UART_STATE_BUSY_RX)
        {
        UART_Recieve_Start();
        }
    }
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM2_Init();
  MX_USART2_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  led_set_pwm();

  UART_Recieve_Start();
  shell_printf("\r\nSTM32F401RE shell ready. Type 'help'.\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 84;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 839;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 99;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 3250;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 8399;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 4999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
  /* DMA1_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
