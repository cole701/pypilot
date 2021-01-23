/* Copyright (C) 2021 Sean D'Epagnier <seandepagnier@gmail.com>
 *
 * This Program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/* this program interfaces with the bmp280 pressure sensor and outputs
   calibrated nmea0183 data over serial at 38400 baud

   anemometer wires

using potentiometer for wind direction
using a reed switch for wind speed

rj12        - arduino - rj12  -  connections
wire color  - pin     - pin
red         - gnd - pin 3 - potentiometer power A - reed switch A
yellow      - 5v  - pin 5 - potentiometer power B
green       - a7  - pin 4 - potentiometer reading
black       - d2  - pin 2 -                         reed switch B
*/

#include <avr/eeprom.h>


#include <Arduino.h>
#include <stdint.h>
#include <avr/sleep.h>
#include <HardwareSerial.h>
#include <avr/boot.h>

extern "C" {
  #include <twi.h>
}

#define NONE      0 
#define NOKIA5110L 1
#define JLX12864G 2

#define LCD NOKIA5110L
//#define LCD JLX12864G

#if LCD == NOKIA5110L
#include "PCD8544.h"
static PCD8544 lcd(13, 11, 8, 7, 4);
#elif LCD == JLX12864G
#include "JLX12864.h"
static JLX12864 lcd(13, 11, 8, 7, 4);
#endif

const int analogInPin = A7;  // Analog input pin that the potentiometer is attached to
const int analogLightPin = A6;
const int analogBacklightPin = 9;

int16_t cross_count = 1000; // calibrate range during first 1000 crossings of 0

uint8_t have_bmp280 = 0;
uint8_t bmX280_tries = 10;  // try 10 times to configure
uint16_t dig_T1, dig_P1;
int16_t dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

int16_t cal_wind_min_reading, cal_wind_max_reading;
uint32_t eeprom_write_timeout = 0;

struct eeprom_data_struct {
    char signature[6];
    int16_t wind_min_reading, wind_max_reading;
    uint8_t sensor_type; // 0=mlx90316, 1=davis
    uint8_t display_orientation; // 0=normal, 1=flip
    uint8_t backlight_setting; // 0=off, 1=on, 2=auto
    uint8_t direction_type; // 0=-180 to 180, 1=0 to 360
    uint8_t temperature_units; // 0=C, 1=F
    uint8_t leds_on; // 1=power leds
    uint8_t display_page; // 0=default, 1=baro graph, 2-8=settings
    uint8_t baro_page; // 5 min, 1 hour, 24 hour
} eeprom_data;

#if LCD == JLX12864G
const int history_len = 64;
#else
const int history_len = 48;
#endif

static struct baro_history_t {
    int8_t data[history_len];
    uint16_t last;
    uint32_t last_updatetime;
    uint8_t pos;
} baro_history[3]; // 5 minute, hour, day

const uint32_t baro_times[3] = {60000L*5/history_len, 60000L*60/history_len, 60000L*60*24/history_len};


void bmX280_setup()
{
  Serial.println(F("bmX280 setup"));

  // NOTE:  local version of twi does not enable pullup as
  // bmX_280 device is 3.3v and arduino runs at 5v

  twi_init();

  // incase arduino version (although pullups will put
  // too high voltage for short time anyway....
  pinMode(SDA, INPUT);
  pinMode(SCL, INPUT);
  delay(1);

  have_bmp280 = 0;
  bmX280_tries--;

  uint8_t d[24];
  d[0] = 0xd0;

  if(twi_writeTo(0x76, d, 1, 1, 1) == 0 &&
     twi_readFrom(0x76, d, 1, 1) == 1 &&
     d[0] == 0x58)
      have_bmp280 = 1;
  else {
      Serial.print(F("bmp280 not found: "));
      Serial.println(d[0]);
      // attempt reset command
      //d[0] = 0xe0;
      //d[1] = 0xb6;
      //twi_writeTo(0x76, d, 2, 1, 1);
      TWCR &= ~(_BV(TWEN));
      return;
  }

  d[0] = 0x88;
  if(twi_writeTo(0x76, d, 1, 1, 1) != 0)
      have_bmp280 = 0;

  uint8_t c = twi_readFrom(0x76, d, 24, 1);
  if(c != 24) {
      Serial.println(F("bmp280 failed to read calibration"));
      have_bmp280 = 0;
  }
      
  dig_T1 = d[0] | d[1] << 8;
  dig_T2 = d[2] | d[3] << 8;
  dig_T3 = d[4] | d[5] << 8;
  dig_P1 = d[6] | d[7] << 8;
  dig_P2 = d[8] | d[9] << 8;
  dig_P3 = d[10] | d[11] << 8;
  dig_P4 = d[12] | d[13] << 8;
  dig_P5 = d[14] | d[15] << 8;
  dig_P6 = d[16] | d[17] << 8;
  dig_P7 = d[18] | d[19] << 8;
  dig_P8 = d[20] | d[21] << 8;
  dig_P9 = d[22] | d[23] << 8;

  
#if 0
  Serial.println("bmp280 pressure compensation:");
  Serial.println(dig_T1);
  Serial.println(dig_T2);
  Serial.println(dig_T3);
  Serial.println(dig_P1);
  Serial.println(dig_P2);
  Serial.println(dig_P3);
  Serial.println(dig_P4);
  Serial.println(dig_P5);
  Serial.println(dig_P6);
  Serial.println(dig_P7);
  Serial.println(dig_P8);
  Serial.println(dig_P9);
#endif  

  // b00011111  // configure
  d[0] = 0xf4;
  d[1] = 0xff;
  if(twi_writeTo(0x76, d, 2, 1, 1) != 0)
      have_bmp280 = 0;
}

