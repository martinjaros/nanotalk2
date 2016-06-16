/*
 * Copyright © 2015 Canonical Limited
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef __GLIB_COMPAT_H__
#define __GLIB_COMPAT_H__

#include <glib.h>
#ifndef GLIB_VERSION_2_44

/* these macros are private */
#define _GLIB_AUTOPTR_FUNC_NAME(TypeName) glib_autoptr_cleanup_##TypeName
#define _GLIB_AUTOPTR_TYPENAME(TypeName)  TypeName##_autoptr
#define _GLIB_AUTO_FUNC_NAME(TypeName)    glib_auto_cleanup_##TypeName
#define _GLIB_CLEANUP(func)               __attribute__((cleanup(func)))
#define _GLIB_DEFINE_AUTOPTR_CHAINUP(ModuleObjName, ParentName) \
  typedef ModuleObjName *_GLIB_AUTOPTR_TYPENAME(ModuleObjName);                                          \
  static inline void _GLIB_AUTOPTR_FUNC_NAME(ModuleObjName) (ModuleObjName **_ptr) {                     \
    _GLIB_AUTOPTR_FUNC_NAME(ParentName) ((ParentName **) _ptr); }                                        \

/* these macros are API */
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(TypeName, func) \
  typedef TypeName *_GLIB_AUTOPTR_TYPENAME(TypeName);                                                           \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTOPTR_FUNC_NAME(TypeName) (TypeName **_ptr) { if (*_ptr) (func) (*_ptr); }         \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(TypeName, func) \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTO_FUNC_NAME(TypeName) (TypeName *_ptr) { (func) (_ptr); }                         \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define G_DEFINE_AUTO_CLEANUP_FREE_FUNC(TypeName, func, none) \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTO_FUNC_NAME(TypeName) (TypeName *_ptr) { if (*_ptr != none) (func) (*_ptr); }     \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define g_autoptr(TypeName) _GLIB_CLEANUP(_GLIB_AUTOPTR_FUNC_NAME(TypeName)) _GLIB_AUTOPTR_TYPENAME(TypeName)
#define g_auto(TypeName) _GLIB_CLEANUP(_GLIB_AUTO_FUNC_NAME(TypeName)) TypeName
#define g_autofree _GLIB_CLEANUP(g_autoptr_cleanup_generic_gfree)

