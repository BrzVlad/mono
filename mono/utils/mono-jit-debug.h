/*
 * mono-jit-debug.h: Helper API for debugging jit with third party software.
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

#ifndef __MONO_VTUNE_H__
#define __MONO_VTUNE_H__

#include <mono/mini/mini.h>

void mono_jit_debug_init (void);
void mono_jit_debug_register_method (MonoCompile *cfg);
void mono_jit_debug_register_code (const char *name, gpointer start, guint len);
gboolean mono_running_on_valgrind (void);

#endif
