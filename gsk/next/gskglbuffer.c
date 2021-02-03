/* gskglbufferprivate.h
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "gskglbufferprivate.h"

/**
 * gsk_gl_buffer_new:
 * @target: the target buffer such as %GL_ARRAY_BUFFER or %GL_UNIFORM_BUFFER
 * @element_size: the size of elements within the buffer
 *
 * Creates a new #GskGLBuffer which can be used to deliver data to shaders
 * within a GLSL program. You can use this to store vertices such as with
 * %GL_ARRAY_BUFFER or uniform data with %GL_UNIFORM_BUFFER.
 *
 * Note that only writing to this buffer is allowed (see %GL_WRITE_ONLY for
 * more details).
 *
 * The buffer will be bound to target upon returning from this function.
 */
GskGLBuffer *
gsk_gl_buffer_new (GLenum target,
                   guint  element_size)
{
  GskGLBuffer *buffer;

  buffer = g_new0 (GskGLBuffer, 1);
  buffer->buffer = g_malloc (8092);
  buffer->buffer_len = 8092;
  buffer->buffer_pos = 0;
  buffer->target = target;
  buffer->element_size = element_size;
  buffer->count = 0;

  return g_steal_pointer (&buffer);
}

GLuint
gsk_gl_buffer_submit (GskGLBuffer *buffer)
{
  GLuint id;

  glGenBuffers (1, &id);
  glBindBuffer (buffer->target, id);
  glBufferData (buffer->target, buffer->buffer_pos, buffer->buffer, GL_STATIC_DRAW);

  buffer->buffer_pos = 0;
  buffer->count = 0;

  return id;
}

void
gsk_gl_buffer_free (GskGLBuffer *buffer)
{
  g_free (buffer->buffer);
  g_free (buffer);
}
