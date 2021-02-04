/* gskgluniformstate.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gsk/gskroundedrectprivate.h>
#include <string.h>

#include "gskgluniformstateprivate.h"

typedef struct { float v0; } Uniform1f;
typedef struct { float v0; float v1; } Uniform2f;
typedef struct { float v0; float v1; float v2; } Uniform3f;
typedef struct { float v0; float v1; float v2; float v3; } Uniform4f;

typedef struct { int v0; } Uniform1i;
typedef struct { int v0; int v1; } Uniform2i;
typedef struct { int v0; int v1; int v2; } Uniform3i;
typedef struct { int v0; int v1; int v2; int v3; } Uniform4i;

typedef struct { guint v0; } Uniform1ui;

static guint8 uniform_sizes[] = {
  0,

  sizeof (Uniform1f),
  sizeof (Uniform2f),
  sizeof (Uniform3f),
  sizeof (Uniform4f),

  sizeof (Uniform1f),
  sizeof (Uniform2f),
  sizeof (Uniform3f),
  sizeof (Uniform4f),

  sizeof (Uniform1i),
  sizeof (Uniform2i),
  sizeof (Uniform3i),
  sizeof (Uniform4i),

  sizeof (Uniform1ui),

  sizeof (guint),

  sizeof (graphene_matrix_t),
  sizeof (GskRoundedRect),
  sizeof (GdkRGBA),

  0,
};

#define REPLACE_UNIFORM(info, u, format, count)                                         \
  G_STMT_START {                                                                        \
    guint offset;                                                                       \
    if ((info)->initial && count == (info)->array_count)                                \
      {                                                                                 \
        u = (gpointer)(state->values_buf + (info)->offset);                             \
      }                                                                                 \
    else                                                                                \
      {                                                                                 \
        g_assert (uniform_sizes[format] > 0);                                           \
        u = alloc_uniform_data(state, uniform_sizes[format] * MAX (1, count), &offset); \
        (info)->offset = offset;                                                        \
        /* We might have increased array length */                                      \
        (info)->array_count = count;                                                    \
      }                                                                                 \
  } G_STMT_END

static inline gboolean
rounded_rect_equal (const GskRoundedRect *r1,
                    const GskRoundedRect *r2)
{
  /* Ensure we're dealing with tightly packed floats that
   * should allow us to compare without any gaps using memcmp().
   */
  G_STATIC_ASSERT (sizeof *r1 == (sizeof (float) * 12));

  if (r1 == r2)
    return TRUE;

  if (r1 == NULL)
    return FALSE;

  return memcmp (r1, r2, sizeof *r1) == 0;
}

static void
clear_program_info (gpointer data)
{
  GskGLUniformProgram *program_info = data;

  g_clear_pointer (&program_info->uniform_info, g_array_unref);
  g_clear_pointer (&program_info->changed, g_array_unref);
}

GskGLUniformState *
gsk_gl_uniform_state_new (void)
{
  GskGLUniformState *state;

  state = g_atomic_rc_box_new0 (GskGLUniformState);
  state->program_info = g_array_new (FALSE, TRUE, sizeof (GskGLUniformProgram));
  state->values_len = 4096;
  state->values_pos = 0;
  state->values_buf = g_malloc (4096);

  g_array_set_clear_func (state->program_info, clear_program_info);

  return g_steal_pointer (&state);
}

GskGLUniformState *
gsk_gl_uniform_state_ref (GskGLUniformState *state)
{
  return g_atomic_rc_box_acquire (state);
}

static void
gsk_gl_uniform_state_finalize (gpointer data)
{
  GskGLUniformState *state = data;

  g_clear_pointer (&state->program_info, g_array_unref);
  g_clear_pointer (&state->values_buf, g_free);
}

void
gsk_gl_uniform_state_unref (GskGLUniformState *state)
{
  g_atomic_rc_box_release_full (state, gsk_gl_uniform_state_finalize);
}

