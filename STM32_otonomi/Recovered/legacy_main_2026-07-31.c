/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ESC_STOP       1500        // orta nokta = DUR
#define ESC_SAFE_CMD   200         // GÜVENLİK: kullanıcı komut sınırı ±200 (pervanesiz test)
                                   // komut 0=dur, +ileri, -geri. ESC us = 1500 + komut/2
// --- BNO055 IMU ---
#define BNO055_ADDR       (0x28 << 1)   // I2C 7-bit adres 0x28, HAL 8-bit ister
#define BNO055_OPR_MODE   0x3D
#define BNO055_PWR_MODE   0x3E
#define BNO055_SYS_TRIGGER 0x3F
#define BNO055_UNIT_SEL   0x3B
#define BNO055_EUL_H_LSB  0x1A
#define BNO055_CALIB_STAT 0x35
#define BNO055_MODE_NDOF  0x0C

#define RAMP_STEP_US   5u
#define RAMP_DELAY_MS  20u
#define STEP1_PORT GPIOA
#define STEP1_PIN  GPIO_PIN_10
#define DIR1_PORT  GPIOB
#define DIR1_PIN   GPIO_PIN_3
#define STEP2_PORT GPIOB
#define STEP2_PIN  GPIO_PIN_5
#define DIR2_PORT  GPIOB
#define DIR2_PIN   GPIO_PIN_4
#define LED_PORT   GPIOA
#define LED_PIN    GPIO_PIN_5
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile int32_t  step1_toggles = 0;
volatile int32_t  step2_toggles = 0;
volatile int32_t  step1_pos     = 0;
volatile int32_t  step2_pos     = 0;
volatile int8_t   step1_dir     = 1;
volatile int8_t   step2_dir     = 1;

uint16_t esc1_us = ESC_STOP;
uint16_t esc2_us = ESC_STOP;
uint8_t  armed   = 0;
uint32_t stepsPerRev = 800;

float imu_heading = 0, imu_roll = 0, imu_pitch = 0;
uint8_t imu_calib = 0;
uint8_t imu_ok = 0;

uint8_t  rx_byte;
char     cmd_buf[48];
uint8_t  cmd_idx = 0;
volatile uint8_t cmd_ready = 0;
char     cmd_line[48];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void uart_print(const char *s);
void process_cmd(char *cmd);
void set_esc(uint8_t n, int32_t us);
void ramp_esc(uint8_t n, int32_t target);
void arm_sequence(void);
void disarm(void);
void move_step(uint8_t n, int32_t steps);
void set_step_speed(uint8_t n, uint32_t sps);
void emergency_stop(void);
void print_help(void);
void print_status(void);
void move_both(int32_t steps1, int32_t steps2);
// --- BNO055 yardimci ---
uint8_t bno_read8(uint8_t reg) {
  uint8_t val = 0;
  HAL_I2C_Mem_Read(&hi2c1, BNO055_ADDR, reg, 1, &val, 1, 100);
  return val;
}
void bno_write8(uint8_t reg, uint8_t val) {
  HAL_I2C_Mem_Write(&hi2c1, BNO055_ADDR, reg, 1, &val, 1, 100);
}

void bno_init(void) {
  // Cihaz var mi? (CHIP_ID register 0x00 -> 0xA0 olmali)
  uint8_t chip = bno_read8(0x00);
  if (chip != 0xA0) { imu_ok = 0; return; }
  bno_write8(BNO055_OPR_MODE, 0x00);   // CONFIG mode
  HAL_Delay(25);
  bno_write8(BNO055_SYS_TRIGGER, 0x00);
  HAL_Delay(10);
  bno_write8(BNO055_PWR_MODE, 0x00);   // normal power
  HAL_Delay(10);
  bno_write8(BNO055_UNIT_SEL, 0x00);   // derece, m/s2
  HAL_Delay(10);
  bno_write8(BNO055_OPR_MODE, BNO055_MODE_NDOF); // 9-eksen fuzyon
  HAL_Delay(30);
  imu_ok = 1;
}

