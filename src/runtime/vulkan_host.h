#pragma once
#include <stdint.h>
struct SDL_Window;
#ifdef __cplusplus
extern "C" {
#endif
int lvk_host_init(struct SDL_Window *window, int width, int height);
int lvk_host_can_thread(void);
int lvk_host_thread_enter(void);
void lvk_host_present(void);
void lvk_host_input(void); // consume renderer-published input on the game thread
unsigned lvk_create_drawable(int width, int height);
void lvk_destroy_drawable(unsigned id);
void lvk_bind_drawable(unsigned id);
#ifdef __cplusplus
}
#endif
