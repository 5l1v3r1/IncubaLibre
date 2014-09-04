/*
* ----------------------------------------------------------------------------
* "THE BEER-WARE LICENSE" (Revision 42):
* <fakufaku@gmail.com> wrote this file. As long as you retain this notice you
* can do whatever you want with this stuff. If we meet some day, and you think
* this stuff is worth it, you can buy me a beer in return -- Robin Scheibler
* ----------------------------------------------------------------------------
*/

/*
 * IncubaLibre
 * ===========
 *
 * This is a cheap controller to regulate temperature in closed environment
 * using a consumer heating element such as a light bulb or a blow dryer.
 *
 * Control Loop Flow
 * -----------------
 *
 * The code uses TIMER1 as a slow PWM to switch a relay. 
 * The duty cycle of the relay is controlled by a PID loop.
 * The temperature is measured using a thermistor.
 *
 * The system clock of the ATtiny85 is left to the default 1MHz
 * and the prescaler of TIMER1 is set to clk/16834, which means
 * the loop period is ~4 seconds.
 *
 * Counter value ->
 * +---------------+------------------------------------------+
 * 0x0             OCR1A                                   0xFF
 * |               |                                          |
 * | Relay on      | Relay off (only when PWM on)             |
 * |               |                                          |
 * |               | On compare match interrupt               |
 * |               |   - measure trimpot                      |
 * |               |     - if trimpot != 0, start pwm         |
 * |               |     - else stop pwm                      |
 * |               |   - measure temperature                  |
 * |               |     - if incubator running recompute PID |
 * +---------------+------------------------------------------+
 *
 * Thermistor
 * ----------
 *
 * The thermistor was calibrated using a thermocouple and some hot water.
 * A look up table indexed on the ADC value is stored in the flash
 * memory.
 *
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <math.h>

// Allows to disable interrupt in some parts of the code
#define ENTER_CRIT()    {char volatile saved_sreg = SREG; cli()
#define LEAVE_CRIT()    SREG = saved_sreg;}

// Time interval is 1MHz/16384/256
#define TIME_INTERVAL 4.194304
#define TIME_INTERVAL_INV 0.2384186

// the state variable
#define PWM_ON 0
#define PWM_OFF 1
static uint8_t state = PWM_OFF;

// error when we consider set point is reached
#define TEMP_SET_ERROR 1.0

// measurements
uint8_t trimpot_val = 0;
float temperature_avg = 0.;
uint8_t temp_avg_n = 0;

// Temperature set point
float temperature_setPoints[4] = { 0.0, 37.5, 40.0, 45.0 };

// PID constants
float K_p = 10.;
float K_i = 0.5;
float K_d = 0.0;

// PID values
float error_previous = 0.;
float ITerm = 0.0;

// We limit PWM value to 4 from below to spare the relay
// and to 128 from above to avoid overheating of the incubator
#define PWM_MAX 200
#define PWM_MIN 6
uint8_t pwm_val = 0;

// Relay and LEDs macros
#define RELAY_ON()  PORTB &= ~(1 << PB1)
#define RELAY_OFF() PORTB |= (1 << PB1)
#define LED1_ON()   PORTB |= (1 << PB0)
#define LED1_OFF()  PORTB &= ~(1 << PB0)
#define LED2_ON()   PORTB |= (1 << PB4)
#define LED2_OFF()  PORTB &= ~(1 << PB4)

#define START_PWM_1A() TCCR1 |= (1 << PWM1A) | (1 << COM1A1) | (1 << COM1A0)
#define STOP_PWM_1A()  TCCR1 &= ~((1 << PWM1A) | (1 << COM1A1) | (1 << COM1A0))

// Look-up table of Thermistor values
const float therm_lut[] PROGMEM = { -66.89, -53.98, -47.36, -42.76, -39.19, -36.24, -33.71, -31.49, -29.51, -27.72, 
  -26.07, -24.55, -23.14, -21.81, -20.56, -19.37, -18.25, -17.17, -16.15, -15.17, 
  -14.22, -13.31, -12.43, -11.58, -10.76, -9.96, -9.18, -8.43, -7.69, -6.97, 
  -6.27, -5.59, -4.92, -4.26, -3.62, -2.99, -2.37, -1.77, -1.17, -0.59, 
  -0.01, 0.56, 1.12, 1.67, 2.21, 2.75, 3.27, 3.80, 4.31, 4.82, 
  5.32, 5.82, 6.31, 6.80, 7.28, 7.76, 8.23, 8.70, 9.16, 9.62, 
  10.08, 10.53, 10.98, 11.42, 11.86, 12.30, 12.74, 13.17, 13.60, 14.03, 
  14.45, 14.88, 15.30, 15.71, 16.13, 16.54, 16.95, 17.36, 17.77, 18.18, 
  18.58, 18.98, 19.38, 19.78, 20.18, 20.58, 20.98, 21.37, 21.76, 22.16, 
  22.55, 22.94, 23.33, 23.72, 24.11, 24.49, 24.88, 25.27, 25.65, 26.04, 
  26.42, 26.81, 27.19, 27.58, 27.96, 28.35, 28.73, 29.11, 29.50, 29.88, 
  30.26, 30.65, 31.03, 31.42, 31.80, 32.19, 32.57, 32.96, 33.34, 33.73, 
  34.12, 34.51, 34.89, 35.28, 35.67, 36.06, 36.46, 36.85, 37.24, 37.64, 
  38.03, 38.43, 38.83, 39.23, 39.63, 40.03, 40.44, 40.84, 41.25, 41.66, 
  42.07, 42.48, 42.89, 43.31, 43.72, 44.14, 44.56, 44.99, 45.41, 45.84, 
  46.27, 46.70, 47.14, 47.57, 48.01, 48.46, 48.90, 49.35, 49.80, 50.26, 
  50.71, 51.17, 51.64, 52.11, 52.58, 53.05, 53.53, 54.01, 54.50, 54.99, 
  55.49, 55.99, 56.49, 57.00, 57.51, 58.03, 58.55, 59.08, 59.62, 60.16, 
  60.70, 61.26, 61.82, 62.38, 62.95, 63.53, 64.12, 64.71, 65.31, 65.92, 
  66.54, 67.16, 67.79, 68.44, 69.09, 69.75, 70.43, 71.11, 71.80, 72.51, 
  73.23, 73.96, 74.70, 75.46, 76.23, 77.02, 77.82, 78.64, 79.47, 80.32, 
  81.20, 82.09, 83.00, 83.93, 84.89, 85.87, 86.87, 87.90, 88.96, 90.06, 
  91.18, 92.34, 93.53, 94.76, 96.04, 97.35, 98.72, 100.14, 101.61, 103.14, 
  104.74, 106.41, 108.16, 110.00, 111.92, 113.95, 116.10, 118.38, 120.80, 123.39, 
  126.16, 129.15, 132.39, 135.93, 139.81, 144.12, 148.94, 154.41, 160.71, 168.12, 
  177.05, 188.24, 202.99, 224.20, 260.13, 630.92 };

void measure_temperature()
{
  /* Measure Temperature */

  // select ADC3 single-ended channel
  // Left adjust to have 8 upper bits in the upper register
  ADMUX = (1 << MUX1) | (1 << MUX0) | (1 << ADLAR);

  // start conversion
  ADCSRA |= (1 << ADSC);

  // wait for conversion to finish
  while (ADCSRA & (1 << ADSC))
    ;

  // We only use the 8 upper bit of the ADC value
  // for the temperature
  uint8_t high = ADCH;

  // average the temperature measurement
  temperature_avg = pgm_read_float(&(therm_lut[high]));
  //temperature_avg = 0.8*temperature_avg + 0.2*pgm_read_float(&(therm_lut[high]));
}

