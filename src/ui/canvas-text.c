#include "ui/canvas-private.h"

/* The Text tool.
 *
 * While you type, the text is not in the picture. It's laid out with Pango,
 * rendered into `text_clip` and shown above the picture as a texture, so you
 * can keep editing, resizing and moving it. Only when you click away
 * (canvas_text_commit) is it blended into the real pixels as one undo step.
 * Per-character style (bold, colour...) is kept in `text_attr`, one entry per
 * byte of the text. */

gboolean
canvas_text_sel_range (EaselCanvas *self, int *a, int *b)
{
  if (!self->text_active || self->text_anchor < 0 || self->text_anchor == self->text_cursor)
    return FALSE;
  *a = MIN (self->text_anchor, self->text_cursor);
  *b = MAX (self->text_anchor, self->text_cursor);
  return TRUE;
}

/* Tells the toolbox and the Edit menu about the caret and selection, so the
 * Bold/Italic buttons and Cut/Copy can enable or disable themselves. */
static void
text_emit_state (EaselCanvas *self)
{
  guint flags = self->text_typing;
  int a, b;

  if (canvas_text_sel_range (self, &a, &b))
    {
      flags = 0xF;
      for (int i = a; i < b; i++)
        flags &= g_array_index (self->text_attr, TextAttr, i).flags;
    }
  g_signal_emit (self, canvas_signals[SIGNAL_TEXT_STATE], 0, flags);
  g_signal_emit (self, canvas_signals[SIGNAL_SELECTION_CHANGED], 0);
}

static void
text_buf_insert (EaselCanvas *self, int pos, const char *str)
{
  int len = (int) strlen (str);
  TextAttr at = { (guint8) self->text_typing, self->foreground | 0xFF000000u };

  g_string_insert (self->text_buf, pos, str);
  for (int i = 0; i < len; i++)
    g_array_insert_val (self->text_attr, pos + i, at);
}

static void
text_buf_erase (EaselCanvas *self, int pos, int len)
{
  g_string_erase (self->text_buf, pos, len);
  g_array_remove_range (self->text_attr, pos, len);
}

void
canvas_text_free_render (EaselCanvas *self)
{
  g_clear_pointer (&self->text_clip, easel_clip_free);
  g_clear_object (&self->text_texture);
  g_clear_object (&self->text_layout);
}

static uint32_t
text_rgb (uint32_t c)
{
  return c | 0xFF000000u;
}

/* Renders the text into text_clip, which is as big as the box. Smoothing is
 * off unless the user turned it on, to keep the crisp XP look. In Opaque mode
 * the box is filled with the background colour, in Transparent mode only the
 * letters get pixels. The box grows downward if the text needs more room. */