void bno_read(void) {
  if (!imu_ok) return;
  uint8_t buf[6];
  HAL_I2C_Mem_Read(&hi2c1, BNO055_ADDR, BNO055_EUL_H_LSB, 1, buf, 6, 100);
  int16_t h = (int16_t)((buf[1]<<8) | buf[0]);
  int16_t r = (int16_t)((buf[3]<<8) | buf[2]);
  int16_t p = (int16_t)((buf[5]<<8) | buf[4]);
  imu_heading = h / 16.0f;
  imu_roll    = r / 16.0f;
  imu_pitch   = p / 16.0f;
  imu_calib   = bno_read8(BNO055_CALIB_STAT);
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM5_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ESC_STOP);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ESC_STOP);

  __HAL_TIM_SET_AUTORELOAD(&htim2, 624);
  __HAL_TIM_SET_AUTORELOAD(&htim5, 624);

  HAL_UART_Receive_IT(&huart2, &rx_byte, 1);

  uart_print("========================================\r\n");
  uart_print(" SEA STARS Motor Kontrol (CubeIDE) - HAZIR\r\n");
  uart_print(" 'HELP' yaz -> komut listesi\r\n");
  uart_print("========================================\r\n");

  bno_init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

	  /* USER CODE BEGIN 3 */
	      if (cmd_ready) {
	        cmd_ready = 0;
	        process_cmd(cmd_line);
	      }
	      // Balast pozisyon yayini (motor hareket ederken, her ~100ms)
	      static uint32_t last_pos_tx = 0;
	      if (HAL_GetTick() - last_pos_tx >= 100) {
	        last_pos_tx = HAL_GetTick();
	        if (step1_toggles > 0 || step2_toggles > 0) {
	          char pmsg[48];
	          sprintf(pmsg, "POS1:%ld POS2:%ld\r\n", (long)step1_pos, (long)step2_pos);
	          uart_print(pmsg);
	        }
	      }
	      // IMU okuma ve yayin (her ~200ms)
	      static uint32_t last_imu = 0;
	      if (HAL_GetTick() - last_imu >= 200) {
	        last_imu = HAL_GetTick();
	        bno_read();
	        char imsg[64];
	        sprintf(imsg, "IMU H:%d R:%d P:%d C:%d\r\n",
	          (int)imu_heading, (int)imu_roll, (int)imu_pitch, imu_calib);
	        uart_print(imsg);
	      }
	    /* USER CODE END 3 */
  }
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL6;
  RCC_OscInitStruct.PLL.PLLDIV = RCC_PLL_DIV3;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void uart_print(const char *s) {
  HAL_UART_Transmit(&huart2, (uint8_t*)s, strlen(s), HAL_MAX_DELAY);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART2) {
    char c = (char)rx_byte;
    if (c == '\n' || c == '\r') {
      if (cmd_idx > 0) {
        cmd_buf[cmd_idx] = '\0';
        strcpy(cmd_line, cmd_buf);
        cmd_ready = 1;
        cmd_idx = 0;
      }
    } else if (cmd_idx < sizeof(cmd_buf) - 1) {
      cmd_buf[cmd_idx++] = c;
    }
    HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
  if (htim->Instance == TIM2) {
    if (step1_toggles > 0) {
      HAL_GPIO_TogglePin(STEP1_PORT, STEP1_PIN);
      step1_toggles--;
      if ((step1_toggles & 1) == 0) step1_pos += step1_dir;
      if (step1_toggles == 0) HAL_TIM_Base_Stop_IT(&htim2);
    }
  } else if (htim->Instance == TIM5) {
    if (step2_toggles > 0) {
      HAL_GPIO_TogglePin(STEP2_PORT, STEP2_PIN);
      step2_toggles--;
      if ((step2_toggles & 1) == 0) step2_pos += step2_dir;
      if (step2_toggles == 0) HAL_TIM_Base_Stop_IT(&htim5);
    }
  }
}

