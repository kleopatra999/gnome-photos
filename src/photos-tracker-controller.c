/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2012 – 2016 Red Hat, Inc.
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/* Based on code from:
 *   + Documents
 */


#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>

#include <gio/gio.h>

#include "photos-base-manager.h"
#include "photos-debug.h"
#include "photos-enums.h"
#include "photos-filterable.h"
#include "photos-item-manager.h"
#include "photos-marshalers.h"
#include "photos-query-builder.h"
#include "photos-search-context.h"
#include "photos-tracker-controller.h"
#include "photos-tracker-queue.h"


struct _PhotosTrackerControllerPrivate
{
  GCancellable *cancellable;
  GError *queue_error;
  PhotosBaseManager *item_mngr;
  PhotosBaseManager *src_mngr;
  PhotosModeController *mode_cntrlr;
  PhotosOffsetController *offset_cntrlr;
  PhotosQuery *current_query;
  PhotosTrackerQueue *queue;
  PhotosWindowMode mode;
  gboolean is_frozen;
  gboolean is_started;
  gboolean query_queued;
  gboolean querying;
  gboolean refresh_pending;
  gint query_queued_flags;
  gint64 last_query_time;
};

enum
{
  PROP_0,
  PROP_MODE
};

enum
{
  QUERY_ERROR,
  QUERY_STATUS_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (PhotosTrackerController, photos_tracker_controller, G_TYPE_OBJECT);


typedef enum
{
  PHOTOS_TRACKER_REFRESH_FLAGS_NONE = 0,
  PHOTOS_TRACKER_REFRESH_FLAGS_DONT_SET_QUERY_STATUS = 1 << 0,
  PHOTOS_TRACKER_REFRESH_FLAGS_RESET_OFFSET = 1 << 1,
} PhotosTrackerRefreshFlags;

static void photos_tracker_controller_refresh_internal (PhotosTrackerController *self, gint flags);
static void photos_tracker_controller_set_query_status (PhotosTrackerController *self, gboolean query_status);


static void
photos_tracker_controller_query_error (PhotosTrackerController *self, GError *error)
{
  const gchar *primary = _("Unable to fetch the list of photos");

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  g_signal_emit (self, signals[QUERY_ERROR], 0, primary, error->message);
}


static void
photos_tracker_controller_query_finished (PhotosTrackerController *self, GError *error)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  photos_tracker_controller_set_query_status (self, FALSE);

  if (error != NULL)
    photos_tracker_controller_query_error (self, error);
  else
    photos_offset_controller_reset_count (priv->offset_cntrlr);

  if (priv->query_queued)
    {
      priv->query_queued = FALSE;
      photos_tracker_controller_refresh_internal (self, priv->query_queued_flags);
    }
}


static void
photos_tracker_controller_cursor_next (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (user_data);
  PhotosTrackerControllerPrivate *priv;
  TrackerSparqlCursor *cursor = TRACKER_SPARQL_CURSOR (source_object);
  gboolean success;
  gint64 now;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->item_mngr == NULL)
    goto out;

  success = tracker_sparql_cursor_next_finish (cursor, res, NULL); /* TODO: use GError */
  if (!success)
    {
      tracker_sparql_cursor_close (cursor);
      photos_tracker_controller_query_finished (self, NULL);
      goto out;
    }

  now = g_get_monotonic_time ();
  photos_debug (PHOTOS_DEBUG_TRACKER, "Query Cursor: %" G_GINT64_FORMAT, (now - priv->last_query_time) / 1000000);

  photos_item_manager_add_item (PHOTOS_ITEM_MANAGER (priv->item_mngr), cursor);
  tracker_sparql_cursor_next_async (cursor,
                                    priv->cancellable,
                                    photos_tracker_controller_cursor_next,
                                    g_object_ref (self));

 out:
  g_object_unref (self);
}


static void
photos_tracker_controller_query_executed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (user_data);
  PhotosTrackerControllerPrivate *priv;
  TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source_object);
  GError *error;
  TrackerSparqlCursor *cursor;

  priv = photos_tracker_controller_get_instance_private (self);

  error = NULL;
  cursor = tracker_sparql_connection_query_finish (connection, res, &error);
  if (error != NULL)
    {
      photos_tracker_controller_query_finished (self, error);
      g_error_free (error);
      return;
    }

  tracker_sparql_cursor_next_async (cursor,
                                    priv->cancellable,
                                    photos_tracker_controller_cursor_next,
                                    g_object_ref (self));
  g_object_unref (cursor);
}


