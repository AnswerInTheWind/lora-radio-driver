/*!
 * \file      sx127x.c
 *
 * \brief     SX127x driver implementation
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
 * \author    Wael Guibene ( Semtech )
 *
 * \author    Forest-Rain
 */
#include <rtthread.h>
#include "drv_spi.h"

#include <math.h>
#include <string.h>
#ifdef PKG_USING_MULTI_RTIMER
#include "hw_rtc_stm32l.h"
#include "multi_rtimer.h"
#endif
#include "lora-radio.h"
//#include "delay.h"

#include "board.h" 
#include "sx127x.h"
#include "sx127x-board.h"

#ifndef LORA_RADIO0_DEVICE_NAME
#define LORA_RADIO0_DEVICE_NAME  "lora-radio0"
#endif


#ifdef RT_USING_ULOG
#define DRV_DEBUG
#define LOG_TAG             "PHY.LoRa.SX127X"
#include <drv_log.h>
#include <ulog.h> 
#else
#define LOG_D(...)
#define LOG_I(...)
#endif

/*
 * Local types definition
 */

/*!
 * Radio registers definition
 */
typedef struct
{
    RadioModems_t Modem;
    uint8_t       Addr;
    uint8_t       Value;
}RadioRegisters_t;

/*!
 * FSK bandwidth definition
 */
typedef struct
{
    uint32_t bandwidth;
    uint8_t  RegValue;
}FskBandwidth_t;

/*
 * Private functions prototypes
 */

#if defined( USING_LORA_RADIO_SX1276 ) 
/*!
 * Performs the Rx chain calibration for LF and HF bands
 * \remark Must be called just after the reset so all registers are at their
 *         default values
 */
static void RxChainCalibration( void );
#endif

/*!
 * \brief Sets the SX127x in transmission mode for the given time
 * \param [IN] timeout Transmission timeout [ms] [0: continuous, others timeout]
 */
void SX127xSetTx( uint32_t timeout );

/*!
 * \brief Writes the buffer contents to the SX127x FIFO
 *
 * \param [IN] buffer Buffer containing data to be put on the FIFO.
 * \param [IN] size Number of bytes to be written to the FIFO
 */
void SX127xWriteFifo( uint8_t *buffer, uint8_t size );

/*!
 * \brief Reads the contents of the SX127x FIFO
 *
 * \param [OUT] buffer Buffer where to copy the FIFO read data.
 * \param [IN] size Number of bytes to be read from the FIFO
 */
void SX127xReadFifo( uint8_t *buffer, uint8_t size );

/*!
 * \brief Sets the SX127x operating mode
 *
 * \param [IN] opMode New operating mode
 */
void SX127xSetOpMode( uint8_t opMode );

/*
 * SX127x DIO IRQ callback functions prototype
 */

/*!
 * \brief DIO 0 IRQ callback
 */
void SX127xOnDio0Irq( void  );

/*!
 * \brief DIO 1 IRQ callback
 */
void SX127xOnDio1Irq( void  );

/*!
 * \brief DIO 2 IRQ callback
 */
void SX127xOnDio2Irq( void  );

/*!
 * \brief DIO 3 IRQ callback
 */
void SX127xOnDio3Irq( void  );

/*!
 * \brief DIO 4 IRQ callback
 */
void SX127xOnDio4Irq( void  );

/*!
 * \brief DIO 5 IRQ callback
 */
void SX127xOnDio5Irq( void  );

/*!
 * \brief Tx & Rx timeout timer callback
 */
void SX127xOnTimeoutIrq( void );

/*
 * Private global constants
 */

/*!
 * Radio hardware registers initialization
 *
 * \remark RADIO_INIT_REGISTERS_VALUE is defined in sx1276-board.h file
 */
const RadioRegisters_t RadioRegsInit[] = RADIO_INIT_REGISTERS_VALUE;

#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
/*!
 * Constant values need to compute the RSSI value
 */
#define RSSI_OFFSET_LF                              -164
#define RSSI_OFFSET_HF                              -157

#elif defined( USING_LORA_RADIO_SX1272 )

#define RSSI_OFFSET_HF                              -139

#else 
#error "LoRa Chip Undefined!"
#endif
/*!
 * Precomputed FSK bandwidth registers values
 */
const FskBandwidth_t FskBandwidths[] =
{
    { 2600  , 0x17 },
    { 3100  , 0x0F },
    { 3900  , 0x07 },
    { 5200  , 0x16 },
    { 6300  , 0x0E },
    { 7800  , 0x06 },
    { 10400 , 0x15 },
    { 12500 , 0x0D },
    { 15600 , 0x05 },
    { 20800 , 0x14 },
    { 25000 , 0x0C },
    { 31300 , 0x04 },
    { 41700 , 0x13 },
    { 50000 , 0x0B },
    { 62500 , 0x03 },
    { 83333 , 0x12 },
    { 100000, 0x0A },
    { 125000, 0x02 },
    { 166700, 0x11 },
    { 200000, 0x09 },
    { 250000, 0x01 },
    { 300000, 0x00 }, // Invalid Bandwidth
};

/*
 * Private global variables
 */

/*!
 * Radio callbacks variable
 */
static RadioEvents_t *RadioEvents;

/*!
 * Reception buffer
 */
static uint8_t RxTxBuffer[RX_BUFFER_SIZE];

/*
 * Public global variables
 */

/*!
 * Radio hardware and global parameters
 */
SX127x_t SX127x;

/*!
 * Hardware DIO IRQ callback initialization
 */
DioIrqHandler *SX127xDioIrq[] = { SX127xOnDio0Irq, SX127xOnDio1Irq,
                                  SX127xOnDio2Irq, SX127xOnDio3Irq,
                                  SX127xOnDio4Irq, NULL };

/*!
 * Tx and Rx timers
 */
TimerEvent_t TxTimeoutTimer;
TimerEvent_t RxTimeoutTimer;
TimerEvent_t RxTimeoutSyncWord;

extern struct rt_spi_device *lora_radio_spi_init(const char *bus_name, const char *lora_device_name, rt_uint8_t param);
                            
/*
 * Radio spi check
 * 0 - spi access fail                                
 */
uint8_t SX127xCheck(void)
{
    uint8_t test = 0;

    LOG_D("LoRa Chip is SX127X, Packet Type is %s\n",( SX127x.Settings.Modem == MODEM_LORA )? "LoRa":"FSK");

    if( SX127x.Settings.Modem == MODEM_LORA ) 
    {        
        /*SPI 验证,选一个寄存器来做验证*/
        SX127xWrite(REG_LR_PAYLOADLENGTH, 0x55); 
        test = SX127xRead(REG_LR_PAYLOADLENGTH);
        if (test != 0x55)
        {
            LOG_D("LoRa Chip is SX127X, Packet Type is %s\n");
            return 0;
        }		
    }
    return test;
}

/*
 * Radio driver functions implementation
 */

void SX127xInit( RadioEvents_t *events )
{
    uint8_t i;
    
    // Initialize spi bus
    SX127x.spi = lora_radio_spi_init(LORA_RADIO0_SPI_BUS_NAME, LORA_RADIO0_DEVICE_NAME, RT_NULL);
    
    RadioEvents = events;

    #ifdef PKG_USING_MULTI_RTIMER
    hw_rtc_init();
    #endif
    
    // Initialize driver timeout timers
    TimerInit( &TxTimeoutTimer, SX127xOnTimeoutIrq );
    TimerInit( &RxTimeoutTimer, SX127xOnTimeoutIrq );
    TimerInit( &RxTimeoutSyncWord, SX127xOnTimeoutIrq );

    SX127xReset( );
    
#if defined( USING_LORA_RADIO_SX1276 ) 
    RxChainCalibration( );
#endif    

    SX127xSetOpMode( RF_OPMODE_SLEEP );

    SX127xIoIrqInit( SX127xDioIrq );

    for( i = 0; i < sizeof( RadioRegsInit ) / sizeof( RadioRegisters_t ); i++ )
    {
        SX127xSetModem( RadioRegsInit[i].Modem );
        SX127xWrite( RadioRegsInit[i].Addr, RadioRegsInit[i].Value );
    }

    SX127xSetModem( MODEM_FSK );

    SX127x.Settings.State = RF_IDLE;
}

RadioState_t SX127xGetStatus( void )
{
    return SX127x.Settings.State;
}

void SX127xSetChannel( uint32_t freq )
{
    SX127x.Settings.Channel = freq;
    freq = ( uint32_t )( ( double )freq / ( double )FREQ_STEP );
    SX127xWrite( REG_FRFMSB, ( uint8_t )( ( freq >> 16 ) & 0xFF ) );
    SX127xWrite( REG_FRFMID, ( uint8_t )( ( freq >> 8 ) & 0xFF ) );
    SX127xWrite( REG_FRFLSB, ( uint8_t )( freq & 0xFF ) );
}

bool SX127xIsChannelFree( RadioModems_t modem, uint32_t freq, int16_t rssiThresh, uint32_t maxCarrierSenseTime )
{
    bool status = true;
    int16_t rssi = 0;
    uint32_t carrierSenseTime = 0;

    if( SX127xGetStatus( ) != RF_IDLE )
    {
        return false;
    }

    SX127xSetModem( modem );

    SX127xSetChannel( freq );

    SX127xSetOpMode( RF_OPMODE_RECEIVER );

    DelayMs( 1 );

    carrierSenseTime = TimerGetCurrentTime( );

    // Perform carrier sense for maxCarrierSenseTime
    while( TimerGetElapsedTime( carrierSenseTime ) < maxCarrierSenseTime )
    {
        rssi = SX127xReadRssi( modem );

        if( rssi > rssiThresh )
        {
            status = false;
            break;
        }
    }
    SX127xSetSleep( );
    return status;
}

