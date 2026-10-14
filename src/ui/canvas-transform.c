#include "ui/canvas-private.h"

/* ---- Image menu: flip, rotate, stretch, skew, invert ------------------- */

typedef struct
{
  double   stretch_h, stretch_v;  /* percent, 100 = unchanged */
  double   skew_h, skew_v;        /* degrees */
  gboolean flip_h, flip_v, invert;
  int      turns;                 /* clockwise quarter turns */
} Transform;

typedef struct { void *data; int w, h; size_t elem; } Buf;

static void
buf_replace (Buf *b, void *data, int w, int h)
{
  g_free (b->data);
  b->data = data;
  b->w = w;
  b->h = h;
}

/* Runs the transform over the pixels (and the matching mask, if any). */
static gboolean
run_transform (const Transform *t, Buf *pix, Buf *mask, uint32_t pix_fill)
{
  uint8_t mask_fill = 0;
  void *out;
  int nw, nh;

  if (t->invert)
    easel_invert (pix->data, (size_t) pix->w * pix->h);

  if (t->stretch_h != 100 || t->stretch_v != 100)
    {
      nw = MAX (1, (int) lround (pix->w * t->stretch_h / 100.0));
      nh = MAX (1, (int) lround (pix->h * t->stretch_v / 100.0));
      if (nw > EASEL_MAX_DIMENSION || nh > EASEL_MAX_DIMENSION)
        return FALSE;
      out = easel_scale (pix->data, pix->w, pix->h, 4, nw, nh);
      if (out == NULL)
        return FALSE;
      if (mask != NULL)
        {
          void *mout = easel_scale (mask->data, mask->w, mask->h, 1, nw, nh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, nw, nh);
        }
      buf_replace (pix, out, nw, nh);
    }

  for (int axis = 0; axis < 2; axis++)
    {
      double deg = axis == 0 ? t->skew_h : t->skew_v;
      if (deg == 0)
        continue;
      out = easel_skew (pix->data, pix->w, pix->h, 4, deg, axis == 0, &pix_fill, &nw, &nh);
      if (out == NULL || nw > EASEL_MAX_DIMENSION || nh > EASEL_MAX_DIMENSION)
        {
          free (out);
          return FALSE;
        }
      if (mask != NULL)
        {
          void *mout = easel_skew (mask->data, mask->w, mask->h, 1, deg, axis == 0, &mask_fill, &nw, &nh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, nw, nh);
        }
      buf_replace (pix, out, nw, nh);
    }

  if (t->flip_h)
    {
      easel_flip (pix->data, pix->w, pix->h, 4, 1);
      if (mask != NULL)
        easel_flip (mask->data, mask->w, mask->h, 1, 1);
    }
  if (t->flip_v)
    {
      easel_flip (pix->data, pix->w, pix->h, 4, 0);
      if (mask != NULL)
        easel_flip (mask->data, mask->w, mask->h, 1, 0);
    }
  if (t->turns % 4 != 0)
    {
      out = easel_rotate (pix->data, pix->w, pix->h, 4, t->turns, &nw, &nh);
      if (out == NULL)
        return FALSE;
      if (mask != NULL)
        {
          int mw, mh;
          void *mout = easel_rotate (mask->data, mask->w, mask->h, 1, t->turns, &mw, &mh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, mw, mh);
        }
      buf_replace (pix, out, nw, nh);
    }
  return TRUE;
}

