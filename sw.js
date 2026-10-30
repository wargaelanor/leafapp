const CACHE = 'leaf-v25';
const ASSETS = [
  './', './index.html', './manifest.webmanifest', './leaf.png',
  './icon-192.png', './icon-512.png', './apple-touch-icon.png',
  './fonts/nissan_regular.ttf', './fonts/nissan_light.ttf',
  // car status overlays — optional, but pre-cached when present
  './car-lights.png', './car-cable.png',
  './car-wheel1.png', './car-wheel2.png', './car-wheel3.png', './car-wheel4.png'
];

self.addEventListener('install', e => {
  // Add assets one by one: the optional car overlays may not exist yet, and a
  // single 404 must not break the whole service worker install.
  e.waitUntil(
    caches.open(CACHE)
      .then(c => Promise.all(ASSETS.map(a => c.add(a).catch(() => null))))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  const url = new URL(e.request.url);
  // never cache API, settings, OTA or cross-origin requests
  if (url.pathname.startsWith('/api/')) return;
  if (url.pathname.startsWith('/settings') || url.pathname.startsWith('/ota')) return;
  if (url.origin !== location.origin) return;

  // navigations: network-first so a deploy is picked up immediately,
  // cache as offline fallback
  if (e.request.mode === 'navigate') {
    e.respondWith(
      fetch(e.request).then(resp => {
        if (resp.ok) {
          const clone = resp.clone();
          caches.open(CACHE).then(c => c.put('./index.html', clone));
        }
        return resp;
      }).catch(() => caches.match('./index.html'))
    );
    return;
  }

  // static assets: cache-first, refreshed in background
  e.respondWith(
    caches.match(e.request).then(cached => {
      const fetched = fetch(e.request).then(resp => {
        if (resp.ok) {
          const clone = resp.clone();
          caches.open(CACHE).then(c => c.put(e.request, clone));
        }
        return resp;
      }).catch(() => cached);
      return cached || fetched;
    })
  );
});