static void upper_trim(char *s) {
  int i = 0, j = 0;
  while (s[i] == ' ') i++;
  while (s[i]) { s[j++] = (s[i] >= 'a' && s[i] <= 'z') ? s[i]-32 : s[i]; i++; }
  while (j > 0 && (s[j-1]==' '||s[j-1]=='\r'||s[j-1]=='\n')) j--;
  s[j] = '\0';
}

void process_cmd(char *cmd) {
  upper_trim(cmd);
  char msg[64];

  if      (!strcmp(cmd,"STOP"))   { emergency_stop(); return; }
  else if (!strcmp(cmd,"HELP"))   { print_help();     return; }
  else if (!strcmp(cmd,"STATUS")) { print_status();   return; }
  else if (!strcmp(cmd,"POS")) {
    sprintf(msg,"POS step1=%ld step2=%ld (SPR=%lu)\r\n",(long)step1_pos,(long)step2_pos,(unsigned long)stepsPerRev);
    uart_print(msg); return;
  }
  else if (!strcmp(cmd,"IMU")) {
      char m[64]; bno_read();
      sprintf(m,"IMU heading=%d roll=%d pitch=%d calib=0x%02X\r\n",
        (int)imu_heading,(int)imu_roll,(int)imu_pitch,imu_calib);
      uart_print(m); return;
    }
  else if (!strcmp(cmd,"CALMAX")) {
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 2000);
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 2000);
      armed = 1;
      uart_print("CAL: MAX (2000us) gonderiliyor. Simdi GUC VER. Bip bekle, sonra CALMIN yaz.\r\n");
      return;
    }
    else if (!strcmp(cmd,"CALMIN")) {
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 1000);
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 1000);
      uart_print("CAL: MIN (1000us). Onay bip bekle. Kalibrasyon tamam olmali.\r\n");
      return;
    }
  else if (!strcmp(cmd,"ARM"))    { arm_sequence(); return; }
  else if (!strcmp(cmd,"DISARM")) { disarm();       return; }
  else if (!strcmp(cmd,"ZERO1"))  { step1_pos=0; uart_print("OK: step1 pozisyon=0\r\n"); return; }
  else if (!strcmp(cmd,"ZERO2"))  { step2_pos=0; uart_print("OK: step2 pozisyon=0\r\n"); return; }
  else if (!strcmp(cmd,"REV1"))   { move_step(1,(int32_t)stepsPerRev); uart_print("-> step1: 1 tam tur\r\n"); return; }
  else if (!strcmp(cmd,"REV2"))   { move_step(2,(int32_t)stepsPerRev); uart_print("-> step2: 1 tam tur\r\n"); return; }

  else if (!strncmp(cmd,"SETPOS1 ",8)) { step1_pos = atoi(cmd+8);
      sprintf(msg,"OK: step1_pos=%ld\r\n",(long)step1_pos); uart_print(msg); return; }
    else if (!strncmp(cmd,"SETPOS2 ",8)) { step2_pos = atoi(cmd+8);
      sprintf(msg,"OK: step2_pos=%ld\r\n",(long)step2_pos); uart_print(msg); return; }

  else if (!strncmp(cmd,"T1 ",3))   { set_esc(1, atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"T2 ",3))   { set_esc(2, atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"RAMP1 ",6)){ ramp_esc(1, atoi(cmd+6)); return; }
  else if (!strncmp(cmd,"RAMP2 ",6)){ ramp_esc(2, atoi(cmd+6)); return; }
  else if (!strncmp(cmd,"S1 ",3))   { move_step(1, atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"S2 ",3))   { move_step(2, atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"V1 ",3))   { set_step_speed(1, (uint32_t)atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"V2 ",3))   { set_step_speed(2, (uint32_t)atoi(cmd+3)); return; }
  else if (!strncmp(cmd,"SB ",3)) {
      char *p = cmd + 3;
      int32_t s1 = atoi(p);
      char *sp = strchr(p, ' ');
      int32_t s2 = sp ? atoi(sp+1) : s1;
      move_both(s1, s2);
      return;
    }
  else if (!strncmp(cmd,"SPR ",4))  { stepsPerRev=(uint32_t)atoi(cmd+4);
    sprintf(msg,"OK: stepsPerRev=%lu\r\n",(unsigned long)stepsPerRev); uart_print(msg); return; }

  uart_print("? Bilinmeyen komut. 'HELP' yaz.\r\n");
}