uint32_t SX127xRandom( void )
{
    uint8_t i;
    uint32_t rnd = 0;

    /*
     * Radio setup for random number generation
     */
    // Set LoRa modem ON
    SX127xSetModem( MODEM_LORA );

    // Disable LoRa modem interrupts
    SX127xWrite( REG_LR_IRQFLAGSMASK, RFLR_IRQFLAGS_RXTIMEOUT |
                  RFLR_IRQFLAGS_RXDONE |
                  RFLR_IRQFLAGS_PAYLOADCRCERROR |
                  RFLR_IRQFLAGS_VALIDHEADER |
                  RFLR_IRQFLAGS_TXDONE |
                  RFLR_IRQFLAGS_CADDONE |
                  RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                  RFLR_IRQFLAGS_CADDETECTED );

    // Set radio in continuous reception
    SX127xSetOpMode( RF_OPMODE_RECEIVER );

    for( i = 0; i < 32; i++ )
    {
        DelayMs( 1 );
        // Unfiltered RSSI value reading. Only takes the LSB value
        rnd |= ( ( uint32_t )SX127xRead( REG_LR_RSSIWIDEBAND ) & 0x01 ) << i;
    }

    SX127xSetSleep( );

    return rnd;
}

#if defined( USING_LORA_RADIO_SX1276 )
/*!
 * Performs the Rx chain calibration for LF and HF bands
 * \remark Must be called just after the reset so all registers are at their
 *         default values
 */
static void RxChainCalibration( void )
{
    uint8_t regPaConfigInitVal;
    uint32_t initialFreq;

    // Save context
    regPaConfigInitVal = SX127xRead( REG_PACONFIG );
    initialFreq = ( double )( ( ( uint32_t )SX127xRead( REG_FRFMSB ) << 16 ) |
                              ( ( uint32_t )SX127xRead( REG_FRFMID ) << 8 ) |
                              ( ( uint32_t )SX127xRead( REG_FRFLSB ) ) ) * ( double )FREQ_STEP;

    // Cut the PA just in case, RFO output, power = -1 dBm
    SX127xWrite( REG_PACONFIG, 0x00 );

    // Launch Rx chain calibration for LF band
    SX127xWrite( REG_IMAGECAL, ( SX127xRead( REG_IMAGECAL ) & RF_IMAGECAL_IMAGECAL_MASK ) | RF_IMAGECAL_IMAGECAL_START );
    while( ( SX127xRead( REG_IMAGECAL ) & RF_IMAGECAL_IMAGECAL_RUNNING ) == RF_IMAGECAL_IMAGECAL_RUNNING )
    {
    }

    // Sets a Frequency in HF band
    SX127xSetChannel( 868000000 );

    // Launch Rx chain calibration for HF band
    SX127xWrite( REG_IMAGECAL, ( SX127xRead( REG_IMAGECAL ) & RF_IMAGECAL_IMAGECAL_MASK ) | RF_IMAGECAL_IMAGECAL_START );
    while( ( SX127xRead( REG_IMAGECAL ) & RF_IMAGECAL_IMAGECAL_RUNNING ) == RF_IMAGECAL_IMAGECAL_RUNNING )
    {
    }

    // Restore context
    SX127xWrite( REG_PACONFIG, regPaConfigInitVal );
    SX127xSetChannel( initialFreq );
}
#endif

/*!
 * Returns the known FSK bandwidth registers value
 *
 * \param [IN] bandwidth Bandwidth value in Hz
 * \retval regValue Bandwidth register value.
 */
static uint8_t GetFskBandwidthRegValue( uint32_t bandwidth )
{
    uint8_t i;

    for( i = 0; i < ( sizeof( FskBandwidths ) / sizeof( FskBandwidth_t ) ) - 1; i++ )
    {
        if( ( bandwidth >= FskBandwidths[i].bandwidth ) && ( bandwidth < FskBandwidths[i + 1].bandwidth ) )
        {
            return FskBandwidths[i].RegValue;
        }
    }
    // ERROR: Value not found
    while( 1 );
}

void SX127xSetRxConfig( RadioModems_t modem, uint32_t bandwidth,
                         uint32_t datarate, uint8_t coderate,
                         uint32_t bandwidthAfc, uint16_t preambleLen,
                         uint16_t symbTimeout, bool fixLen,
                         uint8_t payloadLen,
                         bool crcOn, bool freqHopOn, uint8_t hopPeriod,
                         bool iqInverted, bool rxContinuous )
{
    SX127xSetModem( modem );

    switch( modem )
    {
    case MODEM_FSK:
        {
            SX127x.Settings.Fsk.Bandwidth = bandwidth;
            SX127x.Settings.Fsk.Datarate = datarate;
            SX127x.Settings.Fsk.BandwidthAfc = bandwidthAfc;
            SX127x.Settings.Fsk.FixLen = fixLen;
            SX127x.Settings.Fsk.PayloadLen = payloadLen;
            SX127x.Settings.Fsk.CrcOn = crcOn;
            SX127x.Settings.Fsk.IqInverted = iqInverted;
            SX127x.Settings.Fsk.RxContinuous = rxContinuous;
            SX127x.Settings.Fsk.PreambleLen = preambleLen;
            SX127x.Settings.Fsk.RxSingleTimeout = ( uint32_t )( symbTimeout * ( ( 1.0 / ( double )datarate ) * 8.0 ) * 1000 );

            datarate = ( uint16_t )( ( double )XTAL_FREQ / ( double )datarate );
            SX127xWrite( REG_BITRATEMSB, ( uint8_t )( datarate >> 8 ) );
            SX127xWrite( REG_BITRATELSB, ( uint8_t )( datarate & 0xFF ) );

            SX127xWrite( REG_RXBW, GetFskBandwidthRegValue( bandwidth ) );
            SX127xWrite( REG_AFCBW, GetFskBandwidthRegValue( bandwidthAfc ) );

            SX127xWrite( REG_PREAMBLEMSB, ( uint8_t )( ( preambleLen >> 8 ) & 0xFF ) );
            SX127xWrite( REG_PREAMBLELSB, ( uint8_t )( preambleLen & 0xFF ) );

            if( fixLen == 1 )
            {
                SX127xWrite( REG_PAYLOADLENGTH, payloadLen );
            }
            else
            {
                SX127xWrite( REG_PAYLOADLENGTH, 0xFF ); // Set payload length to the maximum
            }

            SX127xWrite( REG_PACKETCONFIG1,
                         ( SX127xRead( REG_PACKETCONFIG1 ) &
                           RF_PACKETCONFIG1_CRC_MASK &
                           RF_PACKETCONFIG1_PACKETFORMAT_MASK ) |
                           ( ( fixLen == 1 ) ? RF_PACKETCONFIG1_PACKETFORMAT_FIXED : RF_PACKETCONFIG1_PACKETFORMAT_VARIABLE ) |
                           ( crcOn << 4 ) );
            SX127xWrite( REG_PACKETCONFIG2, ( SX127xRead( REG_PACKETCONFIG2 ) | RF_PACKETCONFIG2_DATAMODE_PACKET ) );
        }
        break;
    case MODEM_LORA:
        {

            if( bandwidth > 2 )
            {
                // Fatal error: When using LoRa modem only bandwidths 125, 250 and 500 kHz are supported
                while( 1 );
            }
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 			
            bandwidth += 7;
#endif
            SX127x.Settings.LoRa.Bandwidth = bandwidth;
            SX127x.Settings.LoRa.Datarate = datarate;
            SX127x.Settings.LoRa.Coderate = coderate;
            SX127x.Settings.LoRa.PreambleLen = preambleLen;
            SX127x.Settings.LoRa.FixLen = fixLen;
            SX127x.Settings.LoRa.PayloadLen = payloadLen;
            SX127x.Settings.LoRa.CrcOn = crcOn;
            SX127x.Settings.LoRa.FreqHopOn = freqHopOn;
            SX127x.Settings.LoRa.HopPeriod = hopPeriod;
            SX127x.Settings.LoRa.IqInverted = iqInverted;
            SX127x.Settings.LoRa.RxContinuous = rxContinuous;

            if( datarate > 12 )
            {
                datarate = 12;
            }
            else if( datarate < 6 )
            {
                datarate = 6;
            }
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
            if( ( ( bandwidth == 7 ) && ( ( datarate == 11 ) || ( datarate == 12 ) ) ) ||
                ( ( bandwidth == 8 ) && ( datarate == 12 ) ) )
            {
                SX127x.Settings.LoRa.LowDatarateOptimize = 0x01;
            }
            else
            {
                SX127x.Settings.LoRa.LowDatarateOptimize = 0x00;
            }

            SX127xWrite( REG_LR_MODEMCONFIG1,
                         ( SX127xRead( REG_LR_MODEMCONFIG1 ) &
                           RFLR_MODEMCONFIG1_BW_MASK &
                           RFLR_MODEMCONFIG1_CODINGRATE_MASK &
                           RFLR_MODEMCONFIG1_IMPLICITHEADER_MASK ) |
                           ( bandwidth << 4 ) | ( coderate << 1 ) |
                           fixLen );

            SX127xWrite( REG_LR_MODEMCONFIG2,
                         ( SX127xRead( REG_LR_MODEMCONFIG2 ) &
                           RFLR_MODEMCONFIG2_SF_MASK &
                           RFLR_MODEMCONFIG2_RXPAYLOADCRC_MASK &
                           RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK ) |
                           ( datarate << 4 ) | ( crcOn << 2 ) |
                           ( ( symbTimeout >> 8 ) & ~RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK ) );

            SX127xWrite( REG_LR_MODEMCONFIG3,
                         ( SX127xRead( REG_LR_MODEMCONFIG3 ) &
                           RFLR_MODEMCONFIG3_LOWDATARATEOPTIMIZE_MASK ) |
                           ( SX127x.Settings.LoRa.LowDatarateOptimize << 3 ) );

#elif defined( USING_LORA_RADIO_SX1272 )
      		if( ( ( bandwidth == 0 ) && ( ( datarate == 11 ) || ( datarate == 12 ) ) ) ||
                ( ( bandwidth == 1 ) && ( datarate == 12 ) ) )
            {
                SX1272.Settings.LoRa.LowDatarateOptimize = 0x01;
            }
            else
            {
                SX1272.Settings.LoRa.LowDatarateOptimize = 0x00;
            }

            SX1272Write( REG_LR_MODEMCONFIG1,
                         ( SX1272Read( REG_LR_MODEMCONFIG1 ) &
                           RFLR_MODEMCONFIG1_BW_MASK &
                           RFLR_MODEMCONFIG1_CODINGRATE_MASK &
                           RFLR_MODEMCONFIG1_IMPLICITHEADER_MASK &
                           RFLR_MODEMCONFIG1_RXPAYLOADCRC_MASK &
                           RFLR_MODEMCONFIG1_LOWDATARATEOPTIMIZE_MASK ) |
                           ( bandwidth << 6 ) | ( coderate << 3 ) |
                           ( fixLen << 2 ) | ( crcOn << 1 ) |
                           SX1272.Settings.LoRa.LowDatarateOptimize );

            SX1272Write( REG_LR_MODEMCONFIG2,
                         ( SX1272Read( REG_LR_MODEMCONFIG2 ) &
                           RFLR_MODEMCONFIG2_SF_MASK &
                           RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK ) |
                           ( datarate << 4 ) |
                           ( ( symbTimeout >> 8 ) & ~RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK ) );

#endif

            SX127xWrite( REG_LR_SYMBTIMEOUTLSB, ( uint8_t )( symbTimeout & 0xFF ) );

            SX127xWrite( REG_LR_PREAMBLEMSB, ( uint8_t )( ( preambleLen >> 8 ) & 0xFF ) );
            SX127xWrite( REG_LR_PREAMBLELSB, ( uint8_t )( preambleLen & 0xFF ) );

            if( fixLen == 1 )
            {
                SX127xWrite( REG_LR_PAYLOADLENGTH, payloadLen );
            }

            if( SX127x.Settings.LoRa.FreqHopOn == true )
            {
                SX127xWrite( REG_LR_PLLHOP, ( SX127xRead( REG_LR_PLLHOP ) & RFLR_PLLHOP_FASTHOP_MASK ) | RFLR_PLLHOP_FASTHOP_ON );
                SX127xWrite( REG_LR_HOPPERIOD, SX127x.Settings.LoRa.HopPeriod );
            }
			
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
            if( ( bandwidth == 9 ) && ( SX127x.Settings.Channel > RF_MID_BAND_THRESH ) )
            {
                // ERRATA 2.1 - Sensitivity Optimization with a 500 kHz Bandwidth
                SX127xWrite( REG_LR_HIGHBWOPTIMIZE1, 0x02 );
                SX127xWrite( REG_LR_HIGHBWOPTIMIZE2, 0x64 );
            }
            else if( bandwidth == 9 )
            {
                // ERRATA 2.1 - Sensitivity Optimization with a 500 kHz Bandwidth
                SX127xWrite( REG_LR_HIGHBWOPTIMIZE1, 0x02 );
                SX127xWrite( REG_LR_HIGHBWOPTIMIZE2, 0x7F );
            }
            else
            {
                // ERRATA 2.1 - Sensitivity Optimization with a 500 kHz Bandwidth
                SX127xWrite( REG_LR_HIGHBWOPTIMIZE1, 0x03 );
            }
#endif 

            if( datarate == 6 )
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE,
                             ( SX127xRead( REG_LR_DETECTOPTIMIZE ) &
                               RFLR_DETECTIONOPTIMIZE_MASK ) |
                               RFLR_DETECTIONOPTIMIZE_SF6 );
                SX127xWrite( REG_LR_DETECTIONTHRESHOLD,
                             RFLR_DETECTIONTHRESH_SF6 );
            }
            else
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE,
                             ( SX127xRead( REG_LR_DETECTOPTIMIZE ) &
                             RFLR_DETECTIONOPTIMIZE_MASK ) |
                             RFLR_DETECTIONOPTIMIZE_SF7_TO_SF12 );
                SX127xWrite( REG_LR_DETECTIONTHRESHOLD,
                             RFLR_DETECTIONTHRESH_SF7_TO_SF12 );
            }
        }
        break;
    }
}


