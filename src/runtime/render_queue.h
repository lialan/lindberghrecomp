#pragma once
#include <stdint.h>

/* Shared guest-graphics queue. GL supplies context ownership callbacks;
 * Vulkan uses the same snapshots/state filtering with no current context. */
int glq_active(void);
int glq_start(int (*release)(void), int (*take)(void));
uint32_t glq_job(void (*job)(void *), void *arg);
void glq_wait(uint32_t seq);
void glq_drain(void);
void glq_report(unsigned frames);
