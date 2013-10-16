/*
 * Copyright (c) 2013 Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Reference:
 * ARM document DEN 0028A: SMC CALLING CONVENTION
 * version: 0.9.0
 */

#include <compiler.h>
#include <debug.h>
#include <err.h>
#include <trace.h>
#include <kernel/mutex.h>
#include <lib/sm.h>
#include <lib/sm/smcall.h>
#include <lib/sm/sm_err.h>
#include <lk/init.h>

#define LOCAL_TRACE	1

static mutex_t smc_table_lock = MUTEX_INITIAL_VALUE(smc_table_lock);

/* Defined elsewhere */
long smc_go_nonsecure(smc32_args_t *args);
long smc_fiq_exit(smc32_args_t *args);
long smc_fastcall_secure_monitor(smc32_args_t *args);

#define TRACE_SMC(msg, args)	do {			\
	u_int _i;					\
	LTRACEF("%s\n", msg);				\
	LTRACEF("SMC: 0x%x (%s entity %d function 0x%x)\n", \
			(args)->smc_nr,			\
			SMC_IS_FASTCALL(args->smc_nr) ? "Fastcall" : "Stdcall", \
			SMC_ENTITY(args->smc_nr), SMC_FUNCTION(args->smc_nr)); \
	for(_i = 0; _i < SMC_NUM_PARAMS; _i++)		\
		LTRACEF("param%d: 0x%x\n", _i, (args)->params[_i]); \
} while (0)

long smc_undefined(smc32_args_t *args)
{
	TRACE_SMC("Undefined monitor call!", args);
	return SM_ERR_UNDEFINED_SMC;
}

/* Restarts should never be dispatched like this */
static long smc_restart_stdcall(smc32_args_t *args)
{
	TRACE_SMC("Unexpected stdcall restart!", args);
	return SM_ERR_UNEXPECTED_RESTART;
}

static smc32_handler_t sm_stdcall_function_table[] = {
	[SMC_FUNCTION(SMC_SC_RESTART_LAST)] = smc_restart_stdcall,
};

static long smc_stdcall_secure_monitor(smc32_args_t *args)
{
	u_int function = SMC_FUNCTION(args->smc_nr);
	smc32_handler_t handler_fn = NULL;

	if (function < countof(sm_stdcall_function_table))
		handler_fn = sm_stdcall_function_table[function];

	if (!handler_fn)
		handler_fn = smc_undefined;

	return handler_fn(args);
}

smc32_handler_t sm_fastcall_function_table[] = {
	[SMC_FUNCTION(SMC_FC_GO_NONSECURE)] = smc_go_nonsecure,
	[SMC_FUNCTION(SMC_FC_REQUEST_FIQ)] = smc_intc_request_fiq,
	[SMC_FUNCTION(SMC_FC_FIQ_EXIT)] = smc_fiq_exit,
};

uint32_t sm_nr_fastcall_functions = countof(sm_fastcall_function_table);

/* SMC dispatch tables */
smc32_handler_t sm_fastcall_table[SMC_NUM_ENTITIES] = {
	[0 ... SMC_ENTITY_SECURE_MONITOR - 1] = smc_undefined,
	[SMC_ENTITY_SECURE_MONITOR] = smc_fastcall_secure_monitor,
	[SMC_ENTITY_SECURE_MONITOR + 1 ... SMC_NUM_ENTITIES - 1] = smc_undefined
};

smc32_handler_t sm_stdcall_table[SMC_NUM_ENTITIES] = {
	[0 ... SMC_ENTITY_SECURE_MONITOR - 1] = smc_undefined,
	[SMC_ENTITY_SECURE_MONITOR] = smc_stdcall_secure_monitor,
	[SMC_ENTITY_SECURE_MONITOR + 1 ... SMC_NUM_ENTITIES - 1] = smc_undefined
};

status_t sm_register_entity(uint entity_nr, smc32_entity_t *entity)
{
	status_t err = NO_ERROR;

	if (entity_nr >= SMC_NUM_ENTITIES)
		return ERR_INVALID_ARGS;

	if (entity_nr >= SMC_ENTITY_RESERVED && entity_nr < SMC_ENTITY_TRUSTED_APP)
		return ERR_NOT_ALLOWED;

	if (!entity)
		return ERR_INVALID_ARGS;

	if (!entity->fastcall_handler && !entity->stdcall_handler)
		return ERR_NOT_VALID;

	mutex_acquire(&smc_table_lock);

	/* Check if entity is already claimed */
	if (sm_fastcall_table[entity_nr] != smc_undefined ||
		sm_stdcall_table[entity_nr] != smc_undefined) {
		err = ERR_ALREADY_EXISTS;
		goto unlock;
	}

	if (entity->fastcall_handler)
		sm_fastcall_table[entity_nr] = entity->fastcall_handler;

	if (entity->stdcall_handler)
		sm_stdcall_table[entity_nr] = entity->stdcall_handler;
unlock:
	mutex_release(&smc_table_lock);
	return err;
}