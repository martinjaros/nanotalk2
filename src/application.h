/*
 * Copyright (C) 2016 - Martin Jaros <xjaros32@stud.feec.vutbr.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __APPLICATION_H__
#define __APPLICATION_H__

#include "dhtclient.h"

typedef struct _application Application;

#if ENABLE_GUI

void application_add_option_group(GOptionContext *context);
gboolean application_init(GError **error);

Application* application_new(DhtClient *client, const gchar *aliases_path, const gchar *sound_file);
void application_free(Application *app);

void application_run();

#else /* !ENABLE_GUI */
#define application_add_option_group(context)
#define application_init(error) TRUE
#define application_new(client, arg1, arg2) ((Application*)g_object_ref(client))
#define application_free(app) g_object_unref(app)
static inline void application_run()
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
}
#endif  /* !ENABLE_GUI */

#endif /* __APPLICATION_H__ */