volatile unsigned int rotation_count;
volatile uint16_t lastperiod;

void isr_anemometer_count()
{
    uint8_t pin = digitalRead(2);
//    Serial.print("read:  ");
//      Serial.println(pin);

    static uint16_t lastt, lastofft;
    static uint8_t lastpin;
    int t = millis();

    if(!pin && lastpin) {
        uint16_t period = t-lastt;
        uint16_t offperiod = t-lastofft;

        if(period > 10)
#if 0
        if(offperiod > 10 && // debounce, at least for less than 120 knots of wind
           offperiod < period) // test good reading
#endif
        {
            lastt = t;            
            lastperiod += period;
            rotation_count++;
        }
    }
    lastpin = pin;
    lastofft = t;
}

void apply_settings()
{
    if(eeprom_data.sensor_type)
        pinMode(3, INPUT); // do not use pullup for potentiometer style
    else // apply weak pullup to input wind direction to detect if wire is disconnected
        pinMode(3, INPUT_PULLUP);

    lcd.flip = eeprom_data.display_orientation;
    if(eeprom_data.leds_on) {
        pinMode( A3, OUTPUT);
        digitalWrite(A3, HIGH);
    } else {        
        digitalWrite(A3, LOW);
        pinMode( A3, INPUT);
    }
}
        
#include <avr/wdt.h>

volatile uint8_t calculated_clock = 0; // must be volatile to work correctly

void setup()
{
    cli();
    CLKPR = _BV(CLKPCE);
    CLKPR = _BV(CLKPS1); // divide clock by 4
    MCUSR = 0;

    // set watchdog interrupt 16 ms to detect the clock speed (support 8mhz or 16mhz crystal)
    wdt_reset();

    WDTCSR = (1<<WDCE)|(1<<WDE);
    WDTCSR = (1<<WDIE);
    sei();

    uint32_t start = micros();
    while(!calculated_clock);  // wait for watchdog to fire
    uint16_t clock_time = micros() - start;
    // after timing the clock frequency set the correct divider
    if(clock_time > 6000) {
        cli();
        CLKPR = _BV(CLKPCE);
        CLKPR = _BV(CLKPS0); // divide by 2
        sei();
    } else {
        cli();
        CLKPR = _BV(CLKPCE);
        CLKPR = 0; // divide by 1
        sei();
    }        

    // now we are at 8mhz with either 8mhz or 16mhz xtal, must compile with C_FPU = 8mhz
    
    // set up watchdog again
    cli();
    WDTCSR = (1<<WDCE) | (1<<WDE);
    WDTCSR = (1<<WDIE) | (1<<WDP2) | (1<<WDP1); // interrupt in 1 second

    sei();

    Serial.begin(38400);  // start serial for output
    Serial.print(F("STARTUP\n"));
    Serial.print(clock_time);
    // default values
  
    // read eeprom and determine if it is valid
    struct eeprom_data_struct ram_eeprom;
    eeprom_read_block(&ram_eeprom, 0, sizeof ram_eeprom);

    char signature[] PROGMEM = "arws12";

    // defaults
    memcpy_P(eeprom_data.signature, signature, sizeof signature);

    cal_wind_min_reading = cal_wind_max_reading = 512;
    
    eeprom_data.wind_min_reading = 131;
    eeprom_data.wind_max_reading = 884;
    eeprom_data.sensor_type = 0;
    eeprom_data.display_orientation = 0;
    eeprom_data.backlight_setting = 2;
    eeprom_data.direction_type = 0;
    eeprom_data.temperature_units = 0;
    eeprom_data.leds_on = 1;
    eeprom_data.display_page = 0;
  
    if(memcmp_P(ram_eeprom.signature, signature, sizeof ram_eeprom.signature) == 0)
        memcpy(&eeprom_data, &ram_eeprom, sizeof eeprom_data);
    
    if(eeprom_data.wind_min_reading > 0 && // ensure somewhat sane range
       eeprom_data.wind_max_reading < 1024 &&
       eeprom_data.wind_min_reading < eeprom_data.wind_max_reading-100) {
        Serial.print(F("Calibration valid  "));
        Serial.print(eeprom_data.wind_min_reading);
        Serial.print(F("  "));
        Serial.println(eeprom_data.wind_max_reading);
    } else {
        Serial.print(F("Warning: calibration invalid, resetting it"));
        eeprom_data.wind_min_reading = 300;
        eeprom_data.wind_max_reading = 650;
    }
  
    // read fuses, and report this as flag if they are wrong
    uint8_t lowBits      = boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS);
    uint8_t highBits     = boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS);
    uint8_t extendedBits = boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS);
    //uint8_t lockBits     = boot_lock_fuse_bits_get(GET_LOCK_BITS);
    if(lowBits != 0xFF || highBits != 0xda ||
       (extendedBits != 0xFD && extendedBits != 0xFC)
       //|| lockBits != 0xCF
        )
        Serial.print(F("Warning, fuses set wrong, flash may become corrupted"));

    bmX280_setup();

    attachInterrupt(0, isr_anemometer_count, CHANGE);
    pinMode(analogInPin, INPUT);
    pinMode(2, INPUT_PULLUP);

    apply_settings();
    
    ADMUX = _BV(REFS0) | 6;
    ADCSRA |= _BV(ADIE);
    ADCSRA |= _BV(ADSC);   // Set the Start Conversion flag.