static void
photos_tracker_controller_perform_current_query (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->current_query != NULL)
    photos_query_free (priv->current_query);

  priv->current_query = PHOTOS_TRACKER_CONTROLLER_GET_CLASS (self)->get_query (self);
  g_return_if_fail (priv->current_query != NULL);

  g_cancellable_reset (priv->cancellable);

  if (G_UNLIKELY (priv->queue == NULL))
    {
      photos_tracker_controller_query_error (self, priv->queue_error);
      return;
    }

  photos_tracker_queue_select (priv->queue,
                               priv->current_query->sparql,
                               priv->cancellable,
                               photos_tracker_controller_query_executed,
                               g_object_ref (self),
                               g_object_unref);
}


static void
photos_tracker_controller_offset_changed (PhotosTrackerController *self)
{
  photos_tracker_controller_refresh_internal (self, PHOTOS_TRACKER_REFRESH_FLAGS_DONT_SET_QUERY_STATUS);
}


static void
photos_tracker_controller_set_query_status (PhotosTrackerController *self, gboolean query_status)
{
  PhotosTrackerControllerPrivate *priv;
  gint64 now;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->querying == query_status)
    return;

  now = g_get_monotonic_time ();
  if (query_status)
    priv->last_query_time = now;
  else
    {
      photos_debug (PHOTOS_DEBUG_TRACKER,
                    "Query Elapsed: %" G_GINT64_FORMAT,
                    (now - priv->last_query_time) / 1000000);
      priv->last_query_time = 0;
    }

  priv->querying = query_status;
  g_signal_emit (self, signals[QUERY_STATUS_CHANGED], 0, priv->querying);
}


static void
photos_tracker_controller_refresh_internal (PhotosTrackerController *self, gint flags)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  priv->is_started = TRUE;

  if (priv->is_frozen)
    return;

  if (flags & PHOTOS_TRACKER_REFRESH_FLAGS_RESET_OFFSET)
    photos_offset_controller_reset_offset (priv->offset_cntrlr);

  if (photos_tracker_controller_get_query_status (self))
    {
      g_cancellable_cancel (priv->cancellable);
      priv->query_queued = TRUE;
      priv->query_queued_flags = flags;
      return;
    }

  if (!(flags & PHOTOS_TRACKER_REFRESH_FLAGS_DONT_SET_QUERY_STATUS))
    photos_tracker_controller_set_query_status (self, TRUE);

  photos_tracker_controller_perform_current_query (self);
}


static void
photos_tracker_controller_refresh_for_source (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->current_query->source != NULL)
    {
      const gchar *id;

      id = photos_filterable_get_id (PHOTOS_FILTERABLE (priv->current_query->source));
      if (g_strcmp0 (id, PHOTOS_SOURCE_STOCK_ALL) == 0)
        photos_tracker_controller_refresh_internal (self, PHOTOS_TRACKER_REFRESH_FLAGS_NONE);
    }

  priv->refresh_pending = FALSE;
}


static void
photos_tracker_controller_source_object_added_removed (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;
  PhotosWindowMode mode;

  priv = photos_tracker_controller_get_instance_private (self);

  g_return_if_fail (priv->mode_cntrlr != NULL);

  mode = photos_mode_controller_get_window_mode (priv->mode_cntrlr);
  if (mode == priv->mode)
    photos_tracker_controller_refresh_for_source (self);
  else
    priv->refresh_pending = TRUE;
}


static void
photos_tracker_controller_window_mode_changed (PhotosTrackerController *self,
                                               PhotosWindowMode mode,
                                               PhotosWindowMode old_mode)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->refresh_pending && mode == priv->mode)
    photos_tracker_controller_refresh_for_source (self);
}


static void
photos_tracker_controller_constructed (GObject *object)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (object);
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  G_OBJECT_CLASS (photos_tracker_controller_parent_class)->constructed (object);

  priv->offset_cntrlr = PHOTOS_TRACKER_CONTROLLER_GET_CLASS (self)->get_offset_controller (self);
  g_signal_connect_swapped (priv->offset_cntrlr,
                            "offset-changed",
                            G_CALLBACK (photos_tracker_controller_offset_changed),
                            self);
}


static void
photos_tracker_controller_dispose (GObject *object)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (object);
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  g_clear_object (&priv->src_mngr);
  g_clear_object (&priv->offset_cntrlr);
  g_clear_object (&priv->queue);

  G_OBJECT_CLASS (photos_tracker_controller_parent_class)->dispose (object);
}


