/*
 * mono-jit-debug.c: Helper API for debugging jit with third party software.
 *
 * Copyright (C) 2014 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <mono/utils/mono-jit-debug.h>
#include <mono/utils/mono-dl.h>
#include <mono/utils/valgrind.h>


#ifdef HAVE_VTUNE
#include <jitprofiling.h>
#endif

#ifdef VALGRIND_JIT_REGISTER_MAP
static gboolean valgrind_register = FALSE;
#endif

#ifdef HAVE_VTUNE
static void
vtune_register_code (const char *name, gpointer start, guint len)
{
	iJIT_Method_Load m = {0};

	if (iJIT_IsProfilingActive () != iJIT_SAMPLING_ON) {
		return;
	}

	m.method_id = iJIT_GetNewMethodID ();
	m.method_name = (char*) name;
	m.method_load_address = start;
	m.method_size = len;

	iJIT_NotifyEvent (iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&m); 
}

static void
vtune_register_method (MonoCompile *cfg)
{
	iJIT_Method_Load m = {0};
	char *nm;

	if (iJIT_IsProfilingActive () != iJIT_SAMPLING_ON) {
		return;
	}

	nm = mono_method_full_name (cfg->method, TRUE);

	m.method_id = iJIT_GetNewMethodID ();
	m.method_name = nm;
	m.method_load_address = cfg->native_code;
	m.method_size = cfg->code_size;

	iJIT_NotifyEvent (iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&m);

	g_free (nm);
}
#endif

gboolean
mono_running_on_valgrind (void)
{
	if (RUNNING_ON_VALGRIND)
		return TRUE;
	return FALSE;
}

#ifdef VALGRIND_JIT_REGISTER_MAP
static void
valgrind_init (void)
{
	if (RUNNING_ON_VALGRIND) {
		valgrind_register = TRUE;
	}
}

static void
valgrind_register_code (const char *name, gpointer start, guint len)
{
	if (valgrind_register) {
		VALGRIND_JIT_REGISTER_MAP (name, start, start + len);
	}
}

static void
valgrind_register_method (MonoCompile *cfg)
{
	if (valgrind_register) {
		char *nm = mono_method_full_name (cfg->method, TRUE);
		VALGRIND_JIT_REGISTER_MAP (nm, cfg->native_code, cfg->native_code + cfg->code_len);
		g_free (nm);
	}
}
#endif

void
mono_jit_debug_init (void)
{
#ifdef VALGRIND_JIT_REGISTER_MAP
	valgrind_init ();
#endif
}

void
mono_jit_debug_register_code (const char *name, gpointer start, guint len)
{
#ifdef HAVE_VTUNE
	vtune_register_code (name, start, len);
#endif
#ifdef VALGRIND_JIT_REGISTER_MAP
	valgrind_register_code (name, start, len);
#endif
}

void
mono_jit_debug_register_method (MonoCompile *cfg)
{
#ifdef HAVE_VTUNE
	vtune_register_method (cfg);
#endif
#ifdef VALGRIND_JIT_REGISTER_MAP
	valgrind_register_method (cfg);
#endif
}
