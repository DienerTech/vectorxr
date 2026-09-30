// Captures the documentation screenshots in docs/screenshots from the desktop
// app's web build (browser mode, sample configuration from seed-config.ts).
//
//   npm --prefix app run dev        (in another terminal)
//   node --experimental-strip-types scripts/docs-screenshots/capture.ts [name ...]
//
// Uses headless Microsoft Edge through the Chrome DevTools Protocol, so no
// browser automation package is required. Set VECTORXR_BROWSER to use Chrome.
import { spawn } from 'node:child_process'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { sampleConfig } from './seed-config.ts'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const output = process.env.VECTORXR_SHOTS_OUT ?? join(root, 'docs', 'screenshots')
const browser = process.env.VECTORXR_BROWSER ?? 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
const appUrl = process.env.VECTORXR_APP_URL ?? 'http://localhost:5173/'
const port = 9333
const viewport = { width: 1300, height: 1000, deviceScaleFactor: 1.25 }

class Cdp {
  private id = 0
  private pending = new Map<number, { resolve: (value: any) => void; reject: (error: Error) => void }>()
  private socket: WebSocket
  constructor(socket: WebSocket) {
    this.socket = socket
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data))
      const waiter = message.id ? this.pending.get(message.id) : undefined
      if (!waiter) return
      this.pending.delete(message.id)
      if (message.error) waiter.reject(new Error(`${message.error.message}`))
      else waiter.resolve(message.result)
    })
  }
  static async connect(url: string) {
    const socket = new WebSocket(url)
    await new Promise((ready, fail) => { socket.onopen = ready; socket.onerror = fail })
    return new Cdp(socket)
  }
  send(method: string, params: Record<string, unknown> = {}) {
    const id = ++this.id
    this.socket.send(JSON.stringify({ id, method, params }))
    return new Promise<any>((resolve, reject) => this.pending.set(id, { resolve, reject }))
  }
  close() { this.socket.close() }
}

const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms))

// Page-side helpers, installed once per document.
const helpers = String.raw`
window.__shot = {
  visible(el) { const r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden' },
  text(el) { return (el.innerText ?? el.textContent ?? '').replace(/\s+/g, ' ').trim() },
  find(text, selector, exact) {
    const all = [...document.querySelectorAll(selector)].filter((el) => this.visible(el))
    return all.find((el) => this.text(el) === text) ?? (exact ? null : all.find((el) => this.text(el).includes(text)))
  },
  scroller() {
    return [...document.querySelectorAll('main, main *')].find((el) => {
      const style = getComputedStyle(el)
      return /(auto|scroll)/.test(style.overflowY) && el.scrollHeight > el.clientHeight + 4
    }) ?? document.scrollingElement
  },
}`

class Page {
  private cdp: Cdp
  constructor(cdp: Cdp) { this.cdp = cdp }
  async eval<T = unknown>(expression: string): Promise<T> {
    const result = await this.cdp.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? expression)
    return result.result.value as T
  }
  async load(theme: 'light' | 'dark' = 'light') {
    await this.cdp.send('Page.navigate', { url: appUrl })
    await sleep(800)
    await this.eval(`localStorage.setItem('vectorxr-config', ${JSON.stringify(JSON.stringify(sampleConfig()))});
      localStorage.setItem('vectorxr-theme-preference', '${theme}'); true`)
    await this.cdp.send('Page.reload', { ignoreCache: true })
    await sleep(1500)
    await this.eval(helpers)
    await this.eval('document.fonts.ready.then(() => true)')
  }
  async click(text: string, selector = 'button, a, label, summary, [role="button"], [role="tab"]', exact = true) {
    const found = await this.eval<boolean>(`(() => { const el = __shot.find(${JSON.stringify(text)}, ${JSON.stringify(selector)}, ${exact});
      if (!el) return false; el.scrollIntoView({ block: 'center' }); el.click(); return true })()`)
    if (!found) throw new Error(`No clickable element with text "${text}"`)
    await sleep(500)
  }
  async tab(label: string) { await this.click(label, 'nav button, nav a, aside button, aside a'); await this.top() }
  async top() { await this.eval('(__shot.scroller().scrollTop = 0, true)'); await sleep(300) }
  // Scrolls the content so the element containing text sits offset CSS px below the top.
  async scrollTo(text: string, offset = 24, selector = 'h1, h2, h3, h4, p, span, div, summary, button, label') {
    const ok = await this.eval<boolean>(`(() => {
      const el = __shot.find(${JSON.stringify(text)}, ${JSON.stringify(selector)}, false); if (!el) return false
      const scroller = __shot.scroller(); const box = scroller.getBoundingClientRect ? scroller.getBoundingClientRect().top : 0
      scroller.scrollTop += el.getBoundingClientRect().top - box - ${offset}; return true })()`)
    if (!ok) throw new Error(`No element with text "${text}"`)
    await sleep(400)
  }
  async scrollBy(pixels: number) { await this.eval(`(__shot.scroller().scrollTop += ${pixels}, true)`); await sleep(300) }
  async shot(name: string) {
    await sleep(450)
    const { data } = await this.cdp.send('Page.captureScreenshot', { format: 'jpeg', quality: 86 })
    writeFileSync(join(output, `${name}.jpg`), Buffer.from(data, 'base64'))
    console.log(`captured ${name}.jpg`)
    if (process.env.VECTORXR_SHOTS_TEXT) console.log(await this.eval<string>(`document.querySelector('main')?.innerText ?? document.body.innerText`))
  }
}