static inline void
program_changed (GskGLUniformState *state,
                 GskGLUniformInfo  *info,
                 guint              program,
                 guint              location)
{
  if (info->changed == FALSE)
    {
      const GskGLUniformProgram *program_info = &g_array_index (state->program_info, GskGLUniformProgram, program);

      g_assert (program < state->program_info->len);

      info->changed = TRUE;
      info->initial = FALSE;

      g_array_append_val (program_info->changed, location);
    }
}

void
gsk_gl_uniform_state_clear_program (GskGLUniformState *state,
                                    guint              program)
{
  GskGLUniformProgram *program_info;

  g_return_if_fail (state != NULL);

  if (program == 0 || program >= state->program_info->len)
    return;

  program_info = &g_array_index (state->program_info, GskGLUniformProgram, program);

  g_clear_pointer (&program_info->changed, g_array_unref);
  g_clear_pointer (&program_info->uniform_info, g_array_unref);
}

static inline guint
alloc_alignment (guint current_pos,
                 guint size)
{
  guint align = size > 8 ? 16 : (size > 4 ? 8 : 4);
  guint masked = current_pos & (align - 1);

  g_assert (size > 0);
  g_assert (align == 4 || align == 8 || align == 16);
  g_assert (masked < align);

  return align - masked;
}

static gpointer
alloc_uniform_data (GskGLUniformState *state,
                    guint              size,
                    guint             *offset)
{
  guint padding = alloc_alignment (state->values_pos, size);

  if G_UNLIKELY (state->values_len - padding - size < state->values_pos)
    {
      state->values_len *= 2;
      state->values_buf = g_realloc (state->values_buf, state->values_len);
    }

  *offset = state->values_pos + padding;
  state->values_pos += padding + size;

  return state->values_buf + *offset;
}

static gpointer
get_uniform (GskGLUniformState  *state,
             guint               program,
             GskGLUniformFormat  format,
             guint               array_count,
             guint               location,
             GskGLUniformInfo  **infoptr)
{
  GskGLUniformProgram *program_info;
  GskGLUniformInfo *info;
  guint offset;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (array_count < 256);
  g_assert ((int)format >= 0 && format < GSK_GL_UNIFORM_FORMAT_LAST);
  g_assert (format > 0);
  g_assert (location < GL_MAX_UNIFORM_LOCATIONS || location == (guint)-1);

  /* Handle unused uniforms gracefully */
  if G_UNLIKELY (location == (guint)-1)
    return NULL;

  /* Fast path for common case (state already initialized) */
  if G_LIKELY (program < state->program_info->len &&
               (program_info = &g_array_index (state->program_info, GskGLUniformProgram, program)) &&
               program_info->uniform_info != NULL &&
               location < program_info->uniform_info->len)
    {
      info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, location);

      if G_LIKELY (format == info->format)
        {
          if G_LIKELY (array_count <= info->array_count)
            {
              *infoptr = info;
              return state->values_buf + info->offset;
            }

          /* We found the uniform, but there is not enough space for the
           * amount that was requested. Instead, allocate new space and
           * set the value to "initial" so that the caller just writes
           * over the previous value.
           *
           * This can happen when using dynamic array lengths like the
           * "n_color_stops" in gradient shaders.
           */
          goto setup_info;
        }
      else if (info->format == 0)
        {
          goto setup_info;
        }
      else
        {
          g_critical ("Attempt to access uniform with different type of value "
                      "than it was initialized with. Program %u Location %u. "
                      "Was %d now %d (array length %d now %d).",
                      program, location, info->format, format,
                      info->array_count, array_count);
          *infoptr = NULL;
          return NULL;
        }
    }

