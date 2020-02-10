#include <stdio.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>

#include "main.h"
#include "EEPROM.h"
#include "Trinamic_TMC2130.h"
#include "MCP23S17.h"
#include "config.h"
#include "Countimer.h"
#include "thermistor.h"
#include "version.h"
#include "USBCore.h"
#include "MenuList.h"
#include "Selftest.h"
#include "SpeedControl.h"
#include "i18n.h"

using Ter = LiquidCrystal_Prusa::Terminator;

Countimer tDown;
Countimer tUp;

CSelftest selftest;
CSpeedControl speed_control;

thermistor therm1(A4, 5);

Trinamic_TMC2130 myStepper(CS_PIN);

MCP outputchip(0, 8);

LiquidCrystal_Prusa lcd(LCD_PINS_RS, LCD_PINS_ENABLE, LCD_PINS_D4, LCD_PINS_D5, LCD_PINS_D6, LCD_PINS_D7);

enum menu_state : uint8_t {
	HOME,
	SPEED_STATE,
	SPEED_CURING,
	SPEED_WASHING,
	TIME,
	TIME_CURING,
	TIME_DRYING,
	TIME_WASHING,
	TIME_RESIN_PREHEAT,
	SETTINGS,
	PREHEAT,
	TARGET_TEMP,
	RESIN_TARGET_TEMP,
	RUN_MODE,
	SOUND_SETTINGS,
	FANS,
	LED_INTENSITY,
	FAN1_CURING,
	FAN1_DRYING,
	FAN2_CURING,
	FAN2_DRYING,
	RUNNING,
	RUN_MENU,
	BEEP,
	INFO,
	CONFIRM,
	ERROR,
	SELFTEST
};

enum units : uint8_t {
	XOFTEN,
	MINUTES,
	PERCENT,
	TEMPERATURE_C,
	TEMPERATURE_F,
};

//! @brief legacy configuration store structure
//!
//! It is restored when magic read from eeprom equals magic "CURWA".
//! Do not change.
typedef struct {
	byte washing_speed;
	byte curing_speed;
	byte washing_run_time;
	byte curing_run_time;
	byte finish_beep_mode;
	byte drying_run_time;
	byte sound_response;
	byte curing_machine_mode;
	byte heat_to_target_temp;
	byte target_temp_celsius;
	byte target_temp_fahrenheit;
	byte SI_unit_system;
	bool heater_failure;
} eeprom_v1_t;

//! @brief configuration store structure
//!
//! It is restored when magic read from eeprom equals magic "CW1v2"
//! Do not change. If new items needs to be stored, magic needs to be
//! changed, this struct needs to be made legacy and new structure needs
//! to be created.
typedef struct {
	byte washing_speed;
	byte curing_speed;
	byte washing_run_time;
	byte curing_run_time;
	byte finish_beep_mode;
	byte drying_run_time;
	byte sound_response;
	byte curing_machine_mode;
	byte heat_to_target_temp;
	byte target_temp;
	byte resin_target_temp;		// v1 change!
	byte SI_unit_system;
	bool heater_failure;

	byte resin_preheat_run_time;
	byte led_pwm_value;
	fans_duty_t fans_curing_speed;
	fans_duty_t fans_drying_speed;
	fans_duty_t fans_preheat_speed;
} eeprom_v2_t;

#define EEPROM_OFFSET	128
#define MAGIC_SIZE		6
#define EEPROM_BASE		E2END + 1 - EEPROM_OFFSET
static_assert(sizeof(eeprom_v2_t) <= EEPROM_OFFSET, "eeprom_t doesn't fit in it's reserved space in the memory.");

//! @brief configuration
//!
//! Default values definition,
//! it can be overridden by user and stored to
//! and restored from permanent storage.

eeprom_v2_t config = {
	10,			// washing_speed
	1,			// curing_speed
	4,			// washing_run_time
	3,			// curing_run_time
	1,			// finish_beep_mode
	3,			// drying_run_time
	1,			// sound_response
	0,			// curing_machine_mode
	0,			// heat_to_target_temp
	35,			// target_temp (celsius)
	30,			// resin_target_temp (celsius)
	1,			// SI_unit_system
	false,		// heater_failure

	3,			// resin_preheat_run_time
	100,		// led_pwm_value
	{60, 70},	// fans_curing_speed
	{60, 70},	// fans_drying_speed
	{40, 40},	// fans_preheat_speed
};

fans_duty_t fan_duty;

typedef char Serial_num_t[20]; //!< Null terminated string for serial number
static const char* pgmstr_serial_number = reinterpret_cast<const char*>(0x7fe0); //!< 15 characters

volatile uint16_t* const bootKeyPtr = (volatile uint16_t *)(RAMEND - 1);
static volatile uint16_t bootKeyPtrVal __attribute__ ((section (".noinit")));

uint8_t Back[8] = {
	B00100,
	B01110,
	B11111,
	B00100,
	B11100,
	B00000,
	B00000,
	B00000
};

uint8_t Right[8] = {
	B00000,
	B00100,
	B00010,
	B11111,
	B00010,
	B00100,
	B00000,
	B00000
};

uint8_t Backslash[8] = {
	B00000,
	B10000,
	B01000,
	B00100,
	B00010,
	B00001,
	B00000,
	B00000
};

// constants won't change:
const float period = (1 / FAN_FREQUENCY) * 1000;
const uint8_t max_preheat_run_time = 30;

volatile uint8_t rotary_diff = 128;

fans_duty_t fans_menu_speed = {30, 30};		// 0-100 %
fans_duty_t fans_washing_speed = {60, 70};	// 0-100 %

bool redraw_menu = true;
bool redraw_ms = true;
bool mode_flag = true;	//helping var for selftesting

char config_magic[MAGIC_SIZE] = "CW1v2";

menu_state state = HOME;

int fan_tacho_count[3];
int fan_tacho_last_count[3];

float chamber_temp;

int ams_fan_counter;

uint8_t menu_position = 0;
uint8_t last_menu_position = 0;
uint8_t max_menu_position = 0;
uint8_t last_seconds = 0;

unsigned long time_now = 0;
unsigned long therm_read_time_now = 0;

unsigned long fan1_previous_millis = 0;
unsigned long fan2_previous_millis = 0;

unsigned long us_last = 0;
unsigned long led_time_now = 0;

unsigned long button_timer = 0;

long remain = 0;

bool button_released = false;
bool fan1_pwm_high = false;
bool fan2_pwm_high = false;

bool fan1_on = false;
bool fan2_on = false;

bool heater_error = false;
bool fan_error[2] = {false, false};

bool heater_running = false;
bool curing_mode = false;
bool drying_mode = false;
bool last_curing_mode = false;
bool paused = false;
bool cover_open = false;
bool gastro_pan = false;
bool paused_time = false;
bool led_start = false;

bool button_active = false;
bool long_press_active = false;
bool long_press = false;
bool preheat_complete = false;
bool pid_mode = false;

static void motor_configuration();
static void read_config();
static void fan_tacho1();
static void fan_tacho2();
static void fan_tacho3();
static void menu_move(bool sound_echo);
static void machine_running();
static void button_press();
static void tDownComplete();
static void start_drying();
static void stop_curing_drying();
static void start_curing();
static void start_washing();
static void tUpComplete();
static void fan_pwm_control();
static void fan_rpm();
static void preheat();
static void lcd_time_print();
static void therm1_read();
static float celsius2fahrenheit(float);
static float fahrenheit2celsius(float);

static inline bool is_error() {
	return (fan_error[0] || fan_error[1] || config.heater_failure);
}

//timmer for fan pwm
void setupTimer0() {
	noInterrupts();
	OCR0A = 0xAF;
	TIMSK0 |= _BV(OCIE0A);
	interrupts();
}

void setupTimer4() {
	noInterrupts();
	// Clear registers
	TCCR4A = 0;
	TCCR4B = 0;
	TCNT4 = 0;

	// 100.16025641025641 Hz (16000000/((155+1)*1024))
	OCR4A = 155;
	// CTC
	TCCR4A |= (1 << WGM41);
	// Prescaler 1024
	TCCR4B |= (1 << CS42) | (1 << CS41) | (1 << CS40);
	// Output Compare Match A Interrupt Enable
	TIMSK4 |= (1 << OCIE4A);
	interrupts();
}

//timmer for stepper move
void setupTimer3() {
	noInterrupts();
	// Clear registers
	TCCR3A = 0;
	TCCR3B = 0;
	TCNT3 = 0;
	// 1 Hz (16000000/((15624+1)*1024))
	OCR3A = 200; // 15-50
	// CTC
	TCCR3B |= (1 << WGM32);
	// Prescaler 1024
	TCCR3B |= (1 << CS31) | (1 << CS30);
	// Output Compare Match A Interrupt Enable
	TIMSK3 |= (1 << OCIE3A);
	interrupts();
}