void SX127xSetRfTxPower( int8_t power )
{
    uint8_t paConfig = 0;
    uint8_t paDac = 0;

    paConfig = SX127xRead( REG_PACONFIG );
    paDac = SX127xRead( REG_PADAC );

    paConfig = ( paConfig & RF_PACONFIG_PASELECT_MASK ) | SX127xGetPaSelect( power );

    if( ( paConfig & RF_PACONFIG_PASELECT_PABOOST ) == RF_PACONFIG_PASELECT_PABOOST )
    {
        if( power > 17 )
        {
            paDac = ( paDac & RF_PADAC_20DBM_MASK ) | RF_PADAC_20DBM_ON;
        }
        else
        {
            paDac = ( paDac & RF_PADAC_20DBM_MASK ) | RF_PADAC_20DBM_OFF;
        }
        if( ( paDac & RF_PADAC_20DBM_ON ) == RF_PADAC_20DBM_ON )
        {
            if( power < 5 )
            {
                power = 5;
            }
            if( power > 20 )
            {
                power = 20;
            }
            paConfig = ( paConfig & RF_PACONFIG_OUTPUTPOWER_MASK ) | ( uint8_t )( ( uint16_t )( power - 5 ) & 0x0F );
        }
        else
        {
            if( power < 2 )
            {
                power = 2;
            }
            if( power > 17 )
            {
                power = 17;
            }
            paConfig = ( paConfig & RF_PACONFIG_OUTPUTPOWER_MASK ) | ( uint8_t )( ( uint16_t )( power - 2 ) & 0x0F );
        }
    }
    else
    {
        if( power > 0 )
        {
            if( power > 15 )
            {
                power = 15;
            }
            paConfig = ( paConfig & RF_PACONFIG_MAX_POWER_MASK & RF_PACONFIG_OUTPUTPOWER_MASK ) | ( 7 << 4 ) | ( power );
        }
        else
        {
            if( power < -4 )
            {
                power = -4;
            }
            paConfig = ( paConfig & RF_PACONFIG_MAX_POWER_MASK & RF_PACONFIG_OUTPUTPOWER_MASK ) | ( 0 << 4 ) | ( power + 4 );
        }
    }
    SX127xWrite( REG_PACONFIG, paConfig );
    SX127xWrite( REG_PADAC, paDac );
}

