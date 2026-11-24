const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { Script } = require('node:vm');
const { gzipSync } = require('node:zlib');
const { chromium } = require('playwright');
const firmware = path.resolve(__dirname, '../..');
const source = path.join(firmware, 'master', 'main', 'www', 'index.html');
const evidence = path.join(firmware, 'test-output', 'dashboard');
fs.mkdirSync(evidence, { recursive: true });
const body = fs.readFileSync(source, 'utf8');
new Script(body.split('<script>')[1].split('</script>')[0]);
console.log('Syntax passed. HTML bytes:', Buffer.byteLength(body), 'gzip:', gzipSync(body, {level:9}).length);

const tile = Buffer.alloc(240000);
for (let py = 0; py < 400; py++) for (let px = 0; px < 600; px++) {
  const cx = -0.5 + (px / 600 - .5) * 3.1;
  const cy = (py / 400 - .5) * 3.1 * 2 / 3;
  let x = 0, y = 0, count = 0;
  while (x * x + y * y <= 4 && count < 95) {
    const xx = x * x - y * y + cx;
    y = 2 * x * y + cy;
    x = xx;
    count++;
  }
  tile[py * 600 + px] = count === 95 ? 255 : Math.floor(count * 254 / 95);
}
function pageTiles(since, frame, first) {
  const start = Math.max(0, since - first);
  const count = Math.max(0, Math.min(8, 150 - start));
  const packet = Buffer.alloc(8 + count * 1604);
  packet.writeUInt16LE(frame, 0);
  packet.writeUInt32LE(first + start + count, 2);
  packet.writeUInt16LE(count, 6);
  for (let n = 0; n < count; n++) {
    const index = start + n, dst = 8 + 1604 * n;
    packet.writeUInt16LE(index, dst);
    packet.writeUInt8(index % 3 + 2, dst + 2);
    for (let row = 0; row < 40; row++) {
      const src = (Math.floor(index / 15) * 40 + row) * 600 + (index % 15) * 40;
      tile.copy(packet, dst + 4 + row * 40, src, src + 40);
    }
  }
  return packet;
}
const taskRows = [
  ['wifi',2,23,0,4,1232], ['tiT',2,18,1,2,1408], ['link_tx',2,10,0,3,1280],
  ['exec_0',0,5,1,41,968], ['exec_1',0,5,0,35,1008], ['exec_2',2,5,-1,0,1088],
  ['exec_3',2,5,-1,0,1088], ['logger',2,3,1,1,1736], ['IDLE0',1,0,0,14,792]
];
const runs = [
  [10,1,'T1 FCFS+RR #1',36.5,267,1030,5320,210,210,'bd447d0c',-1,0],
  [11,1,'T1 PRIO+RR #1',37.6,248,907,5073,210,210,'bd447d0c',-1,0],
  [12,1,'T1 AGING+RR #1',43.2,192,510,4121,210,210,'bd447d0c',-1,0],
  [13,1,'T1 AGING+RR #2',44.8,181,466,3999,210,210,'bd447d0c',-1,0],
  [14,2,'T2 2 workers + 4 producers',63.9,325,700,4850,310,310,'bd447d0c',1,0],
  [15,3,'T3 crash Worker A at 40%',40.2,299,1178,3660,150,150,'bd447d0c',1,421,4,4,810,[0,52,98,0]],
  [16,4,'T4 K=2 #1',51.8,266,790,2316,120,120,'00000000',-1,0,0,0,0,[0,43,38,39]],
  [17,5,'T5 n=32 3 workers #1',57.8,190,444,1730,100,100,'00000000',-1,0,0,0,0,[0,33,34,33]]
];
function makeState({ partial = false, fault = false, frame = 41, base = 0, busy = false, bench = true, singleCoreC = false } = {}) {
  const nodes = Array.from({ length:4 }, (_, i) => ({
    st: partial && i === 2 ? 3 : 1, ip: '192.168.4.' + (i + 1), mac: '00:00:ab:cd:ef:0' + i,
    chip: i ? 'ESP32' : 'ESP32-S3', cores: i === 3 && singleCoreC ? 1 : 2, mhz: 240, hb: i ? 150 + i * 20 : -1,
    cpu: partial && i === 2 ? [] : i === 3 ? [71, singleCoreC ? 0 : 64] : [i ? 42 : 19, i ? 68 : 8], k: 2, n: 4,
    drain: 0, slots_ready: 1, target_k: 2, cred: 1, disp: i * 112, res: i * 95,
    run: i ? 2 : 0, peak: 2, heap: i ? 97824 - i * 1224 : 213812,
    tasks: i === 3 && singleCoreC ? taskRows.filter(row => row[3] !== 1) : taskRows,
    lab: [173244,14028,200000,59028,2,60,152430,14143,15]
  }));
  const pending = [[1974,1,0,3,1,4,4,frame],[1977,2,2,2,1,0,0,0],[1982,3,1,1,1,0,0,0]];
  const inFlight = [[1944,2,2,1,1,3,90,0,0,0],[1956,3,2,2,1,3,120,0,0,0],[1984,1,1,3,1,3,90,5,2,frame]];
  return {
    t: 136042 + base, nodes, ord: 2, plc: 1, auto: 0, cli: 1, local: 0, blocked: 2,
    fr: {no: frame, done: 150, tot: 150, ver: base + 150, ms: 2773, zoom: 1, it: 95, own: '234'.repeat(50)},
    q: {n:3, cap:256, inf:3, fail:0, ret:4, lock:903, cont:14, old:[1001,720,199,25], byp:[0,1,2,0], top:pending, run:inFlight, failed:[]},
    st: {thr:43.7, p95:510, mean:192, done:1624, dup:1, wait:[22,73,122,11],
      recent:[[1939,1,3,1,101,1,10,2],[1941,1,2,2,117,1,8,3],[1942,1,0,3,83,1,7,5]],
      fail:fault ? { node:3,det:421,req:4,inj:1,age:13300 } : null},
    exp: {busy:busy ? 1 : 0, suite:3, run:2, of:2, label:'Worker A recovery'},
    runs:bench ? runs : [], log:[[11,20000,'SYS',0,'Master access point ready'],[12,80000,'RX',1,'Worker A joined the cluster'],[13,133000,'SUP',2,'Worker A disconnected. Four jobs requeued.']],
    logq:0, drop:0
  };
}

