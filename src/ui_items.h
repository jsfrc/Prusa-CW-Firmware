#pragma once

#include "hardware.h"
#include "i18n.h"
#include "states_items.h"
#include "simple_print.h"

namespace UI {

	// UI::Base
	class Base {
	public:
		Base(const char* label, uint8_t last_char = RIGHT_CHAR);
		virtual char* get_menu_label(char* buffer, uint8_t buffer_size);
		virtual void show();
		virtual void loop();
		virtual void invoke();
		virtual void leave();
		Base* process_events(Events& events);
		virtual void event_cover_opened();
		virtual void event_cover_closed();
		virtual void event_tank_inserted();
		virtual void event_tank_removed();
		virtual Base* event_button_short_press();
		virtual Base* event_button_long_press();
		virtual void event_control_up();
		virtual void event_control_down();
		virtual Base* in_menu_action();
	protected:
		const char* label;
		uint8_t last_char;
	};


	// UI::Text
	class Text : public Base {
	public:
		Text(const char* label);
		Base* in_menu_action();
	};


	// UI:SN
	class SN : public Text {
	public:
		SN(const char* label, const char* sn);
		char* get_menu_label(char* buffer, uint8_t buffer_size);
	private:
		const char* const sn;
	};


	// UI:Live_value
	template<class T>
	class Live_value : public Text, public SimplePrint {
	public:
		Live_value(const char* label, T& value);
		char* get_menu_label(char* buffer, uint8_t buffer_size);
	private:
		void write(uint8_t c);
		T& value;
		char* end;
		int8_t size;
	};


	// UI::Menu
	class Menu : public Base {
	public:
		Menu(const char* label, Base* const* items, uint8_t items_count);
		void show();
		void invoke();
		void event_tank_inserted();
		void event_tank_removed();
		Base* event_button_short_press();
		Base* event_button_long_press();
		void event_control_up();
		void event_control_down();
		void set_long_press_ui_item(Base *ui_item);
	private:
		Base* const* const items;
		Base* long_press_ui_item;
		uint8_t const items_count;
		uint8_t menu_offset;
		uint8_t cursor_position;
		uint8_t max_items;
	};


	// UI::Menu_self_redraw
	class Menu_self_redraw : public Menu {
	public:
		Menu_self_redraw(const char* label, Base* const* items, uint8_t items_count, uint16_t redraw_us);
		void show();
		void loop();
	private:
		uint16_t redraw_us;
		unsigned long us_last;
	};


	// UI::Value
	class Value : public Base {
	public:
		Value(const char* label, uint8_t& value, const char* units, uint8_t max, uint8_t min = 1);
		void show();
		Base* event_button_short_press();
		void event_control_up();
		void event_control_down();
	protected:
		const char* units;
		uint8_t& value;
		uint8_t max_value;
		uint8_t min_value;
	};

	class X_of_ten : public Value {
	public:
		X_of_ten(const char* label, uint8_t& value);
	};

	class Minutes : public Value {
	public:
		Minutes(const char* label, uint8_t& value, uint8_t max = 10);
	};

	class Percent : public Value {
	public:
		Percent(const char* label, uint8_t& value, uint8_t min = 0);
	};

	class Temperature : public Value {
	public:
		Temperature(const char* label, uint8_t& value);
		void init(bool SI);
		void units_change(bool SI);
	};


	// UI:Percent_with_action
	class Percent_with_action : public Percent {
	public:
		Percent_with_action(const char* label, uint8_t& value, uint8_t min, void (*value_setter)(uint8_t));
		void event_control_up();
		void event_control_down();
	private:
		void (*value_setter)(uint8_t);
	};


	// UI:Bool
	class Bool : public Base {
	public:
		Bool(const char* label, uint8_t& value, const char* true_text = pgmstr_on, const char* false_text = pgmstr_off);
		char* get_menu_label(char* buffer, uint8_t buffer_size);
		Base* in_menu_action();
	protected:
		const char* true_text;
		const char* false_text;
		uint8_t& value;
	};


	// UI::SI_switch
	class SI_switch : public Bool {
	public:
		SI_switch(const char* label, uint8_t& value, Temperature* const* to_change, uint8_t to_change_count);
		Base* in_menu_action();
	private:
		Temperature* const* to_change;
		uint8_t to_change_count;
	};


	// UI::Option
	class Option : public Base {
	public:
		Option(const char* label, uint8_t& value, const char** options, uint8_t options_count);
		void show();
		Base* event_button_short_press();
		void event_control_up();
		void event_control_down();
	private:
		uint8_t& value;
		const char** const options;
		uint8_t const options_count;
	};


	// UI::State
	class State : public Base {
	public:
		State(const char* label, States::Base* state, Base* state_menu = nullptr);
		void show();
		void loop();
		void invoke();
		void leave();
		Base* event_button_short_press();
		Base* event_button_long_press();
		void event_cover_opened();
		void event_cover_closed();
		void event_tank_inserted();
		void event_tank_removed();
		void event_control_up();
		void event_control_down();
	protected:
		States::Base* state;
		Base* const state_menu;
	private:
		void clear_time_boundaries();
		const char* old_title;
		const char* old_message;
		uint16_t old_time;
		unsigned long spin_us_last;
		unsigned long bound_us_last;
		uint8_t spin_count;
	};


	// UI::Do_it
	class Do_it : public State {
	public:
		Do_it(const char* label, uint8_t& curing_machine_mode, Base* state_menu = nullptr);
		char* get_menu_label(char* buffer, uint8_t buffer_size);
		void invoke();
	private:
		uint8_t& curing_machine_mode;
	};


	// UI::Pause
	class Pause : public Base {
	public:
		Pause(Base* back);
		char* get_menu_label(char* buffer, uint8_t buffer_size);
		Base* in_menu_action();
	private:
		Base* back;
	};

}