#if LCD==NOKIA5110L
    // PCD8544-compatible displays may have a different resolution...
    lcd.begin(84, 48);
#elif LCD==JLX12864G
    lcd.begin();
#endif
    
    pinMode( analogLightPin, INPUT);
    // enable pullups on buttons
    pinMode( 5, INPUT_PULLUP);
    pinMode( 6, INPUT_PULLUP);

    for(int i=0; i<3; i++) {
        baro_history[i].data[history_len-1] = -127;
        baro_history[i].last = 60000;
    }
}

ISR(WDT_vect)
{
    wdt_reset();
    wdt_disable();
    if(!calculated_clock) {
        calculated_clock = 1; // use watchdog interrupt once at startup to compute the crystal's frequency
        return;
    }
    // normal watchdog event (program stuck)
    delay(1);
    asm volatile ("ijmp" ::"z" (0x0000)); // soft reset
}

static volatile uint8_t adcchannel;
static volatile uint32_t adcval[4];
static volatile int16_t adccount[4];

ISR(ADC_vect)
{
    ADCSRA |= _BV(ADSC);   // Set the Start Conversion flag.
    uint16_t adcw = ADCW;
    if(adcchannel == 0) {
        adccount[0]++;
        // backlight sensor
        if(adcchannel == 0 && adccount[0] >= 1) {
            adcval[0] = adcw;
            ADMUX = _BV(REFS0) | 7, adcchannel = 1;
        }
    } else {
        uint8_t b = adcw < 448 ? 1 : adcw < 576 ? 2 : 3;
        if(adccount[b] < 4000) {
            adccount[b]++;
            if(adccount[b] > 0)
                adcval[b] += adcw;
        }
    }
}

