// node --require pw-scrollbars.cjs: Playwright starts Chromium with --hide-scrollbars, so a page that scrolls loses
// its 10px scrollbar and lays out wider than in WebView2 (the 2.x window). This drops that flag for every launch in the
// process (the scenario scripts run unchanged). PW_DIR = folder with node_modules/playwright.
const { createRequire } = require('node:module');
const { chromium } = createRequire(process.env.PW_DIR || __filename)('playwright');
const launch = chromium.launch.bind(chromium);
chromium.launch = (o = {}) => launch({ ...o, ignoreDefaultArgs: [...(o.ignoreDefaultArgs || []), '--hide-scrollbars'] });