void enable_timer3() {
	// Output Compare Match A Interrupt Enable
	TIMSK3 |= (1 << OCIE3A);
}

void disable_timer3() {
	// Interrupt Disable
	TIMSK3 = 0;
}

void run_motor() {
	outputchip.digitalWrite(EN_PIN, LOW); // enable driver
	speed_control.motor_running = true;
}

void stop_motor() {
	outputchip.digitalWrite(EN_PIN, HIGH); // disable driver
	speed_control.motor_running = false;
}

void run_heater() {
	outputchip.digitalWrite(FAN_HEAT_PIN, HIGH); // enable driver
	heater_running = true;
	wdt_enable(WDTO_4S);
}

void stop_heater() {
	outputchip.digitalWrite(FAN_HEAT_PIN, LOW); // disable driver
	heater_running = false;
	wdt_disable();
}

void motor_configuration() {

	if (curing_mode == true) {
		myStepper.set_IHOLD_IRUN(10, 10, 0);
		myStepper.set_mres(256);
	} else {
		myStepper.set_IHOLD_IRUN(31, 31, 5);
		myStepper.set_mres(16);
	}
}

void run_stop() {
	menu_position = 0;
	pid_mode = false;
	state = HOME;
	stop_motor();
	paused = false;
	cover_open = false;

	outputchip.digitalWrite(EN_PIN, HIGH); // disable driver
	stop_heater(); // turn off heat fan
	fan_duty = fans_menu_speed;
	outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn off led
	digitalWrite(LED_PWM_PIN, LOW);
	tDown.stop();
	tUp.stop();
}

void setup() {

	outputchip.begin();
	outputchip.pinMode(0B0000000010010111);
	outputchip.pullupMode(0B0000000010000011);
	read_config();

	outputchip.digitalWrite(EN_PIN, HIGH); // disable driver

	// buttons
	pinMode(BTN_EN1, INPUT_PULLUP);
	pinMode(BTN_EN2, INPUT_PULLUP);

	pinMode(DIR_PIN, OUTPUT);
	pinMode(STEP_PIN, OUTPUT);

	pinMode(LCD_PWM_PIN, OUTPUT);
	digitalWrite(LCD_PWM_PIN, HIGH);
	pinMode(BEEPER, OUTPUT);

	pinMode(FAN1_PWM_PIN, OUTPUT);
	pinMode(FAN2_PWM_PIN, OUTPUT);

	pinMode(1, INPUT_PULLUP);
	pinMode(2, INPUT_PULLUP);
	pinMode(0, INPUT_PULLUP);

	attachInterrupt(2, fan_tacho1, RISING);
	attachInterrupt(1, fan_tacho2, RISING);
	attachInterrupt(3, fan_tacho3, RISING);

	fan_duty = fans_menu_speed;

	outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn off led
	pinMode(LED_PWM_PIN, OUTPUT);
	digitalWrite(LED_PWM_PIN, LOW);

	// stepper driver init
	myStepper.init();
	myStepper.set_mres(16); // ({1,2,4,8,16,32,64,128,256}) number of microsteps
	myStepper.set_IHOLD_IRUN(10, 10, 0); // ([0-31],[0-31],[0-5]) sets all currents to maximum
	myStepper.set_I_scale_analog(0); // ({0,1}) 0: I_REF internal, 1: sets I_REF to AIN
	myStepper.set_tbl(1); // ([0-3]) set comparator blank time to 16, 24, 36 or 54 clocks, 1 or 2 is recommended
	myStepper.set_toff(8); // ([0-15]) 0: driver disable, 1: use only with TBL>2, 2-15: off time setting during slow decay phase
	myStepper.set_en_pwm_mode(1);// 0: driver disable PWM mode, 1: driver enable PWM mode
	// get ready

	setupTimer3();
	//setupTimer1();
	setupTimer0();
	//pinMode(FAN_HEAT_PIN, OUTPUT);
	stop_heater();

	lcd.createChar(0, Back);
	lcd.createChar(1, Right);
	lcd.createChar(2, Backslash);
	redraw_menu = true;
	menu_move(true);
}

void write_config() {
	config.washing_speed = speed_control.washing_speed;
	config.curing_speed = speed_control.curing_speed;
	EEPROM.put(EEPROM_BASE, reinterpret_cast<uint8_t*>(config_magic), MAGIC_SIZE);
	EEPROM.put(EEPROM_BASE + MAGIC_SIZE, reinterpret_cast<uint8_t*>(&config), sizeof(config));
}

/*! \brief This function loads user-defined values from eeprom.
 *
 *	It loads different amount of variables, depending on the magic variable from eeprom.
 *	If magic is not set in the eeprom, variables keep their default values.
 *	If magic from eeprom is equal to lagacy magic, it loads only variables customizable in older firmware and keeps new variables default.
 *	If magic from eeprom is equal to config_magic, it loads all variables including those added in new firmware.
 *	It won't load undefined (new) variables after flashing new firmware.
 */
void read_config() {
	char test_magic[MAGIC_SIZE];
	EEPROM.get(EEPROM_BASE, reinterpret_cast<uint8_t*>(test_magic), MAGIC_SIZE);
	if (!strncmp(config_magic, test_magic, MAGIC_SIZE)) {
		// latest magic
		EEPROM.get(EEPROM_BASE + MAGIC_SIZE, reinterpret_cast<uint8_t*>(&config), sizeof(config));
	} else if (!strncmp("CURWA", test_magic, MAGIC_SIZE)) {
		// legacy magic
		uint8_t tmp = config.resin_target_temp;	// remember default
		EEPROM.get(EEPROM_BASE + MAGIC_SIZE, reinterpret_cast<uint8_t*>(&config), sizeof(eeprom_v1_t));
		if (config.SI_unit_system) {
			config.resin_target_temp = tmp;
		} else {
			config.target_temp = round(celsius2fahrenheit(config.target_temp));
			config.resin_target_temp = round(celsius2fahrenheit(tmp));
		}
	}
	speed_control.washing_speed = config.washing_speed;
	speed_control.curing_speed = config.curing_speed;
}

uint8_t PI_regulator(float & actualTemp, uint8_t targetTemp) {
	static double summErr = 0;
	double errValue = actualTemp - targetTemp;
	summErr += errValue;

	if ((summErr > 10000) || (summErr < -10000)) {
		summErr = 10000;
	}

	double newSpeed = P * errValue + I * summErr;
	if (newSpeed > 100) {
		newSpeed = 100;
	}

	return newSpeed;
}

void print_menu_cursor(uint8_t line) {
	for (uint8_t i = 0; i < 4; ++i) {
		lcd.setCursor(0, i);
		if (i == line) {
			lcd.write('>');
		} else {
			lcd.write(' ');
		}
	}
}

void generic_menu_P(uint8_t num, ...) {
	va_list argList;
	va_start(argList, num);
	max_menu_position = 0;
	for (; num; num--) {
		lcd.setCursor(1, max_menu_position++);
		lcd.printClear_P(va_arg(argList, const char *), 18, Ter::none);
	}
	va_end(argList);
	max_menu_position--;

	if (rotary_diff > 128) {
		if (menu_position < max_menu_position) {
			menu_position++;
		}
	} else if (rotary_diff < 128) {
		if (menu_position) {
			menu_position--;
		}
	}
	print_menu_cursor(menu_position);
}

void lcd_print_back() {
	lcd.setCursor(19, 0);
	lcd.write(uint8_t(0));
}

void lcd_print_right(uint8_t a) {
	lcd.setCursor(19, a);
	lcd.write(uint8_t(1));
}

void lcd_clear_time_boundaries() {
	lcd.print_P(pgmstr_double_space, LAYOUT_TIME_GT, LAYOUT_TIME_Y);
	lcd.print_P(pgmstr_double_space, LAYOUT_TIME_LT, LAYOUT_TIME_Y);
}

void generic_value_P(const char *label, uint8_t *value, uint8_t min, uint8_t max, units units) {
	if (rotary_diff > 128 && *value < max)
		(*value)++;
	else if (rotary_diff < 128 && *value > min)
		(*value)--;
// TODO
//	else
//		return;

	lcd.setCursor(1, 0);
	lcd.printClear_P(label, 19, Ter::none);

	lcd.print(*value, 5, 2);
	switch (units) {

		case XOFTEN:
			lcd.print_P(pgmstr_xoften);
			break;

		case MINUTES:
			lcd.print_P(pgmstr_minutes);
			break;

		case PERCENT:
			lcd.print_P(pgmstr_percent);
			break;

		case TEMPERATURE_C:
			lcd.print_P(pgmstr_celsius);
			break;

		case TEMPERATURE_F:
			lcd.print_P(pgmstr_fahrenheit);
			break;

		default:
			break;
	}
}