void
canvas_text_render (EaselCanvas *self)
{
  canvas_text_free_render (self);
  if (!self->text_active)
    return;

  PangoContext *ctx = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  cairo_font_options_t *fo = cairo_font_options_create ();
  cairo_font_options_set_antialias (fo, self->text_smooth ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
  cairo_font_options_set_hint_metrics (fo, self->text_smooth ? CAIRO_HINT_METRICS_OFF : CAIRO_HINT_METRICS_ON);
  cairo_font_options_set_hint_style (fo, self->text_smooth ? CAIRO_HINT_STYLE_SLIGHT : CAIRO_HINT_STYLE_FULL);
  pango_cairo_context_set_font_options (ctx, fo);
  pango_cairo_context_set_resolution (ctx, 96);
  cairo_font_options_destroy (fo);

  PangoLayout *layout = pango_layout_new (ctx);
  PangoFontDescription *desc = pango_font_description_from_string (self->font);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);
  pango_layout_set_width (layout, self->text_rect.width * PANGO_SCALE);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_text (layout, self->text_buf->str, (int) self->text_buf->len);
  {
    PangoAttrList *attrs = pango_attr_list_new ();
    guint n = self->text_attr->len, i = 0;

    while (i < n)
      {
        TextAttr a = g_array_index (self->text_attr, TextAttr, i);
        guint j = i + 1;
        while (j < n)
          {
            TextAttr b = g_array_index (self->text_attr, TextAttr, j);
            if (b.flags != a.flags || b.color != a.color)
              break;
            j++;
          }
        PangoAttribute *at[5];
        int k = 0;
        at[k++] = pango_attr_foreground_new (((a.color >> 16) & 0xFF) * 257, ((a.color >> 8) & 0xFF) * 257,
                                             (a.color & 0xFF) * 257);
        if (a.flags & EASEL_TEXT_BOLD)
          at[k++] = pango_attr_weight_new (PANGO_WEIGHT_BOLD);
        if (a.flags & EASEL_TEXT_ITALIC)
          at[k++] = pango_attr_style_new (PANGO_STYLE_ITALIC);
        if (a.flags & EASEL_TEXT_UNDERLINE)
          at[k++] = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
        if (a.flags & EASEL_TEXT_STRIKE)
          at[k++] = pango_attr_strikethrough_new (TRUE);
        for (int m = 0; m < k; m++)
          {
            at[m]->start_index = i;
            at[m]->end_index = j;
            pango_attr_list_insert (attrs, at[m]);
          }
        i = j;
      }
    pango_layout_set_attributes (layout, attrs);
    pango_attr_list_unref (attrs);
  }
  g_object_unref (ctx);

  int lw, lh;
  pango_layout_get_pixel_size (layout, &lw, &lh);
  if (lh > self->text_rect.height)
    self->text_rect.height = lh;

  int w = self->text_rect.width, h = self->text_rect.height;
  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (surf);
  cairo_set_antialias (cr, self->text_smooth ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
  if (self->text_opaque)
    {
      uint32_t bg = text_rgb (self->background);
      cairo_set_source_rgb (cr, ((bg >> 16) & 0xFF) / 255.0, ((bg >> 8) & 0xFF) / 255.0, (bg & 0xFF) / 255.0);
      cairo_paint (cr);
    }
  uint32_t fg = text_rgb (self->foreground);
  cairo_set_source_rgb (cr, ((fg >> 16) & 0xFF) / 255.0, ((fg >> 8) & 0xFF) / 255.0, (fg & 0xFF) / 255.0);
  pango_cairo_show_layout (cr, layout);
  cairo_destroy (cr);
  cairo_surface_flush (surf);

  EaselClip *clip = easel_clip_new (w, h);
  if (clip != NULL)
    {
      const unsigned char *data = cairo_image_surface_get_data (surf);
      int stride = cairo_image_surface_get_stride (surf);

      for (int y = 0; y < h; y++)
        memcpy (clip->pixels + (gsize) y * w, data + (gsize) y * stride, (gsize) w * 4);
      self->text_clip = clip;
      self->text_texture = canvas_texture_from_clip (clip, TRUE, 0, 0);
    }
  cairo_surface_destroy (surf);
  self->text_layout = layout;
  canvas_text_caret_restart (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
text_blink_tick (gpointer data)
{
  EaselCanvas *self = data;

  self->text_caret_on = !self->text_caret_on;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_CONTINUE;
}

void
canvas_text_caret_restart (EaselCanvas *self)
{
  self->text_caret_on = TRUE;
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  if (self->text_active)
    self->text_blink_id = g_timeout_add (EASEL_CARET_BLINK_MS, text_blink_tick, self);
}

static void
text_cancel (EaselCanvas *self)
{
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  self->text_drag = TEXT_DRAG_NONE;
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  canvas_text_free_render (self);
  if (self->text_buf != NULL)
    g_string_truncate (self->text_buf, 0);
  if (self->text_attr != NULL)
    g_array_set_size (self->text_attr, 0);
  self->text_anchor = -1;
  self->text_active = self->text_creating = FALSE;
  self->text_cursor = 0;
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Blends the rendered text into the picture and closes the text box. */
void
canvas_text_commit (EaselCanvas *self)
{
  if (!self->text_active && !self->text_creating)
    return;

  if (self->text_active && self->text_clip != NULL && self->text_buf->len > 0)
    {
      uint32_t *base = canvas_snapshot_take (self);

      /* Normal "source over" blending, so anti-aliased edges mix with what is
       * underneath: result = src + dst * (1 - src_alpha). Pixels are
       * premultiplied, so that works per channel with no extra maths. */
      EaselRect box = self->text_rect, landed = { 0, 0, 0, 0 };
      for (int y = 0; y < self->text_clip->height; y++)
        for (int x = 0; x < self->text_clip->width; x++)
          {
            int dx = box.x + x, dy = box.y + y;
            uint32_t src = self->text_clip->pixels[(gsize) y * self->text_clip->width + x];
            uint32_t a = src >> 24;

            if (dx < 0 || dy < 0 || dx >= self->doc->width || dy >= self->doc->height || a == 0)
              continue;
            uint32_t *dst = &self->doc->pixels[(gsize) dy * self->doc->width + dx];
            if (a != 255)
              {
                uint32_t d = *dst, inv = 255 - a, out = 0;
                for (int sh = 0; sh <= 24; sh += 8)
                  {
                    uint32_t v = ((src >> sh) & 0xFF) + (((d >> sh) & 0xFF) * inv + 127) / 255;
                    out |= MIN (v, 255u) << sh;
                  }
                src = out;
              }
            *dst = src;
            landed = easel_rect_union (landed, (EaselRect) { dx, dy, 1, 1 });
          }
      self->edit_rect = landed;
      canvas_mark_dirty (self, landed);
      canvas_push_undo (self, base);
      canvas_snapshot_drop (self, &base);
    }
  text_cancel (self);
}

static gboolean
text_delete_selection (EaselCanvas *self)
{
  int a, b;

  if (!canvas_text_sel_range (self, &a, &b))
    return FALSE;
  text_buf_erase (self, a, b - a);
  self->text_cursor = a;
  self->text_anchor = -1;
  return TRUE;
}

static void
text_insert (EaselCanvas *self, const char *utf8)
{
  text_delete_selection (self);
  text_buf_insert (self, self->text_cursor, utf8);
  self->text_cursor += (int) strlen (utf8);
  canvas_text_render (self);
  text_emit_state (self);
}

/* Moves the caret to byte offset `index`. With `extend` (Shift held) the old
 * position becomes the other end of the selection. Newly typed text copies the
 * style of the character before the caret, like any word processor. */
void
canvas_text_move_cursor (EaselCanvas *self, int index, gboolean extend)
{
  if (extend && self->text_anchor < 0)
    self->text_anchor = self->text_cursor;
  else if (!extend)
    self->text_anchor = -1;
  self->text_cursor = CLAMP (index, 0, (int) self->text_buf->len);
  if (!extend && self->text_attr->len > 0)
    {
      guint at = self->text_cursor > 0 ? (guint) self->text_cursor - 1 : 0;
      self->text_typing = g_array_index (self->text_attr, TextAttr, at).flags;
    }
  canvas_text_caret_restart (self);
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
canvas_text_index_at (EaselCanvas *self, int ix, int iy)
{
  int idx = 0, trailing = 0;

  if (self->text_layout == NULL)
    return 0;
  pango_layout_xy_to_index (self->text_layout, (ix - self->text_rect.x) * PANGO_SCALE,
                            (iy - self->text_rect.y) * PANGO_SCALE, &idx, &trailing);
  if (trailing > 0)
    idx = (int) (g_utf8_offset_to_pointer (self->text_buf->str + idx, trailing) - self->text_buf->str);
  return idx;
}

/* Turns a style flag (bold, italic...) on or off for the selected text, or for
 * whatever is typed next when nothing is selected. */
static void
text_apply_style (EaselCanvas *self, guint flag, gboolean on)
{
  int a, b;

  if (self->text_active && canvas_text_sel_range (self, &a, &b))
    {
      for (int i = a; i < b; i++)
        {
          TextAttr *at = &g_array_index (self->text_attr, TextAttr, i);
          at->flags = on ? (at->flags | flag) : (at->flags & ~flag);
        }
      canvas_text_render (self);
    }
  else if (on)
    self->text_typing |= flag;
  else
    self->text_typing &= ~flag;
  if (self->text_active)
    text_emit_state (self);
}

/* Colours the selected text, or all of it when nothing is selected. */
void
canvas_text_apply_color (EaselCanvas *self, uint32_t color)
{
  int a = 0, b;

  if (!self->text_active)
    return;
  b = (int) self->text_attr->len;
  canvas_text_sel_range (self, &a, &b);
  for (int i = a; i < b; i++)
    g_array_index (self->text_attr, TextAttr, i).color = color | 0xFF000000u;
  canvas_text_render (self);
}

/* Keyboard handling for the text box. Returns TRUE if the key was used, so
 * the window knows not to treat it as a shortcut. */
gboolean
canvas_text_key (EaselCanvas *self, guint keyval, GdkModifierType state)
{
  const char *str = self->text_buf->str;
  int len = (int) self->text_buf->len;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
    return FALSE;

  switch (keyval)
    {
    case GDK_KEY_Escape:
      canvas_text_commit (self);
      return TRUE;
    case GDK_KEY_BackSpace:
      if (!text_delete_selection (self) && self->text_cursor > 0)
        {
          int prev = (int) (g_utf8_prev_char (str + self->text_cursor) - str);
          text_buf_erase (self, prev, self->text_cursor - prev);
          self->text_cursor = prev;
        }
      canvas_text_render (self);
      text_emit_state (self);
      return TRUE;
    case GDK_KEY_Delete:
      if (!text_delete_selection (self) && self->text_cursor < len)
        {
          int next = (int) (g_utf8_next_char (str + self->text_cursor) - str);
          text_buf_erase (self, self->text_cursor, next - self->text_cursor);
        }
      canvas_text_render (self);
      text_emit_state (self);
      return TRUE;
    case GDK_KEY_Left:
      if (self->text_cursor > 0)
        canvas_text_move_cursor (self, (int) (g_utf8_prev_char (str + self->text_cursor) - str), shift);
      return TRUE;
    case GDK_KEY_Right:
      if (self->text_cursor < len)
        canvas_text_move_cursor (self, (int) (g_utf8_next_char (str + self->text_cursor) - str), shift);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_Down:
      if (self->text_layout != NULL)
        {
          int line, x;
          pango_layout_index_to_line_x (self->text_layout, self->text_cursor, FALSE, &line, &x);
          line += keyval == GDK_KEY_Up ? -1 : 1;
          if (line >= 0 && line < pango_layout_get_line_count (self->text_layout))
            {
              int idx, trailing;
              pango_layout_line_x_to_index (pango_layout_get_line_readonly (self->text_layout, line), x,
                                            &idx, &trailing);
              canvas_text_move_cursor (self, idx, shift);
            }
        }
      return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_End:
      if (self->text_layout != NULL)
        {
          int line, x;
          PangoLayoutLine *l;
          pango_layout_index_to_line_x (self->text_layout, self->text_cursor, FALSE, &line, &x);
          l = pango_layout_get_line_readonly (self->text_layout, line);
          canvas_text_move_cursor (self, keyval == GDK_KEY_Home ? l->start_index : l->start_index + l->length, shift);
        }
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      text_insert (self, "\n");
      return TRUE;
    default:
      {
        gunichar ch = gdk_keyval_to_unicode (keyval);
        if (ch != 0 && g_unichar_isprint (ch))
          {
            char buf[8] = { 0 };
            g_unichar_to_utf8 (ch, buf);
            text_insert (self, buf);
            return TRUE;
          }
      }
    }
  return FALSE;
}

void
canvas_draw_text (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if (!self->text_active && !self->text_creating)
    return;

  EaselRect r = self->text_rect;

  if (self->text_texture != NULL)
    {
      graphene_rect_t at = GRAPHENE_RECT_INIT ((float) (r.x * s), (float) (r.y * s),
                                               (float) (r.width * s), (float) (r.height * s));
      gtk_snapshot_append_scaled_texture (snapshot, self->text_texture, GSK_SCALING_FILTER_NEAREST, &at);
    }

  graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (r.x * s) - 0.5f, (float) (r.y * s) - 0.5f,
                                            (float) (r.width * s) + 1, (float) (r.height * s) + 1);
  GskPathBuilder *builder = gsk_path_builder_new ();
  gsk_path_builder_add_rect (builder, &box);
  GskPath *path = gsk_path_builder_free_to_path (builder);
  const GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 };
  GskStroke *solid = gsk_stroke_new (1.0f), *dashed = gsk_stroke_new (1.0f);
  const float dashes[] = { 4, 4 };

  gsk_stroke_set_dash (dashed, dashes, 2);
  gtk_snapshot_append_stroke (snapshot, path, solid, &white);
  gtk_snapshot_append_stroke (snapshot, path, dashed, &black);
  gsk_stroke_free (solid);
  gsk_stroke_free (dashed);
  gsk_path_unref (path);

  int sa, sb;
  if (canvas_text_sel_range (self, &sa, &sb) && self->text_layout != NULL)
    {
      const GdkRGBA tint = { 0.2f, 0.4f, 0.9f, 0.35f };
      const char *str = self->text_buf->str;

      /* Highlight one rectangle per character. It's more rectangles than
       * needed, but it copes with wrapped lines and empty lines without any
       * special cases. */
      for (int i = sa; i < sb; i = (int) (g_utf8_next_char (str + i) - str))
        {
          PangoRectangle pos;
          pango_layout_index_to_pos (self->text_layout, i, &pos);
          int w = pos.width;
          if (str[i] == '\n' || w <= 0)
            w = 4 * PANGO_SCALE; /* a line break shows as a small block */
          graphene_rect_t hl = GRAPHENE_RECT_INIT ((float) ((r.x + pos.x / (double) PANGO_SCALE) * s),
                                                   (float) ((r.y + pos.y / (double) PANGO_SCALE) * s),
                                                   (float) (w / (double) PANGO_SCALE * s),
                                                   (float) (pos.height / (double) PANGO_SCALE * s));
          gtk_snapshot_append_color (snapshot, &tint, &hl);
        }
    }

  if (self->text_active)
    for (int h = 1; h <= 8; h++)
      {
        double cx, cy;
        canvas_sel_handle_pos (&r, h, s, &cx, &cy);
        graphene_rect_t halo = GRAPHENE_RECT_INIT ((float) cx - 3.5f, (float) cy - 3.5f, 7, 7);
        graphene_rect_t dot = GRAPHENE_RECT_INIT ((float) cx - 2.5f, (float) cy - 2.5f, 5, 5);
        gtk_snapshot_append_color (snapshot, &white, &halo);
        gtk_snapshot_append_color (snapshot, &black, &dot);
      }

  if (self->text_active && self->text_caret_on && self->text_layout != NULL)
    {
      PangoRectangle pos;
      pango_layout_get_cursor_pos (self->text_layout, self->text_cursor, &pos, NULL);
      graphene_rect_t caret = GRAPHENE_RECT_INIT ((float) ((r.x + pos.x / PANGO_SCALE) * s),
                                                  (float) ((r.y + pos.y / PANGO_SCALE) * s),
                                                  (float) MAX (1.0, s * 0.5),
                                                  (float) (MAX (1, pos.height / PANGO_SCALE) * s));
      gtk_snapshot_append_color (snapshot, &black, &caret);
    }
}

/* What is under the pointer: a resize handle (1..8), the border (to move the
 * box), the inside (to place the caret) or nothing. */
int
canvas_text_hit (EaselCanvas *self, double wx, double wy)
{
  if (!self->text_active)
    return TEXT_HIT_NONE;
  double s = canvas_logical_per_pixel (self);

  for (int h = 1; h <= 8; h++)
    {
      double cx, cy;
      canvas_sel_handle_pos (&self->text_rect, h, s, &cx, &cy);
      if (fabs (wx - cx) <= 5 && fabs (wy - cy) <= 5)
        return h;
    }
  double x0 = self->text_rect.x * s, y0 = self->text_rect.y * s;
  double x1 = x0 + self->text_rect.width * s, y1 = y0 + self->text_rect.height * s;

  if (wx < x0 - 4 || wx > x1 + 4 || wy < y0 - 4 || wy > y1 + 4)
    return TEXT_HIT_NONE;
  if (wx < x0 + 3 || wx > x1 - 3 || wy < y0 + 3 || wy > y1 - 3)
    return TEXT_HIT_BORDER;
  return TEXT_HIT_INSIDE;
}

void
canvas_text_resize_to (EaselCanvas *self, int x, int y)
{
  EaselRect o = self->text_orig;
  int l = o.x, t = o.y, r = o.x + o.width, b = o.y + o.height, h = self->text_handle;

  if (h == 1 || h == 4 || h == 6) l = MIN (x, r - 3);
  if (h == 3 || h == 5 || h == 8) r = MAX (x + 1, l + 3);
  if (h <= 3) t = MIN (y, b - 3);
  if (h >= 6) b = MAX (y + 1, t + 3);
  self->text_rect = (EaselRect) { l, t, r - l, b - t };
  canvas_text_render (self);
}

void
canvas_text_begin_drag (EaselCanvas *self, int x, int y)
{
  canvas_text_commit (self);
  if (self->text_buf == NULL)
    self->text_buf = g_string_new (NULL);
  if (self->text_attr == NULL)
    self->text_attr = g_array_new (FALSE, FALSE, sizeof (TextAttr));
  self->text_creating = TRUE;
  self->text_rect = (EaselRect) { CLAMP (x, 0, self->doc->width - 1), CLAMP (y, 0, self->doc->height - 1), 1, 1 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
canvas_text_drag_to (EaselCanvas *self, int x0, int y0, int x1, int y1)
{
  int l = CLAMP (MIN (x0, x1), 0, self->doc->width - 1), t = CLAMP (MIN (y0, y1), 0, self->doc->height - 1);
  int r = CLAMP (MAX (x0, x1), 0, self->doc->width - 1), b = CLAMP (MAX (y0, y1), 0, self->doc->height - 1);

  self->text_rect = (EaselRect) { l, t, r - l + 1, b - t + 1 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
canvas_text_end_drag (EaselCanvas *self)
{
  self->text_creating = FALSE;
  if (self->text_rect.width < 8 || self->text_rect.height < 8)
    {
      /* A click, not a drag: open a box of a comfortable default size. */
      self->text_rect.width = MIN (EASEL_DEFAULT_TEXT_WIDTH, self->doc->width - self->text_rect.x);
      self->text_rect.height = MIN (48, self->doc->height - self->text_rect.y);
      if (self->text_rect.width < 3 || self->text_rect.height < 3)
        {
          text_cancel (self);
          return;
        }
    }
  self->text_active = TRUE;
  self->text_cursor = 0;
  g_string_truncate (self->text_buf, 0);
  g_array_set_size (self->text_attr, 0);
  self->text_anchor = -1;
  canvas_text_render (self);
  text_emit_state (self);
}

void
easel_canvas_set_text_opaque (EaselCanvas *self, gboolean opaque)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_opaque == opaque)
    return;
  self->text_opaque = opaque;
  canvas_text_render (self);
}

void
easel_canvas_set_text_smooth (EaselCanvas *self, gboolean smooth)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_smooth == smooth)
    return;
  self->text_smooth = smooth;
  canvas_text_render (self);
}

void
easel_canvas_text_apply_style (EaselCanvas *self, guint flag, gboolean on)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  text_apply_style (self, flag, on);
}

gboolean
easel_canvas_text_active (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  return self->text_active;
}

void
easel_canvas_text_select_all (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (!self->text_active)
    return;
  self->text_anchor = 0;
  self->text_cursor = (int) self->text_buf->len;
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_text_copy (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  int a, b;

  if (!canvas_text_sel_range (self, &a, &b))
    return;
  g_autofree char *piece = g_strndup (self->text_buf->str + a, (gsize) (b - a));
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), piece);
}

void
easel_canvas_text_delete (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (text_delete_selection (self))
    {
      canvas_text_render (self);
      text_emit_state (self);
    }
}

void
easel_canvas_text_cut (EaselCanvas *self)
{
  easel_canvas_text_copy (self);
  easel_canvas_text_delete (self);
}

static void
on_clipboard_text (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselCanvas *self = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, &error);

  if (text != NULL && self->text_active)
    text_insert (self, text);
  g_object_unref (self);
}

void
easel_canvas_text_paste (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (GTK_WIDGET (self)), NULL,
                                 on_clipboard_text, g_object_ref (self));
}

void
easel_canvas_set_font (EaselCanvas *self, const char *description)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (g_strcmp0 (self->font, description) == 0)
    return;
  g_free (self->font);
  self->font = g_strdup (description);
  canvas_text_render (self);
}