void measure_trimpot()
{
  /* Measure the trimpot value */
  
  // select ADC1 single-ended channel
  // Left adjust to have 8 upper bits in the upper register
  ADMUX = (1 << MUX0) | (1 << ADLAR);

  // start conversion
  ADCSRA |= (1 << ADSC);

  // wait for conversion to finish
  while (ADCSRA & (1 << ADSC))
    ;

  // We only use few upper bit of the ADC value
  // for the trimpot value
  uint8_t high = ADCH;
  trimpot_val = high >> 6;
}

void PID_compute()
{
  /*Compute all the working error variables*/
  float error = 37.5 - temperature_avg;

  // turn the LED2 on if we are close to set temperature
  if (fabs(error) < TEMP_SET_ERROR)
    LED2_ON();
  else
    LED2_OFF();

  // integral term (using trapeze method)
  ITerm += (K_i * TIME_INTERVAL * (error + error_previous)*0.5);
  if (ITerm > PWM_MAX) 
    ITerm = PWM_MAX;
  else if (ITerm < PWM_MIN) 
  {
    ITerm = PWM_MIN;
  }

  // differential term
  float dInput = (error - error_previous) * TIME_INTERVAL_INV;

  /*Compute PID Output*/
  float output = K_p * error + ITerm + K_d * dInput;

  // set PWM value
  if (output > PWM_MAX)
    pwm_val = PWM_MAX;
  else if (output < PWM_MIN) 
    pwm_val = 0.;
  else
    pwm_val = (uint8_t)output;

  /*Remember some variables for next time*/
  error_previous = error;
}

// Here we turn the relay off
SIGNAL(TIMER1_COMPA_vect)
{
  measure_trimpot();
  measure_temperature();
  
  if (trimpot_val > 0)
  {
    if (state == PWM_OFF)
    {
      START_PWM_1A();
      state = PWM_ON;

      LED1_ON();
    }

    // do the PID magic
    PID_compute();
  }
  else
  {
    if (state == PWM_ON)
    {
      STOP_PWM_1A();
      state = PWM_OFF;

      RELAY_OFF();

      LED1_OFF();
      pwm_val = 0;
      ITerm = 0.0;
      error_previous = 0.0;
    }
  }
}

// The timer overflow interrupt routine
SIGNAL(TIMER1_OVF_vect)
{
  // set pwm duty cycle to value computed by PID
  OCR1A = pwm_val;
}

int main()
{
  int i;

  // enable interrupts
  sei();

  // ADC setting, enabled, prescaled clk/16
  ADCSRA = (1 << ADEN) | (1 << ADPS2);

  // set up timer 1 as system clock
  TCNT1 = 0x0;            // counter at zero
  TIMSK |= (1 << TOIE1) | (1 << OCIE1A);   // set the overflow interrupt
  TCCR1 = (1 << CS13) | (1 << CS12) | (1 << CS11) | (1 << CS10); // T1 clock to clk/16384

  // configure PB1 to switch relay
  // configure PB0 and PB4 for the two LEDs
  DDRB = (1 << PB1) | (1 << PB0) | (1 << PB4);
  RELAY_OFF();
  LED1_OFF();
  LED2_OFF();

  // The infinite loop
  while (1)
  {

  }

}