void generic_items_P(const char *label, uint8_t *value, uint8_t num, ...) {
	lcd.setCursor(1, 0);
	lcd.printClear_P(label, 19, Ter::none);
	const char *items[num];
	if (*value > num) {
		*value = 0;
	}

	va_list argList;
	va_start(argList, num);
	uint8_t i = 0;
	for (; num; num--) {
		items[i++] = va_arg(argList, const char *);
	}
	va_end(argList);

	if (rotary_diff > 128) {
		if (*value < i - 1) {
			(*value)++;
		}
	} else if (rotary_diff < 128) {
		if (*value) {
			(*value)--;
		}
	}

	if (*value < i) {
		lcd.setCursor(0, 2);
		lcd.printClear_P(pgmstr_emptystr, 20, Ter::none);
		uint8_t len = strlen_P(items[*value]);
		if (*value) {
			len += 2;
		}
		if (*value < i - 1) {
			len += 2;
		}
		lcd.setCursor((20 - len) / 2, 2);
		if (*value) {
			lcd.print_P(pgmstr_lt);
		}
		lcd.print_P(items[*value]);
		if (*value < i - 1) {
			lcd.print_P(pgmstr_gt);
		}
	}
}

void echo(void) {
	for (uint8_t i = 0; i < 10; ++i) {
		digitalWrite(BEEPER, HIGH);
		delayMicroseconds(100);
		digitalWrite(BEEPER, LOW);
		delayMicroseconds(100);
	}
}

void beep(void) {
	analogWrite(BEEPER, 220);
	delay(50);
	digitalWrite(BEEPER, LOW);
	delay(250);
	analogWrite(BEEPER, 220);
	delay(50);
	digitalWrite(BEEPER, LOW);
}

void warning_beep(void) {
	analogWrite(BEEPER, 220);
	delay(50);
	digitalWrite(BEEPER, LOW);
	delay(250);
}

void redraw_selftest_vals() {
	if (selftest.phase == 3 && selftest.vent_test != true) {
		lcd.print(selftest.fan_tacho[0], 7, 1);
		lcd.print(selftest.fan_tacho[1], 7, 2);
	}
	if (selftest.phase == 5 && selftest.heater_test != true) {
		lcd.print(chamber_temp, 5, 1);
		lcd.print_P(config.SI_unit_system ? pgmstr_celsius : pgmstr_fahrenheit);
	}
	if (selftest.phase == 6 && selftest.rotation_test != true) {
		lcd.print((uint8_t)mode_flag, 12, 1);
		lcd.setCursor(14,1);
		if (mode_flag) {
			if (speed_control.curing_speed <= 11)
				lcd.print((uint8_t)(speed_control.curing_speed - 1));
		} else {
			if (speed_control.washing_speed <= 11)
				lcd.print(uint8_t(speed_control.washing_speed - 1));
		}
	}
	if (selftest.phase == 3 || selftest.phase == 4 || selftest.phase == 5) {
		uint8_t lcd_min = selftest.tCountDown.getCurrentMinutes();
		uint8_t lcd_sec = selftest.tCountDown.getCurrentSeconds();
		lcd.printTime(lcd_min, lcd_sec, 7, 3);
	}
}

void loop() {
	if (*bootKeyPtr != MAGIC_KEY) {
		wdt_reset();
	}
	tDown.run();
	tUp.run();

	if (state == SELFTEST) {

		selftest.tCountDown.run();
		static unsigned long ms_last_count = millis();

		if ((millis() - ms_last_count) >= 1000) {
			ms_last_count = millis();
			redraw_menu = true;
		}

		switch (selftest.phase) {
			case 1:
				selftest.measured_state = outputchip.digitalRead(COVER_OPEN_PIN) == HIGH;
				redraw_menu = selftest.universal_pin_test();
				break;
			case 2:
				selftest.measured_state = outputchip.digitalRead(WASH_DETECT_PIN) == HIGH;
				redraw_menu = selftest.universal_pin_test();
				break;
			case 3:
				selftest.ventilation_test(fan_error[0], fan_error[1]);
				fan_duty = selftest.fans_speed;
				break;
			case 4:
				selftest.cover_down = outputchip.digitalRead(COVER_OPEN_PIN) == LOW;
				if (selftest.cover_down) {
					if (selftest.is_first_loop()) {
						outputchip.digitalWrite(LED_RELE_PIN, HIGH); // turn LED on
						analogWrite(LED_PWM_PIN, map(config.led_pwm_value, 0, 100, 0, 255));
					}
					if (outputchip.digitalRead(LED_RELE_PIN) == HIGH) {
						if (selftest.led_test == false) {
							selftest.LED_test();
						} else {
							outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn LED off
							digitalWrite(LED_PWM_PIN, LOW);
						}
					} else {
						if (selftest.isCounterRunning) {
							selftest.fail_flag = true;
							selftest.tCountDown.stop();
							selftest.isCounterRunning = false;
							selftest.led_test = true;
							digitalWrite(LED_PWM_PIN, LOW);
						}
					}
				} else {
					if (selftest.isCounterRunning)
						selftest.tCountDown.pause();
				}
				break;

			case 5:
				if (!selftest.heater_test) {
					if (outputchip.digitalRead(WASH_DETECT_PIN) == HIGH) {
						selftest.fans_speed.fan1 = 10;
						if (outputchip.digitalRead(COVER_OPEN_PIN) == LOW) {
							selftest.fans_speed.fan2 = 10;
							if (selftest.is_first_loop()) {
								pid_mode = true;
								run_heater();
								fan_duty = fans_menu_speed;
							}
							selftest.heat_test(heater_error);
						} else {
							selftest.fans_speed.fan2 = 0;
							if (selftest.isCounterRunning)
								selftest.heat_test(heater_error);
						}
					} else {
						selftest.fans_speed.fan1 = 0;
					}
				} else if (heater_running) {
					stop_heater();
					fan_duty = fans_menu_speed;
					pid_mode = false;
				}
				break;

			case 6:
				if (!selftest.rotation_test && selftest.motor_rotation_timer()) {
					if (selftest.is_first_loop()) {
						if (mode_flag) {
							speed_control.curing_speed = 1;
							myStepper.set_IHOLD_IRUN(10, 10, 0);			//motor_configuration for curing mode;
							myStepper.set_mres(256);
						} else {
							speed_control.washing_speed = 1;
							myStepper.set_IHOLD_IRUN(31, 31, 5);			//motor_configuration for washing mode
							myStepper.set_mres(16);
						}
						speed_control.speed_configuration(mode_flag);
						run_motor();
						selftest.set_first_loop(false);
					} else {
						if (speed_control.curing_speed <= 10 && speed_control.washing_speed <= 10) {
							if (!mode_flag) {
								uint8_t backup = speed_control.microstep_control;	 //needed for smooth gear-up of the motor
								speed_control.speed_configuration(mode_flag);
								speed_control.microstep_control = backup;
							} else {
								speed_control.speed_configuration(mode_flag);
							}
						}
					}

					if (mode_flag)
						speed_control.curing_speed++;
					else
						speed_control.washing_speed++;

					if (mode_flag && speed_control.curing_speed > 11) {
						stop_motor();
						selftest.clean_up();
						speed_control.curing_speed = 1;	 //default value
						mode_flag = false;
					}
					if (!mode_flag && speed_control.washing_speed > 11) {
						stop_motor();
						speed_control.washing_speed = 10; //default value
						selftest.rotation_test = true;
					}
				}
				break;

			default:
				break;
			}
	}

	if (heater_error) {
		if (config.heat_to_target_temp) {
			tDown.stop();
		} else {
			tUp.stop();
		}
		stop_heater(); // turn off heater and fan
		stop_motor(); // turn off motor
		fan_duty = fans_menu_speed;
	}

	if (state == HOME) {
		if (outputchip.digitalRead(WASH_DETECT_PIN) == HIGH) {
			curing_mode = true;
		} else {
			curing_mode = false;
		}
	}

	if (heater_error) {
		lcd.print_P(pgmstr_heater_error, 1, 0);
		lcd.print_P(pgmstr_please_restart, 1, 2);
		state = ERROR;
	}

	if (state == CONFIRM) {
		unsigned long us_now = millis();
		if (us_now - us_last > 1000) {
			beep();
			us_last = us_now;
		}
	}

	if (last_curing_mode != curing_mode) {
		last_curing_mode = curing_mode;
		redraw_menu = true;
	}

	// stepper motor speed up function
	if (speed_control.acceleration_flag == true) {
		unsigned long us_now = millis();
		if (us_now - us_last > 50) {
			speed_control.acceleration50ms();
			us_last = us_now;
		}
		if (speed_control.acceleration_flag == false) {
			myStepper.set_IHOLD_IRUN(10, 10, 5);
		}
	}

	// rotary "click" is 4 "micro steps"
	if (rotary_diff <= 124 || rotary_diff >= 132 || redraw_menu) { //124, 132
		menu_move(true);
	}

	if (state == RUNNING || state == RUN_MENU) {
		machine_running();
	}

	if (outputchip.digitalRead(BTN_ENC) == LOW) {
		if (button_active == false) {
			button_active = true;
			button_timer = millis();
		}
		if ((millis() - button_timer > LONG_PRESS_TIME) && (long_press_active == false)) {
			long_press_active = true;
			switch (state) {
				case HOME:
					state = RUN_MODE;
					long_press = true;
					redraw_menu = true;
					menu_move(true);
					break;
				case INFO:
					state = SELFTEST;
					menu_position = 0;
					redraw_menu = true;
					menu_move(true);
					break;
				case RUNNING:
					run_stop();
					redraw_menu = true;
					menu_move(true);
					break;
				default:
					break;
			}
		}
	} else {
		if (button_active == true) {
			if (long_press_active == true) {
				long_press_active = false;
			} else {
				if (!heater_error) {
					button_press();
				}
			}
			button_active = false;
		}
	}

	// FIXME s this needed to fix ESD shock? Any better solution?
	if (millis() > time_now + 5500) {
		if (state == HOME || state == PREHEAT || state == SOUND_SETTINGS || state == SPEED_STATE) {
			last_menu_position = menu_position;
		}

		time_now = millis();
		lcd.reinit();
		lcd.createChar(0, Back);
		lcd.createChar(1, Right);
		lcd.createChar(2, Backslash);
		redraw_menu = true;
		menu_move(false);

		if (state == HOME || state == PREHEAT || state == SOUND_SETTINGS || state == SPEED_STATE) {
			menu_position = last_menu_position;
			print_menu_cursor(menu_position);
		}
	}

	if (millis() > therm_read_time_now + 2000) {
		therm_read_time_now = millis();
		therm1_read();
	}
}

