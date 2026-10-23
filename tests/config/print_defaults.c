/* Prints "name=value" for every option's built-in default, as cake-adapt -D does. */
#include "config/config.h"
#include "config/defaults.h"
#include "constants/options.h"

#include <stdio.h>

int main(void)
{
	struct config defaults = { 0 };
	char value[CONFIG_STRING_SIZE];

	defaults_apply(&defaults);
	for (size_t index = 0U; index < config_option_count(); index++) {
		(void)config_option_value(&defaults, index, value, sizeof(value));
		(void)printf("%s=%s\n", config_option_name(index), value);
	}
	return 0;
}
