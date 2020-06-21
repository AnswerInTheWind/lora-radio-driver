/*!
 * \file      sx126x-board.c
 *
 * \brief     Target board SX126x shield driver implementation
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 *
 * \author    Forest-Rain
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#include "sx126x-board.h"
#include "drv_gpio.h" // for GET_PIN

#define DRV_DEBUG
#define LOG_TAG             "lora.radio.sx1268" // LSD4RF-2R717N40
#include <drv_log.h>
#include <ulog.h> 

extern void RadioOnDioIrq( void* context );

/*!
 * Debug GPIO pins objects
 */
#if defined( USE_RADIO_DEBUG )
Gpio_t DbgPinTx;
Gpio_t DbgPinRx;
#endif

#ifdef LORA_RADIO_GPIO_SETUP_BY_PIN_NAME
int stm32_pin_get(char *pin_name)
{
    //eg: pin_name : "A4"  ( GPIOA, GPIO_PIN_4 )--> drv_gpio.c pin
    char pin_index = strtol(&pin_name[1],0,10);
    
    if(pin_name[0] < 'A' || pin_name[0] > 'Z')
    {
        return -1;
    }

    return (16 * (pin_name[0]-'A') + pin_index);
}
#endif

void SX126xIoInit( void )
{
////    GpioInit( &SX126x.Spi.Nss, RADIO_NSS, PIN_OUTPUT, PIN_PUSH_PULL, PIN_PULL_UP, 1 );
////    GpioInit( &SX126x.BUSY, RADIO_BUSY, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
////    GpioInit( &SX126x.DIO1, RADIO_DIO_1, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
////    GpioInit( &DeviceSel, RADIO_DEVICE_SEL, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );

    rt_pin_mode(LORA_RADIO_NSS_PIN, PIN_MODE_OUTPUT); 
    rt_pin_mode(LORA_RADIO_BUSY_PIN, PIN_MODE_INPUT);
    rt_pin_mode(LORA_RADIO_DIO1_PIN, PIN_MODE_INPUT_PULLDOWN);
#if defined( LORA_RADIO_DIO2_PIN ) 
    rt_pin_mode(LORA_RADIO_DIO2_PIN, PIN_MODE_INPUT_PULLDOWN);
#endif
#if defined( LORA_RADIO_RFSW1_PIN ) && defined ( LORA_RADIO_RFSW2_PIN )   
    rt_pin_mode(LORA_RADIO_RFSW1_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(LORA_RADIO_RFSW2_PIN, PIN_MODE_OUTPUT);
#endif

}

void SX126xIoIrqInit( DioIrqHandler dioIrq )
{
////    GpioSetInterrupt( &SX126x.DIO1, IRQ_RISING_EDGE, IRQ_HIGH_PRIORITY, dioIrq );
   
    rt_pin_mode(LORA_RADIO_DIO1_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_attach_irq(LORA_RADIO_DIO1_PIN, PIN_IRQ_MODE_RISING, RadioOnDioIrq,(void*)"callback args");
    rt_pin_irq_enable(LORA_RADIO_DIO1_PIN, PIN_IRQ_ENABLE);  
}

void SX126xIoDeInit( void )
{
////    GpioInit( &SX126x.Spi.Nss, RADIO_NSS, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_PULL_UP, 1 );
////    GpioInit( &SX126x.BUSY, RADIO_BUSY, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
////    GpioInit( &SX126x.DIO1, RADIO_DIO_1, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
}

void SX126xIoDbgInit( void )
{
#if defined( USE_RADIO_DEBUG )
    GpioInit( &DbgPinTx, RADIO_DBG_PIN_TX, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &DbgPinRx, RADIO_DBG_PIN_RX, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
#endif
}

void SX126xIoTcxoInit( void )
{
    // Initialize TCXO control
    CalibrationParams_t calibParam;
    
    // +clear OSC_START_ERR for reboot or cold-start from sleep
    SX126xClearDeviceErrors();
    
    // TCXO_CTRL_1_7V -> TCXO_CTRL_2_7V 64*15.0625US
    SX126xSetDio3AsTcxoCtrl( TCXO_CTRL_2_7V, 320);//SX126xGetBoardTcxoWakeupTime( ) << 6 ); // convert from ms to SX126x time base
    
    calibParam.Value = 0x7F;
    SX126xCalibrate( calibParam );
}

uint32_t SX126xGetBoardTcxoWakeupTime( void )
{
    return BOARD_TCXO_WAKEUP_TIME;
}

void SX126xReset( void )
{
////    DelayMs( 10 );
////    GpioInit( &SX126x.Reset, RADIO_RESET, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
////    DelayMs( 20 );
////    GpioInit( &SX126x.Reset, RADIO_RESET, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 ); // internal pull-up
////    DelayMs( 10 );
    
    DelayMs( 10 );
    rt_pin_mode(LORA_RADIO_RESET_PIN, PIN_MODE_OUTPUT); 
    rt_pin_write(LORA_RADIO_RESET_PIN, PIN_LOW);
    DelayMs( 20 );
     // internal pull-up
    rt_pin_mode(LORA_RADIO_RESET_PIN, PIN_MODE_INPUT); 
    DelayMs( 10 ); 
}

void SX126xWaitOnBusy( void )
{
////    while( GpioRead( &SX126x.BUSY ) == 1 );
    while( rt_pin_read( LORA_RADIO_BUSY_PIN ) == PIN_HIGH );
}

void SX126xAntSwOn( void )
{
   /// GpioInit( &AntPow, RADIO_ANT_SWITCH_POWER, PIN_OUTPUT, PIN_PUSH_PULL, PIN_PULL_UP, 1 );
}

void SX126xAntSwOff( void )
{
    ////GpioInit( &AntPow, RADIO_ANT_SWITCH_POWER, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    
#if defined( LORA_RADIO_RFSW1_PIN ) && defined ( LORA_RADIO_RFSW2_PIN )   
    rt_pin_write(LORA_RADIO_RFSW1_PIN, PIN_LOW);
    rt_pin_write(LORA_RADIO_RFSW2_PIN, PIN_LOW);
#endif
}

void SX126xSetAntSw( RadioOperatingModes_t mode )
{
    if( mode == MODE_TX )
    // Transmit
    {
        rt_pin_write(LORA_RADIO_RFSW1_PIN, PIN_HIGH);
        rt_pin_write(LORA_RADIO_RFSW2_PIN, PIN_LOW);
    }
    else 
    {
        rt_pin_write(LORA_RADIO_RFSW1_PIN, PIN_LOW);
        rt_pin_write(LORA_RADIO_RFSW2_PIN, PIN_HIGH);
    }

}

bool SX126xCheckRfFrequency( uint32_t frequency )
{
    // Implement check. Currently all frequencies are supported
    return true;
}

#if defined( USE_RADIO_DEBUG )
void SX126xDbgPinTxWrite( uint8_t state )
{
    GpioWrite( &DbgPinTx, state );
}

void SX126xDbgPinRxWrite( uint8_t state )
{
    GpioWrite( &DbgPinRx, state );
}
#endif