async function verify(browser, viewport, key) {
  const options = { viewport, reducedMotion: key === 'mobile' ? 'reduce' : 'no-preference' };
  if (key === 'mobile') Object.assign(options, { isMobile: true, hasTouch: true, deviceScaleFactor: 2 });
  const page = await browser.newPage(options);
  const errors = [], networks = [], sockets = [], commands = [], watermarks = [];
  let frame = 41, base = 0, earlyFrame = true;
  let latestSnapshot;
  function sendState(state) {
    latestSnapshot = JSON.stringify(state);
    sockets[0].send(latestSnapshot);
  }
  // Mirror periodic board telemetry so timing between assertions cannot close the fixture socket.
  const telemetry = setInterval(() => {
    if (latestSnapshot) sockets[0].send(latestSnapshot);
  }, 500);
  page.once('close', () => clearInterval(telemetry));
  page.on('pageerror', e => errors.push(e.message));
  page.on('request', req => networks.push(req.url()));
  await page.routeWebSocket('**/ws', socket => sockets.push(socket));
  await page.route('http://nexus.local/**', async route => {
    const u = new URL(route.request().url());
    if (u.pathname === '/') return route.fulfill({ status:200, contentType:'text/html', body });
    if (u.pathname === '/api/tiles') {
      const since = Number(u.searchParams.get('since'));
      watermarks.push([frame, since]);
      if (earlyFrame) { earlyFrame = false; return route.fulfill({ status:200, contentType:'application/octet-stream', body:pageTiles(since, frame + 1, base) }); }
      return route.fulfill({ status:200, contentType:'application/octet-stream', body:pageTiles(since, frame, base) });
    }
    if (u.pathname === '/api/cmd') {
      commands.push(Object.fromEntries(u.searchParams));
      const blocked = u.searchParams.get('op') === 'storm';
      return route.fulfill({ status:blocked ? 409 : 200, contentType:'text/plain', body:blocked ? 'Cluster unavailable for storm' : 'ok' });
    }
    return route.fulfill({ status:404, body:'not found' });
  });

  await page.goto('http://nexus.local/');
  await page.waitForFunction(() => document.querySelector('#conn').textContent === 'Connecting');
  assert.equal(await page.locator('#fLocal').isDisabled(), true);
  if (key === 'desktop') await page.keyboard.press('Tab');
  if (key === 'desktop') assert.equal(await page.evaluate(() => document.activeElement?.className), 'skip');
  await page.waitForFunction(() => document.querySelector('#chWait').height > 100);
  assert.ok(sockets.length);
  sendState(makeState({ partial:true, bench:false, singleCoreC:true }));
  await page.waitForFunction(() => document.querySelector('#conn').textContent === 'Live');
  await page.waitForFunction(() => document.querySelector('#nodeRows').textContent.includes('OFFLINE'));
  assert.equal(await page.locator('#nodeRows .nrow').count(), 4);
  assert.equal(await page.locator('#k-nodes').textContent(), '3/4 nodes');
  await page.getByRole('button', {name:'Worker B',exact:true}).click();
  assert.ok((await page.locator('#taskWrap').textContent()).includes('No current task samples'));
  assert.equal(await page.locator('#benchEmpty').isVisible(), true);
  if (key === 'desktop') await page.screenshot({path:path.join(evidence,'partial.png'), animations:'disabled'});
  await page.getByRole('button', {name:'Worker C',exact:true}).click();
  assert.equal(await page.locator('#coreOne').isVisible(), false);
  assert.equal(await page.locator('#c0').textContent(), '71%');
  assert.equal(await page.locator('#slots3 .slot.over').count(), 1);

  frame = 42; base = 150;
  sendState(makeState({ frame, base, fault:true }));
  await page.waitForFunction(() => tileVer === 300);
  assert.ok(watermarks.some(([f,s]) => f === 42 && s === 0), 'request should reset after a mismatched frame');
  assert.ok(watermarks.some(([f,s]) => f === 42 && s === 158), 'page follows a server watermark');
  assert.equal(await page.locator('#frameProgress').getAttribute('aria-valuenow'), '150');
  assert.equal(await page.locator('#k-nodes').textContent(), '4/4 nodes');
  assert.ok((await page.locator('#fstat').textContent()).includes('C 50'));
  assert.ok((await page.locator('#colR').textContent()).includes('C · running'));
  assert.ok((await page.locator('#colD').textContent()).includes('C · 83 ms'));
  assert.ok((await page.locator('#recov').textContent()).includes('4 requeued'));
  assert.ok((await page.locator('#recov').textContent()).includes('Worker C'));
  assert.equal(await page.locator('#benchChart .bench-bar').count(), 8);
  await page.locator('#benchSuite').selectOption('5');
  assert.ok((await page.locator('#benchChart').textContent()).includes('3 workers'));
  assert.ok((await page.locator('#expRuns').textContent()).includes('C 33'));
  await page.locator('#benchSuite').selectOption('1');
  await page.locator('#benchMetric').selectOption('5');
  assert.equal(await page.locator('#benchChart .bench-bar').count(), 3);
  assert.ok((await page.locator('#benchChart').textContent()).includes('488 ms avg'));
  await page.locator('#benchSuite').selectOption('0');

  await page.locator('#stage').focus();
  await page.keyboard.press('Enter');
  await page.waitForFunction(() => document.querySelector('#commandStatus').textContent === 'Command accepted.');
  assert.deepEqual(commands.find(c => c.op === 'zoom'), { op:'zoom', x:'0.5', y:'0.5' });
  await page.locator('#fStorm').click();
  await page.getByText('Cluster unavailable for storm').waitFor();
  assert.equal(await page.locator('#commandStatus').evaluate(el => el.classList.contains('error')), true);
  await page.getByRole('button', {name:'Worker overlay'}).click();
  assert.equal(await page.getByRole('button', {name:'Worker overlay'}).getAttribute('aria-pressed'), 'true');
  await page.waitForFunction(() => document.querySelector('#ocv').getContext('2d').getImageData(100, 20, 1, 1).data[3] > 0);
  await page.getByRole('button', {name:'Worker overlay'}).click();
  await page.getByRole('button', {name:'Worker C',exact:true}).click();
  assert.equal(await page.locator('#coreOne').isVisible(), true);
  assert.equal(await page.locator('#c0').textContent(), '71%');
  assert.equal(await page.locator('#c1').textContent(), '64%');
  assert.equal(await page.locator('#slots3 .slot.busy').count(), 2);
  assert.equal(await page.locator('#slots3 .slot.over').count(), 0);
  await page.getByRole('button', {name:'Increase Worker C permits'}).focus();
  sendState(makeState({ frame, base:150, fault:true }));
  await page.waitForTimeout(100);
  assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('aria-label')), 'Increase Worker C permits');
  await Promise.all([
    page.waitForResponse(r => r.url().includes('/api/cmd?op=k')),
    page.keyboard.press('Enter')
  ]);
  assert.ok(commands.some(c => c.op === 'k' && c.node === '3' && c.v === '3'));
  await Promise.all([
    page.waitForResponse(r => r.url().includes('/api/cmd?op=drain')),
    page.locator('#drainC').click()
  ]);
  assert.ok(commands.some(c => c.op === 'drain' && c.node === '3' && c.v === '1'));
  await page.locator('#faultTarget').selectOption('3');
  await Promise.all([page.waitForResponse(r => r.url().includes('/api/cmd?op=mute')), page.locator('#fPart').click()]);
  assert.ok(commands.some(c => c.op === 'mute' && c.node === '3' && c.v === '4000'));
  await Promise.all([page.waitForResponse(r => r.url().includes('/api/cmd?op=reboot')), page.locator('#fKillC').click()]);
  assert.ok(commands.some(c => c.op === 'reboot' && c.node === '3'));
  await page.locator('#labTarget').selectOption('3');
  await Promise.all([page.waitForResponse(r => r.url().includes('/api/cmd?op=lab')), page.locator('[data-lab="C"]').click()]);
  assert.ok(commands.some(c => c.op === 'lab' && c.node === '3' && c.v === '2'));
  assert.equal(await page.locator('#labWhere').textContent(), 'Runs on Worker C');
  const noOverflow = await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth);
  assert.equal(noOverflow, true, 'viewport has horizontal page overflow');
  if (key === 'mobile') assert.ok(await page.locator('#btnMap').evaluate(b => b.getBoundingClientRect().height >= 44));

  await page.evaluate(() => window.scrollTo(0,0));
  await page.locator('#commandStatus').evaluate(el => { el.textContent = ''; });
  await page.screenshot({path:path.join(evidence,key+'.png'), animations:'disabled'});
  if (key === 'desktop') await page.locator('#benchmarks').screenshot({path:path.join(evidence,'benchmarks.png'), animations:'disabled'});

  sendState(makeState({ frame, base:150, busy:true }));
  await page.waitForFunction(() => document.querySelector('#expStop').disabled === false);
  assert.equal(await page.locator('#drainC').isDisabled(), true);
  assert.equal(await page.locator('#stage').getAttribute('aria-disabled'), 'true');
  await Promise.all([
    page.waitForResponse(r => r.url().includes('/api/cmd?op=expstop')),
    page.locator('#expStop').click()
  ]);
  assert.ok(commands.some(c => c.op === 'expstop'));
  clearInterval(telemetry);
  sockets[0].close();
  await page.waitForFunction(() => document.querySelector('#conn').textContent === 'Reconnecting');
  assert.equal(await page.locator('#fLocal').isDisabled(), true);
  assert.deepEqual(errors, []);
  assert.ok(networks.every(r => r.startsWith('http://nexus.local/')), 'external network request during offline operation');
  await page.close();
  return { key, viewport, requests: networks.length, tileRequests: watermarks.length, commands };
}
(async () => {
  const browser = await chromium.launch({executablePath:process.env.CHROME_PATH || undefined,headless:true});
  const results = [];
  try {
    for (const [viewport,key] of [[{width:1440,height:900},'desktop'],[{width:390,height:844},'mobile']]) {
      const result = await verify(browser,viewport,key);
      results.push(result);
      console.log(JSON.stringify(result));
    }
    fs.writeFileSync(path.join(evidence, 'smoke.json'), JSON.stringify(results, null, 2) + '\n');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode=1; });
