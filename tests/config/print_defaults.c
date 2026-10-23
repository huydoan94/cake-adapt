/* Prints "name=value" for every option's built-in default: cake-adapt -D. */
#include "config/describe.h"

int main(void)
{
	return config_print_defaults();
}