void SX127xSetTxConfig( RadioModems_t modem, int8_t power, uint32_t fdev,
                        uint32_t bandwidth, uint32_t datarate,
                        uint8_t coderate, uint16_t preambleLen,
                        bool fixLen, bool crcOn, bool freqHopOn,
                        uint8_t hopPeriod, bool iqInverted, uint32_t timeout )
{
    SX127xSetModem( modem );

    SX127xSetRfTxPower( power );

    switch( modem )
    {
    case MODEM_FSK:
        {
            SX127x.Settings.Fsk.Power = power;
            SX127x.Settings.Fsk.Fdev = fdev;
            SX127x.Settings.Fsk.Bandwidth = bandwidth;
            SX127x.Settings.Fsk.Datarate = datarate;
            SX127x.Settings.Fsk.PreambleLen = preambleLen;
            SX127x.Settings.Fsk.FixLen = fixLen;
            SX127x.Settings.Fsk.CrcOn = crcOn;
            SX127x.Settings.Fsk.IqInverted = iqInverted;
            SX127x.Settings.Fsk.TxTimeout = timeout;

            fdev = ( uint16_t )( ( double )fdev / ( double )FREQ_STEP );
            SX127xWrite( REG_FDEVMSB, ( uint8_t )( fdev >> 8 ) );
            SX127xWrite( REG_FDEVLSB, ( uint8_t )( fdev & 0xFF ) );

            datarate = ( uint16_t )( ( double )XTAL_FREQ / ( double )datarate );
            SX127xWrite( REG_BITRATEMSB, ( uint8_t )( datarate >> 8 ) );
            SX127xWrite( REG_BITRATELSB, ( uint8_t )( datarate & 0xFF ) );

            SX127xWrite( REG_PREAMBLEMSB, ( preambleLen >> 8 ) & 0x00FF );
            SX127xWrite( REG_PREAMBLELSB, preambleLen & 0xFF );

            SX127xWrite( REG_PACKETCONFIG1,
                         ( SX127xRead( REG_PACKETCONFIG1 ) &
                           RF_PACKETCONFIG1_CRC_MASK &
                           RF_PACKETCONFIG1_PACKETFORMAT_MASK ) |
                           ( ( fixLen == 1 ) ? RF_PACKETCONFIG1_PACKETFORMAT_FIXED : RF_PACKETCONFIG1_PACKETFORMAT_VARIABLE ) |
                           ( crcOn << 4 ) );
            SX127xWrite( REG_PACKETCONFIG2, ( SX127xRead( REG_PACKETCONFIG2 ) | RF_PACKETCONFIG2_DATAMODE_PACKET ) );
        }
        break;
    case MODEM_LORA:
        {
            SX127x.Settings.LoRa.Power = power;
            if( bandwidth > 2 )
            {
                // Fatal error: When using LoRa modem only bandwidths 125, 250 and 500 kHz are supported
                while( 1 );
            }
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
            bandwidth += 7;
#endif
            SX127x.Settings.LoRa.Bandwidth = bandwidth;
            SX127x.Settings.LoRa.Datarate = datarate;
            SX127x.Settings.LoRa.Coderate = coderate;
            SX127x.Settings.LoRa.PreambleLen = preambleLen;
            SX127x.Settings.LoRa.FixLen = fixLen;
            SX127x.Settings.LoRa.FreqHopOn = freqHopOn;
            SX127x.Settings.LoRa.HopPeriod = hopPeriod;
            SX127x.Settings.LoRa.CrcOn = crcOn;
            SX127x.Settings.LoRa.IqInverted = iqInverted;
            SX127x.Settings.LoRa.TxTimeout = timeout;

            if( datarate > 12 )
            {
                datarate = 12;
            }
            else if( datarate < 6 )
            {
                datarate = 6;
            }
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
            if( ( ( bandwidth == 7 ) && ( ( datarate == 11 ) || ( datarate == 12 ) ) ) ||
                ( ( bandwidth == 8 ) && ( datarate == 12 ) ) )
            {
                SX127x.Settings.LoRa.LowDatarateOptimize = 0x01;
            }
            else
            {
                SX127x.Settings.LoRa.LowDatarateOptimize = 0x00;
            }

            if( SX127x.Settings.LoRa.FreqHopOn == true )
            {
                SX127xWrite( REG_LR_PLLHOP, ( SX127xRead( REG_LR_PLLHOP ) & RFLR_PLLHOP_FASTHOP_MASK ) | RFLR_PLLHOP_FASTHOP_ON );
                SX127xWrite( REG_LR_HOPPERIOD, SX127x.Settings.LoRa.HopPeriod );
            }

            SX127xWrite( REG_LR_MODEMCONFIG1,
                         ( SX127xRead( REG_LR_MODEMCONFIG1 ) &
                           RFLR_MODEMCONFIG1_BW_MASK &
                           RFLR_MODEMCONFIG1_CODINGRATE_MASK &
                           RFLR_MODEMCONFIG1_IMPLICITHEADER_MASK ) |
                           ( bandwidth << 4 ) | ( coderate << 1 ) |
                           fixLen );

            SX127xWrite( REG_LR_MODEMCONFIG2,
                         ( SX127xRead( REG_LR_MODEMCONFIG2 ) &
                           RFLR_MODEMCONFIG2_SF_MASK &
                           RFLR_MODEMCONFIG2_RXPAYLOADCRC_MASK ) |
                           ( datarate << 4 ) | ( crcOn << 2 ) );

            SX127xWrite( REG_LR_MODEMCONFIG3,
                         ( SX127xRead( REG_LR_MODEMCONFIG3 ) &
                           RFLR_MODEMCONFIG3_LOWDATARATEOPTIMIZE_MASK ) |
                           ( SX127x.Settings.LoRa.LowDatarateOptimize << 3 ) );
#elif defined ( LORA_CHIP_SX1272 )
            if( ( ( bandwidth == 0 ) && ( ( datarate == 11 ) || ( datarate == 12 ) ) ) ||
                ( ( bandwidth == 1 ) && ( datarate == 12 ) ) )
            {
                SX1272.Settings.LoRa.LowDatarateOptimize = 0x01;
            }
            else
            {
                SX1272.Settings.LoRa.LowDatarateOptimize = 0x00;
            }

            if( SX1272.Settings.LoRa.FreqHopOn == true )
            {
                SX1272Write( REG_LR_PLLHOP, ( SX1272Read( REG_LR_PLLHOP ) & RFLR_PLLHOP_FASTHOP_MASK ) | RFLR_PLLHOP_FASTHOP_ON );
                SX1272Write( REG_LR_HOPPERIOD, SX1272.Settings.LoRa.HopPeriod );
            }

            SX1272Write( REG_LR_MODEMCONFIG1,
                         ( SX1272Read( REG_LR_MODEMCONFIG1 ) &
                           RFLR_MODEMCONFIG1_BW_MASK &
                           RFLR_MODEMCONFIG1_CODINGRATE_MASK &
                           RFLR_MODEMCONFIG1_IMPLICITHEADER_MASK &
                           RFLR_MODEMCONFIG1_RXPAYLOADCRC_MASK &
                           RFLR_MODEMCONFIG1_LOWDATARATEOPTIMIZE_MASK ) |
                           ( bandwidth << 6 ) | ( coderate << 3 ) |
                           ( fixLen << 2 ) | ( crcOn << 1 ) |
                           SX1272.Settings.LoRa.LowDatarateOptimize );

            SX1272Write( REG_LR_MODEMCONFIG2,
                        ( SX1272Read( REG_LR_MODEMCONFIG2 ) &
                          RFLR_MODEMCONFIG2_SF_MASK ) |
                          ( datarate << 4 ) );
#endif
            SX127xWrite( REG_LR_PREAMBLEMSB, ( preambleLen >> 8 ) & 0x00FF );
            SX127xWrite( REG_LR_PREAMBLELSB, preambleLen & 0xFF );

            if( datarate == 6 )
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE,
                             ( SX127xRead( REG_LR_DETECTOPTIMIZE ) &
                               RFLR_DETECTIONOPTIMIZE_MASK ) |
                               RFLR_DETECTIONOPTIMIZE_SF6 );
                SX127xWrite( REG_LR_DETECTIONTHRESHOLD,
                             RFLR_DETECTIONTHRESH_SF6 );
            }
            else
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE,
                             ( SX127xRead( REG_LR_DETECTOPTIMIZE ) &
                             RFLR_DETECTIONOPTIMIZE_MASK ) |
                             RFLR_DETECTIONOPTIMIZE_SF7_TO_SF12 );
                SX127xWrite( REG_LR_DETECTIONTHRESHOLD,
                             RFLR_DETECTIONTHRESH_SF7_TO_SF12 );
            }
        }
        break;
    }
}

uint32_t SX127xGetTimeOnAir( RadioModems_t modem, uint8_t pktLen )
{
    uint32_t airTime = 0;

    switch( modem )
    {
    case MODEM_FSK:
        {
            airTime = round( ( 8 * ( SX127x.Settings.Fsk.PreambleLen +
                                     ( ( SX127xRead( REG_SYNCCONFIG ) & ~RF_SYNCCONFIG_SYNCSIZE_MASK ) + 1 ) +
                                     ( ( SX127x.Settings.Fsk.FixLen == 0x01 ) ? 0.0 : 1.0 ) +
                                     ( ( ( SX127xRead( REG_PACKETCONFIG1 ) & ~RF_PACKETCONFIG1_ADDRSFILTERING_MASK ) != 0x00 ) ? 1.0 : 0 ) +
                                     pktLen +
                                     ( ( SX127x.Settings.Fsk.CrcOn == 0x01 ) ? 2.0 : 0 ) ) /
                                     SX127x.Settings.Fsk.Datarate ) * 1000 );
        }
        break;
    case MODEM_LORA:
        {
            double bw = 0.0;
            // REMARK: When using LoRa modem only bandwidths 125, 250 and 500 kHz are supported
            switch( SX127x.Settings.LoRa.Bandwidth )
            {
                //case 0: // 7.8 kHz
                //    bw = 7800;
                //    break;
                //case 1: // 10.4 kHz
                //    bw = 10400;
                //    break;
                //case 2: // 15.6 kHz
                //    bw = 15600;
                //    break;
                //case 3: // 20.8 kHz
                //    bw = 20800;
                //    break;
                //case 4: // 31.2 kHz
                //    bw = 31200;
                //    break;
                //case 5: // 41.4 kHz
                //    bw = 41400;
                //    break;
                //case 6: // 62.5 kHz
                //    bw = 62500;
                //    break;
                case 7: // 125 kHz
                    bw = 125000;
                    break;
                case 8: // 250 kHz
                    bw = 250000;
                    break;
                case 9: // 500 kHz
                    bw = 500000;
                    break;
            }

            // Symbol rate : time for one symbol (secs)
            double rs = bw / ( 1 << SX127x.Settings.LoRa.Datarate );
            double ts = 1 / rs;
            // time of preamble
            double tPreamble = ( SX127x.Settings.LoRa.PreambleLen + 4.25 ) * ts;
            // Symbol length of payload and time
            double tmp = ceil( ( 8 * pktLen - 4 * SX127x.Settings.LoRa.Datarate +
                                 28 + 16 * SX127x.Settings.LoRa.CrcOn -
                                 ( SX127x.Settings.LoRa.FixLen ? 20 : 0 ) ) /
                                 ( double )( 4 * ( SX127x.Settings.LoRa.Datarate -
                                 ( ( SX127x.Settings.LoRa.LowDatarateOptimize > 0 ) ? 2 : 0 ) ) ) ) *
                                 ( SX127x.Settings.LoRa.Coderate + 4 );
            double nPayload = 8 + ( ( tmp > 0 ) ? tmp : 0 );
            double tPayload = nPayload * ts;
            // Time on air
            double tOnAir = tPreamble + tPayload;
            // return ms secs
            airTime = floor( tOnAir * 1000 + 0.999 );
        }
        break;
    }
    return airTime;
}