/* Applies to the selection if there is one, otherwise to the whole picture. */
static void
apply_transform (EaselCanvas *self, const Transform *t)
{
  if (self->text_active || self->text_creating)
    canvas_text_commit (self);
  canvas_commit_seq (self, FALSE);

  if (self->sel_active)
    {
      canvas_sel_lift (self, FALSE);
      if (self->sel_clip == NULL)
        return;

      Buf pix = { g_memdup2 (self->sel_clip->pixels, (gsize) self->sel_clip->width * self->sel_clip->height * 4),
                  self->sel_clip->width, self->sel_clip->height, 4 };
      Buf mask = { NULL, pix.w, pix.h, 1 };

      if (self->sel_clip->mask != NULL)
        mask.data = g_memdup2 (self->sel_clip->mask, (gsize) pix.w * pix.h);
      else if (t->skew_h != 0 || t->skew_v != 0)
        {
          mask.data = g_malloc (pix.w * (gsize) pix.h); /* skewing leaves empty corners to hide */
          memset (mask.data, 1, pix.w * (gsize) pix.h);
        }

      if (run_transform (t, &pix, mask.data != NULL ? &mask : NULL, 0))
        {
          EaselClip *clip = easel_clip_new (pix.w, pix.h);
          if (clip != NULL)
            {
              memcpy (clip->pixels, pix.data, (gsize) pix.w * pix.h * 4);
              if (mask.data != NULL)
                clip->mask = g_steal_pointer (&mask.data);
              easel_clip_free (self->sel_clip);
              self->sel_clip = clip;
              self->sel_rect.width = pix.w;
              self->sel_rect.height = pix.h;
              g_clear_pointer (&self->sel_path, g_array_unref);
              g_clear_pointer (&self->sel_mask, g_free);
              canvas_sel_refresh_texture (self);
            }
        }
      g_free (pix.data);
      g_free (mask.data);
      return;
    }

  int ow = self->doc->width, oh = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) ow * oh * 4);
  Buf pix = { g_memdup2 (old, (gsize) ow * oh * 4), ow, oh, 4 };

  if (run_transform (t, &pix, NULL, self->background)
      && easel_document_replace (self->doc, pix.w, pix.h, pix.data))
    {
      easel_history_push_full (self->history, old, ow, oh, self->doc);
      gtk_widget_queue_resize (GTK_WIDGET (self));
      canvas_mark_all_dirty (self);
      g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
    }
  g_free (pix.data);
}

void
easel_canvas_turn (EaselCanvas *self, EaselTurn turn)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { 100, 100, 0, 0, FALSE, FALSE, FALSE, 0 };

  switch (turn)
    {
    case EASEL_FLIP_H: t.flip_h = TRUE; break;
    case EASEL_FLIP_V: t.flip_v = TRUE; break;
    case EASEL_ROTATE_90: t.turns = 1; break;
    case EASEL_ROTATE_180: t.turns = 2; break;
    case EASEL_ROTATE_270: t.turns = 3; break;
    }
  apply_transform (self, &t);
}

void
easel_canvas_invert (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { 100, 100, 0, 0, FALSE, FALSE, TRUE, 0 };
  apply_transform (self, &t);
}

void
easel_canvas_black_and_white (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_active || self->text_creating)
    canvas_text_commit (self);
  canvas_commit_seq (self, FALSE);
  easel_canvas_flush (self);

  int w = self->doc->width, h = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) w * h * 4);

  easel_black_and_white (self->doc->pixels, (size_t) w * h);
  if (easel_history_push (self->history, self->doc, old, (EaselRect) { 0, 0, w, h }))
    {
      canvas_mark_all_dirty (self);
      g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_get_transform_size (EaselCanvas *self, int *width, int *height)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  *width = self->sel_active && self->sel_rect.width > 0 ? self->sel_rect.width : self->doc->width;
  *height = self->sel_active && self->sel_rect.height > 0 ? self->sel_rect.height : self->doc->height;
}

void
easel_canvas_stretch_skew (EaselCanvas *self, double stretch_h, double stretch_v, double skew_h, double skew_v)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { stretch_h, stretch_v, skew_h, skew_v, FALSE, FALSE, FALSE, 0 };

  if (stretch_h == 100 && stretch_v == 100 && skew_h == 0 && skew_v == 0)
    return;
  apply_transform (self, &t);
}
