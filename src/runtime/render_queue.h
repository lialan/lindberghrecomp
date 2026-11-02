#pragma once
#include <stdint.h>

/* Shared guest-graphics queue. GL supplies context ownership callbacks;
 * Vulkan uses the same snapshots/state filtering with no current context. */
int glq_active(void);
int glq_start(int (*release)(void), int (*take)(void));
/* A host that hands out object names on any thread (the native Vulkan one)
 * registers them before glq_start, so glGen* need not wait for the worker. */
void glq_set_name_source(uint32_t (*reserve)(uint32_t n), void (*adopt_buffers)(uint32_t n, const uint32_t *names));
uint32_t glq_job(void (*job)(void *), void *arg);
void glq_wait(uint32_t seq);
void glq_drain(void);
void glq_report(unsigned frames);
