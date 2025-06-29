
#ifndef _INPUT_V4L2_H
#define _INPUT_V4L2_H

#include "input.h"

int input_init(input_parameter *param, int id);
int input_stop();
int input_cmd(int plugin, unsigned int control, int value);
void input_help();
int input_parse_input(char *line);

#endif
