'use strict';

// Python injects this flag into its HTML response. Static hosts use the default.
const dashboardBackend = window.LEAN_LOCAL_BACKEND === true;

window.addEventListener('load', () => {
  if (dashboardBackend || !('serviceWorker' in navigator)) return;
  navigator.serviceWorker.register('./service-worker.js', {scope: './', updateViaCache: 'none'})
    .catch(error => console.warn('Offline installation unavailable:', error));
});