static void
photos_tracker_controller_finalize (GObject *object)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (object);
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->item_mngr != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->item_mngr), (gpointer *) &priv->item_mngr);

  if (priv->mode_cntrlr != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->mode_cntrlr), (gpointer *) &priv->mode_cntrlr);

  g_clear_error (&priv->queue_error);

  if (priv->current_query != NULL)
    photos_query_free (priv->current_query);

  G_OBJECT_CLASS (photos_tracker_controller_parent_class)->finalize (object);
}


static void
photos_tracker_controller_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  PhotosTrackerController *self = PHOTOS_TRACKER_CONTROLLER (object);
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_MODE:
      priv->mode = (PhotosWindowMode) g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_tracker_controller_init (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;
  GApplication *app;
  PhotosSearchContextState *state;

  priv = photos_tracker_controller_get_instance_private (self);

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  priv->cancellable = g_cancellable_new ();

  priv->item_mngr = state->item_mngr;
  g_object_add_weak_pointer (G_OBJECT (priv->item_mngr), (gpointer *) &priv->item_mngr);

  priv->src_mngr = g_object_ref (state->src_mngr);
  g_signal_connect_swapped (priv->src_mngr,
                            "object-added",
                            G_CALLBACK (photos_tracker_controller_source_object_added_removed),
                            self);
  g_signal_connect_swapped (priv->src_mngr,
                            "object-removed",
                            G_CALLBACK (photos_tracker_controller_source_object_added_removed),
                            self);

  priv->mode_cntrlr = state->mode_cntrlr;
  g_object_add_weak_pointer (G_OBJECT (priv->mode_cntrlr), (gpointer *) &priv->mode_cntrlr);
  g_signal_connect_swapped (priv->mode_cntrlr,
                            "window-mode-changed",
                            G_CALLBACK (photos_tracker_controller_window_mode_changed),
                            self);

  priv->queue = photos_tracker_queue_dup_singleton (NULL, &priv->queue_error);
}


static void
photos_tracker_controller_class_init (PhotosTrackerControllerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->constructed = photos_tracker_controller_constructed;
  object_class->dispose = photos_tracker_controller_dispose;
  object_class->set_property = photos_tracker_controller_set_property;
  object_class->finalize = photos_tracker_controller_finalize;

  g_object_class_install_property (object_class,
                                   PROP_MODE,
                                   g_param_spec_enum ("mode",
                                                      "PhotosWindowMode enum",
                                                      "The mode handled by this controller",
                                                      PHOTOS_TYPE_WINDOW_MODE,
                                                      PHOTOS_WINDOW_MODE_NONE,
                                                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  signals[QUERY_ERROR] = g_signal_new ("query-error",
                                       G_TYPE_FROM_CLASS (class),
                                       G_SIGNAL_RUN_LAST,
                                       G_STRUCT_OFFSET (PhotosTrackerControllerClass,
                                                        query_error),
                                       NULL, /*accumulator */
                                       NULL, /*accu_data */
                                       _photos_marshal_VOID__STRING_STRING,
                                       G_TYPE_NONE,
                                       2,
                                       G_TYPE_STRING,
                                       G_TYPE_STRING);

  signals[QUERY_STATUS_CHANGED] = g_signal_new ("query-status-changed",
                                                G_TYPE_FROM_CLASS (class),
                                                G_SIGNAL_RUN_LAST,
                                                G_STRUCT_OFFSET (PhotosTrackerControllerClass,
                                                                 query_status_changed),
                                                NULL, /*accumulator */
                                                NULL, /*accu_data */
                                                g_cclosure_marshal_VOID__BOOLEAN,
                                                G_TYPE_NONE,
                                                1,
                                                G_TYPE_BOOLEAN);
}


void
photos_tracker_controller_refresh_for_object (PhotosTrackerController *self)
{
  photos_tracker_controller_refresh_internal (self, PHOTOS_TRACKER_REFRESH_FLAGS_RESET_OFFSET);
}


void
photos_tracker_controller_set_frozen (PhotosTrackerController *self, gboolean frozen)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);
  priv->is_frozen = frozen;
}


void
photos_tracker_controller_start (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);

  if (priv->is_started)
    return;

  photos_tracker_controller_refresh_internal (self, PHOTOS_TRACKER_REFRESH_FLAGS_NONE);
}


gboolean
photos_tracker_controller_get_query_status (PhotosTrackerController *self)
{
  PhotosTrackerControllerPrivate *priv;

  priv = photos_tracker_controller_get_instance_private (self);
  return priv->querying;
}
