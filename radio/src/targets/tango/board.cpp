/*
 * Copyright (C) OpenTX
 *
 * Based on code named
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"
#include "../../io/crsf/crsf_utilities.h"

#if defined(__cplusplus) && !defined(SIMU)
extern "C" {
#endif
#include "usb_dcd_int.h"
#include "usb_bsp.h"
#if defined(__cplusplus) && !defined(SIMU)
}
#endif

#if !defined(SIMU)
static uint8_t boardOffState = 0;
static uint8_t isDisableBoardOff();
static uint8_t checkDefaultWord();
#endif

HardwareOptions hardwareOptions;

static uint32_t trampoline[TRAMPOLINE_INDEX_COUNT] = {0};

void watchdogInit(unsigned int duration)
{
  IWDG->KR = 0x5555;      // Unlock registers
  IWDG->PR = 3;           // Divide by 32 => 1kHz clock
  IWDG->KR = 0x5555;      // Unlock registers
  IWDG->RLR = duration;   // 1.5 seconds nominal
  IWDG->KR = 0xAAAA;      // reload
  IWDG->KR = 0xCCCC;      // start
}

static void chargerInit(void)
{
  GPIO_InitTypeDef GPIO_InitStructure;
  GPIO_InitStructure.GPIO_Pin = CHARGER_STATE_GPIO_PIN | CHARGER_FAULT_GPIO_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(CHARGER_STATE_GPIO, &GPIO_InitStructure);
}

#define PWR_PRESSED_CNT     3
static void detectChargingMode(void)
{
  tmr10ms_t tm10ms = g_tmr10ms;
  tmr10ms_t tm100ms = g_tmr10ms;
  uint8_t  wt_cnt = PWR_PRESSED_CNT;
  uint8_t pwrPressedCnt = 0;

  while (wt_cnt > 0) {
    if ((g_tmr10ms- tm100ms) > 0) {
      usbPlugged(); //call several times continuously for debouncing and ensure stable connection
      wt_cnt--;
      tm100ms = g_tmr10ms;
      if(pwrPressed())
        pwrPressedCnt++;
    }
  }

  if(pwrPressedCnt == PWR_PRESSED_CNT)
    return;

  if (usbPlugged() && !pwrPressed()) {
    if (!rambackupRestore()) {
      g_eeGeneral.txVoltageCalibration = BATT_CALIB_OFFSET; //ram backup failed, use the default calibration value for battery voltage
    }
  }

  while (IS_CHARGING_STATE() && !IS_CHARGING_FAULT() && usbPlugged() && !pwrPressed()) {
    if(WAS_RESET_BY_SOFTWARE()) {
      // clear reset status if it's not pressed
      RCC_ClearFlag();
    }
    if ((g_tmr10ms - tm10ms) > 9) {
      getADC();
      tm10ms = g_tmr10ms;
    }
    if ((g_tmr10ms- tm100ms) > 49) {
      // lcdClear();
      checkBattery();
      // drawChargingState();
      // lcdRefresh();
      LED_CHARGING_IN_PROGRESS();
      tm100ms = g_tmr10ms;
    }
  }

  wt_cnt = PWR_PRESSED_CNT;
  while (wt_cnt > 0) {
    if ((g_tmr10ms - tm10ms) > 9) {
      getADC();
      tm10ms = g_tmr10ms;
    }
    if ((g_tmr10ms- tm100ms) > 49) {
      checkBattery();
      wt_cnt--;
    }
  }

  while (!IS_CHARGING_STATE() && usbPlugged() && !pwrPressed() && g_vbat100mV >= 40) {
    if ((g_tmr10ms - tm10ms) > 9) {
      getADC();
      tm10ms = g_tmr10ms;
    }
    if ((g_tmr10ms- tm100ms) > 49) {
      // lcdClear();
      checkBattery();
      // drawFullyCharged();
      // lcdRefresh();
      LED_CHARGING_DONE();
      tm100ms = g_tmr10ms;
    }
  }
}

void boardInit()
{
#if !defined(SIMU)
  RCC_AHB1PeriphClockCmd(PWR_RCC_AHB1Periph | 
                         KEYS_RCC_AHB1Periph | LCD_RCC_AHB1Periph |
                         AUDIO_RCC_AHB1Periph |
                         ADC_RCC_AHB1Periph | I2C_RCC_AHB1Periph |
                         SD_RCC_AHB1Periph | HAPTIC_RCC_AHB1Periph |
                         TELEMETRY_RCC_AHB1Periph | LED_RCC_AHB1Periph |
                         AUX_SERIAL_RCC_AHB1Periph, ENABLE);

  RCC_APB1PeriphClockCmd(LCD_RCC_APB1Periph | AUDIO_RCC_APB1Periph | ADC_RCC_APB1Periph |
                         HAPTIC_RCC_APB1Periph | INTERRUPT_xMS_RCC_APB1Periph |
                         TIMER_2MHz_RCC_APB1Periph | LED_RCC_APB1Periph |
                         SD_RCC_APB1Periph |
                         TELEMETRY_RCC_APB1Periph | AUX_SERIAL_RCC_APB1Periph, ENABLE);

  RCC_APB2PeriphClockCmd( ADC_RCC_APB2Periph |
                         HAPTIC_RCC_APB2Periph |
                         INTERRUPT_1MS_RCC_APB2Periph, ENABLE);

  pwrInit();
  keysInit();

  // we need to initialize g_FATFS_Obj here, because it is in .ram section (because of DMA access)
  // and this section is un-initialized
  memset(&g_FATFS_Obj, 0, sizeof(g_FATFS_Obj));

#if defined(ROTARY_ENCODER_NAVIGATION)
  rotaryEncoderInit();
#endif
  delaysInit();
  adcInit();
  lcdInit(); // delaysInit() must be called before
  audioInit();
  init2MhzTimer();
  init5msTimer();
  CRSF_Init();
  usbInit();
  #if defined(CHARGING_LEDS)
    ledInit();
  #endif
  chargerInit();
  __enable_irq();

#if defined(RTCLOCK) && !defined(COPROCESSOR)
  rtcInit(); // RTC must be initialized before rambackupRestore() is called
#endif

#if defined(DEBUG) && defined(AUX_SERIAL_GPIO)
  auxSerialInit(0, 0); // default serial mode (None if DEBUG not defined)
  TRACE("Tango board started :)");
  TRACE("RCC->CSR = %08x\n", RCC->CSR);
#endif

#if !defined(SIMU)
  if(!isDisableBoardOff() && !WAS_RESET_BY_WATCHDOG()){
    detectChargingMode();
  }
#endif

#if defined(ESP_SERIAL)
  espInit(ESP_UART_BAUDRATE, false);
#endif

#if defined(HAPTIC)
  hapticInit();
#endif

#if defined(DEBUG)
  DBGMCU_APB1PeriphConfig(DBGMCU_IWDG_STOP|DBGMCU_TIM1_STOP|DBGMCU_TIM2_STOP|DBGMCU_TIM3_STOP|DBGMCU_TIM6_STOP|DBGMCU_TIM8_STOP|DBGMCU_TIM10_STOP|DBGMCU_TIM13_STOP|DBGMCU_TIM14_STOP, ENABLE);
#endif

#if defined(PWR_BUTTON_PRESS)
  if (WAS_RESET_BY_WATCHDOG_OR_SOFTWARE()) {
    pwrOn();
  }
#endif
#endif // !defined(SIMU)

  if (!UNEXPECTED_SHUTDOWN())
    sdInit();
}

void boardOff()
{
  TRACE("power off\n");
  
#if defined(AUDIO_MUTE_GPIO_PIN)
  GPIO_SetBits(AUDIO_MUTE_GPIO, AUDIO_MUTE_GPIO_PIN); // mute
#endif

#if !defined(SIMU)
  crossfireOff();
#endif

#if defined(ESP_SERIAL)
  espOff();
#endif

#if defined(PCBTANGO) && defined(HAPTIC)
  if (haptic.busy())
  {
    haptic.stop();
  }
  hapticOff();
#endif

  BACKLIGHT_DISABLE();

#if defined(CHARGING_LEDS)
  ledOff();
#endif
  lcdOff();
  SysTick->CTRL = 0; // turn off systick

  // immediate software reset to do power off charging
  if (usbPlugged())
    NVIC_SystemReset();

  pwrOff();

  // disable interrupts
  __disable_irq();

  while (1) {
    wdt_reset();
#if defined(PWR_BUTTON_PRESS)
    // needs watchdog reset because CPU is still running while
    // the power key is held pressed by the user.
    // The power key should be released by now, but we must make sure
    if (!pwrPressed()) {
      // Put the CPU into sleep to reduce the consumption,
      // it might help with the RTC reset issue
      PWR->CR |= PWR_CR_CWUF;
      /* Select STANDBY mode */
      PWR->CR |= PWR_CR_PDDS;
      /* Set SLEEPDEEP bit of Cortex System Control Register */
      SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
      /* Request Wait For Event */
      __WFE();
    }
