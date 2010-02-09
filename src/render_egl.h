/* MegaZeux
 *
 * Copyright (C) 2009 Alistair John Strachan <alistair@devzero.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef __RENDER_EGL_H
#define __RENDER_EGL_H

#include "compat.h"

__M_BEGIN_DECLS

#include "render_gl.h"

#define GL_LOAD_SYM_EXT(OBJ,FUNC)               \
  OBJ->FUNC = (void *)eglGetProcAddress(#FUNC); \
  if(!OBJ->FUNC)                                \
    return false;                               \

#define GL_LOAD_SYM GL_LOAD_SYM_EXT

#define GL_CAN_USE true

bool gl_set_video_mode(struct graphics_data *graphics, int width, int height,
 int depth, bool fullscreen, bool resize);
bool gl_check_video_mode(struct graphics_data *graphics, int width, int height,
 int depth, bool fullscreen, bool resize);
void gl_set_attributes(struct graphics_data *graphics);
bool gl_swap_buffers(struct graphics_data *graphics);

__M_END_DECLS

#endif // __RENDER_EGL_H