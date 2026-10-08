// A small DOM, just enough to run the setup page's script in node:vm and look at what it built.
// It has the page's static elements (by id) and creates the rest as the script asks.
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';

class FakeNode {
  constructor(tag, id = '') {
    this.tagName = tag.toUpperCase();
    this.id = id;
    this.children = [];
    this.listeners = new Map();
    this.attributes = new Map();
    this.className = '';
    this.value = '';
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.type = '';
    this.title = '';
    this.innerHTML = '';
    this.own = '';
  }

  get textContent() {
    return this.own + this.children.map((child) => child.textContent ?? child).join('');
  }

  set textContent(value) {
    this.own = String(value);
    this.children = [];
  }

  append(...items) {
    for (const item of items) this.children.push(item);
  }

  replaceChildren(...items) {
    this.children = [];
    this.own = '';
    this.append(...items);
  }

  setAttribute(name, value) {
    this.attributes.set(name, String(value));
  }

  getAttribute(name) {
    return this.attributes.get(name) ?? null;
  }

  addEventListener(type, handler) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(handler);
  }

  // Run the handlers of an event, as the browser would, and let the script finish its async work.
  async dispatch(type, extra = {}) {
    const event = { type, target: this, preventDefault() {}, ...extra };
    const handlers = [...(this.listeners.get(type) ?? [])];
    if (typeof this[`on${type}`] === 'function') handlers.push(this[`on${type}`]);
    for (const handler of handlers) handler(event);
    await flush();
  }

  click() {
    return this.dispatch('click');
  }

  async type_(text) {
    this.value = text;
    await this.dispatch('input');
  }

  focus() {
    this.focused = true;
  }

  setSelectionRange() {}
}

export const flush = async () => {
  for (let i = 0; i < 12; i += 1) await new Promise((resolve) => setImmediate(resolve));
};

export const all = (root) => [
  root,
  ...root.children.filter((c) => c instanceof FakeNode).flatMap(all),
];
export const find = (root, predicate) => all(root).find(predicate);
export const byClass = (root, name) =>
  all(root).filter((n) => n.className.split(' ').includes(name));
export const byText = (root, text, tag) =>
  all(root).find((n) => (!tag || n.tagName === tag.toUpperCase()) && n.textContent === text);
export const textOf = (root) => root.textContent;

// Load the page. `html` is the built page; the script inside it runs against a fake DOM.
export function loadPage(
  html,
  {
    protocol = 'file:',
    pathname = '/',
    origin = 'null',
    hash = '',
    search = '',
    serial = null,
    fetchImpl = null,
    top = true,
  } = {},
) {
  const ids = new Map();
  for (const [, id] of html.matchAll(/\sid="([^"]+)"/g)) ids.set(id, new FakeNode('div', id));
  const documentListeners = new Map();
  const windowListeners = new Map();
  let now = 1_800_000_000_000;
  let nextTimer = 1;
  const timers = new Map();
  const log = { fetches: [], opened: [], replaced: [], errors: [] };
  const document = {
    hidden: false,
    documentElement: new FakeNode('html'),
    getElementById: (id) => ids.get(id) ?? null,
    createElement: (tag) => new FakeNode(tag),
    addEventListener: (type, handler) => documentListeners.set(type, handler),
  };
  const self = {};
  const window = {
    self,
    top: top ? self : {},
    addEventListener: (type, handler) => windowListeners.set(type, handler),
    open: (...args) => log.opened.push(args),
  };
  const location = { protocol, pathname, origin, hash, search };
  const context = {
    document,
    window,
    location,
    history: { replaceState: (_s, _t, url) => log.replaced.push(url) },
    navigator: { serial: serial ?? undefined },
    fetch: async (...args) => {
      log.fetches.push(args);
      if (!fetchImpl) throw Error('unexpected network request');
      return fetchImpl(...args);
    },
    crypto: webcrypto,
    Date: { now: () => now },
    TextEncoder,
    TextDecoder,
    URLSearchParams,
    URL,
    ReadableStream,
    WritableStream,
    console,
    setTimeout(callback, ms = 0) {
      if (ms <= 0) {
        // A zero delay is not virtual time: let the event loop run it.
        setImmediate(callback);
        return 0;
      }
      const id = nextTimer++;
      timers.set(id, { callback, at: now + ms, every: 0 });
      return id;
    },
    setInterval(callback, ms) {
      const id = nextTimer++;
      timers.set(id, { callback, at: now + ms, every: ms });
      return id;
    },
    clearTimeout: (id) => timers.delete(id),
    clearInterval: (id) => timers.delete(id),
  };
  vm.createContext(context);
  const script = html.match(/<script type="module">([\s\S]*?)<\/script>/)[1];
  let thrown = null;
  try {
    vm.runInContext(script, context);
  } catch (error) {
    thrown = error;
  }
  return {
    document,
    window,
    location,
    context,
    log,
    thrown,
    node: (id) => ids.get(id),
    documentEvent: async (type, event = {}) => {
      documentListeners.get(type)?.({ type, ...event });
      await flush();
    },
    windowEvent: async (type) => {
      windowListeners.get(type)?.({ type });
      await flush();
    },
    // Move the page's clock forward, running timers that fall due, then let promises settle.
    async advance(ms, step = 250) {
      for (let spent = 0; spent < ms; spent += step) {
        now += Math.min(step, ms - spent);
        for (const [id, timer] of [...timers]) {
          while (timers.has(id) && timer.at <= now) {
            if (timer.every) timer.at += timer.every;
            else timers.delete(id);
            timer.callback();
          }
        }
        await flush();
      }
    },
    timerCount: () => timers.size,
  };
}
