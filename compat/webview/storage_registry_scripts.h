// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Uses only the live built-in browser interfaces; no copied database or origin registry.
#pragma once

namespace gesher::webview::storage_registry_scripts {

inline constexpr wchar_t kInitializeIndexedDb[] = LR"JS((async () => {
    if (!['edge:', 'chrome:'].includes(location.protocol) || location.hostname !== 'indexeddb-internals')
      return {unavailable: 'not the built-in IndexedDB document', url: location.href};
    let module;
    try { module = await import('./indexed_db_internals.mojom-webui.js'); }
    catch (error) { return {unavailable: 'built-in IndexedDB bindings unavailable', detail: String(error)}; }
    if (typeof module.IdbInternalsHandler?.getRemote !== 'function')
      return {unavailable: 'IndexedDB discovery interface unavailable'};
    const handler = module.IdbInternalsHandler.getRemote();
    if (typeof handler?.getAllBucketsAcrossAllStorageKeys !== 'function')
      return {unavailable: 'IndexedDB discovery method unavailable'};
    const result = await handler.getAllBucketsAcrossAllStorageKeys();
    if (result?.error !== null) throw Error('IndexedDB discovery failed: ' + result?.error);
    if (!Array.isArray(result.partitions) || result.partitions.length === 0 || result.partitions.length > 64)
      throw Error('invalid IndexedDB discovery partitions');
    return {initialized: true, partitions: String(result.partitions.length)};
  })())JS";

inline constexpr wchar_t kReadRegistry[] = LR"JS((async () => {
    if (!['edge:', 'chrome:'].includes(location.protocol) || location.hostname !== 'quota-internals')
      return {unavailable: 'not the built-in quota document', url: location.href};
    let module;
    try { module = await import('./quota_internals_browser_proxy.js'); }
    catch (error) { return {unavailable: 'built-in proxy module unavailable', detail: String(error)}; }
    const Type = module.QuotaInternalsBrowserProxy;
    if (typeof Type?.getInstance !== 'function') return {unavailable: 'proxy class unavailable'};
    const proxy = Type.getInstance();
    if (typeof proxy.getGlobalUsage !== 'function' || typeof proxy.retrieveBucketsTable !== 'function')
      return {unavailable: 'live quota methods unavailable'};
    const global = await proxy.getGlobalUsage(0);
    const result = await proxy.retrieveBucketsTable();
    if (!Array.isArray(result.entries)) throw Error('missing bucket entries');
    return {globalUsage: String(global.usage), entries: result.entries.map(entry => ({
      storageKey: entry.storageKey, bucketId: String(entry.bucketId), name: entry.name, usage: String(entry.usage)
    }))};
  })())JS";

}  // namespace gesher::webview::storage_registry_scripts
