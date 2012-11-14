/* GStreamer
 * Copyright (C) 2012 Wim Taymans <wim.taymans at gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <gio/gio.h>

#include "rtsp-address-pool.h"

GST_DEBUG_CATEGORY_STATIC (rtsp_address_pool_debug);
#define GST_CAT_DEFAULT rtsp_address_pool_debug

#define GST_RTSP_ADDRESS_POOL_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_ADDRESS_POOL, GstRTSPAddressPoolPrivate))

struct _GstRTSPAddressPoolPrivate
{
  GMutex lock;
  GList *addresses;
  GList *allocated;
};

#define ADDR_IS_IPV4(a)      ((a)->size == 4)
#define ADDR_IS_IPV6(a)      ((a)->size == 16)
#define ADDR_IS_EVEN_PORT(a) (((a)->port & 1) == 0)

typedef struct
{
  guint8 bytes[16];
  gsize size;
  guint16 port;
} Addr;

typedef struct
{
  Addr min;
  Addr max;
  guint8 ttl;
} AddrRange;

#define RANGE_IS_SINGLE(r) (memcmp ((r)->min.bytes, (r)->max.bytes, (r)->min.size) == 0)

#define gst_rtsp_address_pool_parent_class parent_class
G_DEFINE_TYPE (GstRTSPAddressPool, gst_rtsp_address_pool, G_TYPE_OBJECT);

static void gst_rtsp_address_pool_finalize (GObject * obj);

static void
gst_rtsp_address_pool_class_init (GstRTSPAddressPoolClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_rtsp_address_pool_finalize;

  g_type_class_add_private (klass, sizeof (GstRTSPAddressPoolPrivate));

  GST_DEBUG_CATEGORY_INIT (rtsp_address_pool_debug, "rtspaddresspool", 0,
      "GstRTSPAddressPool");
}

static void
gst_rtsp_address_pool_init (GstRTSPAddressPool * pool)
{
  pool->priv = GST_RTSP_ADDRESS_POOL_GET_PRIVATE (pool);

  g_mutex_init (&pool->priv->lock);
}

static void
free_range (AddrRange * range)
{
  g_slice_free (AddrRange, range);
}

static void
gst_rtsp_address_pool_finalize (GObject * obj)
{
  GstRTSPAddressPool *pool;

  pool = GST_RTSP_ADDRESS_POOL (obj);

  g_list_free_full (pool->priv->addresses, (GDestroyNotify) free_range);
  g_list_free_full (pool->priv->allocated, (GDestroyNotify) free_range);
  g_mutex_clear (&pool->priv->lock);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

/**
 * gst_rtsp_address_pool_new:
 *
 * Make a new #GstRTSPAddressPool.
 *
 * Returns: a new #GstRTSPAddressPool
 */
GstRTSPAddressPool *
gst_rtsp_address_pool_new (void)
{
  GstRTSPAddressPool *pool;

  pool = g_object_new (GST_TYPE_RTSP_ADDRESS_POOL, NULL);

  return pool;
}

static gboolean
fill_address (const gchar * address, guint16 port, Addr * addr)
{
  GInetAddress *inet;

  inet = g_inet_address_new_from_string (address);
  if (inet == NULL)
    return FALSE;

  addr->size = g_inet_address_get_native_size (inet);
  memcpy (addr->bytes, g_inet_address_to_bytes (inet), addr->size);
  g_object_unref (inet);
  addr->port = port;

  return TRUE;
}

static gchar *
get_address_string (Addr * addr)
{
  gchar *res;
  GInetAddress *inet;

  inet = g_inet_address_new_from_bytes (addr->bytes,
      addr->size == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6);
  res = g_inet_address_to_string (inet);
  g_object_unref (inet);

  return res;
}

/**
 * gst_rtsp_address_pool_add_range:
 * @pool: a #GstRTSPAddressPool
 * @min_address: a minimum address to add
 * @max_address: a maximum address to add
 * @min_port: the minimum port
 * @max_port: the maximum port
 * @ttl: a TTL
 *
 * Adds the multicast addresses from @min_addess to @max_address (inclusive)
 * to @pool. The valid port range for the addresses will be from @min_port to
 * @max_port inclusive.
 *
 * Returns: %TRUE if the addresses could be added.
 */
