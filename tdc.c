#include <pigpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#define AUTOINC_METHOD
#define PIGPIO

//TDC register addresses; easier to define using enum
enum TDC_REG_ADDR {
    TDC_CONFIG1 = 0x0, 
    TDC_CONFIG2, 
    TDC_INT_STATUS, 
    TDC_INT_MASK, 
    TDC_COARSE_CNTR_OVF_H,
    TDC_COARSE_CNTR_OVF_L,
    TDC_CLOCK_CNTR_OVF_H,
    TDC_CLOCK_CNTR_OVF_L,
    TDC_CLOCK_CNTR_STOP_MASK_H,
    TDC_CLOCK_CNTR_STOP_MASK_L,
    TDC_TIME1 = 0x10,
    TDC_CLOCK_COUNT1,
    TDC_TIME2,
    TDC_CLOCK_COUNT2,    
    TDC_TIME3,
    TDC_CLOCK_COUNT3,    
    TDC_TIME4,
    TDC_CLOCK_COUNT4,    
    TDC_TIME5,
    TDC_CLOCK_COUNT5,    
    TDC_TIME6,
    TDC_CALIBRATION1,
    TDC_CALIBRATION2
};

//communication related definitions
#define TDC_CMD(auto_inc, write, tdc_addr) (auto_inc << 7) | (write << 6) | (tdc_addr)
#define TDC_PARITY_MASK 0x00800000  // bit mask for extracting parity bit from 24-bit data registers (TIMEn, CLOCK_COUNTN, etc.)

//TEST definitions
#define TDC_CLK_PIN 4   // physical pin 7; GPIOCLK0 for TDC reference
#define TDC_ENABLE_PIN 27   // physical pin 13; TDC Enable
#define TDC_INT_PIN 22  // physical pin 15; TDC interrupt pin
#define TDC_BAUD (uint32_t) 20E6
#define TDC_START_PIN 23         // physical pin 16; provides TDC start signal for debugging 
#define TDC_STOP_PIN 18         // physical pin 12; provides TDC stop signal for debugging 
#define TDC_TIMEOUT_USEC (uint32_t)5E6
#define TDC_CLK_FREQ (uint32_t)19.2E6/2

//Laser definitions
#define LASER_PULSE_PIN 23 //physical pin 16
#define LASER_SHUTTER_PIN
#define LASER_ENABLE_PIN


typedef struct TDC {
    uint8_t clk_pin;    // Proivdes TDC reference clock
    uint8_t enable_pin; // active HIGH
    uint8_t int_pin;    // Pin at which to read the TDC interrupt pin; active LO until next measurement
    uint32_t clk_freq;  // frequency of reference clock provided to TDC
    int spi_handle;
} tdc_t;

void printArray(char* arr, int arr_size)
{
    for (int i = 0; i < arr_size; i++)
    {
        printf("%X ", arr[i]);
    }
    printf("\n");
} // end printArray()

//Returns true if n has odd parity
bool checkOddParity(uint32_t n)
{
    /**Calculate parity by repeatedly dividing the bits of n into halves
     * and XORing them together. 1 = Odd parity
     * 
     * 8-bit example:
     * n = b7 b6 b5 b4 b3 b2 b1 b0
     * n ^= (n >> 4) --> n = b7 b6 b5 b4 (b7^b3) (b6^b2) (b5^b1) (b4^b0)
     * n ^= (n >> 2) --> n = b7 b6 b5 b4 (b7^b3) (b6^b2) (b7^b5^b3^b1) (b6^b4^b2^b0)
     * n ^= (n >> 1) --> n = b7 b6 b5 b4 (b7^b3) (b6^b2) (b7^b5^b3^b1) (b7^b6^b5^b4^b3^b2^b1^b0)
     * return n & 1 = return (b7^b6^b5^b4^b3^b2^b1^b0)
     */        
    for (uint8_t i = (sizeof(n)*8 >> 1); i > 0; i >>= 1)
    {
        n ^= (n >> i);
    }

    return n & 1;
} // end getParity

