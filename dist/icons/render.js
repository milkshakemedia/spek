// Render the toolbar SVGs to PNG with Chromium: node dist/icons/render.js OUTDIR
const { chromium } = require('playwright');
const fs = require('fs'), path = require('path');
(async () => {
  const out = process.argv[2];
  const b = await chromium.launch();
  const sizes = { open: 24, save: 24, help: 24, close: 16 };
  for (const [name, px] of Object.entries(sizes)) {
    const svg = fs.readFileSync(path.join(__dirname, name + '.svg'), 'utf8');
    for (const scale of [1, 2, 4]) {
      const p = await b.newPage({ viewport: { width: px, height: px }, deviceScaleFactor: scale });
      await p.setContent(`<html><body style="margin:0;background:transparent">${svg.replace('<svg ', `<svg width="${px}" height="${px}" `)}</body></html>`);
      const suffix = scale === 1 ? '' : `@${scale}x`;
      await p.screenshot({ path: path.join(out, `${name}${suffix}.png`), omitBackground: true });
      await p.close();
    }
  }
  await b.close();
})();