void set_esc(uint8_t n, int32_t cmd) {
  char msg[48];
  if (!armed) { uart_print("HATA: once ARM yap\r\n"); return; }
  // Güvenlik: komut ±ESC_SAFE_CMD ile sinirli (pervanesiz test)
  if (cmd >  (int32_t)ESC_SAFE_CMD) cmd =  ESC_SAFE_CMD;
  if (cmd < -(int32_t)ESC_SAFE_CMD) cmd = -ESC_SAFE_CMD;
  // Cift tarafli ESC: 0=dur(1500), +ileri, -geri
  int32_t us = ESC_STOP + cmd/2;
  if (n==1) { __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, us); esc1_us=us; }
  else      { __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, us); esc2_us=us; }
  sprintf(msg,"OK: ESC%u komut=%ld -> %ld us\r\n", n, (long)cmd, (long)us); uart_print(msg);
}

void ramp_esc(uint8_t n, int32_t target) {
  char msg[48];
  if (!armed) { uart_print("HATA: once ARM yap\r\n"); return; }
  // target: kullanici komutu (-1000..+1000). Guvenlik ±ESC_SAFE_CMD.
  if (target >  (int32_t)ESC_SAFE_CMD) target =  ESC_SAFE_CMD;
  if (target < -(int32_t)ESC_SAFE_CMD) target = -ESC_SAFE_CMD;
  int32_t target_us = ESC_STOP + target/2;
  int32_t cur = (n==1) ? esc1_us : esc2_us;
  int32_t dir = (target_us > cur) ? RAMP_STEP_US : -(int32_t)RAMP_STEP_US;
  while (cur != target_us) {
    cur += dir;
    if ((dir>0 && cur>target_us) || (dir<0 && cur<target_us)) cur = target_us;
    if (n==1) { __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, cur); esc1_us=cur; }
    else      { __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, cur); esc2_us=cur; }
    HAL_Delay(RAMP_DELAY_MS);
  }
  sprintf(msg,"OK: ESC%u rampa komut=%ld -> %ld us\r\n", n, (long)target, (long)target_us); uart_print(msg);
}

void arm_sequence(void) {
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ESC_STOP);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ESC_STOP);
  esc1_us = ESC_STOP; esc2_us = ESC_STOP;
  HAL_Delay(2000);
  armed = 1;
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
  uart_print("OK: ARMED (ESC dur=1500). T1/T2 komut: 0=dur +ileri -geri\r\n");
}

void disarm(void) {
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ESC_STOP);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ESC_STOP);
  esc1_us = ESC_STOP; esc2_us = ESC_STOP; armed = 0;
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
  uart_print("OK: DISARMED (ESC dur=1500)\r\n");
}