gboolean
gst_rtsp_address_pool_add_range (GstRTSPAddressPool * pool,
    const gchar * min_address, const gchar * max_address,
    guint16 min_port, guint16 max_port, guint8 ttl)
{
  AddrRange *range;
  GstRTSPAddressPoolPrivate *priv;

  g_return_val_if_fail (GST_IS_RTSP_ADDRESS_POOL (pool), FALSE);
  g_return_val_if_fail (min_port <= max_port, FALSE);

  priv = pool->priv;

  range = g_slice_new0 (AddrRange);

  if (!fill_address (min_address, min_port, &range->min))
    goto invalid;
  if (!fill_address (max_address, max_port, &range->max))
    goto invalid;

  if (range->min.size != range->max.size)
    goto invalid;
  if (memcmp (range->min.bytes, range->max.bytes, range->min.size) > 0)
    goto invalid;

  range->ttl = ttl;

  GST_DEBUG_OBJECT (pool, "adding %s-%s:%u-%u ttl %u", min_address, max_address,
      min_port, max_port, ttl);

  g_mutex_lock (&priv->lock);
  priv->addresses = g_list_prepend (priv->addresses, range);
  g_mutex_unlock (&priv->lock);

  return TRUE;

  /* ERRORS */
invalid:
  {
    GST_ERROR_OBJECT (pool, "invalid address range %s-%s", min_address,
        max_address);
    g_slice_free (AddrRange, range);
    return FALSE;
  }
}

static void
inc_address (GstRTSPAddressPool * pool, Addr * addr)
{
  gint i;
  guint carry;

  carry = 1;
  for (i = addr->size - 1; i >= 0 && carry > 0; i--) {
    carry += addr->bytes[i];
    addr->bytes[i] = carry & 0xff;
    carry >>= 8;
  }
}

static AddrRange *
split_range (GstRTSPAddressPool * pool, AddrRange * range, gint skip,
    gint n_ports)
{
  GstRTSPAddressPoolPrivate *priv = pool->priv;
  AddrRange *temp;

  if (!RANGE_IS_SINGLE (range)) {
    /* min and max are not the same, we have more than one address. */
    temp = g_slice_dup (AddrRange, range);
    /* increment the range min address */
    inc_address (pool, &temp->min);
    /* and store back in pool */
    priv->addresses = g_list_prepend (priv->addresses, temp);

    /* adjust range with only the first address */
    memcpy (range->max.bytes, range->min.bytes, range->min.size);
  }

  /* range now contains only one single address */
  if (skip > 0) {
    /* make a range with the skipped ports */
    temp = g_slice_dup (AddrRange, range);
    temp->max.port = temp->min.port + skip;
    /* and store back in pool */
    priv->addresses = g_list_prepend (priv->addresses, temp);

    /* increment range port */
    range->min.port += skip;
  }
  /* range now contains single address with desired port number */
  if (range->max.port - range->min.port + 1 > n_ports) {
    /* make a range with the remaining ports */
    temp = g_slice_dup (AddrRange, range);
    temp->min.port += n_ports;
    /* and store back in pool */
    priv->addresses = g_list_prepend (priv->addresses, temp);

    /* and truncate port */
    range->max.port = range->min.port + n_ports - 1;
  }
  return range;
}

/**
 * gst_rtsp_address_pool_acquire_address:
 * @pool: a #GstRTSPAddressPool
 * @flags: flags
 * @n_ports: the amount of ports
 * @address: result address
 * @port: result port
 * @ttl: result TTL
 *
 * Take an address and ports from @pool. @flags can be used to control the
 * allocation. @n_ports consecutive ports will be allocated of which the first
 * one can be found in @port.
 *
 * Returns: a pointer that should be used to release the address with
 *   gst_rtsp_address_pool_release_address() after usage or %NULL when no
 *   address could be acquired.
 */