void menu_move(bool sound_echo) {
	if (!redraw_menu) {
		if (sound_echo && config.sound_response) {
			echo();
		}
	} else {
		lcd.clear();
	}

	redraw_menu = false;

	switch (state) {
		case HOME:
			static const char* first_line;
			if (curing_mode) {
				switch (config.curing_machine_mode) {
					case 3:
						first_line = pgmstr_start_resin_preheat;
						break;
					case 2:
						first_line = pgmstr_start_drying;
						break;
					case 1:
						first_line = pgmstr_start_curing;
						break;
					default:
						first_line = pgmstr_start_drying_curing;
						break;
				}
			} else {
				first_line = pgmstr_start_washing;
			}
			generic_menu_P(3, first_line, pgmstr_run_time, is_error() ? pgmstr_settings_error : pgmstr_settings);
			lcd_print_right(1);
			lcd_print_right(2);
			break;

		case SPEED_STATE:
			generic_menu_P(3, pgmstr_back, pgmstr_curing_speed, pgmstr_washing_speed);
			lcd_print_back();
			lcd_print_right(1);
			lcd_print_right(2);
			break;

		case SPEED_CURING:
			generic_value_P(pgmstr_curing_speed, &speed_control.curing_speed, 1, 10, XOFTEN);
			break;

		case SPEED_WASHING:
			generic_value_P(pgmstr_washing_speed, &speed_control.washing_speed, 1, 10, XOFTEN);
			break;

		case TIME:
			{
				Scrolling_item items[] = {
					{pgmstr_back, true, Ter::back},
					{pgmstr_curing_run_time, true, Ter::right},
					{pgmstr_drying_run_time, true, Ter::right},
					{pgmstr_washing_run_time, true, Ter::right},
					{pgmstr_resin_preheat_time, true, Ter::right},
				};
				menu_position = scrolling_list_P(items);
				break;
			}

		case TIME_CURING:
			generic_value_P(pgmstr_curing_run_time, &config.curing_run_time, 1, 10, MINUTES);
			break;

		case TIME_DRYING:
			generic_value_P(pgmstr_drying_run_time, &config.drying_run_time, 1, 10, MINUTES);
			break;

		case TIME_WASHING:
			generic_value_P(pgmstr_washing_run_time, &config.washing_run_time, 1, 10, MINUTES);
			break;

		case TIME_RESIN_PREHEAT:
			generic_value_P(pgmstr_resin_preheat_time, &config.resin_preheat_run_time, 1, 10, MINUTES);
			break;

		case SETTINGS:
			{
				Scrolling_item items[] = {
					{pgmstr_back, true, Ter::back},
					{pgmstr_rotation_speed, true, Ter::right},
					{pgmstr_run_mode, true, Ter::right},
					{pgmstr_preheat, true, Ter::right},
					{pgmstr_sound, true, Ter::right},
					{pgmstr_fans, true, Ter::right},
					{pgmstr_led_intensity, true, Ter::right},
					{is_error() ? pgmstr_information_error : pgmstr_information, true, Ter::right},
				};
				menu_position = scrolling_list_P(items);
				break;
			}

		case PREHEAT:
			{
				Scrolling_item items[] = {
					{pgmstr_back, true, Ter::back},
					{config.heat_to_target_temp ? pgmstr_preheat_on : pgmstr_preheat_off, true, Ter::none},
					{pgmstr_drying_curing_temp, true, Ter::right},
					{pgmstr_resin_preheat_temp, true, Ter::right},
					{config.SI_unit_system ? pgmstr_units_C : pgmstr_units_F, true, Ter::none},
				};
				menu_position = scrolling_list_P(items);
				break;
			}

		case TARGET_TEMP:
			if (config.SI_unit_system) {
				generic_value_P(pgmstr_target_temp, &config.target_temp, MIN_TARGET_TEMP_C, MAX_TARGET_TEMP_C, TEMPERATURE_C);
			} else {
				generic_value_P(pgmstr_target_temp, &config.target_temp, MIN_TARGET_TEMP_F, MAX_TARGET_TEMP_F, TEMPERATURE_F);
			}
			break;

		case RESIN_TARGET_TEMP:
			if (config.SI_unit_system) {
				generic_value_P(pgmstr_target_temp, &config.resin_target_temp, MIN_TARGET_TEMP_C, MAX_TARGET_TEMP_C, TEMPERATURE_C);
			} else {
				generic_value_P(pgmstr_target_temp, &config.resin_target_temp, MIN_TARGET_TEMP_F, MAX_TARGET_TEMP_F, TEMPERATURE_F);
			}
			break;

		case RUN_MODE:
			generic_items_P(pgmstr_run_mode, &config.curing_machine_mode, 4, pgmstr_drying_curing, pgmstr_curing, pgmstr_drying, pgmstr_resin_preheat);
			break;

		case SOUND_SETTINGS:
			generic_menu_P(3, pgmstr_back,
				config.sound_response ? pgmstr_control_echo_on : pgmstr_control_echo_off,
				pgmstr_finish_beep);
			lcd_print_back();
			lcd_print_right(2);
			break;

		case BEEP:
			generic_items_P(pgmstr_finish_beep, &config.finish_beep_mode, 3, pgmstr_none, pgmstr_once, pgmstr_continuous);
			break;

		case FANS:
			{
				Scrolling_item items[] = {
					{pgmstr_back, true, Ter::back},
					{pgmstr_fan1_curing_speed, true, Ter::right},
					{pgmstr_fan1_drying_speed, true, Ter::right},
					{pgmstr_fan2_curing_speed, true, Ter::right},
					{pgmstr_fan2_drying_speed, true, Ter::right},
				};
				menu_position = scrolling_list_P(items);
				break;
			}

		case LED_INTENSITY:
			generic_value_P(pgmstr_led_intensity, &config.led_pwm_value, 1, 100, PERCENT);
			break;

		case FAN1_CURING:
			generic_value_P(pgmstr_fan1_curing_speed, &config.fans_curing_speed.fan1, 0, 100, PERCENT);
			break;

		case FAN1_DRYING:
			generic_value_P(pgmstr_fan1_drying_speed, &config.fans_drying_speed.fan1, 0, 100, PERCENT);
			break;

		case FAN2_CURING:
			generic_value_P(pgmstr_fan2_curing_speed, &config.fans_curing_speed.fan2, 0, 100, PERCENT);
			break;

		case FAN2_DRYING:
			generic_value_P(pgmstr_fan2_drying_speed, &config.fans_drying_speed.fan2, 0, 100, PERCENT);
			break;

		case INFO:
			{
				Scrolling_item items[] = {
					{pgmstr_fw_version, true, Ter::none},
					{pgmstr_fan1_failure, fan_error[0], Ter::none},
					{pgmstr_fan2_failure, fan_error[1], Ter::none},
					{pgmstr_heater_failure, config.heater_failure, Ter::none},
					{pgmstr_serial_number, true, Ter::serialNumber},
					{pgmstr_build_nr, true, Ter::none},
					{pgmstr_fw_hash, true, Ter::none},
					{pgmstr_workspace, true, Ter::none}
				};
				menu_position = scrolling_list_P(items);
				break;
			}

		case RUN_MENU:
			if (!curing_mode && paused_time) {
				generic_menu_P(3, paused ? pgmstr_ipa_tank_removed : pgmstr_pause, pgmstr_stop, pgmstr_back);
			} else {
				generic_menu_P(3, paused ? pgmstr_continue : pgmstr_pause, pgmstr_stop, pgmstr_back);
			}
			break;

		case RUNNING:
			lcd.setCursor(1, 0);
			if (curing_mode) {
				if (paused) {
					if (config.heat_to_target_temp || (config.curing_machine_mode == 3) || (preheat_complete == false)) {
						lcd.print_P(paused ? pgmstr_paused : drying_mode ? pgmstr_heating : pgmstr_curing);
					} else {
						lcd.print_P(paused ? pgmstr_paused : drying_mode ? pgmstr_drying : pgmstr_curing);
					}
				} else {
					if (config.heat_to_target_temp || (config.curing_machine_mode == 3)) {
						if (!preheat_complete) {
							lcd.print_P(cover_open ? pgmstr_cover_is_open : drying_mode ? pgmstr_heating : pgmstr_curing);
						} else {
							lcd.print_P(cover_open ? pgmstr_cover_is_open : drying_mode ? pgmstr_drying : pgmstr_curing);
						}
					} else {
						lcd.print_P(cover_open ? pgmstr_cover_is_open : drying_mode ? pgmstr_drying : pgmstr_curing);
					}
				}
			} else {
				lcd.print_P(cover_open ? pgmstr_cover_is_open : (paused ? pgmstr_paused : pgmstr_washing));
			}
			if (curing_mode && drying_mode && config.heat_to_target_temp && !preheat_complete) {
				lcd_clear_time_boundaries();
			} else {
				if (rotary_diff > 128) {
					if (tDown.getCurrentMinutes() <= 9) {
						uint8_t mins = tDown.getCurrentMinutes();
						uint8_t secs = tDown.getCurrentSeconds();
						lcd_clear_time_boundaries();
						lcd.print_P(pgmstr_double_gt, LAYOUT_TIME_GT, LAYOUT_TIME_Y);

						if (secs <= 30) {
							tDown.setCounter(0, mins, secs + 30, tDown.COUNT_DOWN, tDownComplete);
						} else {
							tDown.setCounter(0, mins + 1, 30 - (60 - secs), tDown.COUNT_DOWN, tDownComplete);
						}
					} else {
						lcd_clear_time_boundaries();
						lcd.print_P(pgmstr_max_symb, LAYOUT_TIME_GT, LAYOUT_TIME_Y);
					}
				} else if (rotary_diff < 128) {
					if (tDown.getCurrentSeconds() >= 30 || tDown.getCurrentMinutes() >= 1) {
						uint8_t mins = tDown.getCurrentMinutes();
						uint8_t secs = tDown.getCurrentSeconds();
						lcd_clear_time_boundaries();
						lcd.print_P(pgmstr_double_lt, LAYOUT_TIME_LT, LAYOUT_TIME_Y);

						if (secs >= 30) {
							tDown.setCounter(0, mins, secs - 30, tDown.COUNT_DOWN, tDownComplete);
						} else {
							tDown.setCounter(0, mins - 1, 60 - (30 - secs), tDown.COUNT_DOWN, tDownComplete);
						}
					} else {
						lcd_clear_time_boundaries();
						lcd.print_P(pgmstr_min_symb, LAYOUT_TIME_LT, LAYOUT_TIME_Y);
					}
				}
			}
			redraw_ms = true; // for print MM:SS part
			break;

		case CONFIRM:
			lcd.print_P(pgmstr_finished, 1, 0);
			lcd.print_P(pgmstr_press2continue, 1, 2);
			break;

		case SELFTEST:
			if (selftest.phase == 0) {
				generic_menu_P(2, pgmstr_back, pgmstr_selftest);
				lcd_print_back();
				lcd_print_right(1);
			} else if (selftest.phase == 1) {
				lcd.setCursor(1,0);
				if (!selftest.cover_test) {
					if (!selftest.measured_state)
						lcd.print_P(pgmstr_open_cover);
					else
						lcd.print_P(pgmstr_close_cover);
				} else {
					lcd.print_P(pgmstr_test_success);
					lcd.print_P(pgmstr_press2continue, 1, 2);
				}
			} else if (selftest.phase == 2) {
				lcd.setCursor(1,0);
				if (!selftest.tank_test) {
					if (!selftest.measured_state)
						lcd.print_P(pgmstr_remove_tank);
					else
						lcd.print_P(pgmstr_insert_tank);
				} else {
					lcd.print_P(pgmstr_test_success);
					lcd.print_P(pgmstr_press2continue, 1, 2);
				}
			} else {
				lcd.print_P(selftest.print(), 1, 0);
				lcd.print_P(pgmstr_press2continue, 1, 2);
			}

			if (selftest.phase == 3 && !selftest.vent_test) {
				lcd.print_P(pgmstr_fan1_test, 1, 1);
				lcd.print_P(pgmstr_fan2_test, 1, 2);
			}
			if (selftest.phase == 6 && !selftest.rotation_test) {
				lcd.print_P(pgmstr_mode_gear, 1, 1);
				lcd.print_P(pgmstr_slash, 13, 1);
			}
			redraw_selftest_vals();
			break;

		default:
			break;
	}
	rotary_diff = 128;
}