#endif
  }

  // this function must not return!
}

uint16_t getBatteryVoltage()
{
extern uint32_t libCrsf_MyHwID;
  int32_t instant_vbat = anaIn(TX_VOLTAGE); // using filtered ADC value on purpose
  float batt_scale;
  switch(libCrsf_MyHwID & ~HW_ID_MASK)
  {
    case 0x02:
      batt_scale = BATT_SCALE2;
      break;
    default:
      batt_scale = BATT_SCALE;
      break;
  }
  instant_vbat = instant_vbat / batt_scale + g_eeGeneral.txVoltageCalibration;
  instant_vbat = instant_vbat > BATTERY_MAX * 10 ? BATTERY_MAX * 10 : instant_vbat;
  return (uint16_t)instant_vbat;
}

#if !defined(SIMU)
void boardTurnOffRf()
{
  while(!get_crsf_flag(CRSF_FLAG_RF_OFF)) {     
    lcdRefreshWait();
    lcdClear();
    POPUP_CONFIRMATION(STR_JOYSTICK_RF, nullptr);
    SET_WARNING_INFO(STR_TURN_OFF_RF, sizeof(TR_TURN_OFF_RF), 0);
    event_t evt = getEvent(false);
    DISPLAY_WARNING(evt);
    lcdRefresh();

    if (warningResult) {
      warningResult = 0;
#if defined(ESP_SERIAL)
      espOff();
#endif
      set_crsf_flag(CRSF_FLAG_RF_OFF);
      break;
    }
    else if (!warningText) {
      break;
    }
  }
}