void SX127xSend( uint8_t *buffer, uint8_t size )
{
    uint32_t txTimeout = 0;

    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        {
            SX127x.Settings.FskPacketHandler.NbBytes = 0;
            SX127x.Settings.FskPacketHandler.Size = size;

            if( SX127x.Settings.Fsk.FixLen == false )
            {
                SX127xWriteFifo( ( uint8_t* )&size, 1 );
            }
            else
            {
                SX127xWrite( REG_PAYLOADLENGTH, size );
            }

            if( ( size > 0 ) && ( size <= 64 ) )
            {
                SX127x.Settings.FskPacketHandler.ChunkSize = size;
            }
            else
            {
                //memcpy1( RxTxBuffer, buffer, size );
                rt_memcpy( RxTxBuffer, buffer, size );
                SX127x.Settings.FskPacketHandler.ChunkSize = 32;
            }

            // Write payload buffer
            SX127xWriteFifo( buffer, SX127x.Settings.FskPacketHandler.ChunkSize );
            SX127x.Settings.FskPacketHandler.NbBytes += SX127x.Settings.FskPacketHandler.ChunkSize;
            txTimeout = SX127x.Settings.Fsk.TxTimeout;
        }
        break;
    case MODEM_LORA:
        {
            if( SX127x.Settings.LoRa.IqInverted == true )
            {
                SX127xWrite( REG_LR_INVERTIQ, ( ( SX127xRead( REG_LR_INVERTIQ ) & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK ) | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_ON ) );
                SX127xWrite( REG_LR_INVERTIQ2, RFLR_INVERTIQ2_ON );
            }
            else
            {
                SX127xWrite( REG_LR_INVERTIQ, ( ( SX127xRead( REG_LR_INVERTIQ ) & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK ) | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_OFF ) );
                SX127xWrite( REG_LR_INVERTIQ2, RFLR_INVERTIQ2_OFF );
            }

            SX127x.Settings.LoRaPacketHandler.Size = size;

            // Initializes the payload size
            SX127xWrite( REG_LR_PAYLOADLENGTH, size );

            // Full buffer used for Tx
            SX127xWrite( REG_LR_FIFOTXBASEADDR, 0 );
            SX127xWrite( REG_LR_FIFOADDRPTR, 0 );

            // FIFO operations can not take place in Sleep mode
            if( ( SX127xRead( REG_OPMODE ) & ~RF_OPMODE_MASK ) == RF_OPMODE_SLEEP )
            {
                SX127xSetStby( );
                DelayMs( 1 );
            }
            // Write payload buffer
            SX127xWriteFifo( buffer, size );
            txTimeout = SX127x.Settings.LoRa.TxTimeout;
        }
        break;
    }

    SX127xSetTx( txTimeout );
}

void SX127xSetSleep( void )
{
    TimerStop( &RxTimeoutTimer );
    TimerStop( &TxTimeoutTimer );
    TimerStop( &RxTimeoutSyncWord );

    SX127xSetOpMode( RF_OPMODE_SLEEP );

#ifdef USE_LORA_RADIO_TCXO
    // Disable TCXO radio is in SLEEP mode
    SX127xSetBoardTcxo( false );
#endif

    SX127x.Settings.State = RF_IDLE;
}

void SX127xSetStby( void )
{
    TimerStop( &RxTimeoutTimer );
    TimerStop( &TxTimeoutTimer );
    TimerStop( &RxTimeoutSyncWord );

    SX127xSetOpMode( RF_OPMODE_STANDBY );
    SX127x.Settings.State = RF_IDLE;
}

void SX127xSetRx( uint32_t timeout )
{
    bool rxContinuous = false;
    TimerStop( &TxTimeoutTimer );

    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        {
            rxContinuous = SX127x.Settings.Fsk.RxContinuous;

            // DIO0=PayloadReady
            // DIO1=FifoLevel
            // DIO2=SyncAddr
            // DIO3=FifoEmpty
            // DIO4=Preamble
            // DIO5=ModeReady
            SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RF_DIOMAPPING1_DIO0_MASK &
                                                                            RF_DIOMAPPING1_DIO1_MASK &
                                                                            RF_DIOMAPPING1_DIO2_MASK ) |
                                                                            RF_DIOMAPPING1_DIO0_00 |
                                                                            RF_DIOMAPPING1_DIO1_00 |
                                                                            RF_DIOMAPPING1_DIO2_11 );

            SX127xWrite( REG_DIOMAPPING2, ( SX127xRead( REG_DIOMAPPING2 ) & RF_DIOMAPPING2_DIO4_MASK &
                                                                            RF_DIOMAPPING2_MAP_MASK ) |
                                                                            RF_DIOMAPPING2_DIO4_11 |
                                                                            RF_DIOMAPPING2_MAP_PREAMBLEDETECT );

            SX127x.Settings.FskPacketHandler.FifoThresh = SX127xRead( REG_FIFOTHRESH ) & 0x3F;

            SX127xWrite( REG_RXCONFIG, RF_RXCONFIG_AFCAUTO_ON | RF_RXCONFIG_AGCAUTO_ON | RF_RXCONFIG_RXTRIGER_PREAMBLEDETECT );

            SX127x.Settings.FskPacketHandler.PreambleDetected = false;
            SX127x.Settings.FskPacketHandler.SyncWordDetected = false;
            SX127x.Settings.FskPacketHandler.NbBytes = 0;
            SX127x.Settings.FskPacketHandler.Size = 0;
        }
        break;
    case MODEM_LORA:
        {
            if( SX127x.Settings.LoRa.IqInverted == true )
            {
                SX127xWrite( REG_LR_INVERTIQ, ( ( SX127xRead( REG_LR_INVERTIQ ) & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK ) | RFLR_INVERTIQ_RX_ON | RFLR_INVERTIQ_TX_OFF ) );
                SX127xWrite( REG_LR_INVERTIQ2, RFLR_INVERTIQ2_ON );
            }
            else
            {
                SX127xWrite( REG_LR_INVERTIQ, ( ( SX127xRead( REG_LR_INVERTIQ ) & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK ) | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_OFF ) );
                SX127xWrite( REG_LR_INVERTIQ2, RFLR_INVERTIQ2_OFF );
            }
			
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
            // ERRATA 2.3 - Receiver Spurious Reception of a LoRa Signal
            if( SX127x.Settings.LoRa.Bandwidth < 9 )
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE, SX127xRead( REG_LR_DETECTOPTIMIZE ) & 0x7F );
                SX127xWrite( REG_LR_IFFREQ2, 0x00 );
                switch( SX127x.Settings.LoRa.Bandwidth )
                {
                case 0: // 7.8 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x48 );
                    SX127xSetChannel(SX127x.Settings.Channel + 7810 );
                    break;
                case 1: // 10.4 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x44 );
                    SX127xSetChannel(SX127x.Settings.Channel + 10420 );
                    break;
                case 2: // 15.6 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x44 );
                    SX127xSetChannel(SX127x.Settings.Channel + 15620 );
                    break;
                case 3: // 20.8 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x44 );
                    SX127xSetChannel(SX127x.Settings.Channel + 20830 );
                    break;
                case 4: // 31.2 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x44 );
                    SX127xSetChannel(SX127x.Settings.Channel + 31250 );
                    break;
                case 5: // 41.4 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x44 );
                    SX127xSetChannel(SX127x.Settings.Channel + 41670 );
                    break;
                case 6: // 62.5 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x40 );
                    break;
                case 7: // 125 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x40 );
                    break;
                case 8: // 250 kHz
                    SX127xWrite( REG_LR_IFFREQ1, 0x40 );
                    break;
                }
            }
            else
            {
                SX127xWrite( REG_LR_DETECTOPTIMIZE, SX127xRead( REG_LR_DETECTOPTIMIZE ) | 0x80 );
            }
#endif

            rxContinuous = SX127x.Settings.LoRa.RxContinuous;

            if( SX127x.Settings.LoRa.FreqHopOn == true )
            {
                SX127xWrite( REG_LR_IRQFLAGSMASK, //RFLR_IRQFLAGS_RXTIMEOUT |
                                                  //RFLR_IRQFLAGS_RXDONE |
                                                  //RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                                  RFLR_IRQFLAGS_VALIDHEADER |
                                                  RFLR_IRQFLAGS_TXDONE |
                                                  RFLR_IRQFLAGS_CADDONE |
                                                  //RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                                  RFLR_IRQFLAGS_CADDETECTED );

                // DIO0=RxDone, DIO2=FhssChangeChannel
                SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RFLR_DIOMAPPING1_DIO0_MASK & RFLR_DIOMAPPING1_DIO2_MASK  ) | RFLR_DIOMAPPING1_DIO0_00 | RFLR_DIOMAPPING1_DIO2_00 );
            }
            else
            {
                SX127xWrite( REG_LR_IRQFLAGSMASK, //RFLR_IRQFLAGS_RXTIMEOUT |
                                                  //RFLR_IRQFLAGS_RXDONE |
                                                  //RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                                  RFLR_IRQFLAGS_VALIDHEADER |
                                                  RFLR_IRQFLAGS_TXDONE |
                                                  RFLR_IRQFLAGS_CADDONE |
                                                  RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                                  RFLR_IRQFLAGS_CADDETECTED );

                // DIO0=RxDone
                SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RFLR_DIOMAPPING1_DIO0_MASK ) | RFLR_DIOMAPPING1_DIO0_00 );
            }
            SX127xWrite( REG_LR_FIFORXBASEADDR, 0 );
            SX127xWrite( REG_LR_FIFOADDRPTR, 0 );
        }
        break;
    }

    memset( RxTxBuffer, 0, ( size_t )RX_BUFFER_SIZE );

    SX127x.Settings.State = RF_RX_RUNNING;
    if( timeout != 0 )
    {
        TimerSetValue( &RxTimeoutTimer, timeout );
        TimerStart( &RxTimeoutTimer );
    }

    if( SX127x.Settings.Modem == MODEM_FSK )
    {
        SX127xSetOpMode( RF_OPMODE_RECEIVER );

        TimerSetValue( &RxTimeoutSyncWord, SX127x.Settings.Fsk.RxSingleTimeout );
        TimerStart( &RxTimeoutSyncWord );
    }
    else
    {
        if( rxContinuous == true )
        {
            SX127xSetOpMode( RFLR_OPMODE_RECEIVER );
        }
        else
        {
            SX127xSetOpMode( RFLR_OPMODE_RECEIVER_SINGLE );
        }
    }
}

