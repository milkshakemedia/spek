// Render the app icon SVG to PNGs with Chromium: node dist/appicon/render.js SVG OUTDIR
// Writes spek-<size>.png for each size the Mac, Windows and Linux icons need.
const { chromium } = require('playwright');
const fs = require('fs'), path = require('path');
(async () => {
  const [svgPath, out] = process.argv.slice(2);
  const svg = fs.readFileSync(svgPath, 'utf8');
  const b = await chromium.launch();
  for (const px of [16, 22, 24, 32, 48, 64, 96, 128, 256, 512, 1024]) {
    const p = await b.newPage({ viewport: { width: px, height: px } });
    await p.setContent(`<html><body style="margin:0;background:transparent">${svg.replace('<svg ', `<svg width="${px}" height="${px}" `)}</body></html>`);
    await p.screenshot({ path: path.join(out, `spek-${px}.png`), omitBackground: true });
    await p.close();
  }
  await b.close();
})();