void boardSetSkipWarning()
{
  setStatusFlag( DEVICE_RESTART_WITHOUT_WARN_FLAG );
}

uint32_t readBackupReg(uint8_t index){
  return *(__IO uint32_t *) (BKPSRAM_BASE + index*4);
}

void writeBackupReg(uint8_t index, uint32_t data){
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
  PWR_BackupRegulatorCmd(ENABLE);
  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_BKPSRAM, ENABLE);
  PWR_BackupAccessCmd(ENABLE);
  while(PWR_GetFlagStatus(PWR_FLAG_BRR) == RESET);
  *(__IO uint32_t *) (BKPSRAM_BASE + index*4) = data;
}

void boot2bootloader(uint32_t isNeedFlash, uint32_t HwId, uint32_t sn){
  usbStop();
#if !defined(SIMU)
  crossfireOff();
  RTOS_SET_FLAG(get_task_flag(XF_TASK_FLAG));
#endif
  writeBackupReg(BOOTLOADER_IS_NEED_FLASH_ADDR, isNeedFlash);
  writeBackupReg(BOOTLOADER_HW_ID_ADDR, HwId);
  writeBackupReg(BOOTLOADER_SERIAL_NO_ADDR, sn);
  boardSetSkipWarning();
  NVIC_SystemReset();
}

