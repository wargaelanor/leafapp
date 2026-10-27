const CACHE = 'leaf-v15';
const ASSETS = [
  './', './index.html', './manifest.webmanifest', './leaf.webp',
  './icon-192.png', './icon-512.png', './apple-touch-icon.png',
  './fonts/nissan_regular.ttf', './fonts/nissan_light.ttf'
];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting()));
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
