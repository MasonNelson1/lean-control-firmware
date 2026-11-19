// Render installation icons from the unchanged wheelchair SVG using Chromium.
// Run after npm ci and npx playwright install chromium. On Windows an installed
// Chrome is used; LEAN_BROWSER_CHANNEL overrides the browser channel anywhere.
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
async function main() {
  const channel = process.env.LEAN_BROWSER_CHANNEL || (process.platform === 'win32' ? 'chrome' : undefined);
  const browser = await chromium.launch({headless: true, ...(channel ? {channel} : {})});
  try {
    const page = await browser.newPage();
    const svg = fs.readFileSync(path.join(__dirname, 'assets/lean-wheelchair.svg'), 'utf8');
    for (const [name, size, maskable] of [['192', 192, false], ['512', 512, false], ['512-maskable', 512, true]]) {
      const png = await page.evaluate(async ({svg, size, maskable}) => {
        const image = new Image();
        image.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = canvas.height = size;
        const context = canvas.getContext('2d');
        if (maskable) { context.fillStyle = '#fff'; context.fillRect(0, 0, size, size); }
        // Only the maskable variant has an opaque background, with the complete
        // logo inside the safe circle. Ordinary icons preserve transparency.
        const bounds = size * (maskable ? .60 : .90);
        const scale = Math.min(bounds / image.naturalWidth, bounds / image.naturalHeight);
        const width = image.naturalWidth * scale, height = image.naturalHeight * scale;
        context.drawImage(image, (size - width) / 2, (size - height) / 2, width, height);
        return canvas.toDataURL('image/png').split(',')[1];
      }, {svg, size, maskable});
      fs.writeFileSync(path.join(__dirname, 'icons', name + '.png'), Buffer.from(png, 'base64'));
    }
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