uint8_t getBoardOffState(){
  return boardOffState;
}

static uint8_t isDisableBoardOff(){
  uint8_t value = (uint8_t)readBackupReg(BOOTLOADER_IS_SKIP_BOARD_OFF_ADDR);
  writeBackupReg(BOOTLOADER_IS_SKIP_BOARD_OFF_ADDR, 0);
  boardOffState = getStatusFlag(DEVICE_RESTART_WITHOUT_WARN_FLAG);
  clrStatusFlag(DEVICE_RESTART_WITHOUT_WARN_FLAG);
  boardOffState |= value;
  if(!checkDefaultWord()){
    boardOffState = 0;
  }
  return boardOffState;
}

static uint8_t checkDefaultWord(){
  union{
    uint8_t b[4];
    uint32_t word;
  } defaultWord;
  defaultWord.b[0] = (uint8_t)'t';
  defaultWord.b[1] = (uint8_t)'a';
  defaultWord.b[2] = (uint8_t)'n';
  defaultWord.b[3] = (uint8_t)'g';
  uint32_t value = (uint32_t)readBackupReg(BOOTLOADER_DEFAULT_WORD_ADDR);
  if(value != defaultWord.word){
    writeBackupReg(BOOTLOADER_DEFAULT_WORD_ADDR, defaultWord.word);
    return 0;
  }
  return 1;
}

uint8_t getStatusFlag(uint32_t flag){
  uint32_t value = (uint32_t)readBackupReg(BOOTLOADER_FLAG_ADDR);
  return ((value & (1 << flag)) ? 1 : 0);
}

void setStatusFlag(uint32_t flag){
  uint32_t value = (uint32_t)readBackupReg(BOOTLOADER_FLAG_ADDR);
  value |= (1 << flag);
  writeBackupReg(BOOTLOADER_FLAG_ADDR, value);
}

void clrStatusFlag(uint32_t flag){
  uint32_t value = (uint32_t)readBackupReg(BOOTLOADER_FLAG_ADDR);
  value &= ~(1 << flag);
  writeBackupReg(BOOTLOADER_FLAG_ADDR, value);
}

void crossfireOff( void ){
  set_crsf_flag( CRSF_FLAG_POWER_OFF );
}
#endif

void PrintData(char* header, uint8_t* data){
  TRACE_NOCRLF("\r\n%s: ", header);
  for(int i = 0; i < data[1] + 2; i++){
    TRACE_NOCRLF("%02X ", data[i]);
  }
  TRACE_NOCRLF("\r\n");
}

void trampolineInit( void )
{
  memset( trampoline, 0, sizeof(uint32_t) * TRAMPOLINE_INDEX_COUNT );
#if !defined(SIMU)
  trampoline[RTOS_WAIT_FLAG_TRAMPOILINE] = (uint32_t)(&CoWaitForSingleFlag);
  trampoline[RTOS_CLEAR_FLAG_TRAMPOILINE] = (uint32_t)(&CoClearFlag);
  crossfireSharedData.trampoline = trampoline;
#endif
}

void loadDefaultRadioSettings(void)
{
  // this is to reset incorrect radio settings. should be removed later.
  g_eeGeneral.lightAutoOff = g_eeGeneral.lightAutoOff < BACKLIGHT_TIMEOUT_MIN ? 6 : g_eeGeneral.lightAutoOff;
  g_eeGeneral.backlightMode = g_eeGeneral.backlightMode < e_backlight_mode_keys ? e_backlight_mode_keys : g_eeGeneral.backlightMode;
  g_eeGeneral.switchConfig = DEFAULT_SWITCH_CONFIG;
  g_eeGeneral.jitterFilter = 0;
}

