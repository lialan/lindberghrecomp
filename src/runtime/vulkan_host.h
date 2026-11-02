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
uint32_t lvk_reserve_names(uint32_t n);                             // object names, from any thread
void lvk_adopt_buffer_names(uint32_t n, const uint32_t *names);     // make reserved names empty buffers
#ifdef __cplusplus
}
#endif