void machine_running() {

	if (curing_mode) { //curing mode
		if (outputchip.digitalRead(COVER_OPEN_PIN) == HIGH) { //cover check
			if (!cover_open) {
				if (!paused) {
					lcd.print_P(pgmstr_cover_is_open, 1, 0);
				}
				redraw_menu = true;
				cover_open = true;
			}
		} else {
			if (cover_open) {
				redraw_menu = true;
				cover_open = false;
			}
		}

		if (cover_open == true) {
			stop_motor();
			motor_configuration();
			speed_control.speed_configuration(curing_mode);
			stop_heater(); // turn off heat fan
			outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn off led
			digitalWrite(LED_PWM_PIN, LOW);
		} else if (!paused) { // cover closed
			run_motor();
			unsigned long us_now = millis();
			remain -= us_now - us_last ;
			us_last = us_now;
		}

		switch (config.curing_machine_mode) {
			case 3: // Resin preheat
				if (!preheat_complete) {
					if (tUp.isCounterCompleted() == false) {
						if (!drying_mode) {
							drying_mode = true;
							redraw_menu = true;
						}
						start_drying();
					} else {
						if (drying_mode) {
							//drying_mode = false;
							redraw_menu = true;
							preheat_complete = true;
							remain = config.resin_preheat_run_time;
							tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
							tDown.start();
						}
					}
				} else {
					if (tDown.isCounterCompleted() == false) {
						if (!drying_mode) {
							drying_mode = true;
							redraw_menu = true;
						}
						start_drying();
					} else {
						if (drying_mode) {
							drying_mode = false;
							redraw_menu = true;
						}
						preheat_complete = false;
						stop_curing_drying();
					}
				}

				break;

			case 2: // Drying
				if (!config.heat_to_target_temp) {
					if (tDown.isCounterCompleted() == false) {
						if (!drying_mode) {
							drying_mode = true;
							redraw_menu = true;
						}
						start_drying();
					} else {
						if (drying_mode) {
							drying_mode = false;
							redraw_menu = true;
						}
						stop_curing_drying();
					}
				} else {
					if (!preheat_complete) {
						if (tUp.isCounterCompleted() == false) {
							if (!drying_mode) {
								drying_mode = true;
								redraw_menu = true;
							}
							start_drying();
						} else {
							if (drying_mode) {
								//drying_mode = false;
								redraw_menu = true;
								preheat_complete = true;
								remain = config.drying_run_time;
								tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
								tDown.start();
							}
						}
					} else {
						if (tDown.isCounterCompleted() == false) {
							if (!drying_mode) {
								drying_mode = true;
								redraw_menu = true;
							}
							start_drying();
						} else {
							if (drying_mode) {
								drying_mode = false;
								redraw_menu = true;
							}
							preheat_complete = false;
							stop_curing_drying();
						}
					}
				}
				break;

			case 1: // Curing
				if (tDown.isCounterCompleted() == false) {
					if (drying_mode) {
						drying_mode = false;
						redraw_menu = true;
					}
					start_curing();
				} else {
					stop_curing_drying();
				}
				break;

			case 0: // Drying and curing
			default:
				if (!config.heat_to_target_temp) {
					if ((drying_mode == true) && (tDown.isCounterCompleted() == false)) {
						start_drying();
					} else {
						if (drying_mode) {
							drying_mode = false;
							remain = config.curing_run_time;
							tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
							tDown.start();
							redraw_menu = true;
							menu_move(true);
						}
						if (tDown.isCounterCompleted() == false) {
							start_curing();
							fan_duty = config.fans_curing_speed;
						} else {
							stop_curing_drying();
						}
					}
				} else {
					if (!preheat_complete) {
						if (tUp.isCounterCompleted() == false) {
							if (!drying_mode) {
								drying_mode = true;
								redraw_menu = true;
							}
							start_drying();
						} else {
							if (drying_mode) {
								//drying_mode = false;
								redraw_menu = true;
								preheat_complete = true;
								remain = config.drying_run_time;
								tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
								tDown.start();
							}
						}
					} else {
						if ((drying_mode == true) && (tDown.isCounterCompleted() == false)) {
							start_drying();
						} else {
							if (drying_mode) {
								drying_mode = false;
								remain = config.curing_run_time;
								tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
								tDown.start();
								redraw_menu = true;
								menu_move(true);
							}
							if (tDown.isCounterCompleted() == false) {
								start_curing();
							} else {
								stop_curing_drying();
							}
						}
					}
				}
				break;
		}
	}
	if (!curing_mode) { // washing mode
		start_washing();
	}
}