void SX127xSetTx( uint32_t timeout )
{
    TimerStop( &RxTimeoutTimer );

    TimerSetValue( &TxTimeoutTimer, timeout );

    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        {
            // DIO0=PacketSent
            // DIO1=FifoEmpty
            // DIO2=FifoFull
            // DIO3=FifoEmpty
            // DIO4=LowBat
            // DIO5=ModeReady
            SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RF_DIOMAPPING1_DIO0_MASK &
                                                                            RF_DIOMAPPING1_DIO1_MASK &
                                                                            RF_DIOMAPPING1_DIO2_MASK ) |
                                                                            RF_DIOMAPPING1_DIO1_01 );

            SX127xWrite( REG_DIOMAPPING2, ( SX127xRead( REG_DIOMAPPING2 ) & RF_DIOMAPPING2_DIO4_MASK &
                                                                            RF_DIOMAPPING2_MAP_MASK ) );
            SX127x.Settings.FskPacketHandler.FifoThresh = SX127xRead( REG_FIFOTHRESH ) & 0x3F;
        }
        break;
    case MODEM_LORA:
        {
            if( SX127x.Settings.LoRa.FreqHopOn == true )
            {
                SX127xWrite( REG_LR_IRQFLAGSMASK, RFLR_IRQFLAGS_RXTIMEOUT |
                                                  RFLR_IRQFLAGS_RXDONE |
                                                  RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                                  RFLR_IRQFLAGS_VALIDHEADER |
                                                  //RFLR_IRQFLAGS_TXDONE |
                                                  RFLR_IRQFLAGS_CADDONE |
                                                  //RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                                  RFLR_IRQFLAGS_CADDETECTED );

                // DIO0=TxDone, DIO2=FhssChangeChannel
                SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RFLR_DIOMAPPING1_DIO0_MASK & RFLR_DIOMAPPING1_DIO2_MASK ) | RFLR_DIOMAPPING1_DIO0_01 | RFLR_DIOMAPPING1_DIO2_00 );
            }
            else
            {
                SX127xWrite( REG_LR_IRQFLAGSMASK, RFLR_IRQFLAGS_RXTIMEOUT |
                                                  RFLR_IRQFLAGS_RXDONE |
                                                  RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                                  RFLR_IRQFLAGS_VALIDHEADER |
                                                  //RFLR_IRQFLAGS_TXDONE |
                                                  RFLR_IRQFLAGS_CADDONE |
                                                  RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                                  RFLR_IRQFLAGS_CADDETECTED );

                // DIO0=TxDone
                SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RFLR_DIOMAPPING1_DIO0_MASK ) | RFLR_DIOMAPPING1_DIO0_01 );
            }
        }
        break;
    }

    SX127x.Settings.State = RF_TX_RUNNING;
    TimerStart( &TxTimeoutTimer );
    SX127xSetOpMode( RF_OPMODE_TRANSMITTER );
}

void SX127xStartCad( void )
{
    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        {

        }
        break;
    case MODEM_LORA:
        {
            SX127xWrite( REG_LR_IRQFLAGSMASK, RFLR_IRQFLAGS_RXTIMEOUT |
                                        RFLR_IRQFLAGS_RXDONE |
                                        RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                        RFLR_IRQFLAGS_VALIDHEADER |
                                        RFLR_IRQFLAGS_TXDONE |
                                        //RFLR_IRQFLAGS_CADDONE |
                                        RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL // |
                                        //RFLR_IRQFLAGS_CADDETECTED
                                        );

            // DIO3=CADDone
            SX127xWrite( REG_DIOMAPPING1, ( SX127xRead( REG_DIOMAPPING1 ) & RFLR_DIOMAPPING1_DIO3_MASK ) | RFLR_DIOMAPPING1_DIO3_00 );

            SX127x.Settings.State = RF_CAD;
            SX127xSetOpMode( RFLR_OPMODE_CAD );
        }
        break;
    default:
        break;
    }
}

void SX127xSetTxContinuousWave( uint32_t freq, int8_t power, uint16_t time )
{
    uint32_t timeout = ( uint32_t )( time * 1000 );

    SX127xSetChannel( freq );

    SX127xSetTxConfig( MODEM_FSK, power, 0, 0, 4800, 0, 5, false, false, 0, 0, 0, timeout );

    SX127xWrite( REG_PACKETCONFIG2, ( SX127xRead( REG_PACKETCONFIG2 ) & RF_PACKETCONFIG2_DATAMODE_MASK ) );
    // Disable radio interrupts
    SX127xWrite( REG_DIOMAPPING1, RF_DIOMAPPING1_DIO0_11 | RF_DIOMAPPING1_DIO1_11 );
    SX127xWrite( REG_DIOMAPPING2, RF_DIOMAPPING2_DIO4_10 | RF_DIOMAPPING2_DIO5_10 );

    TimerSetValue( &TxTimeoutTimer, timeout );

    SX127x.Settings.State = RF_TX_RUNNING;
    TimerStart( &TxTimeoutTimer );
    SX127xSetOpMode( RF_OPMODE_TRANSMITTER );
}

int16_t SX127xReadRssi( RadioModems_t modem )
{
    int16_t rssi = 0;

    switch( modem )
    {
    case MODEM_FSK:
        rssi = -( SX127xRead( REG_RSSIVALUE ) >> 1 );
        break;
    case MODEM_LORA:
         	USING_LORA_RADIO_SX1278
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
        if( SX127x.Settings.Channel > RF_MID_BAND_THRESH )
        {
            rssi = RSSI_OFFSET_HF + SX127xRead( REG_LR_RSSIVALUE );
        }
        else
        {
            rssi = RSSI_OFFSET_LF + SX127xRead( REG_LR_RSSIVALUE );
        }
#elif defined( USING_LORA_RADIO_SX1272 )  
			rssi = RSSI_OFFSET_HF + SX127xRead( REG_LR_RSSIVALUE );
#endif		
        break;
    default:
        rssi = -1;
        break;
    }
    return rssi;
}

void SX127xSetOpMode( uint8_t opMode )
{
#if defined( USE_RADIO_DEBUG )
    switch( opMode )
    {
        case RF_OPMODE_TRANSMITTER:
            SX127xDbgPinTxWrite( 1 );
            SX127xDbgPinRxWrite( 0 );
            break;
        case RF_OPMODE_RECEIVER:
        case RFLR_OPMODE_RECEIVER_SINGLE:
            SX127xDbgPinTxWrite( 0 );
            SX127xDbgPinRxWrite( 1 );
            break;
        default:
            SX127xDbgPinTxWrite( 0 );
            SX127xDbgPinRxWrite( 0 );
            break;
    }
#endif
    if( opMode == RF_OPMODE_SLEEP )
    {
        SX127xSetAntSwLowPower( true );
    }
    else
    {
        // Enable TCXO if operating mode different from SLEEP.
#ifdef USE_LORA_RADIO_TCXO		
        SX127xSetBoardTcxo( true );
#endif		
        SX127xSetAntSwLowPower( false );
        SX127xSetAntSw( opMode );
    }
    SX127xWrite( REG_OPMODE, ( SX127xRead( REG_OPMODE ) & RF_OPMODE_MASK ) | opMode );
}

void SX127xSetModem( RadioModems_t modem )
{
    if( ( SX127xRead( REG_OPMODE ) & RFLR_OPMODE_LONGRANGEMODE_ON ) != 0 )
    {
        SX127x.Settings.Modem = MODEM_LORA;
    }
    else
    {
        SX127x.Settings.Modem = MODEM_FSK;
    }

    if( SX127x.Settings.Modem == modem )
    {
        return;
    }

    SX127x.Settings.Modem = modem;
    switch( SX127x.Settings.Modem )
    {
    default:
    case MODEM_FSK:
        SX127xSetOpMode( RF_OPMODE_SLEEP );
        SX127xWrite( REG_OPMODE, ( SX127xRead( REG_OPMODE ) & RFLR_OPMODE_LONGRANGEMODE_MASK ) | RFLR_OPMODE_LONGRANGEMODE_OFF );

        SX127xWrite( REG_DIOMAPPING1, 0x00 );
        SX127xWrite( REG_DIOMAPPING2, 0x30 ); // DIO5=ModeReady
        break;
    case MODEM_LORA:
        SX127xSetOpMode( RF_OPMODE_SLEEP );
        SX127xWrite( REG_OPMODE, ( SX127xRead( REG_OPMODE ) & RFLR_OPMODE_LONGRANGEMODE_MASK ) | RFLR_OPMODE_LONGRANGEMODE_ON );

        SX127xWrite( REG_DIOMAPPING1, 0x00 );
        SX127xWrite( REG_DIOMAPPING2, 0x00 );
        break;
    }
}

void SX127xWrite( uint16_t addr, uint8_t data )
{
    SX127xWriteBuffer( addr, &data, 1 );
}

uint8_t SX127xRead( uint16_t addr )
{
    uint8_t data;
    SX127xReadBuffer( addr, &data, 1 );
    return data;
}

void SX127xWriteFifo( uint8_t *buffer, uint8_t size )
{
    SX127xWriteBuffer( 0, buffer, size );
}

void SX127xReadFifo( uint8_t *buffer, uint8_t size )
{
    SX127xReadBuffer( 0, buffer, size );
}