int spiTransact(int fd, char* tx_buf, char* rx_buf, int count)
{   
    /**Funciton that encapsulates an SPI transfer function
     * using either pigpio or spidev routines depending on 
     * defined macros
     */
    int bytes = 0;
    #ifdef PIGPIO
    bytes = spiXfer(fd, tx_buf, rx_buf, count);
    #else
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long) tx_buf,
        .rx_buf = (unsigned long) rx_buf,
        .len = count,
        .delay_usecs = 0,
        .speed_hz = TDC_BAUD,
        .bits_per_word = 8
    };

    bytes = (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) ? -1 : count);
    #endif

    return bytes;
} // end spiTransact

//convert a subset of a byte array into a 32-bit number
uint32_t convertSubsetToLong(char* start, int len, bool big_endian)
{
    /**Parameters:  char* start - pointer to first element in byte array subset
     *              int len - number of elements, max 4, in the subset
     *              bool big_endian - if true, the first element of start is considerd the MSB
     * Returns:     uint32_t out - the final result of conversion
     */

    len = (len > 4 ? 4 : len); //if len > 4, assign 4. OTW assign user-provided length
    uint32_t out = 0;
    for (int i = 0; i < len; i++)
    {
        // shift the bytes pointed to by start and OR to get output
        out |= (start[(big_endian ? len - 1 - i : i)] << 8*i);
    }

    return out; 
} // end convertSubsetToLong