void button_press() {
	if (config.sound_response) {
		echo();
	}
	switch (state) {
		case HOME:
			switch (menu_position) {
				case 0:
					if (curing_mode) { // curing_mode
						motor_configuration();
						speed_control.speed_configuration(curing_mode);

						switch (config.curing_machine_mode) {
							case 3: // Resin preheat
								pid_mode = true;
								remain = max_preheat_run_time;
								tUp.setCounter(0, remain, 0, tUp.COUNT_UP, tUpComplete);
								tUp.start();
								fan_duty = config.fans_preheat_speed;
								outputchip.digitalWrite(LED_RELE_PIN, LOW); //turn off LED
								digitalWrite(LED_PWM_PIN, LOW);
								drying_mode = true;
								preheat_complete = false;
								break;

							case 2: // Drying
								preheat_complete = false;
								drying_mode = true;
								if (!config.heat_to_target_temp) {
									pid_mode = false;
									remain = config.drying_run_time;
									tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
									tDown.start();
									fan_duty = config.fans_drying_speed;
								} else {
									pid_mode = true;
									remain = max_preheat_run_time;
									tUp.setCounter(0, remain, 0, tUp.COUNT_UP, tUpComplete);
									tUp.start();
									fan_duty = config.fans_preheat_speed;
								}
								outputchip.digitalWrite(LED_RELE_PIN, LOW); //turn off LED
								digitalWrite(LED_PWM_PIN, LOW);
								drying_mode = true;
								break;

							case 1: // Curing
								pid_mode = false;
								remain = config.curing_run_time;
								tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
								tDown.start();
								fan_duty = config.fans_curing_speed;
								drying_mode = false;
								break;

							case 0: // Drying and curing
							default:
								tDown.stop();
								tUp.stop();
								preheat_complete = false;
								drying_mode = true;
								if (!config.heat_to_target_temp) {
									pid_mode = false;
									remain = config.drying_run_time;
									tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
									tDown.start();
									fan_duty = config.fans_drying_speed;
								} else {
									pid_mode = true;
									remain = max_preheat_run_time;
									tUp.setCounter(0, remain, 0, tUp.COUNT_UP, tUpComplete);
									tUp.start();
									fan_duty = config.fans_preheat_speed;
								}
								break;
						}
					} else { // washing_mode
						drying_mode = false;
						run_motor();
						motor_configuration();
						speed_control.speed_configuration(curing_mode);
						remain = config.washing_run_time;
						tDown.setCounter(0, remain, 0, tDown.COUNT_DOWN, tDownComplete);
						tDown.start();
						fan_duty = fans_washing_speed;
					}

					us_last = millis();

					menu_position = 0;
					state = RUNNING;
					redraw_menu = true;
					menu_move(true);
					break;

				case 1:
					menu_position = 0;
					state = TIME;
					break;

				case 2:
					menu_position = 0;
					state = SETTINGS;
					break;

				default:
					break;
			}
			break;

		case SETTINGS:
			switch (menu_position) {
				case 0:
					menu_position = 2;
					state = HOME;
					break;

				case 1:
					menu_position = 0;
					state = SPEED_STATE;
					break;

				case 2:
					menu_position = 0;
					state = RUN_MODE;
					break;

				case 3:
					menu_position = 0;
					state = PREHEAT;
					break;

				case 4:
					menu_position = 0;
					state = SOUND_SETTINGS;
					break;

				case 5:
					menu_position = 0;
					state = FANS;
					break;

				case 6:
					menu_position = 0;
					state = LED_INTENSITY;
					break;

				default:
					menu_position = 0;
					state = INFO;
					break;
			}
			break;

		case SOUND_SETTINGS:
			switch (menu_position) {
				case 0:
					menu_position = 4;
					state = SETTINGS;
					break;

				case 1:
					config.sound_response ^= 1;
					write_config();
					redraw_menu = true;
					break;

				case 2:
					menu_position = 0;
					state = BEEP;
					break;
			}
			break;

		case FANS:
			switch (menu_position) {
				case 0:
					menu_position = 5;
					state = SETTINGS;
					break;

				case 1:
					menu_position = 0;
					state = FAN1_CURING;
					break;

				case 2:
					menu_position = 0;
					state = FAN1_DRYING;
					break;

				case 3:
					menu_position = 0;
					state = FAN2_CURING;
					break;

				default:
					menu_position = 0;
					state = FAN2_DRYING;
					break;
			}
			break;

		case LED_INTENSITY:
			menu_position = 6;
			write_config();
			state = SETTINGS;
			break;

		case FAN1_CURING:
			menu_position = 1;
			write_config();
			state = FANS;
			break;

		case FAN1_DRYING:
			menu_position = 2;
			write_config();
			state = FANS;
			break;

		case FAN2_CURING:
			menu_position = 3;
			write_config();
			state = FANS;
			break;

		case FAN2_DRYING:
			menu_position = 4;
			write_config();
			state = FANS;
			break;

		case PREHEAT:
			switch (menu_position) {
				case 0:
					menu_position = 3;
					state = SETTINGS;
					break;

				case 1:
					config.heat_to_target_temp ^= 1;
					write_config();
					redraw_menu = true;
					break;

				case 2:
					menu_position = 0;
					state = TARGET_TEMP;
					break;

				case 3:
					menu_position = 0;
					state = RESIN_TARGET_TEMP;
					break;

				default:
					config.SI_unit_system ^= 1;
					if (config.SI_unit_system) {
						config.target_temp = round(fahrenheit2celsius(config.target_temp));
						config.resin_target_temp = round(fahrenheit2celsius(config.resin_target_temp));
					} else {
						config.target_temp = round(celsius2fahrenheit(config.target_temp));
						config.resin_target_temp = round(celsius2fahrenheit(config.resin_target_temp));
					}
					write_config();
					break;
			}
			break;

		case SPEED_STATE:
			switch (menu_position) {
				case 0:
					menu_position = 1;
					state = SETTINGS;
					break;

				case 1:
					menu_position = 1;
					state = SPEED_CURING;
					break;

				default:
					menu_position = 2;
					state = SPEED_WASHING;
					break;
			}
			break;

		case SPEED_CURING:
			write_config();
			state = SPEED_STATE;
			break;

		case SPEED_WASHING:
			write_config();
			state = SPEED_STATE;
			break;

		case TIME:
			switch (menu_position) {
				case 0:
					menu_position = 1;
					state = HOME;
					break;

				case 1:
					menu_position = 0;
					state = TIME_CURING;
					break;

				case 2:
					menu_position = 0;
					state = TIME_DRYING;
					break;

				case 3:
					menu_position = 0;
					state = TIME_WASHING;
					break;

				default:
					menu_position = 0;
					state = TIME_RESIN_PREHEAT;
					break;
			}
			break;

		case BEEP:
			write_config();
			menu_position = 2;
			state = SOUND_SETTINGS;
			break;

		case TIME_CURING:
			write_config();
			menu_position = 1;
			state = TIME;
			break;

		case TIME_DRYING:
			write_config();
			menu_position = 2;
			state = TIME;
			break;

		case TIME_WASHING:
			write_config();
			menu_position = 3;
			state = TIME;
			break;

		case TIME_RESIN_PREHEAT:
			write_config();
			menu_position = 4;
			state = TIME;
			break;

		case INFO:
			menu_position = 7;
			state = SETTINGS;
			break;

		case RUN_MODE:
			write_config();
			if (!long_press) {
				menu_position = 2;
				state = SETTINGS;
			} else {
				long_press = false;
				menu_position = 0;
				state = HOME;
			}
			break;

		case TARGET_TEMP:
			write_config();
			menu_position = 2;
			state = PREHEAT;
			break;

		case RESIN_TARGET_TEMP:
			write_config();
			menu_position = 3;
			state = PREHEAT;
			break;

		case CONFIRM:
			menu_position = 0;
			state = HOME;
			break;

		case RUN_MENU:
			switch (menu_position) {
				case 0:
					if (curing_mode) { //curing mode
						if (!gastro_pan) {
							paused ^= 1;
							if (paused) {
								stop_motor();
								stop_heater(); // turn off heat fan
								fan_duty = fans_menu_speed;
							} else {
								run_motor();
								motor_configuration();
								speed_control.speed_configuration(curing_mode);

								if (!config.heat_to_target_temp) {
									fan_duty = config.fans_curing_speed;
								} else {
									fan_duty = config.fans_preheat_speed;
								}
							}
							menu_position = 0;
							state = RUNNING;
						}
					} else { //washing mode
						if (!gastro_pan) {
							paused ^= 1;
							if (paused) {
								stop_motor();
								stop_heater(); // turn off heat fan
								//fan_duty = fans_menu_speed;
							} else {
								run_motor();
								motor_configuration();
								speed_control.speed_configuration(curing_mode);
								fan_duty = fans_washing_speed;
							}
							menu_position = 0;
							state = RUNNING;
						}
					}
					break;

				case 1:
					run_stop();
					break;

				case 2:
					menu_position = 0;
					state = RUNNING;
					break;

				default:
					break;
			}
			break;

		case RUNNING:
			menu_position = 0;
			state = RUN_MENU;
			break;

		case SELFTEST:
			switch (selftest.phase) {
				case 0:
					if (menu_position) {
						selftest.phase++;
					} else {
						state = INFO;
					}
					break;

				case 1:
					if (selftest.cover_test == true) {
						selftest.phase++;
						selftest.clean_up();
					}
					break;
				case 2:
					if (selftest.tank_test == true) {
						selftest.phase++;
						selftest.clean_up();
					}
					break;
				case 3:
					if (selftest.vent_test) {
						if (selftest.fail_flag == true) {
							state = HOME;
						} else {
							selftest.phase++;
						}
						selftest.clean_up();
					}
					break;
				case 4:
					if (selftest.led_test) {
						if (selftest.fail_flag == true) {
							state = HOME;
						} else {
							selftest.phase++;
						}
						selftest.clean_up();
					}
						break;
				case 5:
					if (selftest.heater_test) {
						if (selftest.fail_flag == true) {
							state = HOME;
						} else {
							selftest.phase++;
						}
						selftest.clean_up();
					}
					break;

				case 6:
					if (selftest.rotation_test == true) {
						selftest.phase = 0;
						selftest.clean_up();
					}
					break;

				default:
					break;
			}
			menu_position = 0;
			break;

		default:
			break;
	}
	scrolling_list_set(menu_position);

	rotary_diff = 128;
	redraw_menu = true;
	menu_move(true);
	//delay(475);
}