void move_step(uint8_t n, int32_t steps) {
  char msg[48];
  int8_t dir = (steps >= 0) ? 1 : -1;
  int32_t toggles = (steps >= 0 ? steps : -steps) * 2;
  if (n==1) {
    step1_dir = dir;
    HAL_GPIO_WritePin(DIR1_PORT, DIR1_PIN, (dir>0)?GPIO_PIN_SET:GPIO_PIN_RESET);
    step1_toggles = toggles;
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
    HAL_TIM_Base_Start_IT(&htim2);
  } else {
    step2_dir = dir;
    HAL_GPIO_WritePin(DIR2_PORT, DIR2_PIN, (dir>0)?GPIO_PIN_SET:GPIO_PIN_RESET);
    step2_toggles = toggles;
    __HAL_TIM_CLEAR_FLAG(&htim5, TIM_FLAG_UPDATE);
    HAL_TIM_Base_Start_IT(&htim5);
  }
  sprintf(msg,"OK: step%u -> %ld adim\r\n", n, (long)steps); uart_print(msg);
}
void move_both(int32_t steps1, int32_t steps2) {
  char msg[64];
  // Step 1 yön + toggle
  step1_dir = (steps1 >= 0) ? 1 : -1;
  HAL_GPIO_WritePin(DIR1_PORT, DIR1_PIN, (step1_dir>0)?GPIO_PIN_SET:GPIO_PIN_RESET);
  step1_toggles = (steps1 >= 0 ? steps1 : -steps1) * 2;
  // Step 2 yön + toggle
  step2_dir = (steps2 >= 0) ? 1 : -1;
  HAL_GPIO_WritePin(DIR2_PORT, DIR2_PIN, (step2_dir>0)?GPIO_PIN_SET:GPIO_PIN_RESET);
  step2_toggles = (steps2 >= 0 ? steps2 : -steps2) * 2;
  // İkisini AYNI ANDA başlat (flag temizle + start peş peşe)
  __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
  __HAL_TIM_CLEAR_FLAG(&htim5, TIM_FLAG_UPDATE);
  HAL_TIM_Base_Start_IT(&htim2);
  HAL_TIM_Base_Start_IT(&htim5);
  sprintf(msg,"OK: SB step1=%ld step2=%ld (ayni anda)\r\n", (long)steps1, (long)steps2);
  uart_print(msg);
}
void set_step_speed(uint8_t n, uint32_t sps) {
  char msg[48];
  if (sps < 1) sps = 1;
  uint32_t arr = (1000000UL / (sps * 2UL));
  if (arr < 2) arr = 2;
  if (n==1) __HAL_TIM_SET_AUTORELOAD(&htim2, arr-1);
  else      __HAL_TIM_SET_AUTORELOAD(&htim5, arr-1);
  sprintf(msg,"OK: step%u hiz=%lu adim/sn\r\n", n, (unsigned long)sps); uart_print(msg);
}

void emergency_stop(void) {
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ESC_STOP);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ESC_STOP);
  esc1_us=ESC_STOP; esc2_us=ESC_STOP; armed=0;
  step1_toggles=0; step2_toggles=0;
  HAL_TIM_Base_Stop_IT(&htim2);
  HAL_TIM_Base_Stop_IT(&htim5);
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
  uart_print("!!! ACIL STOP: ESC dur=1500, step dur, DISARMED\r\n");
}

void print_status(void) {
  char msg[96];
  sprintf(msg,"STATUS armed=%u ESC1=%u ESC2=%u STOP=%u step1=%ld step2=%ld\r\n",
          armed, esc1_us, esc2_us, ESC_STOP, (long)step1_pos, (long)step2_pos);
  uart_print(msg);
}

void print_help(void) {
  uart_print("--- KOMUTLAR ---\r\n");
  uart_print("GENEL: HELP | STATUS | POS | STOP(acil)\r\n");
  uart_print("ESC  : ARM | DISARM | T1/T2 <us> | RAMP1/RAMP2 <us>\r\n");
  uart_print("STEP : S1/S2 <adim> | V1/V2 <hiz> | ZERO1/ZERO2\r\n");
  uart_print("KALIB: SPR <adim> | REV1/REV2\r\n");
  uart_print("GUVENLIK: iticileri PERVANESIZ / suda test et.\r\n");
}
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