void SX127xSetMaxPayloadLength( RadioModems_t modem, uint8_t max )
{
    SX127xSetModem( modem );

    switch( modem )
    {
    case MODEM_FSK:
        if( SX127x.Settings.Fsk.FixLen == false )
        {
            SX127xWrite( REG_PAYLOADLENGTH, max );
        }
        break;
    case MODEM_LORA:
        SX127xWrite( REG_LR_PAYLOADMAXLENGTH, max );
        break;
    }
}

void SX127xSetPublicNetwork( bool enable )
{
    SX127xSetModem( MODEM_LORA );
    SX127x.Settings.LoRa.PublicNetwork = enable;
    if( enable == true )
    {
        // Change LoRa modem SyncWord
        SX127xWrite( REG_LR_SYNCWORD, LORA_MAC_PUBLIC_SYNCWORD );
    }
    else
    {
        // Change LoRa modem SyncWord
        SX127xWrite( REG_LR_SYNCWORD, LORA_MAC_PRIVATE_SYNCWORD );
    }
}

uint32_t SX127xGetWakeupTime( void )
{
    return /* SX127xGetBoardTcxoWakeupTime( ) + */ RADIO_WAKEUP_TIME;
}

void SX127xOnTimeoutIrq( void )
{
    switch( SX127x.Settings.State )
    {
    case RF_RX_RUNNING:
        if( SX127x.Settings.Modem == MODEM_FSK )
        {
            SX127x.Settings.FskPacketHandler.PreambleDetected = false;
            SX127x.Settings.FskPacketHandler.SyncWordDetected = false;
            SX127x.Settings.FskPacketHandler.NbBytes = 0;
            SX127x.Settings.FskPacketHandler.Size = 0;

            // Clear Irqs
            SX127xWrite( REG_IRQFLAGS1, RF_IRQFLAGS1_RSSI |
                                        RF_IRQFLAGS1_PREAMBLEDETECT |
                                        RF_IRQFLAGS1_SYNCADDRESSMATCH );
            SX127xWrite( REG_IRQFLAGS2, RF_IRQFLAGS2_FIFOOVERRUN );

            if( SX127x.Settings.Fsk.RxContinuous == true )
            {
                // Continuous mode restart Rx chain
                SX127xWrite( REG_RXCONFIG, SX127xRead( REG_RXCONFIG ) | RF_RXCONFIG_RESTARTRXWITHOUTPLLLOCK );
                TimerStart( &RxTimeoutSyncWord );
            }
            else
            {
                SX127x.Settings.State = RF_IDLE;
                TimerStop( &RxTimeoutSyncWord );
            }
        }
        if( ( RadioEvents != NULL ) && ( RadioEvents->RxTimeout != NULL ) )
        {
            RadioEvents->RxTimeout( );
        }
        LOG_D("PHY RxTimeout\r");
        break;
    case RF_TX_RUNNING:
        // Tx timeout shouldn't happen.
        // But it has been observed that when it happens it is a result of a corrupted SPI transfer
        // it depends on the platform design.
        //
        // The workaround is to put the radio in a known state. Thus, we re-initialize it.

        // BEGIN WORKAROUND

        // Reset the radio
        SX127xReset( );
    
#if defined( USING_LORA_RADIO_SX1276 ) 
        // Calibrate Rx chain
        RxChainCalibration( );
#endif
    
        // Initialize radio default values
        SX127xSetOpMode( RF_OPMODE_SLEEP );

        for( uint8_t i = 0; i < sizeof( RadioRegsInit ) / sizeof( RadioRegisters_t ); i++ )
        {
            SX127xSetModem( RadioRegsInit[i].Modem );
            SX127xWrite( RadioRegsInit[i].Addr, RadioRegsInit[i].Value );
        }
        SX127xSetModem( MODEM_FSK );

        // Restore previous network type setting.
        SX127xSetPublicNetwork( SX127x.Settings.LoRa.PublicNetwork );
        // END WORKAROUND

        SX127x.Settings.State = RF_IDLE;
        if( ( RadioEvents != NULL ) && ( RadioEvents->TxTimeout != NULL ) )
        {
            RadioEvents->TxTimeout( );
        }
        LOG_D("PHY TxTimeout\r");
        break;
    default:
        break;
    }
}