// timmer for stepper move
ISR(TIMER3_COMPA_vect) {
	if (speed_control.motor_running == true) {
		OCR3A = speed_control.microstep_control;
		digitalWrite(STEP_PIN, HIGH);
		delayMicroseconds(2);
		digitalWrite(STEP_PIN, LOW);
		delayMicroseconds(2);
	}
}

void read_encoder() {
	static uint8_t lcd_encoder_bits = 0;
	uint8_t enc = 0;
	if (digitalRead(BTN_EN1) == HIGH) {
		enc |= B01;
	}
	if (digitalRead(BTN_EN2) == HIGH) {
		enc |= B10;
	}
	if (enc != lcd_encoder_bits) {
		switch (enc) {
			case encrot0:
				if (lcd_encoder_bits == encrot3 && rotary_diff < 255) {
					rotary_diff++;
				} else if (lcd_encoder_bits == encrot1 && rotary_diff) {
					rotary_diff--;
				}
				break;
			case encrot1:
				if (lcd_encoder_bits == encrot0 && rotary_diff < 255) {
					rotary_diff++;
				} else if (lcd_encoder_bits == encrot2 && rotary_diff) {
					rotary_diff--;
				}
				break;
			case encrot2:
				if (lcd_encoder_bits == encrot1 && rotary_diff < 255) {
					rotary_diff++;
				} else if (lcd_encoder_bits == encrot3 && rotary_diff) {
					rotary_diff--;
				}
				break;
			case encrot3:
				if (lcd_encoder_bits == encrot2 && rotary_diff < 255) {
					rotary_diff++;
				} else if (lcd_encoder_bits == encrot0 && rotary_diff) {
					rotary_diff--;
				}
				break;
		}
		lcd_encoder_bits = enc;
	}
}

//1ms timer
SIGNAL(TIMER0_COMPA_vect) {
	if (!heater_error) {
		read_encoder();
	}

	if (pid_mode) {
		if (config.curing_machine_mode == 0 || config.curing_machine_mode == 2 || config.curing_machine_mode == 3 || (selftest.phase == 5 && state == SELFTEST)) {
			uint8_t tmpTargetTemp = config.curing_machine_mode == 3 ? config.resin_target_temp : config.target_temp;
			if (chamber_temp >= tmpTargetTemp) {
				fan_duty.fan1 = PI_regulator(chamber_temp, tmpTargetTemp);
				fan_duty.fan2 = fan_duty.fan1;
			} else {
				fan_duty = fans_menu_speed;
			}
		}
	}

	fan_pwm_control();
	fan_rpm();
}

void tDownComplete() {
	tDown.stop();
}

void tUpComplete() {
	tUp.stop();
}

void start_drying() {
	if (cover_open == false && paused == false) {
		if (config.heat_to_target_temp || (config.curing_machine_mode == 3)) {
			if (!preheat_complete) {
				preheat(); // turn on heat fan
			} else {
				run_heater(); // turn on heat fan
			}
		} else {
			run_heater(); // turn on heat fan
		}
	}
	if (cover_open == true || paused == true) {
		if (config.heat_to_target_temp || (config.curing_machine_mode == 3)) {
			if (!paused_time) {
				paused_time = true;
			}
			if (!preheat_complete) {
				tUp.pause();
			} else {
				tDown.pause();
			}

		} else {
			if (!paused_time) {
				paused_time = true;
			}
			tDown.pause();
		}
	} else {
		if (config.heat_to_target_temp || (config.curing_machine_mode == 3)) {
			if (paused_time) {
				paused_time = false;
				redraw_menu = true;
				menu_move(true);
			}
			if (!preheat_complete) {
				tUp.start();
			} else {
				tDown.start();
			}
		} else {
			if (paused_time) {
				paused_time = false;
				redraw_menu = true;
				menu_move(true);
			}
			tDown.start();
		}
	}
	if (outputchip.digitalRead(WASH_DETECT_PIN) == LOW) { //Gastro Pen check
		lcd.print_P(pgmstr_remove_tank, 1, 0);
		paused = true;
		if (!paused_time) {
			paused_time = true;
		}
		tDown.pause();
		stop_heater();
		stop_motor();
		if (!gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = true;
		}
	} else {
		if (gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = false;
		}
	}
	lcd_time_print();
}

void start_curing() {
	stop_heater(); // turn off heat fan
	if (cover_open == false && paused == false) {
		if (!led_start) {
			led_start = true;
			led_time_now = millis();
		}
		if (millis() > led_time_now + LED_DELAY) {
			outputchip.digitalWrite(LED_RELE_PIN, HIGH); // turn LED on
			analogWrite(LED_PWM_PIN, map(config.led_pwm_value, 0, 100, 0, 255));
		}
	} else {
		if (led_start) {
			outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn LED off
			digitalWrite(LED_PWM_PIN, LOW);
			led_start = false;
		}
	}
	if (cover_open == true || paused == true) {
		if (!paused_time) {
			paused_time = true;
		}
		tDown.pause();
	} else {
		if (paused_time) {
			paused_time = false;
			redraw_menu = true;
			menu_move(true);
		}
		tDown.start();
	}
	if (outputchip.digitalRead(WASH_DETECT_PIN) == LOW) { //Gastro Pen check
		lcd.print_P(pgmstr_remove_tank, 1, 0);
		paused = true;
		if (!paused_time) {
			paused_time = true;
		}
		tDown.pause();
		stop_heater();
		stop_motor();
		if (!gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = true;
		}
	} else {
		if (gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = false;
		}
	}
	lcd_time_print();
}

