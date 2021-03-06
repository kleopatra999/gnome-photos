/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2014 Saurav Agarwalla
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

#ifndef PHOTOS_GOOGLE_ITEM_H
#define PHOTOS_GOOGLE_ITEM_H

#include "photos-base-item.h"

G_BEGIN_DECLS

#define PHOTOS_TYPE_GOOGLE_ITEM (photos_google_item_get_type ())

#define PHOTOS_GOOGLE_ITEM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   PHOTOS_TYPE_GOOGLE_ITEM, PhotosGoogleItem))

#define PHOTOS_IS_GOOGLE_ITEM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
   PHOTOS_TYPE_GOOGLE_ITEM))

typedef struct _PhotosGoogleItem      PhotosGoogleItem;
typedef struct _PhotosGoogleItemClass PhotosGoogleItemClass;

GType               photos_google_item_get_type           (void) G_GNUC_CONST;

G_END_DECLS

#endif /* PHOTOS_GOOGLE_ITEM_H */