#define G_DECLARE_FINAL_TYPE(ModuleObjName, module_obj_name, MODULE, OBJ_NAME, ParentName) \
  GType module_obj_name##_get_type (void);                                                               \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                       \
  typedef struct _##ModuleObjName ModuleObjName;                                                         \
  typedef struct { ParentName##Class parent_class; } ModuleObjName##Class;                               \
                                                                                                         \
  _GLIB_DEFINE_AUTOPTR_CHAINUP (ModuleObjName, ParentName)                                               \
                                                                                                         \
  static inline ModuleObjName * MODULE##_##OBJ_NAME (gpointer ptr) {                                     \
    return G_TYPE_CHECK_INSTANCE_CAST (ptr, module_obj_name##_get_type (), ModuleObjName); }             \
  static inline gboolean MODULE##_IS_##OBJ_NAME (gpointer ptr) {                                         \
    return G_TYPE_CHECK_INSTANCE_TYPE (ptr, module_obj_name##_get_type ()); }                            \
  G_GNUC_END_IGNORE_DEPRECATIONS

#define G_DECLARE_DERIVABLE_TYPE(ModuleObjName, module_obj_name, MODULE, OBJ_NAME, ParentName) \
  GType module_obj_name##_get_type (void);                                                               \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                       \
  typedef struct _##ModuleObjName ModuleObjName;                                                         \
  typedef struct _##ModuleObjName##Class ModuleObjName##Class;                                           \
  struct _##ModuleObjName { ParentName parent_instance; };                                               \
                                                                                                         \
  _GLIB_DEFINE_AUTOPTR_CHAINUP (ModuleObjName, ParentName)                                               \
                                                                                                         \
  static inline ModuleObjName * MODULE##_##OBJ_NAME (gpointer ptr) {                                     \
    return G_TYPE_CHECK_INSTANCE_CAST (ptr, module_obj_name##_get_type (), ModuleObjName); }             \
  static inline ModuleObjName##Class * MODULE##_##OBJ_NAME##_CLASS (gpointer ptr) {                      \
    return G_TYPE_CHECK_CLASS_CAST (ptr, module_obj_name##_get_type (), ModuleObjName##Class); }         \
  static inline gboolean MODULE##_IS_##OBJ_NAME (gpointer ptr) {                                         \
    return G_TYPE_CHECK_INSTANCE_TYPE (ptr, module_obj_name##_get_type ()); }                            \
  static inline gboolean MODULE##_IS_##OBJ_NAME##_CLASS (gpointer ptr) {                                 \
    return G_TYPE_CHECK_CLASS_TYPE (ptr, module_obj_name##_get_type ()); }                               \
  static inline ModuleObjName##Class * MODULE##_##OBJ_NAME##_GET_CLASS (gpointer ptr) {                  \
    return G_TYPE_INSTANCE_GET_CLASS (ptr, module_obj_name##_get_type (), ModuleObjName##Class); }       \
  G_GNUC_END_IGNORE_DEPRECATIONS

static inline void
g_autoptr_cleanup_generic_gfree (void *p)
{
  void **pp = (void**)p;
  g_free (*pp);
}

static inline void
g_autoptr_cleanup_gstring_free (GString *string)
{
  if (string)
    g_string_free (string, TRUE);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAsyncQueue, g_async_queue_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBookmarkFile, g_bookmark_file_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBytes, g_bytes_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GChecksum, g_checksum_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDateTime, g_date_time_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDir, g_dir_close)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GError, g_error_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GHashTable, g_hash_table_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GHmac, g_hmac_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GIOChannel, g_io_channel_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GKeyFile, g_key_file_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GList, g_list_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GArray, g_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPtrArray, g_ptr_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GByteArray, g_byte_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMainContext, g_main_context_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMainLoop, g_main_loop_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSource, g_source_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMappedFile, g_mapped_file_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMarkupParseContext, g_markup_parse_context_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNode, g_node_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GOptionContext, g_option_context_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPatternSpec, g_pattern_spec_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GQueue, g_queue_free)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GQueue, g_queue_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GRand, g_rand_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GRegex, g_regex_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMatchInfo, g_match_info_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GScanner, g_scanner_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSequence, g_sequence_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSList, g_slist_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GString, g_autoptr_cleanup_gstring_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GStringChunk, g_string_chunk_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GThread, g_thread_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GMutex, g_mutex_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GCond, g_cond_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTimer, g_timer_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTimeZone, g_time_zone_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTree, g_tree_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariant, g_variant_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantBuilder, g_variant_builder_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GVariantBuilder, g_variant_builder_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantIter, g_variant_iter_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantDict, g_variant_dict_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GVariantDict, g_variant_dict_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantType, g_variant_type_free)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(GStrv, g_strfreev, NULL)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GObject, g_object_unref)

#ifdef __G_IO_H__
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAction, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GActionMap, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAppInfo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAppLaunchContext, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAppInfoMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GApplicationCommandLine, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GApplication, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAsyncInitable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAsyncResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBufferedInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBufferedOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBytesIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GCancellable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GCharsetConverter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GConverter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GConverterInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GConverterOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GCredentials, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDataInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDataOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusActionGroup, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusAuthObserver, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusInterface, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusInterfaceSkeleton, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusMenuModel, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusMethodInvocation, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusNodeInfo, g_dbus_node_info_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObject, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObjectManagerClient, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObjectManager, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObjectManagerServer, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObjectProxy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusObjectSkeleton, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusProxy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusServer, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDrive, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GEmblemedIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GEmblem, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileEnumerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFile, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileInfo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileIOStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFilenameCompleter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFilterInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFilterOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInetAddress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInetAddressMask, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInetSocketAddress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInitable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GIOModule, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GIOStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GLoadableIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMemoryInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMemoryOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMenu, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMenuItem, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMenuModel, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMenuAttributeIter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMenuLinkIter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMount, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMountOperation, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNativeVolumeMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNetworkAddress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNetworkMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNetworkService, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNotification, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPermission, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPollableInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPollableOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPropertyAction, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GProxyAddressEnumerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GProxyAddress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GProxy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GProxyResolver, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GRemoteActionGroup, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GResolver, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSeekable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSettingsBackend, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSettingsSchema, g_settings_schema_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSettings, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSimpleActionGroup, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSimpleAction, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSimpleAsyncResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSimplePermission, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSimpleProxyResolver, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketAddressEnumerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketAddress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketClient, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketConnectable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketControlMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocket, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketListener, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketService, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSubprocess, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSubprocessLauncher, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTask, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTcpConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTcpWrapperConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTestDBus, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GThemedIcon, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GThreadedSocketService, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsBackend, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsCertificate, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsClientConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsDatabase, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsFileDatabase, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsInteraction, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsPassword, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsServerConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVfs, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVolume, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVolumeMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GZlibCompressor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GZlibDecompressor, g_object_unref)
#endif /* __G_IO_H__ */

#endif /* GLIB_VERSION_2_44 */

#endif /* __GLIB_COMPAT_H__ */