#if !defined(SIMU) && defined(DEBUG)
#define UART_INT_MODE_TX     1
#define UART_INT_MODE_RX     2
extern Fifo<uint8_t, 512> auxSerialTxFifo;
extern "C" void AUX_SERIAL_USART_IRQHandler(void)
{
  DEBUG_INTERRUPT(INT_SER2);
  bool xf_active = false;
  bool (*uart_irq)( uint8_t, uint8_t );
  bool xf_valid = false;
  uint8_t data = 0;

  if ( crossfireSharedData.trampoline[DEBUG_UART_IRQ_TRAMPOLINE] ){
    uart_irq = (bool (*)( uint8_t, uint8_t ))crossfireSharedData.trampoline[DEBUG_UART_IRQ_TRAMPOLINE];
    xf_valid = true;
  }

  // Send
  if (USART_GetITStatus(AUX_SERIAL_USART, USART_IT_TXE) != RESET) {
    if( xf_valid )
      xf_active = uart_irq( UART_INT_MODE_TX, 0);
    if( !xf_active ){
      if ( !auxSerialTxFifo.isEmpty() ) {
        /* Write one byte to the transmit data register */
        auxSerialTxFifo.pop(data);
        USART_SendData(AUX_SERIAL_USART, data);
      }
      else {
        USART_ITConfig(AUX_SERIAL_USART, USART_IT_TXE, DISABLE);
      }
    }
  }

  // Receive
  uint32_t status = AUX_SERIAL_USART->SR;
  while (status & (USART_FLAG_RXNE | USART_FLAG_ERRORS)) {
    uint8_t data = AUX_SERIAL_USART->DR;
    if (!(status & USART_FLAG_ERRORS)) {
#if defined(CLI)
      switch (auxSerialMode) {
        case UART_MODE_DEBUG:
          cliRxFifo.push(data);
          break;
      }
#else
      if( xf_valid )
        uart_irq( UART_INT_MODE_RX, data);
#endif
    }
    status = AUX_SERIAL_USART->SR;
  }
}
#endif

