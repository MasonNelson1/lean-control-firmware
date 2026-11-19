'use strict';

const base = self.registration.scope;
const prefix = 'lean-debug-viewer:' + base + ':';
const cacheName = prefix + __REVISION__;
const assets = __ASSETS__.map(asset => new URL(asset, base).href);

self.addEventListener('install', event => {
  event.waitUntil(caches.open(cacheName).then(cache => cache.addAll(assets)));
  // Keep the default waiting lifecycle. Never skipWaiting or force a reload:
  // open windows may own a serial connection or unapplied configuration edits.
});
self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(names => Promise.all(names
    .filter(name => name.startsWith(prefix) && name !== cacheName)
    .map(name => caches.delete(name)))));
});
self.addEventListener('fetch', event => {
  const request = event.request;
  if (request.method !== 'GET' || !request.url.startsWith(base)) return;
  const url = new URL(request.url);
  url.search = '';
  const asset = request.mode === 'navigate' ? new URL('index.html', base).href : url.href;
  if (!assets.includes(asset)) return;
  event.respondWith(caches.open(cacheName).then(async cache =>
    (await cache.match(asset)) || fetch(request)));
});
