/* input.h - @TITLE@ keyboard layout and input wiring (see input.c). */
#ifndef @PREFIX@_INPUT_H
#define @PREFIX@_INPUT_H

/* Install the @TITLE@ keyboard layout. The keyboard answers as player 1 while
 * RECOMP_KEYBOARD=1 (main.c sets it by default). */
void game_input_init(void);

#endif