#if !defined(SIMU)
extern "C" {
void INTERRUPT_EXTI_IRQHandler(void)
{
  DEBUG_INTERRUPT(INT_EXTI15_10);
  CoEnterISR();
  void (*exti_irq)(void);
  if ( crossfireSharedData.trampoline[DIO_IRQ_TRAMPOLINE] ){
    exti_irq = (void (*)(void))crossfireSharedData.trampoline[DIO_IRQ_TRAMPOLINE];
    /* call DIOCN handler of crossfire */
    exti_irq();
    isr_SetFlag( get_task_flag( XF_TASK_FLAG ));
  }
  CoExitISR();
}

void INTERRUPT_TIM13_IRQHandler()
{
  DEBUG_INTERRUPT(INT_TIM13);
  CoEnterISR();
  if( INTERRUPT_NOT_TIMER->SR & TIM_SR_UIF )
  {
    INTERRUPT_NOT_TIMER->SR &= ~TIM_SR_UIF;
    void (*timer_irq)(void);
    if( crossfireSharedData.trampoline[NOTIFICATION_TIMER_IRQ_TRAMPOLINE] ){
      timer_irq = (void (*)(void))crossfireSharedData.trampoline[NOTIFICATION_TIMER_IRQ_TRAMPOLINE];
      /* call notification timer handler of crossfire */
      timer_irq();
      isr_SetFlag( get_task_flag( XF_TASK_FLAG ));
    }
  }
  CoExitISR();
}

#if defined(DEBUG) && defined(AUX_SERIAL_GPIO)
#include <stdio.h>
#include <stdarg.h>
void uart_tx(uint8_t byte)
{
  while(USART_GetFlagStatus(AUX_SERIAL_USART, USART_FLAG_TXE) == RESET);
    USART_SendData(AUX_SERIAL_USART, byte);
}

void hf_printf(const char * TxBuf, ...)
{
  uint8_t UartBuf[200];
  va_list arglist;
  volatile uint8_t *fp;

  va_start(arglist, TxBuf);
  vsprintf((char*)UartBuf, (const char*)TxBuf, arglist);
  va_end(arglist);

  fp = UartBuf;
  while(*fp)
  {
    uart_tx(*fp);
    fp++;
  }
}

void _general_exception_handler (unsigned int * hardfault_args)
{
  bool xf_active = true;
  bool (*uart_irq)( uint8_t, uint8_t ) = (bool (*)( uint8_t, uint8_t ))crossfireSharedData.trampoline[DEBUG_UART_IRQ_TRAMPOLINE];
  unsigned int stacked_r0;
  unsigned int stacked_r1;
  unsigned int stacked_r2;
  unsigned int stacked_r3;
  unsigned int stacked_r12;
  unsigned int stacked_lr;
  unsigned int stacked_pc;
  unsigned int stacked_psr;

  stacked_r0 = ((unsigned long) hardfault_args[0]);
  stacked_r1 = ((unsigned long) hardfault_args[1]);
  stacked_r2 = ((unsigned long) hardfault_args[2]);
  stacked_r3 = ((unsigned long) hardfault_args[3]);

  stacked_r12 = ((unsigned long) hardfault_args[4]);
  stacked_lr  = ((unsigned long) hardfault_args[5]);
  stacked_pc  = ((unsigned long) hardfault_args[6]);
  stacked_psr = ((unsigned long) hardfault_args[7]);

  // to print all buffered messages before printing the stack
  while(1)
  {
    wdt_reset();
    if (USART_GetFlagStatus(AUX_SERIAL_USART, USART_FLAG_TXE) != RESET)
      xf_active = uart_irq( UART_INT_MODE_TX, 0);
    if(!xf_active)
      break;
  }

  hf_printf ("\r\n\n***FreedomTx Hard Fault Handler Debug Printing***\r\n");
  hf_printf ("R0\t\t= 0x%.8x\r\n", stacked_r0);
  hf_printf ("R1\t\t= 0x%.8x\r\n", stacked_r1);
  hf_printf ("R2\t\t= 0x%.8x\r\n", stacked_r2);
  hf_printf ("R3\t\t= 0x%.8x\r\n", stacked_r3);
  hf_printf ("R12\t\t= 0x%.8x\r\n", stacked_r12);
  hf_printf ("LR [R14]\t\t= 0x%.8x\r\n", stacked_lr);
  hf_printf ("PC [R15]\t\t= 0x%.8x\r\n", stacked_pc);
  hf_printf ("PSR\t\t= 0x%.8x\r\n", stacked_psr);
  hf_printf ("BFAR\t\t= 0x%.8x\r\n", (*((volatile unsigned long *)(0xE000ED38))));
  hf_printf ("CFSR\t\t= 0x%.8x\r\n", (*((volatile unsigned long *)(0xE000ED28))));
  hf_printf ("HFSR\t\t= 0x%.8x\r\n", (*((volatile unsigned long *)(0xE000ED2C))));
  hf_printf ("AFSR\t\t= 0x%.8x\r\n", (*((volatile unsigned long *)(0xE000ED3C))));
  hf_printf ("SCB_SHCSR\t= 0x%.8x\r\n", SCB->SHCSR);

  while (1);
}
#endif

void HardFault_Handler(void)
{
#if defined(DEBUG)
  __asm("TST LR, #4");
  __asm("ITE EQ");
  __asm("MRSEQ R0, MSP");
  __asm("MRSNE R0, PSP");
  __asm("B _general_exception_handler");
#else
  NVIC_SystemReset();
#endif
}
}  //extern "C" {
#endif