setup_info:

  if (program >= state->program_info->len ||
      g_array_index (state->program_info, GskGLUniformProgram, program).uniform_info == NULL)
    {
      if (program >= state->program_info->len)
        g_array_set_size (state->program_info, program + 1);

      program_info = &g_array_index (state->program_info, GskGLUniformProgram, program);
      program_info->uniform_info = g_array_new (FALSE, TRUE, sizeof (GskGLUniformInfo));
      program_info->changed = g_array_new (FALSE, FALSE, sizeof (guint));
    }

  g_assert (program_info != NULL);
  g_assert (program_info->uniform_info != NULL);

  if (location >= program_info->uniform_info->len)
    {
      guint prev = program_info->uniform_info->len;

      g_array_set_size (program_info->uniform_info, location + 1);

      for (guint i = prev; i < program_info->uniform_info->len; i++)
        g_array_index (program_info->uniform_info, GskGLUniformInfo, i).initial = TRUE;
    }

  alloc_uniform_data (state,
                      uniform_sizes[format] * MAX (1, array_count),
                      &offset);

  info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, location);
  info->changed = FALSE;
  info->format = format;
  info->offset = offset;
  info->array_count = array_count;
  info->initial = TRUE;

  *infoptr = info;

  return state->values_buf + offset;
}

void
gsk_gl_uniform_state_set1f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0)
{
  Uniform1f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1F, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1F, 1);
          u->v0 = value0;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set2f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1)
{
  Uniform2f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2F, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2F, 1);
          u->v0 = value0;
          u->v1 = value1;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set3f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1,
                            float              value2)
{
  Uniform3f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3F, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1 || u->v2 != value2)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3F, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set4f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1,
                            float              value2,
                            float              value3)
{
  Uniform4f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4F, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1 || u->v2 != value2 || u->v3 != value3)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4F, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          u->v3 = value3;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set1ui (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              value0)
{
  Uniform1ui *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1UI, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1UI, 1);
          u->v0 = value0;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set1i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0)
{
  Uniform1i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1I, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1I, 1);
          u->v0 = value0;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set2i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1)
{
  Uniform2i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2I, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2I, 1);
          u->v0 = value0;
          u->v1 = value1;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set3i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1,
                            int                value2)
{
  Uniform3i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3I, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1 || u->v2 != value2)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3I, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set4i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1,
                            int                value2,
                            int                value3)
{
  Uniform4i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4I, 1, location, &info)))
    {
      if (info->initial || u->v0 != value0 || u->v1 != value1 || u->v2 != value2 || u->v3 != value3)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4I, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          u->v3 = value3;
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set_rounded_rect (GskGLUniformState    *state,
                                       guint                 program,
                                       guint                 location,
                                       const GskRoundedRect *rounded_rect)
{
  GskRoundedRect *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (rounded_rect != NULL);
  g_assert (uniform_sizes[GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT] == sizeof *rounded_rect);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT, 1, location, &info)))
    {
      if (info->initial || !rounded_rect_equal (rounded_rect, u))
        {
          g_assert (!info->send_corners || info->changed);

          if (!info->send_corners)
            {
              if (info->initial ||
                  !graphene_size_equal (&u->corner[0], &rounded_rect->corner[0]) ||
                  !graphene_size_equal (&u->corner[1], &rounded_rect->corner[1]) ||
                  !graphene_size_equal (&u->corner[2], &rounded_rect->corner[2]) ||
                  !graphene_size_equal (&u->corner[3], &rounded_rect->corner[3]))
                info->send_corners = TRUE;
            }

          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT, 1);

          memcpy (u, rounded_rect, sizeof *rounded_rect);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set_matrix (GskGLUniformState       *state,
                                 guint                    program,
                                 guint                    location,
                                 const graphene_matrix_t *matrix)
{
  graphene_matrix_t *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (matrix != NULL);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_MATRIX, 1, location, &info)))
    {
      if (!info->initial && graphene_matrix_equal_fast (u, matrix))
        return;

      REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_MATRIX, 1);
      memcpy (u, matrix, sizeof *matrix);
      program_changed (state, info, program, location);
    }
}

/**
 * gsk_gl_uniform_state_set_texture:
 * @state: a #GskGLUniformState
 * @program: the program id
 * @location: the location of the texture
 * @texture_slot: a texturing slot such as GL_TEXTURE0
 *
 * Sets the uniform expecting a texture to @texture_slot. This API
 * expects a texture slot such as GL_TEXTURE0 to reduce chances of
 * miss-use by the caller.
 *
 * The value stored to the uniform is in the form of 0 for GL_TEXTURE0,
 * 1 for GL_TEXTURE1, and so on.
 */