gpointer
gst_rtsp_address_pool_acquire_address (GstRTSPAddressPool * pool,
    GstRTSPAddressFlags flags, gint n_ports, gchar ** address,
    guint16 * port, guint8 * ttl)
{
  GstRTSPAddressPoolPrivate *priv;
  GList *walk, *next;
  AddrRange *result;

  g_return_val_if_fail (GST_IS_RTSP_ADDRESS_POOL (pool), NULL);
  g_return_val_if_fail (n_ports > 0, NULL);
  g_return_val_if_fail (address != NULL, NULL);
  g_return_val_if_fail (port != NULL, NULL);
  g_return_val_if_fail (ttl != NULL, NULL);

  priv = pool->priv;
  result = NULL;

  g_mutex_lock (&priv->lock);
  /* go over available ranges */
  for (walk = priv->addresses; walk; walk = next) {
    AddrRange *range;
    gint ports, skip;

    range = walk->data;
    next = walk->next;

    /* check address type when given */
    if (flags & GST_RTSP_ADDRESS_FLAG_IPV4 && !ADDR_IS_IPV4 (&range->min))
      continue;
    if (flags & GST_RTSP_ADDRESS_FLAG_IPV6 && !ADDR_IS_IPV6 (&range->min))
      continue;

    /* check for enough ports */
    ports = range->max.port - range->min.port + 1;
    if (flags & GST_RTSP_ADDRESS_FLAG_EVEN_PORT
        && !ADDR_IS_EVEN_PORT (&range->min))
      skip = 1;
    else
      skip = 0;
    if (ports - skip < n_ports)
      continue;

    /* we found a range, remove from the list */
    priv->addresses = g_list_delete_link (priv->addresses, walk);
    /* now split and exit our loop */
    result = split_range (pool, range, skip, n_ports);
    priv->allocated = g_list_prepend (priv->allocated, result);
    break;
  }
  g_mutex_unlock (&priv->lock);

  if (result) {
    *address = get_address_string (&result->min);
    *port = result->min.port;
    *ttl = result->ttl;

    GST_DEBUG_OBJECT (pool, "got address %s:%u ttl %u", *address, *port, *ttl);
  }
  return result;
}

/**
 * gst_rtsp_address_pool_release_address:
 * @pool: a #GstRTSPAddressPool
 * @id: an address id
 *
 * Release a previously acquired address (with
 * gst_rtsp_address_pool_acquire_address()) back into @pool.
 */
void
gst_rtsp_address_pool_release_address (GstRTSPAddressPool * pool, gpointer id)
{
  GstRTSPAddressPoolPrivate *priv;
  GList *find;

  g_return_if_fail (GST_IS_RTSP_ADDRESS_POOL (pool));
  g_return_if_fail (id != NULL);

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  find = g_list_find (priv->allocated, id);
  if (find == NULL)
    goto not_found;

  priv->allocated = g_list_delete_link (priv->allocated, find);

  /* FIXME, merge and do something clever */
  priv->addresses = g_list_prepend (priv->addresses, id);
  g_mutex_unlock (&priv->lock);

  return;

  /* ERRORS */
not_found:
  {
    g_warning ("Released unknown id %p", id);
    g_mutex_unlock (&priv->lock);
    return;
  }
}

static void
dump_range (AddrRange * range, GstRTSPAddressPool * pool)
{
  gchar *addr1, *addr2;

  addr1 = get_address_string (&range->min);
  addr2 = get_address_string (&range->max);
  g_print ("  address %s-%s, port %u-%u, ttl %u\n", addr1, addr2,
      range->min.port, range->max.port, range->ttl);
  g_free (addr1);
  g_free (addr2);
}

/**
 * gst_rtsp_address_pool_dump:
 * @pool: a #GstRTSPAddressPool
 *
 * Dump the free and allocated addresses to stdout.
 */
void
gst_rtsp_address_pool_dump (GstRTSPAddressPool * pool)
{
  GstRTSPAddressPoolPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_ADDRESS_POOL (pool));

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  g_print ("free:\n");
  g_list_foreach (priv->addresses, (GFunc) dump_range, pool);
  g_print ("allocated:\n");
  g_list_foreach (priv->allocated, (GFunc) dump_range, pool);
  g_mutex_unlock (&priv->lock);
}