void SX127xOnDio0Irq( void )
{
    volatile uint8_t irqFlags = 0;

    switch( SX127x.Settings.State )
    {
        case RF_RX_RUNNING:
            //TimerStop( &RxTimeoutTimer );
            // RxDone interrupt
            switch( SX127x.Settings.Modem )
            {
            case MODEM_FSK:
                if( SX127x.Settings.Fsk.CrcOn == true )
                {
                    irqFlags = SX127xRead( REG_IRQFLAGS2 );
                    if( ( irqFlags & RF_IRQFLAGS2_CRCOK ) != RF_IRQFLAGS2_CRCOK )
                    {
                        // Clear Irqs
                        SX127xWrite( REG_IRQFLAGS1, RF_IRQFLAGS1_RSSI |
                                                    RF_IRQFLAGS1_PREAMBLEDETECT |
                                                    RF_IRQFLAGS1_SYNCADDRESSMATCH );
                        SX127xWrite( REG_IRQFLAGS2, RF_IRQFLAGS2_FIFOOVERRUN );

                        TimerStop( &RxTimeoutTimer );

                        if( SX127x.Settings.Fsk.RxContinuous == false )
                        {
                            TimerStop( &RxTimeoutSyncWord );
                            SX127x.Settings.State = RF_IDLE;
                        }
                        else
                        {
                            // Continuous mode restart Rx chain
                            SX127xWrite( REG_RXCONFIG, SX127xRead( REG_RXCONFIG ) | RF_RXCONFIG_RESTARTRXWITHOUTPLLLOCK );
                            TimerStart( &RxTimeoutSyncWord );
                        }

                        if( ( RadioEvents != NULL ) && ( RadioEvents->RxError != NULL ) )
                        {
                            RadioEvents->RxError( );
                        }
                        LOG_D("PHY RX Error\r");
                        
                        SX127x.Settings.FskPacketHandler.PreambleDetected = false;
                        SX127x.Settings.FskPacketHandler.SyncWordDetected = false;
                        SX127x.Settings.FskPacketHandler.NbBytes = 0;
                        SX127x.Settings.FskPacketHandler.Size = 0;
                        break;
                    }
                }

                // Read received packet size
                if( ( SX127x.Settings.FskPacketHandler.Size == 0 ) && ( SX127x.Settings.FskPacketHandler.NbBytes == 0 ) )
                {
                    if( SX127x.Settings.Fsk.FixLen == false )
                    {
                        SX127xReadFifo( ( uint8_t* )&SX127x.Settings.FskPacketHandler.Size, 1 );
                    }
                    else
                    {
                        SX127x.Settings.FskPacketHandler.Size = SX127xRead( REG_PAYLOADLENGTH );
                    }
                    SX127xReadFifo( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes, SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                    SX127x.Settings.FskPacketHandler.NbBytes += ( SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                }
                else
                {
                    SX127xReadFifo( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes, SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                    SX127x.Settings.FskPacketHandler.NbBytes += ( SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                }

                TimerStop( &RxTimeoutTimer );

                if( SX127x.Settings.Fsk.RxContinuous == false )
                {
                    SX127x.Settings.State = RF_IDLE;
                    TimerStop( &RxTimeoutSyncWord );
                }
                else
                {
                    // Continuous mode restart Rx chain
                    SX127xWrite( REG_RXCONFIG, SX127xRead( REG_RXCONFIG ) | RF_RXCONFIG_RESTARTRXWITHOUTPLLLOCK );
                    TimerStart( &RxTimeoutSyncWord );
                }

                if( ( RadioEvents != NULL ) && ( RadioEvents->RxDone != NULL ) )
                {
                    RadioEvents->RxDone( RxTxBuffer, SX127x.Settings.FskPacketHandler.Size, SX127x.Settings.FskPacketHandler.RssiValue, 0 );
                }
                LOG_D("PHY RX Done\r");
                SX127x.Settings.FskPacketHandler.PreambleDetected = false;
                SX127x.Settings.FskPacketHandler.SyncWordDetected = false;
                SX127x.Settings.FskPacketHandler.NbBytes = 0;
                SX127x.Settings.FskPacketHandler.Size = 0;
                break;
            case MODEM_LORA:
                {
                    // Clear Irq
                    SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_RXDONE );

                    irqFlags = SX127xRead( REG_LR_IRQFLAGS );
                    if( ( irqFlags & RFLR_IRQFLAGS_PAYLOADCRCERROR_MASK ) == RFLR_IRQFLAGS_PAYLOADCRCERROR )
                    {
                        // Clear Irq
                        SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_PAYLOADCRCERROR );

                        if( SX127x.Settings.LoRa.RxContinuous == false )
                        {
                            SX127x.Settings.State = RF_IDLE;
                        }
                        TimerStop( &RxTimeoutTimer );

                        if( ( RadioEvents != NULL ) && ( RadioEvents->RxError != NULL ) )
                        {
                            RadioEvents->RxError( );
                        }
                        LOG_D("PHY RX Error\r");
                        break;
                    }

                    // Returns SNR value [dB] rounded to the nearest integer value
                    SX127x.Settings.LoRaPacketHandler.SnrValue = ( ( ( int8_t )SX127xRead( REG_LR_PKTSNRVALUE ) ) + 2 ) >> 2;

                    int16_t rssi = SX127xRead( REG_LR_PKTRSSIVALUE );
					
					int16_t rssi_offset = RSSI_OFFSET_HF;
					
#if defined( USING_LORA_RADIO_SX1276 ) || defined( USING_LORA_RADIO_SX1278 ) 
					if( SX127x.Settings.Channel <= RF_MID_BAND_THRESH )
					{
						rssi_offset = RSSI_OFFSET_LF;
					}
#endif					
                    if( SX127x.Settings.LoRaPacketHandler.SnrValue < 0 )
                    {
                        SX127x.Settings.LoRaPacketHandler.RssiValue = rssi_offset + rssi + ( rssi >> 4 ) +
                                                                      SX127x.Settings.LoRaPacketHandler.SnrValue;
                    }
                    else
                    {
                        SX127x.Settings.LoRaPacketHandler.RssiValue = rssi_offset + rssi + ( rssi >> 4 );
                    }

                    SX127x.Settings.LoRaPacketHandler.Size = SX127xRead( REG_LR_RXNBBYTES );
                    SX127xWrite( REG_LR_FIFOADDRPTR, SX127xRead( REG_LR_FIFORXCURRENTADDR ) );
                    SX127xReadFifo( RxTxBuffer, SX127x.Settings.LoRaPacketHandler.Size );

                    if( SX127x.Settings.LoRa.RxContinuous == false )
                    {
                        SX127x.Settings.State = RF_IDLE;
                    }
                    TimerStop( &RxTimeoutTimer );

                    if( ( RadioEvents != NULL ) && ( RadioEvents->RxDone != NULL ) )
                    {
                        RadioEvents->RxDone( RxTxBuffer, SX127x.Settings.LoRaPacketHandler.Size, SX127x.Settings.LoRaPacketHandler.RssiValue, SX127x.Settings.LoRaPacketHandler.SnrValue );
                    }
                    LOG_D("PHY RX Done\r");
                }
                break;
            default:
                break;
            }
            break;
        case RF_TX_RUNNING:
            TimerStop( &TxTimeoutTimer );
            // TxDone interrupt
            switch( SX127x.Settings.Modem )
            {
            case MODEM_LORA:
                // Clear Irq
                SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_TXDONE );
                // Intentional fall through
            case MODEM_FSK:
            default:
                SX127x.Settings.State = RF_IDLE;
                if( ( RadioEvents != NULL ) && ( RadioEvents->TxDone != NULL ) )
                {
                    RadioEvents->TxDone( );
                }
                LOG_D("PHY TX Done\r");
                break;
            }
            break;
        default:
            break;
    }
}

void SX127xOnDio1Irq( void )
{
    switch( SX127x.Settings.State )
    {
        case RF_RX_RUNNING:
            switch( SX127x.Settings.Modem )
            {
            case MODEM_FSK:
                // Stop timer
                TimerStop( &RxTimeoutSyncWord );

                // FifoLevel interrupt
                // Read received packet size
                if( ( SX127x.Settings.FskPacketHandler.Size == 0 ) && ( SX127x.Settings.FskPacketHandler.NbBytes == 0 ) )
                {
                    if( SX127x.Settings.Fsk.FixLen == false )
                    {
                        SX127xReadFifo( ( uint8_t* )&SX127x.Settings.FskPacketHandler.Size, 1 );
                    }
                    else
                    {
                        SX127x.Settings.FskPacketHandler.Size = SX127xRead( REG_PAYLOADLENGTH );
                    }
                }

                // ERRATA 3.1 - PayloadReady Set for 31.25ns if FIFO is Empty
                //
                //              When FifoLevel interrupt is used to offload the
                //              FIFO, the microcontroller should  monitor  both
                //              PayloadReady  and FifoLevel interrupts, and
                //              read only (FifoThreshold-1) bytes off the FIFO
                //              when FifoLevel fires
                if( ( SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes ) >= SX127x.Settings.FskPacketHandler.FifoThresh )
                {
                    SX127xReadFifo( ( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes ), SX127x.Settings.FskPacketHandler.FifoThresh - 1 );
                    SX127x.Settings.FskPacketHandler.NbBytes += SX127x.Settings.FskPacketHandler.FifoThresh - 1;
                }
                else
                {
                    SX127xReadFifo( ( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes ), SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                    SX127x.Settings.FskPacketHandler.NbBytes += ( SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                }
                break;
            case MODEM_LORA:
                // Sync time out
                TimerStop( &RxTimeoutTimer );
                // Clear Irq
                SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_RXTIMEOUT );

                SX127x.Settings.State = RF_IDLE;
                if( ( RadioEvents != NULL ) && ( RadioEvents->RxTimeout != NULL ) )
                {
                    RadioEvents->RxTimeout( );
                }
                LOG_D("PHY Single Rxtimeout\r");
                break;
            default:
                break;
            }
            break;
        case RF_TX_RUNNING:
            switch( SX127x.Settings.Modem )
            {
            case MODEM_FSK:
                // FifoEmpty interrupt
                if( ( SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes ) > SX127x.Settings.FskPacketHandler.ChunkSize )
                {
                    SX127xWriteFifo( ( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes ), SX127x.Settings.FskPacketHandler.ChunkSize );
                    SX127x.Settings.FskPacketHandler.NbBytes += SX127x.Settings.FskPacketHandler.ChunkSize;
                }
                else
                {
                    // Write the last chunk of data
                    SX127xWriteFifo( RxTxBuffer + SX127x.Settings.FskPacketHandler.NbBytes, SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes );
                    SX127x.Settings.FskPacketHandler.NbBytes += SX127x.Settings.FskPacketHandler.Size - SX127x.Settings.FskPacketHandler.NbBytes;
                }
                break;
            case MODEM_LORA:
                break;
            default:
                break;
            }
            break;
        default:
            break;
    }
}

void SX127xOnDio2Irq( void )
{
    switch( SX127x.Settings.State )
    {
        case RF_RX_RUNNING:
            switch( SX127x.Settings.Modem )
            {
            case MODEM_FSK:
                // Checks if DIO4 is connected. If it is not PreambleDetected is set to true.
                ////if( SX127x.DIO4.port == NULL )
                {
                    SX127x.Settings.FskPacketHandler.PreambleDetected = true;
                }

                if( ( SX127x.Settings.FskPacketHandler.PreambleDetected == true ) && ( SX127x.Settings.FskPacketHandler.SyncWordDetected == false ) )
                {
                    TimerStop( &RxTimeoutSyncWord );

                    SX127x.Settings.FskPacketHandler.SyncWordDetected = true;

                    SX127x.Settings.FskPacketHandler.RssiValue = -( SX127xRead( REG_RSSIVALUE ) >> 1 );

                    SX127x.Settings.FskPacketHandler.AfcValue = ( int32_t )( double )( ( ( uint16_t )SX127xRead( REG_AFCMSB ) << 8 ) |
                                                                           ( uint16_t )SX127xRead( REG_AFCLSB ) ) *
                                                                           ( double )FREQ_STEP;
                    SX127x.Settings.FskPacketHandler.RxGain = ( SX127xRead( REG_LNA ) >> 5 ) & 0x07;
                }
                break;
            case MODEM_LORA:
                if( SX127x.Settings.LoRa.FreqHopOn == true )
                {
                    // Clear Irq
                    SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL );

                    if( ( RadioEvents != NULL ) && ( RadioEvents->FhssChangeChannel != NULL ) )
                    {
                        RadioEvents->FhssChangeChannel( ( SX127xRead( REG_LR_HOPCHANNEL ) & RFLR_HOPCHANNEL_CHANNEL_MASK ) );
                    }
                }
                break;
            default:
                break;
            }
            break;
        case RF_TX_RUNNING:
            switch( SX127x.Settings.Modem )
            {
            case MODEM_FSK:
                break;
            case MODEM_LORA:
                if( SX127x.Settings.LoRa.FreqHopOn == true )
                {
                    // Clear Irq
                    SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL );

                    if( ( RadioEvents != NULL ) && ( RadioEvents->FhssChangeChannel != NULL ) )
                    {
                        RadioEvents->FhssChangeChannel( ( SX127xRead( REG_LR_HOPCHANNEL ) & RFLR_HOPCHANNEL_CHANNEL_MASK ) );
                    }
                }
                break;
            default:
                break;
            }
            break;
        default:
            break;
    }
}

void SX127xOnDio3Irq( void  )
{
    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        break;
    case MODEM_LORA:
        if( ( SX127xRead( REG_LR_IRQFLAGS ) & RFLR_IRQFLAGS_CADDETECTED ) == RFLR_IRQFLAGS_CADDETECTED )
        {
            // Clear Irq
            SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_CADDETECTED | RFLR_IRQFLAGS_CADDONE );
            if( ( RadioEvents != NULL ) && ( RadioEvents->CadDone != NULL ) )
            {
                RadioEvents->CadDone( true );
            }
            LOG_D("PHY CAD Done,Detected\r");
        }
        else
        {
            // Clear Irq
            SX127xWrite( REG_LR_IRQFLAGS, RFLR_IRQFLAGS_CADDONE );
            if( ( RadioEvents != NULL ) && ( RadioEvents->CadDone != NULL ) )
            {
                RadioEvents->CadDone( false );
            }
            LOG_D("PHY CAD Done,Not Detected\r");
        }
        
        break;
    default:
        break;
    }
}

void SX127xOnDio4Irq( void  )
{
    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        {
            if( SX127x.Settings.FskPacketHandler.PreambleDetected == false )
            {
                SX127x.Settings.FskPacketHandler.PreambleDetected = true;
            }
        }
        break;
    case MODEM_LORA:
        break;
    default:
        break;
    }
}

void SX127xOnDio5Irq( void )
{
    switch( SX127x.Settings.Modem )
    {
    case MODEM_FSK:
        break;
    case MODEM_LORA:
        break;
    default:
        break;
    }
}

void RadioIrqProcess( uint8_t irq_index )
{
    SX127xDioIrq[irq_index]();
}

