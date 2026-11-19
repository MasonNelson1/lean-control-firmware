# Browser compression dependency

`fflate-0.8.3.js` is the unmodified `umd/index.js` from the npm package
`fflate@0.8.3`, maintained at https://github.com/101arrowz/fflate.
Its MIT license is included in `fflate-LICENSE.txt`.

The package is pinned in package-lock.json for reproducibility. After `npm ci`,
copy `node_modules/fflate/umd/index.js` and its LICENSE here to update the vendor
files. Update the filename, script reference, Python asset route, and packaging
allowlist together. Run profile interoperability and browser tests afterward.
Do not replace this with a runtime CDN: installation and offline use must work
with no third-party network requests.
