#include "reed.h"

#include "driver/gpio.h"

namespace reed {

static gpio_num_t s_pin = GPIO_NUM_NC;

void init(gpio_num_t pin) {
	s_pin = pin;

	const gpio_config_t sensor_cfg = {
			.pin_bit_mask = (1ULL << pin),
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = GPIO_PULLUP_ENABLE,
			.pull_down_en = GPIO_PULLDOWN_DISABLE,
			.intr_type = GPIO_INTR_DISABLE,
	};

	gpio_config(&sensor_cfg);
}

bool is_door_open() {
	if (s_pin == GPIO_NUM_NC) {
		return false;
	}
	return gpio_get_level(s_pin) == 1;
}

}  // namespace reed