int main () 
{
    tdc_t tdc = {
        .enable_pin = TDC_ENABLE_PIN,
        .int_pin = TDC_INT_PIN,
        .clk_pin = TDC_CLK_PIN,
        .clk_freq = TDC_CLK_FREQ
    };

    gpioCfgClock(1, 1,0); //1us sample rate, PCM clock
    gpioInitialise();

    #ifndef PIGPIO
    /******** spidev init ********/
    tdc.spi_handle = open("/dev/spidev0.0", O_RDWR);
    /*****************************/
    #else
    /******** Pigpio SPI init ********/
    tdc.spi_handle = spiOpen(0, TDC_BAUD, 0
            /* 0b00 |          // Positive (MSb 0) clock edge centered (LSb 0) on data bit
            (0b000 << 2) |  // all 3 CE pins are active low
            (0b000 << 5) |  // all 3 CE pins reserved for SPI
            (0 << 8) |      // 0 = Main SPI; 1 = Aux SPI 
            (0 << 9) |      // If 1, 3-wire mode
            ((0 & 0xF) << 10) | // bytes to write before switching to read (N/A if not in 3-wire mode)
            (0 << 14) |         // If 1, tx LSb first (Aux SPI only)
            (0 << 15) |         // If 1, recv LSb first (Aux SPI only)
            ((8 & 0x3F) << 16)  // bits per word; default 8 */

    );
    /******************************/
    #endif

    printf("tdc.spi_handle=%d\n",tdc.spi_handle);
    gpioHardwareClock(tdc.clk_pin,tdc.clk_freq);
    gpioSetMode(tdc.enable_pin, PI_OUTPUT);     // active HI
    gpioSetMode(tdc.int_pin, PI_INPUT);         // active LOW        
    gpioSetMode(TDC_START_PIN, PI_OUTPUT);     
    gpioSetMode(TDC_STOP_PIN, PI_OUTPUT);     
    
    // TDC must see rising edge of ENABLE while powered for proper internal initializaiton
    gpioWrite(tdc.enable_pin, 0);
    gpioDelay(3); // Short delay to make sure TDC sees LOW before rising edge
    gpioWrite(tdc.enable_pin, 1);
    
    //Non-incrementing write to CONFIG2 reg (address 0x01)
    //Clear CONFIG2 to configure 2 calibration clock periods,
    // no averaging, and single stop signal operation
    uint8_t cal_periods = 2;
    char config2_cmds[] = {0x41, 0x00};
    char config2_rx[sizeof(config2_cmds)];

    // char* config2_rx = spiTransact(tdc.spi_handle, config2_cmds, sizeof(config2_cmds));
    printf("config2 spiWrite=%d\n",spiTransact(tdc.spi_handle, config2_cmds, config2_rx, sizeof(config2_cmds)));    
    printf("config2_rx=");
    printArray(config2_rx, sizeof(config2_rx));
    /****************************************/
    
    while (1)
    {
        //first reset START & STOP pins
        gpioWrite(TDC_START_PIN, 0);
        gpioWrite(TDC_STOP_PIN, 0);

        /******** User input *******/
        float f;
        char c;
        // printf("Enter a positive number (floats allowed) to begin a TDC measurement.\n");
        // printf("The provided number will be the delay between the TDC START and STOP signals.\n");
        // printf("Enter a negative number to quit.\n");
        // scanf(" %f", &f);
        printf("Enter a char to start a measurement. q or Q to quit.\n");
        scanf(" %c", &c);
        if (c == 'q' || c == 'Q') break;
        /***************************/

        /******** Starting Measurement ********/
        // static definition of commands to start a TDC measurement
        static char meas_cmds[2] = {
            0x40,   //Write to CONFIG1
            0x43    //Start measurement in mode 2 with parity and rising edge start, stop, trigger signals 
        };
        char meas_cmds_rx[sizeof(meas_cmds)];
        printf("measurement start spiWrite=%d\n", spiTransact(tdc.spi_handle, meas_cmds, meas_cmds_rx, sizeof(meas_cmds))); //start new measurement on TDC
        gpioDelay(10); // small delay to allow TDC to process data
        /**************************************/
        
        /********* Simulate Single Photon Detector *******/
        gpioWrite(TDC_START_PIN, 1);
        gpioDelay(5);
        gpioWrite(TDC_STOP_PIN, 1);
        /*************************************************/

        printf("AFter debug pulse\n");

        //Poll TDC INT pin to signal available data
        uint32_t start_tick = gpioTick();
        while (gpioRead(tdc.int_pin) && (gpioTick() - start_tick) < TDC_TIMEOUT_USEC);

        // ToF Calculation
        if (!gpioRead(tdc.int_pin)) //if TDC INT was pulled low in time
        {
            /********* Variable Declarations *********/
            uint32_t time1;         // internal clock counts from START edge to next external clock edge 
            uint32_t clock_count1;  // external clock counts from TIME1 to STOP edge
            uint32_t time2;         // internal clock counts from CLOCK_COUNT1 to next external clock edge
            uint32_t calibration1;
            uint32_t calibration2;
            double ToF;
            
            /**array to holds return bytes from both SPI transactions retrieving Measurement registers
             * TIME1, CLOCK_COUNT1, TIME2, CALIBRATION1, CALIBRATION2 in that order.
             * These registers are 24-bits long where the MSb is a parity bit.
             * Hence rx_buff holds 5 3-byte data chars and 2 1-byte command chars (17 bytes total)
             * rx_buff[0] = 0 (junk data from Transaction 1 command byte)
             * rx_buff[1-3] = TIME1 bytes in big-endian order
             * rx_buff[4-6] = CLOCK_COUNT1 bytes in big-endian order
             * rx_buff[7-9] = TIME2 bytes in big-endian order
             * rx_buff[10] = 0 (junk data from Transaction 2 command byte)
             * rx_buff[11-13] = CALIBRATION1 in big-endian order
             * rx_buff[14-16] = CALIBRATION2 in big-endian order
             */ 
            char rx_buff[17];
            
            // static array of rx_buff indices pointing to TDC register data
            static uint8_t const rx_data_idx[] = {
                1,  // TIME1 data start index
                4,  // CLOCK_COUNT1 data start index
                7,  // TIME2 data start index
                11, // CALIBRATION1 data start index
                14  // CLAIBRATION2 data start index
            };

            // array to hold converted Measurement Register values in same order as rx_buff
            uint32_t tdc_data[sizeof(rx_data_idx)/sizeof(*rx_data_idx)]; 

            // false if parity of any received data is not even
            bool valid_data_flag = true; 
            /*******************************************/

            #ifdef AUTOINC_METHOD
            /******** Transaction 1 *********/
            /**Transaction 1 starts an auto-incrementing read at register TIME1,
             * reading 9 bytes to obtain the 3-byte long TIME1, CLOCK_COUNT1, 
             * and TIME2 registers.
             */
            char tx_buff1[10] = {0x90}; // start an auto incrementing read to read TIME1, CLOCK_COUNT1, TIME2 in a single command
            printf("Transaction 1 spiXfer=%d\n",spiTransact(tdc.spi_handle, tx_buff1, rx_buff, sizeof(tx_buff1)));
            
            //print returned data
            printf("rx_buff after transaction 1=");
            printArray(rx_buff, sizeof(rx_buff));

            /*********************************/

            /********* Transaction 2 *********/
            /**Transaciton 2 starts an auto-incrementing read at register CALIBRATION1
             * and reads the 24-bit CALIBRATION1 and CALIBRATION2 registers. Hence, 
             * the transaction sends 7 bytes (1 command, 6 reading bytes). The first
             * byte of the return buffer will always be 0
             */
            char tx_buff2[7] = {TDC_CMD(1, 0, TDC_CALIBRATION1)}; //auto incrementing read of CALIBRATION1 and CALIBRATION2
            printf("Transaction 2 spiXfer=%d\n", spiTransact(tdc.spi_handle, tx_buff2, rx_buff+sizeof(tx_buff1),sizeof(tx_buff2)));

            // print returne data
            printf("rx_buff after txaction 2=");
            printArray(rx_buff, sizeof(rx_buff));
            /*********************************/

            /******** Converting Data into 32-bit Numbers ********/
            /**Iterate over the indices in rx_data_idx, converting the 
             * subset of 3 bytes starting at rx_buff[rx_data_idx[i]]
             */
            for (uint8_t i = 0; i < sizeof(rx_data_idx)/sizeof(*rx_data_idx); i++)
            {
                uint32_t conv = convertSubsetToLong(rx_buff+rx_data_idx[i], 3, true);
                if (checkOddParity(conv)) 
                {
                    valid_data_flag = false;
                    break;
                }
                
                tdc_data[i] = conv & ~TDC_PARITY_MASK; // clear the parity bit from data
            }
            /*****************************************************/
            #else
            static char tof_cmds[5] = {
                // TDC commands for retrieving TOF
                0x10, //Read TIME1
                0x11, //Read CLOCK_COUNT1
                0x12, //Read TIME2
                0x1B, //Read CALIBRATION
                0x1C  //Read CALIBRATION2
            };
            
            for (int i = 0; i < sizeof(tof_cmds); i++)
            {
                char tx_temp[4] = {tof_cmds[i]};
                char rx_temp[4];

                printf("Command %02X transaction=%d\n",tof_cmds[i], spiTransact(tdc.spi_handle, tx_temp, rx_temp, sizeof(tx_temp)));
                printf("Command %02X return=", tof_cmds[i]);
                printArray(rx_temp, sizeof(rx_temp));

                uint32_t conv = convertSubsetToLong(rx_temp, 4,true);
                if (checkOddParity(conv))
                {
                    valid_data_flag = false;
                    break;
                } 
                tdc_data[i] = conv & ~TDC_PARITY_MASK; 
                printf("rx_buff1 converted= %u\n", tdc_data[i]);
            }
            #endif

            printf("valid=%d", valid_data_flag);
            
            if (valid_data_flag) 
            {
                time1 = tdc_data[0];
                clock_count1 = tdc_data[1];
                time2 = tdc_data[2];
                calibration1 = tdc_data[3];
                calibration2 = tdc_data[4];

                printf("time1=%u\n", time1);
                printf("clock_count1=%u\n", clock_count1);
                printf("time2=%u\n", time2);
                printf("calibration1=%u\n", calibration1);
                printf("calibration2=%u\n", calibration2);

                double calCount = (calibration2 - calibration1) / (double)(cal_periods - 1);
                if (!calCount)
                    ToF = 0; // catch divide-by-zero error
                else
                {
                    ToF = (((double)time1 - time2) / calCount + clock_count1) / tdc.clk_freq * 1E6;
                    printf("ToF = %f usec\n", ToF);
                }
            } // end if (valid_data_flag)
            else
            {
                // if any invalid data retrieved from TDC, set negative ToF
                printf("Invalid data\n");
                ToF = -1;
            } //end else linked to if (valid_data_flag)
        } // end if (!gpioRead(tdc.int_pin)), i.e. no timeout waiting for TDC
        else //else timeout occured
        {
            printf("TDC timeout occured\n");
        } // end else linked to if (!gpioRead(tdc.int_pin))
    } // end while(1)

    #ifdef PIGPIO
    spiClose(tdc.spi_handle);
    #else
    close(tdc.spi_handle);
    #endif
    gpioTerminate();
} // end main()