void start_washing() {
	if (cover_open) {
		redraw_menu = true;
		cover_open = false;
	}
	if (outputchip.digitalRead(WASH_DETECT_PIN) == HIGH) { //Gastro Pen check
		lcd.print_P(pgmstr_ipa_tank_removed, 1, 0);
		paused = true;
		if (!paused_time) {
			paused_time = true;
		}
		tDown.pause();
		stop_motor();
		if (!gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = true;
		}
	} else {
		if (paused_time) {
			paused_time = false;
		}
		if (gastro_pan) {
			redraw_menu = true;
			menu_move(true);
			gastro_pan = false;
		}
	}
	if (tDown.isCounterCompleted() == false)	{
		if (state == RUNNING) {
			if ((paused == false) && (outputchip.digitalRead(WASH_DETECT_PIN) == LOW)) {
				run_motor();
				tDown.start();
			} else {
				tDown.pause();
			}

			lcd_time_print();
		}
	} else {
		menu_position = 0;
		stop_motor();
		fan_duty = fans_menu_speed;
		stop_heater(); // turn off heat fan
		redraw_menu = true;
		rotary_diff = 128;
		switch (config.finish_beep_mode) {
			case 2:
				beep();

				state = CONFIRM;
				break;
			case 1:
				beep();
				state = HOME;
				break;

			case 0:
			default:
				state = HOME;
				break;
		}
		menu_move(true);
	}
}

void stop_curing_drying() {
	pid_mode = false;
	menu_position = 0;
	outputchip.digitalWrite(LED_RELE_PIN, LOW); // turn off led
	digitalWrite(LED_PWM_PIN, LOW);
	stop_motor();
	stop_heater(); // turn off heat fan
	fan_duty = fans_menu_speed;
	redraw_menu = true;
	rotary_diff = 128;
	switch (config.finish_beep_mode) {
		case 2:
			beep();
			state = CONFIRM;
			break;

		case 1:
			beep();
			state = HOME;
			break;

		case 0:
		default:
			state = HOME;
			break;
	}
	menu_move(true);
}

//! @brief Display remaining time
void lcd_time_print() {
	static uint8_t running_count = 0;
	uint8_t mins;
	uint8_t secs;
	if (config.heat_to_target_temp || (config.curing_machine_mode == 3)) {
		if (drying_mode) {
			if (!preheat_complete) {
				mins = tUp.getCurrentMinutes();
				secs = tUp.getCurrentSeconds();
			} else {
				mins = tDown.getCurrentMinutes();
				secs = tDown.getCurrentSeconds();
			}
		} else {
			mins = tDown.getCurrentMinutes();
			secs = tDown.getCurrentSeconds();
		}
	} else {
		mins = tDown.getCurrentMinutes();
		secs = tDown.getCurrentSeconds();
	}

	if (state == RUNNING && (secs != last_seconds || redraw_ms)) {
		redraw_ms = false;
		last_seconds = secs;
		lcd.printTime(mins, secs, LAYOUT_TIME_X, LAYOUT_TIME_Y);

		if (!paused && !paused_time) {
			lcd.print_P(pgmstr_space, 19, 1);

			if (curing_mode) {
				if (outputchip.digitalRead(COVER_OPEN_PIN) == LOW) {
					lcd.print(chamber_temp, LAYOUT_TEMP_X, LAYOUT_TEMP_Y);
					lcd.print_P(config.SI_unit_system ? pgmstr_celsius : pgmstr_fahrenheit);
				}
			}

			lcd.setCursor(19, 0);
			uint8_t c = pgm_read_byte(pgmstr_progress + running_count);
			lcd.write(c);
		}

		if (++running_count > sizeof(pgmstr_progress)) {
			lcd_clear_time_boundaries();
			running_count = 0;
			redraw_menu = true;
		}
	}
}

void fan_pwm_control() {
	//rev 0.4 - inverted PWM FAN1, FAN2
	// FIXME refactoring needed
	unsigned long currentMillis = millis();
	if (fan_duty.fan1 > 0) {
		if (!fan1_on) {
			fan1_on = true;
			outputchip.digitalWrite(FAN1_PIN, HIGH);
		}
		if (fan1_pwm_high == false) {
			if (currentMillis - fan1_previous_millis >= ((period) * (1 - ((float)fan_duty.fan1 / 100)))) {
				fan1_previous_millis = currentMillis;
				PORTC = PORTC & 0x7F; //OUTPUT FAN1 LOW
				fan1_pwm_high = true;
			}
		}
		if (fan1_pwm_high == true) {
			if (currentMillis - fan1_previous_millis >= ((period) * ((float)fan_duty.fan1 / 100))) {
				fan1_previous_millis = currentMillis;
				PORTC = PORTC | 0x80; //OUTPUT FAN1 HIGH
				fan1_pwm_high = false;
			}
		}
	} else {
		if (fan1_on) {
			fan1_on = false;
			outputchip.digitalWrite(FAN1_PIN, LOW);
			PORTC = PORTC & 0x7F; //OUTPUT FAN1 LOW
		}
	}

	if (fan_duty.fan2 > 0) {
		if (!fan2_on) {
			fan2_on = true;
			outputchip.digitalWrite(FAN2_PIN, HIGH);
		}
		if (fan2_pwm_high == false) {
			if (currentMillis - fan2_previous_millis >= ((period) * (1 - ((float)fan_duty.fan2 / 100)))) {
				fan2_previous_millis = currentMillis;
				PORTB = PORTB & 0x7F; //OUTPUT FAN2 LOW
				fan2_pwm_high = true;
			}
		}
		if (fan2_pwm_high == true) {
			if (currentMillis - fan2_previous_millis >= ((period) * ((float)fan_duty.fan2 / 100))) {
				fan2_previous_millis = currentMillis;
				PORTB = PORTB | 0x80; //OUTPUT FAN2 HIGH
				fan2_pwm_high = false;
			}
		}
	} else {
		if (fan2_on) {
			fan2_on = false;
			outputchip.digitalWrite(FAN2_PIN, LOW);
			PORTB = PORTB & 0x7F; //OUTPUT FAN2 LOW
		}
	}
}

void therm1_read() {
	// FIXME WTF?
	outputchip.digitalWrite(9, LOW);
	outputchip.digitalWrite(10, LOW);
	chamber_temp = config.SI_unit_system ? therm1.analog2temp() : celsius2fahrenheit(therm1.analog2temp());
}

float celsius2fahrenheit(float celsius) {
	return 1.8 * celsius + 32;
}

float fahrenheit2celsius(float fahrenheit) {
	return (fahrenheit - 32) / 1.8;
}

void fan_rpm() {
	ams_fan_counter ++;
	if (ams_fan_counter % 100 == 0) {
		for (uint8_t j = 0; j < 2; j++) {
			if (fan_tacho_count[j] <= fan_tacho_last_count[j]) {
				if (j ? fan_duty.fan2 : fan_duty.fan1 > 0)
					fan_error[j] = true;
			} else {
				fan_error[j] = false;
			}

			selftest.fan_tacho[j] = fan_tacho_count[j] - fan_tacho_last_count[j];
			fan_tacho_last_count[j] = fan_tacho_count[j];
			if (fan_tacho_count[j] >= 10000) {
				fan_tacho_count[j] = 0;
					fan_tacho_last_count[j] = 0;
			}
		}
		if (ams_fan_counter >= 1000) {
			if (heater_running) {
				heater_error = (fan_tacho_count[2] <= fan_tacho_last_count[2]);
				//write to EEPROM only if state is changed
				if (config.heater_failure != heater_error) {
					config.heater_failure = heater_error;
					write_config();
				}
				fan_tacho_last_count[2] = fan_tacho_count[2];
				if (fan_tacho_count[2] >= 10000) {
					fan_tacho_count[2] = 0;
					fan_tacho_last_count[2] = 0;
				}
			}
			ams_fan_counter = 0;
		}
	}
}

void fan_tacho1() {
	fan_tacho_count[0]++;
}

void fan_tacho2() {
	fan_tacho_count[1]++;
}

void fan_tacho3() {
	fan_tacho_count[2]++;
}

void preheat() {
	uint8_t tmpTargetTemp = config.curing_machine_mode == 3 ? config.resin_target_temp : config.target_temp;
	if (chamber_temp < tmpTargetTemp) {
		run_heater();
	} else {
		stop_heater();
		tUp.setCounter(0, 0, 0, tUp.COUNT_UP, tUpComplete);
	}
}

#if 0
//! @brief Get reset flags
//! @return value of MCU Status Register - MCUSR as it was backed up by bootloader
static uint8_t get_reset_flags()
{
	return bootKeyPtrVal;
}
#endif

#define ATTR_INIT_SECTION(SectionIndex) __attribute__ ((used, naked, section (".init" #SectionIndex )))
void get_key_from_boot(void) ATTR_INIT_SECTION(3);

//! @brief Save the value of the boot key memory before it is overwritten
//!
//! Do not call this function, it is placed in one of the initialization sections,
//! which executes automatically before the main function of the application.
//! Refer to the avr-libc manual for more information on the initialization sections.
void get_key_from_boot(void)
{
	bootKeyPtrVal = *bootKeyPtr;
}