void
gsk_gl_uniform_state_set_texture (GskGLUniformState *state,
                                  guint              program,
                                  guint              location,
                                  guint              texture_slot)
{
  GskGLUniformInfo *info;
  guint *u;

  g_assert (texture_slot >= GL_TEXTURE0);
  g_assert (texture_slot < GL_TEXTURE16);

  texture_slot -= GL_TEXTURE0;

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_TEXTURE, 1, location, &info)))
    {
      if (*u != texture_slot || info->initial)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_TEXTURE, 1);
          *u = texture_slot;
          program_changed (state, info, program, location);
        }
    }
}

/**
 * gsk_gl_uniform_state_set_color:
 * @state: a #GskGLUniformState
 * @program: a program id > 0
 * @location: the uniform location
 * @color: a color to set or %NULL for transparent
 *
 * Sets a uniform to the color described by @color. This is a convenience
 * function to allow callers to avoid having to translate colors to floats
 * in other portions of the renderer.
 */
void
gsk_gl_uniform_state_set_color (GskGLUniformState *state,
                                guint              program,
                                guint              location,
                                const GdkRGBA     *color)
{
  static const GdkRGBA transparent = {0};
  GskGLUniformInfo *info;
  GdkRGBA *u;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_COLOR, 1, location, &info)))
    {
      if (color == NULL)
        color = &transparent;

      if (info->initial || !gdk_rgba_equal (u, color))
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_COLOR, 1);
          memcpy (u, color, sizeof *color);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set1fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform1f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (count > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1FV, count, location, &info)))
    {
      gboolean changed = info->initial || memcmp (u, value, sizeof *u  * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1FV, count);
          memcpy (u, value, sizeof (Uniform1f) * count);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set2fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform2f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (count > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2FV, count, location, &info)))
    {
      gboolean changed = info->initial || memcmp (u, value, sizeof *u * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2FV, count);
          memcpy (u, value, sizeof (Uniform2f) * count);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set3fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform3f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (count > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3FV, count, location, &info)))
    {
      gboolean changed = info->initial || memcmp (u, value, sizeof *u * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3FV, count);
          memcpy (u, value, sizeof (Uniform3f) * count);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_set4fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform4f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (count > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4FV, count, location, &info)))
    {
      gboolean changed = info->initial || memcmp (u, value, sizeof *u * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4FV, count);
          memcpy (u, value, sizeof (Uniform4f) * count);
          program_changed (state, info, program, location);
        }
    }
}

void
gsk_gl_uniform_state_end_frame (GskGLUniformState *state)
{
  guint allocator = 0;

  g_return_if_fail (state != NULL);

  /* After a frame finishes, we want to remove all our copies of uniform
   * data that isn't needed any longer. Since we treat it as uninitialized
   * after this frame (to reset it on first use next frame) we can just
   * discard it but keep an allocation around to reuse.
   */

  for (guint i = 0; i < state->program_info->len; i++)
    {
      const GskGLUniformProgram *program_info = &g_array_index (state->program_info, GskGLUniformProgram, i);

      if (program_info->uniform_info == NULL)
        continue;

      for (guint j = 0; j < program_info->uniform_info->len; j++)
        {
          GskGLUniformInfo *info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, j);
          guint size;

          if (info->format == 0)
            continue;

          /* Calculate how much size is needed for the uniform, including arrays */
          size = uniform_sizes[info->format] * MAX (1, info->array_count);

          /* Adjust alignment for value */
          allocator += alloc_alignment (allocator, size);

          info->offset = allocator;
          info->changed = FALSE;
          info->initial = TRUE;
          info->send_corners = FALSE;

          /* Now advance for this items data */
          allocator += size;
        }

      program_info->changed->len = 0;
    }

  state->values_pos = allocator;

  g_assert (allocator <= state->values_len);
}

gsize
gsk_gl_uniform_format_size (GskGLUniformFormat format)
{
  g_assert (format > 0);
  g_assert (format < GSK_GL_UNIFORM_FORMAT_LAST);

  return uniform_sizes[format];
}
