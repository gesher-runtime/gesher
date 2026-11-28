// Copyright (C) 2026 The Gesher Authors; All rights reserved.
// Authentication metadata only: never subscribes to requestBody or intercepts bodies.
'use strict';
(() => {
  const owners = new Map(), registrations = new Map(), pending = new Map(), requests = new Map();
  let sequence = 0;
  function emit(owner, event, fields = {}) {
    const binding = globalThis[owner.binding];
    if (typeof binding !== 'function') return false;
    binding(JSON.stringify({version: 1, channel: owner.channel, epoch: owner.epoch, event, ...fields}));
    return true;
  }
  function settle(ticket, response) {
    const item = pending.get(ticket);
    if (!item) return false;
    pending.delete(ticket);
    item.callback(response);
    return true;
  }
  function cancel(owner, targetId) {
    for (const [ticket, item] of [...pending]) {
      if (item.owner === owner && (!targetId || item.targetId === targetId)) settle(ticket, {cancel: true});
    }
  }
  function unregister(owner, targetId) {
    owner.registering.delete(targetId);
    for (const [requestId, request] of requests) {
      if (request.owner === owner && (!targetId || request.targetId === targetId)) requests.delete(requestId);
    }
    for (const [tab, item] of registrations) {
      if (item.owner === owner && (!targetId || item.targetId === targetId)) registrations.delete(tab);
    }
  }
  function lookup(done) {
    chrome.debugger.getTargets(list => done(chrome.runtime.lastError ? null : list));
  }
  function prune(list, keep) {
    if (!list) return;
    const live = new Map(list.map(target => [target.id, target.tabId]));
    for (const [tab, registration] of [...registrations]) {
      if (live.get(registration.targetId) !== tab) {
        cancel(registration.owner, registration.targetId); registrations.delete(tab);
      }
    }
    for (const owner of [...owners.values()]) {
      const registered = [...registrations.values()].some(item => item.owner === owner);
      if (owner !== keep && !live.has(owner.bootTarget) && !registered && !owner.registering.size) {
        cancel(owner); owners.delete(owner.channel); delete globalThis[owner.binding];
      }
    }
  }
  globalThis.gesherAuthDispatch = command => {
    if (!command || typeof command.channel !== 'string' || !command.channel) return false;
    let owner = owners.get(command.channel);
    if (command.op === 'hello') {
      if (typeof command.epoch !== 'string' || !command.epoch
          || typeof command.binding !== 'string' || !/^gesherAuth_[a-zA-Z0-9_]+$/.test(command.binding)
          || typeof globalThis[command.binding] !== 'function') return false;
      if (!owner) {
        if (owners.size >= 256) {
          lookup(list => {
            prune(list, null);
            if (owners.size < 256) globalThis.gesherAuthDispatch(command);
            else globalThis[command.binding](JSON.stringify({version: 1, channel: command.channel,
              epoch: command.epoch, event: 'unavailable'}));
          });
          return true;
        }
        owner = {channel: command.channel, epoch: '', binding: command.binding, bootTarget: command.bootTarget || '', registering: new Map()};
        owners.set(command.channel, owner);
      }
      if (owner.binding !== command.binding) return false;
      if (owner.epoch !== command.epoch) {
        cancel(owner); unregister(owner); owner.registering.clear(); owner.epoch = command.epoch;
      }
      owner.bootTarget = command.bootTarget || owner.bootTarget;
      const epoch = owner.epoch;
      lookup(list => {
        prune(list, owner);
        if (owners.get(owner.channel) === owner && owner.epoch === epoch) emit(owner, list ? 'ready' : 'unavailable');
      });
      return true;
    }
    if (!owner || command.epoch !== owner.epoch) return false;
    if (command.op === 'register') {
      const targetId = command.targetId;
      if (typeof targetId !== 'string' || !targetId) return false;
      const existingTarget = [...registrations.values()].some(item => item.owner === owner && item.targetId === targetId);
      if (owner.registering.size >= 256 || registrations.size >= 256 && !existingTarget) {
        emit(owner, 'registrationFailed', {targetId}); return false;
      }
      const epoch = owner.epoch, identity = {};
      owner.registering.set(targetId, identity);
      lookup(list => {
        if (owners.get(owner.channel) !== owner || epoch !== owner.epoch
            || owner.registering.get(targetId) !== identity) return;
        owner.registering.delete(targetId);
        const target = list && list.find(value => value.id === targetId);
        const existing = target && registrations.get(target.tabId);
        if (!target || !Number.isInteger(target.tabId) || target.tabId < 0 || existing && existing.owner !== owner) {
          emit(owner, 'registrationFailed', {targetId}); return;
        }
        registrations.set(target.tabId, {owner, targetId, generation: String(command.generation || '0')});
        emit(owner, 'registered', {targetId});
      });
      return true;
    }
    if (command.op === 'respond') {
      const item = pending.get(command.ticket);
      if (!item || item.owner !== owner || item.targetId !== command.targetId) return false;
      if (command.proceed === true && typeof command.user === 'string' && typeof command.password === 'string') {
        return settle(command.ticket, {authCredentials: {username: command.user, password: command.password}});
      }
      return settle(command.ticket, {cancel: true});
    }
    if (command.op === 'cancelTarget' || command.op === 'unregister') {
      owner.registering.delete(command.targetId);
      for (const item of registrations.values()) {
        if (item.owner === owner && item.targetId === command.targetId) item.generation = String(command.generation || '0');
      }
      cancel(owner, command.targetId);
      if (command.op === 'unregister') unregister(owner, command.targetId);
      return true;
    }
    if (command.op === 'shutdown') {
      cancel(owner); unregister(owner); owner.registering.clear(); owners.delete(owner.channel);
      delete globalThis[owner.binding]; return true;
    }
    return false;
  };
  chrome.webRequest.onBeforeRequest.addListener(details => {
    const registration = registrations.get(details.tabId);
    if (!registration || requests.has(details.requestId) || requests.size >= 4096) return;
    requests.set(details.requestId, {owner: registration.owner, targetId: registration.targetId,
      generation: registration.generation, epoch: registration.owner.epoch});
  }, {urls: ['http://*/*', 'https://*/*']});
  chrome.webRequest.onAuthRequired.addListener((details, callback) => {
    const registration = registrations.get(details.tabId);
    if (!registration || typeof globalThis[registration.owner.binding] !== 'function') { callback({}); return; }
    const request = requests.get(details.requestId);
    const {owner, targetId} = registration;
    if (!request || request.owner !== owner || request.targetId !== targetId
        || request.epoch !== owner.epoch || request.generation !== registration.generation) { callback({cancel: true}); return; }
    const ownCount = [...pending.values()].filter(item => item.owner === owner).length;
    if (ownCount >= 64 || pending.size >= 256) {
      callback({cancel: true}); emit(owner, 'capacity', {targetId}); return;
    }
    const ticket = String(++sequence), item = {owner, targetId, requestId: details.requestId, callback};
    pending.set(ticket, item);
    lookup(list => {
      if (pending.get(ticket) !== item) return;
      const target = list && list.find(value => value.id === targetId && value.tabId === details.tabId);
      if (!target || registrations.get(details.tabId) !== registration
          || request.epoch !== owner.epoch || request.generation !== registration.generation
          || owners.get(owner.channel) !== owner) {
        settle(ticket, {cancel: true}); return;
      }
      if (!emit(owner, 'auth', {targetId, ticket, requestId: details.requestId,
        generation: request.generation, url: details.url,
        scheme: details.scheme || '', realm: details.realm || '', isProxy: details.isProxy === true,
        host: details.challenger && details.challenger.host || '', port: details.challenger && details.challenger.port || 0})) {
        settle(ticket, {cancel: true});
      }
    });
  }, {urls: ['http://*/*', 'https://*/*']}, ['asyncBlocking']);
  function ended(details) {
    requests.delete(details.requestId);
    for (const [ticket, item] of [...pending]) {
      if (item.requestId !== details.requestId) continue;
      pending.delete(ticket); emit(item.owner, 'ended', {targetId: item.targetId, ticket});
    }
  }
  chrome.webRequest.onCompleted.addListener(ended, {urls: ['http://*/*', 'https://*/*']});
  chrome.webRequest.onErrorOccurred.addListener(ended, {urls: ['http://*/*', 'https://*/*']});
})();