int32_t t_fine;
int32_t bmp280_compensate_T_int32(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T>>3) - ((int32_t)dig_T1<<1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T>>4) - ((int32_t)dig_T1)) * ((adc_T>>4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

// Returns pressure in Pa as unsigned 32 bit integer in Q24.8 format (24 integer bits and 8 fractional bits).
// Output value of “24674867” represents 24674867/256 = 96386.2 Pa = 963.862 hPa
uint32_t bmp280_compensate_P_int64(int32_t adc_P)
{
    uint64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (uint64_t)dig_P6;
    var2 = var2 + ((var1*(uint64_t)dig_P5)<<17);
    var2 = var2 + (((uint64_t)dig_P4)<<35);
    var1 = ((var1 * var1 * (uint64_t)dig_P3)>>8) + ((var1 * (uint64_t)dig_P2)<<12);
    var1 = (((((int64_t)1)<<47)+var1))*((uint64_t)dig_P1)>>33;
    if (var1 == 0)
    {
        return 0; // avoid exception caused by division by zero
    }
    p = 1048576-adc_P;
    p = (((p<<31)-var2)*3125)/var1;
    var1 = (((uint64_t)dig_P9) * (p>>13) * (p>>13)) >> 25;
    var2 = (((uint64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((uint64_t)dig_P7)<<4);
    return (uint32_t)p;
}

uint8_t checksum(const char *buf)
{
    uint8_t cksum = 0;
    for(uint8_t i=0; i<strlen(buf); i++)
        cksum ^= buf[i];
    return cksum;
}

void send_nmea(const char *buf)
{
  char buf2[128];
  snprintf(buf2, sizeof buf2, ("$%s*%02x\r\n"), buf, checksum(buf));
  Serial.print(buf2);
}

uint8_t lcd_update=1;
float wind_dir, wind_speed, wind_speed_30;

void read_anemometer()
{
    static float lpdir;
    const float lp = .2; // lowpass filter constant

    if(adccount[1] < 100 && adccount[2] < 100 && adccount[3] < 100)
        return; // not enough data
    
    cli();
    uint32_t val[3];
    int16_t count[3];
    for(int i=0; i<3; i++) {
        val[i] = adcval[i+1];
        count[i] = adccount[i+1];
    }
    // reset the adc
    for(int i=0; i<4; i++) {
        adcval[i] = 0;
        adccount[i] = -4;
    }

#if LCD
    // read from backlight sense
    adcchannel = 0;
    ADMUX = _BV(REFS0) | 6;
#else
    ADMUX = _BV(REFS0) | 7, adcchannel = 1;
#endif
    sei();

    // read the analog in value:
    if(count[0] > 8 && count[2] > 8 && count[1] <= 0) {
#if 0
        Serial.print(F("CROSS!!!!   "));
        Serial.print(float(val[0]) / count[0]);
        Serial.print(F("  "));
        Serial.print(float(val[2]) / count[2]);
        Serial.print(F("  "));
        Serial.print(count[0]);
        Serial.print(F("  "));
        Serial.println(count[2]);
#endif
        if(cross_count > 0) {
            int16_t a = val[0]/count[0];
            int16_t b = val[2]/count[2];
            
            cal_wind_min_reading = min(cal_wind_min_reading, a);
            cal_wind_max_reading = max(cal_wind_max_reading, b);
#if 0
            Serial.print(F("Calibrating  "));
            Serial.print(cal_wind_min_reading);
            Serial.print(F(" "));
            Serial.print(eeprom_data.wind_max_reading);
            Serial.print(F(" "));
            Serial.print(cal_wind_max_reading);
            Serial.print(F(" "));
            Serial.print(eeprom_data.wind_max_reading);
            Serial.print(F(" "));
            Serial.println(cross_count);
#endif
            cross_count--;
        }
    
        if(cross_count == 0) { // write at zero
            Serial.println(F("Calibration Finished"));
            cross_count--;

            // don't write update unless there is a significant change
            if(abs(cal_wind_max_reading - eeprom_data.wind_max_reading) > 10 ||
               abs(cal_wind_min_reading - eeprom_data.wind_min_reading) > 10) {
                eeprom_write_timeout = millis() + 10000;
                cross_count = 1000;
                cal_wind_min_reading = 512;
                cal_wind_max_reading = 512;
            }
        }
    }

    if(count[0] > 0 && count[2] > 0) {
        // discard this since we crossed zero and read
        // possibly invalid data
        return;
    }

    uint32_t tval = 0;
    int16_t tcount = 0;
    for(int i=0; i<3; i++)
        if(count[i] > 64) {
            tval += val[i];
            tcount += count[i];
        }

    if(tcount < 64) {
        //Serial.println(count[0]);
        //Serial.println(count[1]);
        //Serial.println(count[2]);
        //Serial.println(tcount);
        //Serial.println("Not enough data!!");
        return; // not enough data
    }

    int16_t sensorValue = tval / tcount; // average data

    // make sure the value is sane
    if(sensorValue < 0 || sensorValue > 1023) {
        Serial.println(F("invalid range: program error"));
        return;
    }

    float dir = 0;
    if(eeprom_data.sensor_type)
    {
        // compensate 13 degree deadband in potentiometer over full range
        dir = 360 - (sensorValue + 13) * .34;
    } else
    {
        if(sensorValue < eeprom_data.wind_min_reading - 40 || sensorValue > eeprom_data.wind_max_reading + 40)
            dir = lpdir = -1; // invalid
        else
        {
            if(sensorValue < eeprom_data.wind_min_reading)
                sensorValue = eeprom_data.wind_min_reading;
            else if(sensorValue > eeprom_data.wind_max_reading)
                sensorValue = eeprom_data.wind_max_reading;
        
            dir = 360 - float(sensorValue - eeprom_data.wind_min_reading)
                / (eeprom_data.wind_max_reading - eeprom_data.wind_min_reading) * 360.0;
        }
    }

    // lowpass wind direction
    if(dir >= 0) {
        if(lpdir - dir > 180)
            dir += 360;
        else if(dir - lpdir > 180)
            dir -= 360;

        if(fabs(lpdir - dir) < 180) // another test which should never fail
            lpdir = lp*dir + (1-lp)*lpdir;

        if(lpdir >= 360)
            lpdir -= 360;
        else if(lpdir < 0)
            lpdir += 360;
    }

    uint16_t time = millis();
    static uint16_t last_time;
    uint16_t dt = time - last_time;
    if(dt >= 100) { // output every 100ms
        last_time += 100;

        cli();
        uint16_t period = lastperiod;
        uint16_t count = rotation_count;
        rotation_count = 0;
        lastperiod = 0;
        sei();
        
        static uint16_t nowindcount;
        static float knots = 0, lastnewknots = 0;
        const int nowindtimeout = 30;
        if(count) {
            if(nowindcount!=nowindtimeout) {
                float newknots = .868976 * 2.25 * 1000 * count / period;
#if 0
                Serial.print(lastnewknots);
                Serial.print(F("   "));
                Serial.print(newknots);
                Serial.print("   ");
                Serial.println(lastnewknots/newknots-1);
#endif
                // if changing too fast, maybe bad reading
                if(lastnewknots == 0 || fabs(lastnewknots - newknots) < 5 || fabs(lastnewknots/newknots-1) <= .5)
                    knots = newknots;

                lastnewknots = newknots;
            }

            nowindcount = 0;
        } else {
            if(nowindcount<nowindtimeout)
                nowindcount++;
            else
                knots = lastnewknots = 0;
        }

        char buf[128];
        if(dir >= 0)
            snprintf_P(buf, sizeof buf, PSTR("ARMWV,%d.%02d,R,%d.%02d,N,A"), (int)lpdir, (uint16_t)(lpdir*100.0)%100U, (int)knots, (int)(knots*100)%100);
        else // invalid wind direction (no magnet?)
            snprintf_P(buf, sizeof buf, PSTR("ARMWV,,R,%d.%02d,N,A"), (int)knots, (int)(knots*100)%100);
        send_nmea(buf);
        
        wind_dir = lpdir;
        wind_speed = knots;
        wind_speed_30 = wind_speed_30*299.0/300.0 + wind_speed/300.0;
        lcd_update = 1;
    }
}

uint32_t pressure, temperature;
int32_t pressure_comp, temperature_comp;
int bmp280_count;

static uint32_t baro_val, baro_count;
void put_baro_history(uint8_t index, int32_t val)
{
    baro_history[index].last_updatetime += baro_times[index];
    val -= 60000;
    int diff = (val - baro_history[index].last)/5;
    if(diff > 127)
        diff = 127;
    else if(diff < -127)
        diff = -127;
    
    baro_history[index].data[baro_history[index].pos++] = diff;
    baro_history[index].last = val;
    if(baro_history[index].pos == history_len)
        baro_history[index].pos = 0;
}

void update_barometer_history()
{
    baro_val += pressure_comp;
    baro_count++;

    uint32_t t = millis();
    uint32_t dt32 = t - baro_history[0].last_updatetime;
    if(dt32 > baro_times[0]) {
        int32_t val = baro_val / baro_count;
        put_baro_history(0, val);
        for(int i=1; i<3; i++) {
            dt32 = t - baro_history[i].last_updatetime;
            if(dt32 > baro_times[i])
                put_baro_history(i, val);
        }
        baro_val = baro_count = 0;
    }
}

void read_pressure_temperature()
{
    if(!bmX280_tries)
        return;

    uint8_t buf[6] = {0};
    buf[0] = 0xf7;
    uint8_t r = twi_writeTo(0x76, buf, 1, 1, 1);
    if(r && have_bmp280) {
        Serial.print(F("bmp280 twierror "));
        Serial.println(r);
    }

    uint8_t c = twi_readFrom(0x76, buf, 6, 1);
    if(c != 6 && have_bmp280)
        Serial.println(F("bmp280 failed to read 6 bytes from bmp280"));

    int32_t p, t;
    
    p = (int32_t)buf[0] << 16 | (int32_t)buf[1] << 8 | (int32_t)buf[2];
    t = (int32_t)buf[3] << 16 | (int32_t)buf[4] << 8 | (int32_t)buf[5];
    
    if(t == 0 || p == 0)
        have_bmp280 = 0;

    pressure += p >> 2;
    temperature += t >> 2;
    bmp280_count++;

    if(!have_bmp280) {
        if(bmp280_count == 256) {
            bmp280_count = 0;
            /* only re-run setup when count elapse */
            bmX280_setup();
        }
        return;
    }

    if(bmp280_count == 1024) {
        bmp280_count = 0;
        pressure >>= 12;
        temperature >>= 12;
    
        temperature_comp = bmp280_compensate_T_int32(temperature);
        pressure_comp = bmp280_compensate_P_int64(pressure) >> 8;
        pressure = temperature = 0;
  
        char buf[128];
        int ap = pressure_comp/100000;
        uint32_t rp = pressure_comp - ap*100000UL;
        snprintf_P(buf, sizeof buf, PSTR("ARMDA,,,%d.%05ld,B,,,,,,,,,,,,,,,,"), ap, rp);
        send_nmea(buf);

        int a = temperature_comp / 100;
        int r = temperature_comp - a*100;
        snprintf_P(buf, sizeof buf, PSTR("ARMTA,%d.%02d,C"), a, abs(r));
        send_nmea(buf);

        lcd_update = 1;
        update_barometer_history();
    }
}

void read_light()
{
    static int lastlight, lighton;
    if(eeprom_data.backlight_setting == 0)
        lighton = 0;
    else if(eeprom_data.backlight_setting == 1)
        lighton = 1;
    else
    {
        cli();
        int16_t light = adcval[0];
        sei();
        // filter and map light value
        light = (light + 31*lastlight) / 32;
        lastlight = light;
        if(lighton) {
            if(light > 900)
                lighton = 0;
        } else {
            if(light < 800)
                lighton = 1;
        }
    }

#ifdef LCD_BL_HIGH
    int pwm = lighton ? 90 : 0; // backlight expects 3v3 but we have 5v!
                                // do not set above 160 or may damage backlight!!
#else
    int pwm = lighton ? 120 : 255;
#endif
    analogWrite(analogBacklightPin, pwm);
}

static uint16_t last_lcd_updatetime = -1000, last_lcd_texttime;
static char status_buf[4][16];
void draw_anemometer()
{
#if LCD
    if(!lcd_update)
        return;

    uint16_t time = millis();
    uint16_t dt = time - last_lcd_updatetime;
    if(dt < 150) // don't update faster than 6-7 frames per second
        return;

    last_lcd_updatetime = time;
    
    lcd_update = 0;
    dt = time - last_lcd_texttime;

    if(dt > 700) { // don't update text so fast
        int a, r;
        last_lcd_texttime = time;

        if(wind_dir>=0) {
            int dir = wind_dir;
            if(!eeprom_data.direction_type)
                if(dir > 180)
                    dir = abs(dir-360);
            snprintf_P(status_buf[0], sizeof status_buf[0], PSTR("%02d"), (int) round(dir));
        }

        snprintf_P(status_buf[1], sizeof status_buf[1], PSTR("%02d"), (int) round(wind_speed));
#if LCD == JLX12864G
        lcd.clear();
        // draw wind speed
        lcd.setfont(5);
        lcd.setpos(0, 0);
        lcd.print(status_buf[1]);
#else
        snprintf_P(status_buf[1], sizeof status_buf[1], PSTR("%02d"), (int) round(wind_speed));
        lcd.clear_lines(46, 83); // clear compass
        // draw wind speed
        lcd.setfont(3);
        lcd.setpos(0, 38);
        lcd.print(status_buf[1]);
#endif

        // draw 30 second average wind speed
        snprintf_P(status_buf[1], sizeof status_buf[1], PSTR("%02d"), (int) round(wind_speed_30));
#if LCD == JLX12864G
        lcd.setfont(3);
        lcd.setpos(36, 5);
        lcd.print(status_buf[1]);
        lcd.setfont(1);
        lcd.setpos(0, 32);
        a = pressure_comp/1e2, r = pressure_comp - a*1e2;
        a = snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%d.%02d"), a, r);
        lcd.print(status_buf[2]);


#else
        lcd.setfont(2);
        lcd.setpos(29, 45);
        lcd.print(status_buf[1]);

        lcd.setfont(0);
        lcd.setpos(0, 61);
        a = pressure_comp/1e2, r = pressure_comp - a*1e2;
        a = snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%d"), a);
        lcd.print(status_buf[2]);
        lcd.rectangle(a*7+3, 73, a*7+4, 73, 255); // draw decimal
        snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%02d"), r);
        lcd.setpos(a*7+6, 61);
        lcd.print(status_buf[2]);
#endif
        char unit = 'C';
        int32_t temp = temperature_comp;
        if(eeprom_data.temperature_units) {
            unit = 'F';
            temp = temp*9/5+3200;
        }

        a = temp / 100;
        r = temp - a*100;
        snprintf_P(status_buf[3], sizeof status_buf[3], PSTR("%d.%02d%c"), a, abs(r), unit);
        
        lcd.setfont(0);
#if LCD == JLX12864G
        lcd.setpos(1, 48);
        lcd.print(status_buf[3]);

        lcd.refresh(1);
#else
        lcd.setpos(1, 71);
        lcd.print(status_buf[3]);
#endif
    }

#if LCD == JLX12864G
    lcd.clear(); // clear compass
    const uint8_t xc = 31, yc = 31, r = 31;
#else
    lcd.clear_lines(0, 45); // clear compass
    const uint8_t xc = 24, yc = 22, r = 22;
#endif
    // draw direction dial for wind
    lcd.circle(xc, yc, r, 255);
    if(wind_dir >= 0) {
        float wind_rad = wind_dir/180.0*M_PI;
        int x = r*sin(wind_rad);
        int y = -r*cos(wind_rad);
        for(int s=1; s<3; s++) {
            int xp = s*cos(wind_rad);
            int yp = s*sin(wind_rad);
            lcd.line(xc+xp, yc+yp, xc+x, yc+y, 255);
            lcd.line(xc-xp, yc-yp, xc+x, yc+y, 255);
        }

        // print the heading under the dial
#if LCD == JLX12864G
        lcd.setfont(2);
#else
        lcd.setfont(1);
#endif
        int xp, yp;
        xp = -11.0*sin(wind_rad), yp = 10.0*cos(wind_rad);
        static float nxp = 0, nyp = 0;
        nxp = (xp + 31*nxp)/32;
        nyp = (yp + 31*nyp)/32;
        xp = xc+nxp-12, yp = yc+nyp-8; 
        lcd.rectangle(xp-1, yp+3, xp+22, yp+14, 0); // clear heading text area
        lcd.setpos(xp, yp);
        lcd.print(status_buf[0]);
    }

#if LCD == JLX12864G
    lcd.refresh(0);
#else
    lcd.refresh();
#endif
#endif
}

void draw_barometer_graph()
{
#if LCD
    static uint8_t page;
    uint8_t curpage = digitalRead(5);

    if(page && !curpage) {
        if(++eeprom_data.baro_page == 3)
            eeprom_data.baro_page = 0;
    }
    page = curpage;

    if(!lcd_update)
        return;

    uint16_t time = millis();
    uint16_t dt = time - last_lcd_updatetime;
    if(dt < 1000) // don't update faster
        return;

    last_lcd_updatetime = time;
    
    lcd_update = 0;
    int a, r;
    last_lcd_texttime = time;

    a = pressure_comp/1e2, r = pressure_comp - a*1e2;

#if LCD == JLX12864G
    lcd.clear();
    lcd.setfont(4);
    lcd.setpos(0, 0);
    lcd.print(F("barometer"));
    lcd.setpos(0, 14);
    switch(eeprom_data.baro_page) {
    case 0: lcd.print(F("plot 5m")); break;
    case 1: lcd.print(F("plot 1h")); break;
    case 2: lcd.print(F("plot 1day")); break;
    }
    lcd.setfont(1);
    lcd.setpos(0, 28);
    snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%d.%02d"), a, r);
    lcd.print(status_buf[2]);
#else
    lcd.clear_lines(50, 83); // clear text
    lcd.setfont(4);
    lcd.setpos(0, 50);
    lcd.print(F("baro"));
    lcd.setpos(0, 60);
    switch(eeprom_data.baro_page) {
    case 0: lcd.print(F("plot 5m")); break;
    case 1: lcd.print(F("plot 1h")); break;
    case 2: lcd.print(F("plot 1d")); break;
    }
    lcd.setpos(0, 70);
    a = snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%d"), a);
    lcd.print(status_buf[2]);
    lcd.rectangle(0, a*7+3, 0, a*7+4, 255); // draw decimal
    snprintf_P(status_buf[2], sizeof status_buf[2], PSTR("%02d"), r);
    lcd.setpos(a*7+6, 70);
    lcd.print(status_buf[2]);
#endif

#if LCD == JLX12864G
    lcd.refresh(1);
#endif

#if 0
    a = temperature_comp / 100;
    snprintf_P(status_buf[3], sizeof status_buf[3], PSTR("%dC"), a);
        
    lcd.setfont(0);
    lcd.setpos(0, 60);
    lcd.print(status_buf[3]);
#endif

#if LCD == JLX12864G
    const int my = 64;
    lcd.clear();
#else
    const int my = 50;
    lcd.clear_lines(0, 50);
#endif
    int index = eeprom_data.baro_page;
    int v = 0;
    for(int i=0; i<history_len; i++) {
        int y = my/2 - v;
        if(y >= 0 && y < my)
            lcd.putpixel(history_len-i-1, y, 255);
        int p = baro_history[index].pos - i - 1;
        if(p < 0)
            p += history_len;
        v -= baro_history[index].data[p];
    }
#if LCD == JLX12864G
    lcd.refresh(0);
#else
    lcd.refresh();
#endif
#endif
}

void draw_setting(uint8_t &setting, const char* name, const char* first, const char* second, const char* third=0)
{
#if LCD
    lcd.clear();
    lcd.setfont(4);
    lcd.setpos(0, 0);
    strcpy_P(status_buf[0], PSTR("setting"));
    lcd.print(status_buf[0]);
    
    strcpy_P(status_buf[0], name);
    lcd.setpos(0, 16);
    lcd.print(status_buf[0]);

    strcpy_P(status_buf[0], first);
    lcd.setpos(0, 30);
    lcd.print(status_buf[0]);

    strcpy_P(status_buf[0], second);
    lcd.setpos(0, 44);
    lcd.print(status_buf[0]);

#if LCD == JLX12864G
    int y = 42+setting*14;
    lcd.line(0, y, 60, y, 255);
    lcd.refresh(0);
    lcd.clear();
    lcd.setpos(0, 0);
#else
    lcd.setpos(0, 58);
#endif
    if(third) {
        strcpy_P(status_buf[0], third);
        lcd.print(status_buf[0]);
#if LCD == JLX12864G
        int y = setting*14-14;
        lcd.line(0, y, 60, y, 255);
#endif
    }

#if LCD == JLX12864G
    lcd.refresh(1);
#else
    int y = 42+setting*14;
    lcd.line(0, y, 44, y, 255);
    lcd.refresh();
#endif
    
    uint8_t cursetting_key = digitalRead(5);
    static uint8_t setting_key;

    if(setting_key && !cursetting_key) {
        setting++;
        int max = third ? 3 : 2;
        if(setting >= max)
            setting = 0;
        apply_settings();
    }
    setting_key = cursetting_key;
#endif
}

void loop()
{
    set_sleep_mode(SLEEP_MODE_IDLE);
    sleep_enable();
    sleep_cpu();
    wdt_reset();

    read_pressure_temperature();
#ifdef LCD
    read_light();
#endif
    read_anemometer();
    switch(eeprom_data.display_page) {
    case 0:
        draw_anemometer();
        break;
    case 1:
        draw_barometer_graph();
        break;
    case 2:
        draw_setting(eeprom_data.sensor_type, PSTR("sensor"), PSTR("pypilot"), PSTR("davis"));
        break;
    case 3:
        draw_setting(eeprom_data.display_orientation, PSTR("display"), PSTR("normal"), PSTR("flip"));
        break;
    case 4:
        draw_setting(eeprom_data.backlight_setting, PSTR("backlight"), PSTR("off"), PSTR("on"), PSTR("auto"));
        break;
    case 5:
        draw_setting(eeprom_data.direction_type, PSTR("direction"), PSTR("+-180"), PSTR("0-360"));
        break;
    case 6:
        draw_setting(eeprom_data.temperature_units, PSTR("temperature"), PSTR("celcius"), PSTR("farenheight"));
        break;
    case 7:
        draw_setting(eeprom_data.leds_on, PSTR("leds"), PSTR("off"), PSTR("on"));
        break;
    }

    static uint8_t page;
    uint8_t curpage = digitalRead(6);

    if(page && !curpage) {
        if(++eeprom_data.display_page == 8) {
            eeprom_data.display_page = 0;
        }
        eeprom_write_timeout = millis();
    }
    page = curpage;

    if(eeprom_write_timeout) {
        uint32_t t = millis();
        if(t-eeprom_write_timeout > 10000) {
            if(eeprom_data.display_page > 1)
                eeprom_data.display_page = 0;
            eeprom_update_block(&eeprom_data, 0, sizeof eeprom_data);
            eeprom_write_timeout = 0;
        }
    }
}