type Shot = { name: string; theme?: 'light' | 'dark'; run: (page: Page) => Promise<void> }

const tab = (label: string) => async (page: Page) => { await page.tab(label) }
const shots: Shot[] = [
  { name: 'home', run: tab('Home') },
  { name: 'home-dark', theme: 'dark', run: tab('Home') },
  { name: 'settings', run: tab('Settings') },
  { name: 'osd', run: async (p) => { await p.tab('On-Screen Display'); await p.click('Legible'); await p.top() } },
  { name: 'osd-compact', run: async (p) => {
    await p.tab('On-Screen Display'); await p.click('Compact'); await p.click('Legible'); await p.scrollTo('Compact', 16, 'button')
  } },
  { name: 'application-registry', run: tab('Application Registry') },
  { name: 'openxr-layer-manager', run: tab('OpenXR Layers') },
  { name: 'about', run: tab('About') },
  { name: 'quadviews', run: tab('Quadviews') },
  { name: 'quadviews-overlay-guide', run: async (p) => { await p.tab('Quadviews'); await p.click('How to read the overlay', 'button', false) } },
  { name: 'depth', run: tab('Depth') },
  { name: 'pivot', run: tab('Pivot') },
  { name: 'pivot-snap-views', run: async (p) => { await p.tab('Pivot'); await p.click('Edit Snap Views…'); await p.top() } },
  { name: 'pivot-snap-view-editor', run: async (p) => {
    await p.tab('Pivot'); await p.click('Edit Snap Views…'); await p.click('Edit'); await p.top()
  } },
  { name: 'pivot-nudges', run: async (p) => { await p.tab('Pivot'); await p.click('Edit'); await p.top() } },
  { name: 'turbo', run: tab('Turbo') },
  { name: 'turbo-profiles', run: async (p) => {
    await p.tab('Turbo'); await p.click('Advanced timing', 'summary', false); await p.scrollTo('Custom Profiles', 16, 'h2, h3, p, span, div')
  } },
  { name: 'turbo-runtime-behavior', run: async (p) => { await p.tab('Turbo'); await p.click('Runtime Behavior', 'button', false); await p.top() } },
  { name: 'turbo-safety', run: async (p) => { await p.tab('Turbo'); await p.click('Details…'); await p.top() } },
]

async function main() {
  const selected = process.argv.slice(2)
  const profile = mkdtempSync(join(tmpdir(), 'vectorxr-shots-'))
  const edge = spawn(browser, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-extensions', 'about:blank'], { stdio: 'ignore' })
  try {
    let target: { webSocketDebuggerUrl: string } | undefined
    for (let attempt = 0; attempt < 50 && !target; ++attempt) {
      await sleep(200)
      try {
        const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json() as any[]
        target = targets.find((entry) => entry.type === 'page')
      } catch { /* browser still starting */ }
    }
    if (!target) throw new Error('Browser did not expose a debugging target')
    const cdp = await Cdp.connect(target.webSocketDebuggerUrl)
    await cdp.send('Page.enable')
    await cdp.send('Runtime.enable')
    await cdp.send('Emulation.setDeviceMetricsOverride', { ...viewport, mobile: false })
    await cdp.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: 'reduce' }] })
    const page = new Page(cdp)
    for (const shot of shots.filter((entry) => !selected.length || selected.includes(entry.name))) {
      await page.load(shot.theme)
      await shot.run(page)
      await page.shot(shot.name)
    }
    cdp.close()
  } finally {
    edge.kill()
    await sleep(500)
    try { rmSync(profile, { recursive: true, force: true }) } catch { /* profile still locked */ }
  }
}

await main()
